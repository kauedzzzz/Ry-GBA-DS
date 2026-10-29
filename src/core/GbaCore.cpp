#include "core/GbaCore.hpp"
#include "core/BuiltInDemoRom.hpp"
#include "core/SaveStateStream.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace gba::core {

namespace {
constexpr std::array<char, 8> STATE_MAGIC = {'R', 'Y', 'U', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t STATE_VERSION = 1u;
} // namespace

GbaCore::GbaCore()
    : m_bus(m_ppu, m_apu, m_input)
    , m_cpu(m_bus) {
    m_ppu.attach(&m_cpu, &m_bus);
    m_apu.attachBus(&m_bus);
}

bool GbaCore::loadBios(std::span<const uint8_t> biosData) {
    return m_bus.loadBios(biosData);
}

bool GbaCore::loadRom(std::span<const uint8_t> romData, SaveType saveType) {
    const bool ok = m_bus.loadRom(romData, saveType);
    if (ok) {
        m_loadedRomPath.clear();
        m_saveFilePath.clear();
        reset();
    }
    return ok;
}

bool GbaCore::loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType) {
    std::ifstream in(romPath, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto fileSize = in.tellg();
    if (fileSize <= 0) return false;

    std::vector<uint8_t> buffer(static_cast<std::size_t>(fileSize));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    if (!loadRom(buffer, saveType)) return false;

    m_loadedRomPath = romPath;
    m_saveFilePath  = romPath;
    m_saveFilePath.replace_extension(".sav");
    m_bus.loadSaveFile(m_saveFilePath);
    return true;
}

void GbaCore::loadBuiltInDemoRom() {
    const auto rom = BuiltInDemoRom::build();
    loadRom(rom, SaveType::AutoDetect);
    m_loadedRomPath = "GBA_Core_Lab_Demo.gba";
    m_saveFilePath  = "gba_demo_lab.sav";
    m_bus.loadSaveFile(m_saveFilePath);
}

void GbaCore::reset() {
    m_ppu.reset();
    m_apu.reset();
    m_cpu.reset(true);
    m_frameCount = 0;
}

void GbaCore::runFrame() {
    if (!m_bus.hasRom()) return;

    int32_t cyclesRemaining = CYCLES_PER_FRAME;
    while (cyclesRemaining > 0) {
        if (m_input.shouldTriggerKeypadIrq()) {
            m_cpu.requestInterrupt(InterruptFlag::Keypad);
        }
        const int32_t elapsed = m_cpu.step();
        m_ppu.step(elapsed);
        m_apu.step(elapsed);
        cyclesRemaining -= elapsed;
    }

    ++m_frameCount;

    // Auto-flush do arquivo .sav a cada 60 quadros (~1 segundo) se houve escrita na Save RAM/Flash/EEPROM
    if ((m_frameCount % 60 == 0) && !m_saveFilePath.empty()) {
        if (const auto* media = m_bus.getBackupMedia(); media && media->isDirty()) {
            m_bus.flushSaveFile(m_saveFilePath);
        }
    }
}

bool GbaCore::saveState(std::vector<uint8_t>& outData) const {
    if (!m_bus.hasRom()) return false;

    SaveStateWriter writer;
    writer.reserve(512 * 1024);
    writer.writePod(STATE_MAGIC);
    writer.writePod(STATE_VERSION);
    writer.writePod(m_frameCount);

    std::array<char, 16> titleBuf{};
    const std::string title = m_bus.getRomTitle();
    std::memcpy(titleBuf.data(), title.data(), std::min<std::size_t>(title.size(), titleBuf.size()));
    writer.writePod(titleBuf);

    m_cpu.serializeState(writer);
    m_ppu.serializeState(writer);
    m_apu.serializeState(writer);
    m_bus.serializeState(writer);

    outData = writer.takeData();
    return true;
}

bool GbaCore::loadState(std::span<const uint8_t> data) {
    if (!m_bus.hasRom()) return false;

    SaveStateReader reader(data);
    std::array<char, 8> magic{};
    uint32_t version = 0;
    uint64_t savedFrame = 0;
    std::array<char, 16> titleBuf{};

    if (!reader.readPod(magic) || magic != STATE_MAGIC) return false;
    if (!reader.readPod(version) || version != STATE_VERSION) return false;
    if (!reader.readPod(savedFrame)) return false;
    if (!reader.readPod(titleBuf)) return false;

    if (!m_cpu.deserializeState(reader)) return false;
    if (!m_ppu.deserializeState(reader)) return false;
    if (!m_apu.deserializeState(reader)) return false;
    if (!m_bus.deserializeState(reader)) return false;

    m_frameCount = savedFrame;
    return reader.good();
}

std::filesystem::path GbaCore::stateSlotPath(int slot) const {
    const int clamped = (slot < 1) ? 1 : ((slot > 9) ? 9 : slot);
    std::filesystem::path base = m_loadedRomPath.empty()
        ? std::filesystem::path("ryugba_memory_rom")
        : m_loadedRomPath;
    base.replace_extension(".ss" + std::to_string(clamped));
    return base;
}

bool GbaCore::saveStateSlot(int slot) const {
    if (slot < 1 || slot > 9 || !m_bus.hasRom()) return false;
    std::vector<uint8_t> stateBytes;
    if (!saveState(stateBytes)) return false;

    const auto path = stateSlotPath(slot);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(stateBytes.data()),
              static_cast<std::streamsize>(stateBytes.size()));
    return out.good();
}

bool GbaCore::loadStateSlot(int slot) {
    if (slot < 1 || slot > 9 || !m_bus.hasRom()) return false;
    const auto path = stateSlotPath(slot);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz <= 0) return false;

    std::vector<uint8_t> stateBytes(static_cast<std::size_t>(sz));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(stateBytes.data()), sz);
    if (!in) return false;

    return loadState(stateBytes);
}

bool GbaCore::hasStateSlot(int slot) const {
    if (slot < 1 || slot > 9 || !m_bus.hasRom()) return false;
    std::error_code ec;
    return std::filesystem::exists(stateSlotPath(slot), ec) && !ec;
}

} // namespace gba::core
