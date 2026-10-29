#include "core/nds/NdsCore.hpp"
#include "core/SaveStateStream.hpp"
#include "core/nds/BuiltInNdsDemoRom.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <system_error>
#include <vector>

namespace gba::core::nds {

namespace {

constexpr std::array<char, 8> NDS_STATE_MAGIC = {'R', 'Y', 'U', 'N', 'D', 'S', 'S', 'T'};
constexpr uint32_t NDS_STATE_VERSION = 1u;

bool readBinaryFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    if (p.empty()) return false;
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz <= 0) return false;
    out.resize(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(f);
}

const char* unitCodeToString(uint8_t code) noexcept {
    switch (code) {
        case 0x00: return "NDS (0x00)";
        case 0x02: return "NDS + DSi Enhanced (0x02)";
        case 0x03: return "DSi Exclusive (0x03)";
        default:   return "NDS Custom";
    }
}

} // namespace

NdsCore::NdsCore()
    : m_bus(m_cart, m_gpu, m_spu, m_touch)
    , m_arm9(m_bus)
    , m_arm7(m_bus) {
    m_bus.attachCpus(&m_arm9, &m_arm7);
    m_legacyDummyFb.fill(0xFF121316u);
}

NdsCore::~NdsCore() {
    m_cart.flushSaveFile();
}

void NdsCore::configureBootFiles(bool directBootHle,
                                 const std::filesystem::path& bios9Path,
                                 const std::filesystem::path& bios7Path,
                                 const std::filesystem::path& firmwarePath) {
    m_directBootHle = directBootHle;

    std::vector<uint8_t> buf;
    if (readBinaryFile(bios9Path, buf)) {
        m_bus.loadBios9(buf);
    }
    if (readBinaryFile(bios7Path, buf)) {
        m_bus.loadBios7(buf);
    }
    if (readBinaryFile(firmwarePath, buf)) {
        m_touch.loadOptionalFirmware(buf.data(), buf.size());
    }
}

bool NdsCore::loadBios(std::span<const uint8_t> biosData) {
    if (biosData.size() == 4096u) {
        return m_bus.loadBios9(biosData);
    }
    if (biosData.size() == 16384u) {
        return m_bus.loadBios7(biosData);
    }
    return false;
}

bool NdsCore::loadRom(std::span<const uint8_t> romData, SaveType /*saveType*/) {
    m_cart.flushSaveFile();
    if (!m_cart.load(romData, {})) {
        return false;
    }
    m_loadedRomPath.clear();
    logCartridgeMetadata();
    reset();
    return true;
}

bool NdsCore::loadRomFromFile(const std::filesystem::path& romPath, SaveType /*saveType*/) {
    std::vector<uint8_t> buffer;
    if (!readBinaryFile(romPath, buffer)) {
        return false;
    }
    m_cart.flushSaveFile();
    if (!m_cart.load(buffer, romPath)) {
        return false;
    }
    m_loadedRomPath = romPath;
    logCartridgeMetadata();
    reset();
    return true;
}

void NdsCore::loadBuiltInDemoRom() {
    const auto rom = BuiltInNdsDemoRom::build();
    m_cart.flushSaveFile();
    m_cart.load(rom, {});
    m_loadedRomPath = "RyuDS_Dragon_DualScreen_Demo.nds";
    logCartridgeMetadata();
    reset();
}

