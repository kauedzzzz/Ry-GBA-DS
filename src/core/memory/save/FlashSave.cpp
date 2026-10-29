#include "core/memory/save/FlashSave.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace gba::core {

FlashSave::FlashSave(bool is128KB)
    : m_is128KB(is128KB)
    , m_manufacturerDeviceId(is128KB ? 0x1362 : 0x1CC2) // 0x1362 = Sanyo 128KB (Pokémon), 0x1CC2 = Macronix 64KB
    , m_data(is128KB ? 128 * 1024 : 64 * 1024, 0xFF) {}

uint8_t FlashSave::readByte(uint32_t address) {
    const uint16_t offset = static_cast<uint16_t>(address & 0xFFFF);
    if (m_chipIdMode && offset < 2) {
        const uint8_t idByte = (offset == 0)
            ? static_cast<uint8_t>(m_manufacturerDeviceId & 0xFF)
            : static_cast<uint8_t>((m_manufacturerDeviceId >> 8) & 0xFF);
        if (m_traceEnabled) {
            std::cout << "[TRACE SAVE] Flash Read ChipID [0x0E00" << std::hex << std::setw(4) << std::setfill('0')
                      << offset << "] -> 0x" << std::setw(2) << static_cast<uint32_t>(idByte) << std::dec << "\n";
        }
        return idByte;
    }
    const std::size_t bankOffset = static_cast<std::size_t>(m_currentBank) * 65536u + offset;
    const uint8_t val = m_data[bankOffset % m_data.size()];
    if (m_traceEnabled) {
        static uint32_t readLogCount = 0;
        if (++readLogCount <= 16) {
            std::cout << "[TRACE SAVE] Flash Read Data (Bank " << static_cast<uint32_t>(m_currentBank)
                      << ") [0x0E00" << std::hex << std::setw(4) << std::setfill('0') << offset
                      << "] -> 0x" << std::setw(2) << static_cast<uint32_t>(val) << std::dec
                      << (readLogCount == 16 ? " (suprimindo leituras subsequentes...)" : "") << "\n";
        }
    }
    return val;
}

