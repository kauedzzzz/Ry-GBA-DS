#pragma once
#include "core/memory/save/IBackupMedia.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gba::core {

class Arm7Tdmi;
class Ppu;
class Apu;
class InputManager;

class MemoryBus {
public:
    MemoryBus(Ppu& ppu, Apu& apu, InputManager& input);

    void attachCpu(Arm7Tdmi* cpu) noexcept { m_cpu = cpu; }
    void reset();

    bool loadBios(std::span<const uint8_t> biosData);
    bool loadRom(std::span<const uint8_t> romData, SaveType forcedSaveType = SaveType::AutoDetect);

    bool loadSaveFile(const std::filesystem::path& path);
    bool flushSaveFile(const std::filesystem::path& path);

    [[nodiscard]] uint8_t  read8(uint32_t addr);
    [[nodiscard]] uint16_t read16(uint32_t addr);
    [[nodiscard]] uint32_t read32(uint32_t addr);

    void write8(uint32_t addr, uint8_t val);
    void write16(uint32_t addr, uint16_t val);
    void write32(uint32_t addr, uint32_t val);

    void stepTimers(int32_t cycles);

    void triggerDmaFifo(uint8_t fifoIndex);
    void triggerDmaVBlank();
    void triggerDmaHBlank();

    [[nodiscard]] uint16_t getIe() const noexcept { return m_ie; }
    [[nodiscard]] uint16_t getIf() const noexcept { return m_if; }
    [[nodiscard]] uint16_t getIme() const noexcept { return m_ime; }
    void requestInterrupt(uint16_t flag) noexcept { m_if |= flag; }

    [[nodiscard]] IBackupMedia* getBackupMedia() noexcept { return m_backupMedia.get(); }
    [[nodiscard]] const IBackupMedia* getBackupMedia() const noexcept { return m_backupMedia.get(); }
    [[nodiscard]] SaveType getActiveSaveType() const noexcept {
        return m_backupMedia ? m_backupMedia->getType() : SaveType::None;
    }
    [[nodiscard]] std::string getRomTitle() const noexcept { return m_romTitle; }

    void setTraceEnabled(bool enabled) noexcept {
        m_traceEnabled = enabled;
        if (m_backupMedia) m_backupMedia->setTraceEnabled(enabled);
    }
    [[nodiscard]] uint64_t getSaveReadCount() const noexcept { return m_saveReadCount; }
    [[nodiscard]] uint64_t getSaveWriteCount() const noexcept { return m_saveWriteCount; }
    [[nodiscard]] const Ppu& ppu() const noexcept { return m_ppu; }
    [[nodiscard]] bool hasRom() const noexcept { return !m_rom.empty(); }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    Ppu&          m_ppu;
    Apu&          m_apu;
    InputManager& m_input;
    Arm7Tdmi*     m_cpu{nullptr};

    std::array<uint8_t, 0x00004000> m_bios{};  // 16 KB BIOS (0x00000000)
    std::array<uint8_t, 0x00040000> m_ewram{}; // 256 KB EWRAM (0x02000000)
    std::array<uint8_t, 0x00008000> m_iwram{}; // 32 KB IWRAM (0x03000000)
    std::array<uint8_t, 0x00000400> m_io{};    // 1 KB MMIO (0x04000000)
    std::vector<uint8_t>            m_rom;     // Game Pak ROM (0x08000000)
    std::unique_ptr<IBackupMedia>   m_backupMedia;
    std::string                     m_romTitle{"GBA DEMO"};

    uint16_t m_ie{0};
    uint16_t m_if{0};
    uint16_t m_ime{0};
    uint16_t m_waitcnt{0};
    int32_t  m_sioCyclesRemaining{0};
    bool     m_traceEnabled{false};
    uint64_t m_saveReadCount{0};
    uint64_t m_saveWriteCount{0};

    struct DmaChannel {
        uint32_t sad{0};
        uint32_t dad{0};
        uint16_t count{0};
        uint16_t control{0};
        uint32_t internalSrc{0};
        uint32_t internalDst{0};
        uint32_t internalCount{0};
    };
    std::array<DmaChannel, 4> m_dma{};

    struct TimerChannel {
        uint16_t reload{0};
        uint16_t counter{0};
        uint16_t control{0};
        int32_t  cyclesAccum{0};
    };
    std::array<TimerChannel, 4> m_timers{};

    void installBuiltInBiosVectors();
    uint8_t readIo8(uint32_t offset);
    void writeIo8(uint32_t offset, uint8_t val);

    void onDmaControlWrite(uint8_t ch, uint16_t oldCtrl, uint16_t newCtrl);
    void executeDma(uint8_t ch);
    void onTimerOverflow(uint8_t timerIdx);
};

} // namespace gba::core

