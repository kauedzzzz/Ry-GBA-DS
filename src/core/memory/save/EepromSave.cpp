#include "core/memory/save/EepromSave.hpp"
#include <fstream>

namespace gba::core {

EepromSave::EepromSave(bool is8KB)
    : m_is8KB(is8KB)
    , m_addressBits(is8KB ? 14 : 6)
    , m_data(is8KB ? 8 * 1024 : 512, 0xFF) {}

uint8_t EepromSave::readByte(uint32_t address) {
    return static_cast<uint8_t>(readHalfWord(address) & 1u);
}

void EepromSave::writeByte(uint32_t address, uint8_t value) {
    writeHalfWord(address, value);
}

uint16_t EepromSave::readHalfWord(uint32_t /*address*/) {
    if (m_readBitsRemaining == 0) {
        return 1; // Bit 0 = 1 indica EEPROM Ready
    }

    // Primeiros 4 bits lidos (68..65) são dummy bits ignorados pela SDK
    if (m_readBitsRemaining > 64) {
        --m_readBitsRemaining;
        return 0;
    }

    --m_readBitsRemaining;
    const uint16_t bit = static_cast<uint16_t>((m_readShiftReg >> m_readBitsRemaining) & 1u);
    return bit;
}

void EepromSave::writeHalfWord(uint32_t /*address*/, uint16_t value) {
    m_bitBuffer.push_back(static_cast<uint8_t>(value & 1u));
    processBitstream();
}

void EepromSave::processBitstream() {
    if (m_bitBuffer.size() < 2) return;

    // Requisição precisa começar com '1'
    if (m_bitBuffer[0] == 0) {
        m_bitBuffer.clear();
        return;
    }

    const bool isReadRequest = (m_bitBuffer[1] == 1); // "11" = Read, "10" = Write
    const std::size_t expectedReadLen  = 2u + m_addressBits + 1u;      // 2 cmd + N addr + 1 stop bit
    const std::size_t expectedWriteLen = 2u + m_addressBits + 64u + 1u; // 2 cmd + N addr + 64 data + 1 stop bit

    if (isReadRequest && m_bitBuffer.size() == expectedReadLen) {
        uint16_t blockAddr = 0;
        for (uint8_t i = 0; i < m_addressBits; ++i) {
            blockAddr = static_cast<uint16_t>((blockAddr << 1) | m_bitBuffer[2 + i]);
        }
        m_currentBlockAddress = blockAddr;
        const std::size_t byteOffset = (static_cast<std::size_t>(blockAddr) * 8u) % m_data.size();
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val = (val << 8) | m_data[byteOffset + i];
        }
        m_readShiftReg = val;
        m_readBitsRemaining = 68; // 4 bits dummy + 64 bits de dados
        m_bitBuffer.clear();
    } else if (!isReadRequest && m_bitBuffer.size() == expectedWriteLen) {
        uint16_t blockAddr = 0;
        for (uint8_t i = 0; i < m_addressBits; ++i) {
            blockAddr = static_cast<uint16_t>((blockAddr << 1) | m_bitBuffer[2 + i]);
        }
        const std::size_t byteOffset = (static_cast<std::size_t>(blockAddr) * 8u) % m_data.size();
        uint64_t val = 0;
        for (int i = 0; i < 64; ++i) {
            val = (val << 1) | m_bitBuffer[2 + m_addressBits + i];
        }
        for (int i = 7; i >= 0; --i) {
            m_data[byteOffset + (7 - i)] = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
        }
        m_dirty = true;
        m_bitBuffer.clear();
    }
}

bool EepromSave::loadFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    m_dirty = false;
    return true;
}

bool EepromSave::saveToFile(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    return out.good();
}

void EepromSave::serializeState(SaveStateWriter& out) const {
    out.writePod(m_currentBlockAddress);
    out.writePod(m_readBitsRemaining);
    out.writePod(m_readShiftReg);
    const uint32_t sz = static_cast<uint32_t>(m_data.size());
    out.writePod(sz);
    out.writeBytes(m_data.data(), m_data.size());
}

bool EepromSave::deserializeState(SaveStateReader& in) {
    uint32_t sz = 0;
    if (!in.readPod(m_currentBlockAddress)) return false;
    if (!in.readPod(m_readBitsRemaining)) return false;
    if (!in.readPod(m_readShiftReg)) return false;
    if (!in.readPod(sz) || sz != m_data.size()) return false;
    m_bitBuffer.clear();
    m_dirty = true;
    return in.readBytes(m_data.data(), m_data.size());
}

} // namespace gba::core