void FlashSave::writeByte(uint32_t address, uint8_t value) {
    const uint16_t offset = static_cast<uint16_t>(address & 0xFFFF);

    switch (m_state) {
        case State::Ready:
            if (offset == 0x5555 && value == 0xAA) {
                m_state = State::Cmd1;
            } else if (value == 0xF0) {
                m_chipIdMode = false;
                m_state = State::Ready;
            }
            break;

        case State::Cmd1:
            if (offset == 0x2AAA && value == 0x55) {
                m_state = State::Cmd2;
            } else {
                m_state = State::Ready;
            }
            break;

        case State::Cmd2:
            if (offset == 0x5555) {
                switch (value) {
                    case 0x90: // Enter Chip ID Mode
                        m_chipIdMode = true;
                        m_state = State::Ready;
                        if (m_traceEnabled) {
                            std::cout << "[TRACE SAVE] Flash Cmd 0x90: Enter Chip ID Mode (ID=0x"
                                      << std::hex << m_manufacturerDeviceId << std::dec << ")\n";
                        }
                        break;
                    case 0xF0: // Exit Chip ID Mode
                        m_chipIdMode = false;
                        m_state = State::Ready;
                        if (m_traceEnabled) {
                            std::cout << "[TRACE SAVE] Flash Cmd 0xF0: Exit Chip ID Mode\n";
                        }
                        break;
                    case 0x80: // Prepare Erase
                        m_state = State::EraseCmd1;
                        break;
                    case 0xA0: // Prepare Single Byte Write
                        m_state = State::WriteSingleByte;
                        break;
                    case 0xB0: // Bank Switch (128KB Flash)
                        m_state = m_is128KB ? State::BankSwitch : State::Ready;
                        break;
                    default:
                        m_state = State::Ready;
                        break;
                }
            } else if (value == 0xF0) {
                m_chipIdMode = false;
                m_state = State::Ready;
            } else {
                m_state = State::Ready;
            }
            break;

        case State::EraseCmd1:
            if (offset == 0x5555 && value == 0xAA) {
                m_state = State::EraseCmd2;
            } else {
                m_state = State::Ready;
            }
            break;

        case State::EraseCmd2:
            if (offset == 0x2AAA && value == 0x55) {
                // Próximo comando define Chip Erase (0x10 em 0x5555) ou Sector Erase (0x30 em 0xS000)
                return;
            }
            if (offset == 0x5555 && value == 0x10) {
                // Erase Entire Chip
                std::fill(m_data.begin(), m_data.end(), 0xFF);
                m_dirty = true;
                if (m_traceEnabled) {
                    std::cout << "[TRACE SAVE] Flash Cmd 0x10: Chip Erase Completo (128KB -> 0xFF)\n";
                }
            } else if (value == 0x30) {
                // Erase 4KB Sector
                const std::size_t sectorStart =
                    static_cast<std::size_t>(m_currentBank) * 65536u + (offset & 0xF000u);
                if (sectorStart + 4096 <= m_data.size()) {
                    std::fill_n(m_data.begin() + static_cast<std::ptrdiff_t>(sectorStart), 4096, 0xFF);
                    m_dirty = true;
                }
                if (m_traceEnabled) {
                    std::cout << "[TRACE SAVE] Flash Cmd 0x30: Sector Erase (Bank "
                              << static_cast<uint32_t>(m_currentBank) << ", Offset 0x"
                              << std::hex << (offset & 0xF000u) << std::dec << ")\n";
                }
            }
            m_state = State::Ready;
            break;

        case State::WriteSingleByte: {
            const std::size_t dst = static_cast<std::size_t>(m_currentBank) * 65536u + offset;
            if (dst < m_data.size()) {
                m_data[dst] &= value; // Memória Flash só zera bits até novo Erase
                m_dirty = true;
            }
            if (m_traceEnabled) {
                static uint32_t writeLogCount = 0;
                if (++writeLogCount <= 16) {
                    std::cout << "[TRACE SAVE] Flash Cmd 0xA0 WriteByte (Bank "
                              << static_cast<uint32_t>(m_currentBank) << ") [0x0E00"
                              << std::hex << std::setw(4) << std::setfill('0') << offset
                              << "] <= 0x" << std::setw(2) << static_cast<uint32_t>(value) << std::dec << "\n";
                }
            }
            m_state = State::Ready;
            break;
        }

        case State::BankSwitch:
            if (offset == 0x0000 && m_is128KB) {
                m_currentBank = value & 1u;
                if (m_traceEnabled) {
                    static uint32_t bankLogCount = 0;
                    if (++bankLogCount <= 6) {
                        std::cout << "[TRACE SAVE] Flash Cmd 0xB0: Bank Switch -> Bank "
                                  << static_cast<uint32_t>(m_currentBank)
                                  << (bankLogCount == 6 ? " (suprimindo trocas de banco subsequentes...)" : "") << "\n";
                    }
                }
            }
            m_state = State::Ready;
            break;
    }
}

bool FlashSave::loadFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    m_dirty = false;
    return true;
}

bool FlashSave::saveToFile(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    return out.good();
}

void FlashSave::serializeState(SaveStateWriter& out) const {
    out.writePod(m_chipIdMode);
    out.writePod(m_currentBank);
    const uint8_t st = static_cast<uint8_t>(m_state);
    out.writePod(st);
    const uint32_t sz = static_cast<uint32_t>(m_data.size());
    out.writePod(sz);
    out.writeBytes(m_data.data(), m_data.size());
}

bool FlashSave::deserializeState(SaveStateReader& in) {
    uint8_t st = 0;
    uint32_t sz = 0;
    if (!in.readPod(m_chipIdMode)) return false;
    if (!in.readPod(m_currentBank)) return false;
    if (!in.readPod(st)) return false;
    if (!in.readPod(sz)) return false;
    if (sz != m_data.size()) return false;
    m_state = static_cast<State>(st);
    m_dirty = true;
    return in.readBytes(m_data.data(), m_data.size());
}

} // namespace gba::core