void NdsCore::logCartridgeMetadata() const {
    if (!m_cart.isLoaded()) return;
    const auto& h = m_cart.header();
    std::cout << "\n==================== [RyūDS: CARTUCHO NINTENDO DS CARREGADO] ====================\n"
              << "  Game Title   : " << h.gameTitle << "\n"
              << "  Game Code    : " << h.gameCode << " | Maker: " << h.makerCode
              << " | Unit: " << unitCodeToString(h.unitCode) << "\n"
              << "  ROM Size     : " << (h.actualFileSize / 1024) << " KB (Capacidade Header: "
              << ((128ull << std::min<uint8_t>(h.deviceCapacity, 16u))) << " KB)\n"
              << "  ARM9 Binary  : ROM=0x" << std::hex << std::setw(8) << std::setfill('0') << h.arm9RomOffset
              << " -> RAM=0x" << std::setw(8) << h.arm9RamAddress
              << " | Entry=0x" << std::setw(8) << h.arm9EntryAddress
              << " (" << std::dec << h.arm9Size << " bytes)\n"
              << "  ARM7 Binary  : ROM=0x" << std::hex << std::setw(8) << std::setfill('0') << h.arm7RomOffset
              << " -> RAM=0x" << std::setw(8) << h.arm7RamAddress
              << " | Entry=0x" << std::setw(8) << h.arm7EntryAddress
              << " (" << std::dec << h.arm7Size << " bytes)\n"
              << "  Backup (.sav): " << ndsBackupTypeToString(m_cart.backupType());
    if (!m_cart.saveFilePath().empty()) {
        std::cout << " [" << m_cart.saveFilePath().string() << "]";
    }
    std::cout << "\n  Banner 32x32 : " << (h.hasBannerIcon ? "Extraido (16 cores 4bpp)" : "Ausente");
    if (!h.bannerTitleUtf8.empty()) {
        std::string flatTitle = h.bannerTitleUtf8;
        std::replace(flatTitle.begin(), flatTitle.end(), '\n', ' ');
        std::cout << " | \"" << flatTitle << "\"";
    }
    std::cout << "\n  Modo de Boot : "
              << ((m_directBootHle || !m_bus.hasCustomBios9() || !m_bus.hasCustomBios7())
                  ? "Direct Boot / HLE (Pos-Firmware)"
                  : "BIOS9 + BIOS7 Real")
              << "\n=================================================================================\n";
}

void NdsCore::reset() {
    m_bus.reset();
    m_arm9.reset();
    m_arm7.reset();
    m_arm9CycleDebt = 0;
    m_arm7CycleDebt = 0;
    m_frameCount    = 0;

    if (!m_cart.isLoaded()) return;

    const bool canBootRealBios = !m_directBootHle && m_bus.hasCustomBios9() && m_bus.hasCustomBios7();
    if (!canBootRealBios) {
        m_bus.setupDirectBoot();
        m_arm9.initDirectBoot(m_cart.header().arm9EntryAddress);
        m_arm7.initDirectBoot(m_cart.header().arm7EntryAddress);
    }
}

void NdsCore::runFrame() {
    if (!m_cart.isLoaded()) return;

    constexpr int32_t SLICE_ARM7_CYCLES = 32; // Fatia de ~0,95 us (64 ciclos ARM9 : 32 ciclos ARM7)
    int32_t remainingArm7 = ARM7_CYCLES_PER_FRAME;

    while (remainingArm7 > 0) {
        const int32_t slice7 = std::min<int32_t>(SLICE_ARM7_CYCLES, remainingArm7);
        const int32_t slice9 = slice7 * 2;

        m_arm9CycleDebt += slice9;
        while (m_arm9CycleDebt > 0) {
            m_arm9CycleDebt -= m_arm9.step();
        }

        m_arm7CycleDebt += slice7;
        while (m_arm7CycleDebt > 0) {
            m_arm7CycleDebt -= m_arm7.step();
        }

        m_bus.stepTimersArm7Cycles(slice7);
        m_gpu.stepArm7Cycles(slice7);
        m_spu.stepArm7Cycles(slice7);

        remainingArm7 -= slice7;
    }

    ++m_frameCount;
    if ((m_frameCount % 60u) == 0u) {
        m_cart.flushSaveFile();
    }
}

std::string NdsCore::romTitle() const noexcept {
    if (!m_cart.isLoaded()) return {};
    if (!m_cart.header().gameTitle.empty()) return m_cart.header().gameTitle;
    return "NDS_ROM";
}

