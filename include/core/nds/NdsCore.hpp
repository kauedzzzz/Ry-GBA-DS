#pragma once
#include "core/ICoreEmulator.hpp"
#include "core/nds/Arm9Cpu.hpp"
#include "core/nds/NdsArm7Cpu.hpp"
#include "core/nds/NdsBus.hpp"
#include "core/nds/NdsCartridge.hpp"
#include "core/nds/NdsGpu.hpp"
#include "core/nds/NdsSpu.hpp"
#include "core/nds/TouchController.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gba::core::nds {

// =============================================================================
// Núcleo Completo Dual-Core Nintendo DS (ARM946E-S @ 66.66MHz + ARM7 @ 33.51MHz)
// =============================================================================
class NdsCore final : public ICoreEmulator {
public:
    static constexpr int32_t ARM7_CYCLES_PER_FRAME = NdsGpu::TOTAL_LINES * NdsGpu::ARM7_CYCLES_LINE; // 560.190

    NdsCore();
    ~NdsCore() override;

    // Configuração de Boot NDS (Direct Boot HLE vs BIOS9/BIOS7/Firmware externos opcionais)
    void configureBootFiles(bool directBootHle,
                            const std::filesystem::path& bios9Path,
                            const std::filesystem::path& bios7Path,
                            const std::filesystem::path& firmwarePath);

    bool loadBios(std::span<const uint8_t> biosData) override;
    bool loadRom(std::span<const uint8_t> romData, SaveType saveType = SaveType::AutoDetect) override;
    bool loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType = SaveType::AutoDetect) override;
    void loadBuiltInDemoRom();

    void reset() override;
    void runFrame() override;

    [[nodiscard]] bool hasRomLoaded() const noexcept override { return m_cart.isLoaded(); }
    [[nodiscard]] std::string romTitle() const noexcept override;
    [[nodiscard]] const std::filesystem::path& romPath() const noexcept override { return m_loadedRomPath; }

    [[nodiscard]] ConsoleType consoleType() const noexcept override { return ConsoleType::NDS; }
    [[nodiscard]] bool isDualScreen() const noexcept override { return true; }
    [[nodiscard]] int screenWidth() const noexcept override { return NdsGpu::SCREEN_WIDTH; }
    [[nodiscard]] int screenHeight() const noexcept override { return NdsGpu::SCREEN_HEIGHT; }

    [[nodiscard]] std::span<const uint32_t, 240 * 160> framebuffer() const noexcept override {
        return m_legacyDummyFb;
    }
    [[nodiscard]] std::span<const uint32_t> topFramebufferSpan() const noexcept override {
        return m_gpu.topFramebuffer();
    }
    [[nodiscard]] std::span<const uint32_t> bottomFramebufferSpan() const noexcept override {
        return m_gpu.bottomFramebuffer();
    }

    [[nodiscard]] bool hasBannerIcon() const noexcept override {
        return m_cart.isLoaded() && m_cart.header().hasBannerIcon;
    }
    [[nodiscard]] std::span<const uint32_t> bannerIcon32x32() const noexcept override {
        if (!hasBannerIcon()) return {};
        return m_cart.header().bannerIconArgb;
    }
    [[nodiscard]] std::string bannerSubtitle() const noexcept override {
        return m_cart.isLoaded() ? m_cart.header().bannerTitleUtf8 : std::string{};
    }

    // Controle Lock-Free de Áudio (16 canais SPU)
    void setVolume(float volume) noexcept override { m_spu.setVolume(volume); }
    [[nodiscard]] float getVolume() const noexcept override { return m_spu.getVolume(); }
    void setMuted(bool state) noexcept override { m_spu.setMuted(state); }
    bool toggleMute() noexcept override { return m_spu.toggleMute(); }
    [[nodiscard]] bool isMuted() const noexcept override { return m_spu.isMuted(); }
    void setAudioSampleRate(uint32_t sampleRateHz) noexcept override { m_spu.setSampleRate(sampleRateHz); }
    std::size_t pullAudioSamples(std::span<int16_t> outBuffer) noexcept override {
        return m_spu.pullAudioSamples(outBuffer);
    }

    // Entradas DS (12 botões + Stylus Touchscreen + Microfone + Lid Hinge)
    void setKeyState(GbaKey key, bool pressed) noexcept override;
    void setExtendedButtonX(bool pressed) noexcept override { m_bus.setNdsKeyState(NdsKey::X, pressed); }
    void setExtendedButtonY(bool pressed) noexcept override { m_bus.setNdsKeyState(NdsKey::Y, pressed); }
    void setStylusState(int x, int y, bool touching) noexcept override { m_touch.setStylusState(x, y, touching); }
    void setMicBlowing(bool active) noexcept override { m_touch.setMicBlowing(active); }
    [[nodiscard]] bool isMicBlowing() const noexcept override { return m_touch.isMicBlowing(); }
    void setLidClosed(bool closed) noexcept override;
    [[nodiscard]] bool isLidClosed() const noexcept override { return m_touch.isLidClosed(); }

    // Save States (Slots 1-9)
    bool saveState(std::vector<uint8_t>& outData) const override;
    bool loadState(std::span<const uint8_t> data) override;
    bool saveStateSlot(int slot) const override;
    bool loadStateSlot(int slot) override;
    [[nodiscard]] bool hasStateSlot(int slot) const override;
    [[nodiscard]] std::filesystem::path stateSlotPath(int slot) const;

    // Acessores diretos para testes automatizados e diagnóstico
    [[nodiscard]] NdsCartridge& cartridge() noexcept { return m_cart; }
    [[nodiscard]] const NdsCartridge& cartridge() const noexcept { return m_cart; }
    [[nodiscard]] TouchController& touch() noexcept { return m_touch; }
    [[nodiscard]] NdsSpu& spu() noexcept { return m_spu; }
    [[nodiscard]] NdsGpu& gpu() noexcept { return m_gpu; }
    [[nodiscard]] NdsBus& bus() noexcept { return m_bus; }
    [[nodiscard]] Arm9Cpu& arm9() noexcept { return m_arm9; }
    [[nodiscard]] NdsArm7Cpu& arm7() noexcept { return m_arm7; }
    [[nodiscard]] uint64_t frameCount() const noexcept { return m_frameCount; }

private:
    NdsCartridge    m_cart;
    TouchController m_touch;
    NdsSpu          m_spu;
    NdsGpu          m_gpu;
    NdsBus          m_bus;
    Arm9Cpu         m_arm9;
    NdsArm7Cpu      m_arm7;

    bool                  m_directBootHle{true};
    int32_t               m_arm9CycleDebt{0};
    int32_t               m_arm7CycleDebt{0};
    uint64_t              m_frameCount{0};
    std::filesystem::path m_loadedRomPath;
    std::array<uint32_t, 240 * 160> m_legacyDummyFb{};

    void logCartridgeMetadata() const;
};

} // namespace gba::core::nds

