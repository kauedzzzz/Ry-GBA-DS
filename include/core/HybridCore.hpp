#pragma once
#include "core/GbaCore.hpp"
#include "core/ICoreEmulator.hpp"
#include "core/nds/NdsCore.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gba::core {

// =============================================================================
// Despachante Multi-Console Híbrido (RyūDS / RyūGBA)
// Detecta automaticamente ROMs de Nintendo DS (.nds, .dsi, .srl) e GBA (.gba, .agb)
// roteando vídeo dual-screen/single-screen, áudio, saves (.sav) e inputs.
// =============================================================================
class HybridCore final : public ICoreEmulator {
public:
    HybridCore();

    void configureNdsBootFiles(bool directBootHle,
                               const std::filesystem::path& bios9Path,
                               const std::filesystem::path& bios7Path,
                               const std::filesystem::path& firmwarePath);

    bool loadBios(std::span<const uint8_t> biosData) override;
    bool loadRom(std::span<const uint8_t> romData, SaveType saveType = SaveType::AutoDetect) override;
    bool loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType = SaveType::AutoDetect) override;

    void loadBuiltInGbaDemo();
    void loadBuiltInNdsDemo();
    void setTraceEnabled(bool enabled) noexcept { m_gba.setTraceEnabled(enabled); }

    void reset() override;
    void runFrame(bool skipRendering = false) override;

    [[nodiscard]] bool hasRomLoaded() const noexcept override { return activeCore().hasRomLoaded(); }
    [[nodiscard]] std::string romTitle() const noexcept override { return activeCore().romTitle(); }
    [[nodiscard]] const std::filesystem::path& romPath() const noexcept override { return activeCore().romPath(); }

    [[nodiscard]] ConsoleType consoleType() const noexcept override { return m_activeType; }
    [[nodiscard]] bool isDualScreen() const noexcept override { return activeCore().isDualScreen(); }
    [[nodiscard]] int screenWidth() const noexcept override { return activeCore().screenWidth(); }
    [[nodiscard]] int screenHeight() const noexcept override { return activeCore().screenHeight(); }

    [[nodiscard]] std::span<const uint32_t, 240 * 160> framebuffer() const noexcept override {
        return m_gba.framebuffer();
    }
    [[nodiscard]] std::span<const uint32_t> topFramebufferSpan() const noexcept override {
        return activeCore().topFramebufferSpan();
    }
    [[nodiscard]] std::span<const uint32_t> bottomFramebufferSpan() const noexcept override {
        return activeCore().bottomFramebufferSpan();
    }

    [[nodiscard]] bool hasBannerIcon() const noexcept override { return activeCore().hasBannerIcon(); }
    [[nodiscard]] std::span<const uint32_t> bannerIcon32x32() const noexcept override {
        return activeCore().bannerIcon32x32();
    }
    [[nodiscard]] std::string bannerSubtitle() const noexcept override { return activeCore().bannerSubtitle(); }

    // Áudio sincronizado entre os dois núcleos
    void setVolume(float volume) noexcept override;
    [[nodiscard]] float getVolume() const noexcept override { return activeCore().getVolume(); }
    void setMuted(bool state) noexcept override;
    bool toggleMute() noexcept override;
    [[nodiscard]] bool isMuted() const noexcept override { return activeCore().isMuted(); }
    void setAudioSampleRate(uint32_t sampleRateHz) noexcept override;
    std::size_t pullAudioSamples(std::span<int16_t> outBuffer) noexcept override {
        return activeCore().pullAudioSamples(outBuffer);
    }

    // Inputs (GBA + NDS Estendido)
    void setKeyState(GbaKey key, bool pressed) noexcept override { activeCore().setKeyState(key, pressed); }
    void setExtendedButtonX(bool pressed) noexcept override { activeCore().setExtendedButtonX(pressed); }
    void setExtendedButtonY(bool pressed) noexcept override { activeCore().setExtendedButtonY(pressed); }
    void setStylusState(int x, int y, bool touching) noexcept override {
        activeCore().setStylusState(x, y, touching);
    }
    void setMicBlowing(bool active) noexcept override { activeCore().setMicBlowing(active); }
    [[nodiscard]] bool isMicBlowing() const noexcept override { return activeCore().isMicBlowing(); }
    void setLidClosed(bool closed) noexcept override { activeCore().setLidClosed(closed); }
    [[nodiscard]] bool isLidClosed() const noexcept override { return activeCore().isLidClosed(); }

    // Save States (Slots 1-9)
    bool saveState(std::vector<uint8_t>& outData) const override { return activeCore().saveState(outData); }
    bool loadState(std::span<const uint8_t> data) override { return activeCore().loadState(data); }
    bool saveStateSlot(int slot) const override { return activeCore().saveStateSlot(slot); }
    bool loadStateSlot(int slot) override { return activeCore().loadStateSlot(slot); }
    [[nodiscard]] bool hasStateSlot(int slot) const override { return activeCore().hasStateSlot(slot); }

    [[nodiscard]] GbaCore& gbaCore() noexcept { return m_gba; }
    [[nodiscard]] const GbaCore& gbaCore() const noexcept { return m_gba; }
    [[nodiscard]] nds::NdsCore& ndsCore() noexcept { return m_nds; }
    [[nodiscard]] const nds::NdsCore& ndsCore() const noexcept { return m_nds; }

private:
    GbaCore      m_gba;
    nds::NdsCore m_nds;
    ConsoleType  m_activeType{ConsoleType::GBA};

    [[nodiscard]] ICoreEmulator& activeCore() noexcept {
        return (m_activeType == ConsoleType::NDS) ? static_cast<ICoreEmulator&>(m_nds)
                                                  : static_cast<ICoreEmulator&>(m_gba);
    }
    [[nodiscard]] const ICoreEmulator& activeCore() const noexcept {
        return (m_activeType == ConsoleType::NDS) ? static_cast<const ICoreEmulator&>(m_nds)
                                                  : static_cast<const ICoreEmulator&>(m_gba);
    }

    [[nodiscard]] static bool hasNdsExtension(const std::filesystem::path& p) noexcept;
};

} // namespace gba::core

