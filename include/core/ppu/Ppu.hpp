#pragma once
#include "core/SaveStateStream.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace gba::core {

class Arm7Tdmi;
class MemoryBus;

class Ppu {
public:
    static constexpr uint32_t SCREEN_WIDTH  = 240;
    static constexpr uint32_t SCREEN_HEIGHT = 160;
    static constexpr uint32_t TOTAL_LINES   = 228;
    static constexpr uint32_t HDRAW_CYCLES  = 960;
    static constexpr uint32_t HBLANK_CYCLES = 272;
    static constexpr uint32_t LINE_CYCLES   = HDRAW_CYCLES + HBLANK_CYCLES; // 1232

    Ppu();
    void attach(Arm7Tdmi* cpu, MemoryBus* bus) noexcept {
        m_cpu = cpu;
        m_bus = bus;
    }

    void reset();
    void step(int32_t cycles);

    [[nodiscard]] uint8_t readPalette(uint32_t offset) const noexcept { return m_pram[offset & 0x3FF]; }
    [[nodiscard]] uint8_t readVram(uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t readOam(uint32_t offset) const noexcept { return m_oam[offset & 0x3FF]; }

    void writePalette(uint32_t offset, uint8_t val) noexcept { m_pram[offset & 0x3FF] = val; }
    void writeVram(uint32_t offset, uint8_t val) noexcept;
    void writeOam(uint32_t offset, uint8_t val) noexcept { m_oam[offset & 0x3FF] = val; }

    [[nodiscard]] uint8_t readRegister(uint32_t offset) const noexcept;
    void writeRegister(uint32_t offset, uint8_t val) noexcept;

    [[nodiscard]] uint16_t getVCount() const noexcept { return m_vcount; }
    [[nodiscard]] uint16_t getDispCnt() const noexcept { return m_dispcnt; }
    [[nodiscard]] uint16_t getDispStat() const noexcept { return m_dispstat; }

    void setTraceEnabled(bool enabled) noexcept { m_traceEnabled = enabled; }
    [[nodiscard]] uint64_t getVBlankIrqCount() const noexcept { return m_vblankIrqCount; }

    void setSkipRender(bool skip) noexcept { m_skipRender = skip; }
    [[nodiscard]] bool isSkipRender() const noexcept { return m_skipRender; }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

    [[nodiscard]] std::span<const uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> getFramebuffer() const noexcept {
        return m_frontBuffer;
    }

    static constexpr uint32_t rgb555ToArgb8888(uint16_t color555) noexcept {
        const uint32_t r5 = (color555 >> 0) & 0x1F;
        const uint32_t g5 = (color555 >> 5) & 0x1F;
        const uint32_t b5 = (color555 >> 10) & 0x1F;
        const uint32_t r8 = (r5 << 3) | (r5 >> 2);
        const uint32_t g8 = (g5 << 3) | (g5 >> 2);
        const uint32_t b8 = (b5 << 3) | (b5 >> 2);
        return (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8;
    }

private:
    Arm7Tdmi*  m_cpu{nullptr};
    MemoryBus* m_bus{nullptr};

    int32_t  m_scanlineCycles{0};
    bool     m_inHBlank{false};
    uint16_t m_vcount{0};
    uint16_t m_dispcnt{0};
    uint16_t m_dispstat{0};
    bool     m_traceEnabled{false};
    uint64_t m_vblankIrqCount{0};
    bool     m_skipRender{false};

    std::array<uint16_t, 4> m_bgCnt{};
    std::array<uint16_t, 4> m_bgHOfs{};
    std::array<uint16_t, 4> m_bgVOfs{};

    // Affine BG2/BG3 parameters
    std::array<int16_t, 2> m_bgPa{0x0100, 0x0100};
    std::array<int16_t, 2> m_bgPb{0, 0};
    std::array<int16_t, 2> m_bgPc{0, 0};
    std::array<int16_t, 2> m_bgPd{0x0100, 0x0100};
    std::array<int32_t, 2> m_bgRefX{0, 0};
    std::array<int32_t, 2> m_bgRefY{0, 0};

    std::array<uint8_t, 0x60>    m_ioRegs{};
    std::array<uint8_t, 0x00400> m_pram{}; // 1 KB Palette RAM
    std::array<uint8_t, 0x18000> m_vram{}; // 96 KB VRAM
    std::array<uint8_t, 0x00400> m_oam{};  // 1 KB OAM

    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_backBuffer{};
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_frontBuffer{};

    struct LayerPixel {
        uint16_t color555{0};
        uint8_t  priority{4};
        bool     opaque{false};
    };

    std::array<std::array<LayerPixel, SCREEN_WIDTH>, 4> m_bgLine{};
    std::array<LayerPixel, SCREEN_WIDTH>                m_objLine{};

    [[nodiscard]] uint16_t readPram16(uint32_t index) const noexcept {
        const uint32_t addr = (index * 2) & 0x3FE;
        return static_cast<uint16_t>(m_pram[addr] | (m_pram[addr + 1] << 8));
    }

    [[nodiscard]] uint16_t readVram16(uint32_t addr) const noexcept {
        const uint32_t a = addr & ~1u;
        return static_cast<uint16_t>(readVram(a) | (readVram(a + 1) << 8));
    }

    [[nodiscard]] uint16_t readOam16(uint32_t addr) const noexcept {
        const uint32_t a = addr & 0x3FE;
        return static_cast<uint16_t>(m_oam[a] | (m_oam[a + 1] << 8));
    }

    void renderScanline();
    void renderTextBackground(uint8_t bgIdx);
    void renderAffineBackground(uint8_t bgIdx);
    void renderMode3Scanline();
    void renderMode4Scanline();
    void renderMode5Scanline();
    void renderSpritesScanline();
    void composeScanline(bool bitmapMode);
};

} // namespace gba::core

