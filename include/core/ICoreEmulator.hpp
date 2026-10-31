#pragma once
#include "core/input/InputManager.hpp"
#include "core/memory/save/IBackupMedia.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gba::core {

enum class ConsoleType : uint8_t {
    GBA = 0,
    NDS = 1
};

// =============================================================================
// Interface Pura de Desacoplamento Core <-> Frontend (Filosofia melonDS)
// Suporta tanto single-screen GBA (240x160) quanto dual-screen NDS (2x 256x192).
// =============================================================================
class ICoreEmulator {
public:
    virtual ~ICoreEmulator() = default;

    virtual bool loadBios(std::span<const uint8_t> biosData) = 0;
    virtual bool loadRom(std::span<const uint8_t> romData, SaveType saveType = SaveType::AutoDetect) = 0;
    virtual bool loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType = SaveType::AutoDetect) = 0;

    virtual void reset() = 0;
    virtual void runFrame(bool skipRendering = false) = 0;

    [[nodiscard]] virtual bool hasRomLoaded() const noexcept = 0;
    [[nodiscard]] virtual std::string romTitle() const noexcept = 0;
    [[nodiscard]] virtual const std::filesystem::path& romPath() const noexcept = 0;

    // Metadados de Console e Vídeo (GBA 240x160 vs Nintendo DS Dual 256x192)
    [[nodiscard]] virtual ConsoleType consoleType() const noexcept { return ConsoleType::GBA; }
    [[nodiscard]] virtual bool isDualScreen() const noexcept { return false; }
    [[nodiscard]] virtual int screenWidth() const noexcept { return 240; }
    [[nodiscard]] virtual int screenHeight() const noexcept { return 160; }

    // Framebuffer legado GBA 240x160 ARGB8888
    [[nodiscard]] virtual std::span<const uint32_t, 240 * 160> framebuffer() const noexcept = 0;

    // Framebuffers genéricos Top / Bottom (NDS retorna 256x192 em ambos; GBA retorna 240x160 no Top)
    [[nodiscard]] virtual std::span<const uint32_t> topFramebufferSpan() const noexcept {
        return framebuffer();
    }
    [[nodiscard]] virtual std::span<const uint32_t> bottomFramebufferSpan() const noexcept {
        return {};
    }

    // Ícone de Banner 32x32 ARGB8888 e título estendido (ROMs .nds)
    [[nodiscard]] virtual bool hasBannerIcon() const noexcept { return false; }
    [[nodiscard]] virtual std::span<const uint32_t> bannerIcon32x32() const noexcept { return {}; }
    [[nodiscard]] virtual std::string bannerSubtitle() const noexcept { return {}; }

    // Controle Lock-Free de Áudio
    virtual void setVolume(float volume) noexcept = 0;
    [[nodiscard]] virtual float getVolume() const noexcept = 0;
    virtual void setMuted(bool state) noexcept = 0;
    virtual bool toggleMute() noexcept = 0;
    [[nodiscard]] virtual bool isMuted() const noexcept = 0;
    virtual void setAudioSampleRate(uint32_t sampleRateHz) noexcept = 0;
    virtual std::size_t pullAudioSamples(std::span<int16_t> outBuffer) noexcept = 0;

    // Entradas virtuais desacopladas (GBA: 10 botões; NDS: +X, +Y, Touchscreen Stylus, Microfone e Tampa)
    virtual void setKeyState(GbaKey key, bool pressed) noexcept = 0;
    virtual void setExtendedButtonX(bool pressed) noexcept { (void)pressed; }
    virtual void setExtendedButtonY(bool pressed) noexcept { (void)pressed; }
    virtual void setStylusState(int x, int y, bool touching) noexcept {
        (void)x;
        (void)y;
        (void)touching;
    }
    virtual void setMicBlowing(bool active) noexcept { (void)active; }
    [[nodiscard]] virtual bool isMicBlowing() const noexcept { return false; }
    virtual void setLidClosed(bool closed) noexcept { (void)closed; }
    [[nodiscard]] virtual bool isLidClosed() const noexcept { return false; }

    // Save States (Slots 1-9 estilo melonDS)
    virtual bool saveState(std::vector<uint8_t>& outData) const = 0;
    virtual bool loadState(std::span<const uint8_t> data) = 0;
    virtual bool saveStateSlot(int slot) const = 0;
    virtual bool loadStateSlot(int slot) = 0;
    [[nodiscard]] virtual bool hasStateSlot(int slot) const = 0;
};

} // namespace gba::core