void NdsCore::setKeyState(GbaKey key, bool pressed) noexcept {
    switch (key) {
        case GbaKey::A:      m_bus.setNdsKeyState(NdsKey::A, pressed); break;
        case GbaKey::B:      m_bus.setNdsKeyState(NdsKey::B, pressed); break;
        case GbaKey::Select: m_bus.setNdsKeyState(NdsKey::Select, pressed); break;
        case GbaKey::Start:  m_bus.setNdsKeyState(NdsKey::Start, pressed); break;
        case GbaKey::Right:  m_bus.setNdsKeyState(NdsKey::Right, pressed); break;
        case GbaKey::Left:   m_bus.setNdsKeyState(NdsKey::Left, pressed); break;
        case GbaKey::Up:     m_bus.setNdsKeyState(NdsKey::Up, pressed); break;
        case GbaKey::Down:   m_bus.setNdsKeyState(NdsKey::Down, pressed); break;
        case GbaKey::R:      m_bus.setNdsKeyState(NdsKey::R, pressed); break;
        case GbaKey::L:      m_bus.setNdsKeyState(NdsKey::L, pressed); break;
    }
}

void NdsCore::setLidClosed(bool closed) noexcept {
    const bool wasClosed = m_touch.isLidClosed();
    m_touch.setLidClosed(closed);
    if (!wasClosed && closed) {
        m_bus.requestIrq7(NdsIrq::LidHinge);
    }
}

bool NdsCore::saveState(std::vector<uint8_t>& outData) const {
    if (!m_cart.isLoaded()) return false;

    SaveStateWriter writer;
    writer.reserve(6 * 1024 * 1024);
    writer.writePod(NDS_STATE_MAGIC);
    writer.writePod(NDS_STATE_VERSION);
    writer.writePod(m_frameCount);
    writer.writePod(m_arm9CycleDebt);
    writer.writePod(m_arm7CycleDebt);

    m_cart.serializeState(writer);
    m_touch.serializeState(writer);
    m_spu.serializeState(writer);
    m_gpu.serializeState(writer);
    m_bus.serializeState(writer);
    m_arm9.serializeState(writer);
    m_arm7.serializeState(writer);

    outData = writer.takeData();
    return true;
}

bool NdsCore::loadState(std::span<const uint8_t> data) {
    if (!m_cart.isLoaded()) return false;

    SaveStateReader reader(data);
    std::array<char, 8> magic{};
    uint32_t version = 0;

    if (!reader.readPod(magic) || magic != NDS_STATE_MAGIC) return false;
    if (!reader.readPod(version) || version != NDS_STATE_VERSION) return false;
    if (!reader.readPod(m_frameCount)) return false;
    if (!reader.readPod(m_arm9CycleDebt)) return false;
    if (!reader.readPod(m_arm7CycleDebt)) return false;

    return m_cart.deserializeState(reader) &&
           m_touch.deserializeState(reader) &&
           m_spu.deserializeState(reader) &&
           m_gpu.deserializeState(reader) &&
           m_bus.deserializeState(reader) &&
           m_arm9.deserializeState(reader) &&
           m_arm7.deserializeState(reader) &&
           reader.good();
}

std::filesystem::path NdsCore::stateSlotPath(int slot) const {
    const int clamped = std::clamp(slot, 1, 9);
    std::filesystem::path base = m_loadedRomPath.empty()
        ? std::filesystem::path("ryuds_memory_rom")
        : m_loadedRomPath;
    base.replace_extension(".nds.ss" + std::to_string(clamped));
    return base;
}

bool NdsCore::saveStateSlot(int slot) const {
    if (slot < 1 || slot > 9 || !m_cart.isLoaded()) return false;
    std::vector<uint8_t> bytes;
    if (!saveState(bytes)) return false;

    std::ofstream out(stateSlotPath(slot), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool NdsCore::loadStateSlot(int slot) {
    if (slot < 1 || slot > 9 || !m_cart.isLoaded()) return false;
    std::vector<uint8_t> bytes;
    if (!readBinaryFile(stateSlotPath(slot), bytes)) return false;
    return loadState(bytes);
}

bool NdsCore::hasStateSlot(int slot) const {
    if (slot < 1 || slot > 9 || !m_cart.isLoaded()) return false;
    std::error_code ec;
    return std::filesystem::exists(stateSlotPath(slot), ec) && !ec;
}

} // namespace gba::core::nds

