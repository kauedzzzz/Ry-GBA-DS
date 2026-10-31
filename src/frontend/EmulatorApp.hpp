#pragma once
#include "config/ConfigManager.hpp"
#include "core/ICoreEmulator.hpp"
#include "input/InputMapper.hpp"
#include "ui/MenuBar.hpp"
#include "ui/Modals.hpp"
#include <SDL2/SDL.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ryugba::frontend {

class EmulatorApp {
public:
    explicit EmulatorApp(gba::core::ICoreEmulator& core);
    ~EmulatorApp();

    bool init(const std::filesystem::path& initialRom = {});
    int  run();
    void shutdown();

private:
    struct NdsViewports {
        SDL_Rect topRect{};
        SDL_Rect bottomRect{};
        SDL_Rect dividerRect{};
        bool     hasDivider{false};
        bool     topIsPiP{false};
        bool     bottomIsPiP{false};
    };

    gba::core::ICoreEmulator& m_core;
    config::ConfigManager     m_cfgMgr;
    input::InputMapper        m_inputMapper;
    ui::MenuBar               m_menuBar;
    ui::ModalManager          m_modals;

    SDL_Window*       m_window{nullptr};
    SDL_Renderer*     m_renderer{nullptr};
    SDL_Texture*      m_gameTexture{nullptr};       // GBA 240x160 ARGB8888
    SDL_Texture*      m_ndsTopTexture{nullptr};     // NDS Top 256x192 ARGB8888
    SDL_Texture*      m_ndsBottomTexture{nullptr};  // NDS Bottom 256x192 ARGB8888 (Touchscreen)
    SDL_Texture*      m_bannerIconTexture{nullptr}; // NDS Banner Icon 32x32 ARGB8888
    SDL_Texture*      m_logoTexture{nullptr};
    SDL_Texture*      m_miniIconTexture{nullptr};
    SDL_AudioDeviceID m_audioDevice{0};

    std::filesystem::path m_assetsDir{};
    std::filesystem::path m_defaultRomsDir{};

    bool   m_running{false};
    bool   m_paused{false};
    bool   m_fastForward{false};
    bool   m_windowExpandedForModal{false};
    double m_currentFps{60.0};

    // Estado da Touchscreen (Stylus) do Nintendo DS
    bool m_stylusTouching{false};
    int  m_stylusDsX{128};
    int  m_stylusDsY{96};

    // Notificação OSD minimalista estilo melonDS (+ suporte a banner 32x32)
    std::string m_osdMessage{};
    uint32_t    m_osdExpireTicks{0};
    uint32_t    m_osdBannerExpireTicks{0};

    // Última amostra estéreo para evitar cliques em underrun de áudio
    int16_t m_lastAudioL{0};
    int16_t m_lastAudioR{0};

    void locateProjectDirectories();
    void seedDiscoveredRomsIfEmpty();
    void syncNdsBootConfig();
    void refreshRomBannerTexture();
    bool loadRomFile(const std::filesystem::path& path);
    void launchBuiltInNdsDemo();
    void releaseFrontendInputs() noexcept;
    void updateWindowTitle();

    void computeTargetWindowSize(int& outW, int& outH) const noexcept;
    void applyVideoSettings(bool resizeWindow);
    void ensureModalWindowSize();
    void restorePostModalWindowSize();
    [[nodiscard]] SDL_Rect computeGameViewport(int winW, int winH) const noexcept;
    [[nodiscard]] NdsViewports computeNdsViewports(int winW, int winH) const noexcept;
    [[nodiscard]] bool mapWindowToStylus(
        int mx, int my, const SDL_Rect& bottomRect, int& outDsX, int& outDsY, bool clampOutside
    ) const noexcept;

    void applyAudioVolume() noexcept;
    void reopenAudioDevice();
    static void sdlAudioCallback(void* userdata, Uint8* stream, int len) noexcept;

    void executeMenuCommand(const ui::MenuCommand& cmd);
    void onHotkeyTriggered(config::HotkeyAction hk);
    void showOsd(std::string msg, uint32_t durationMs = 2200, bool withBanner = false);

    void renderNdsScreens(int winW, int winH, int mouseX, int mouseY);
    void renderStylusCursor(const SDL_Rect& bottomRect, int mouseX, int mouseY);
    void renderIdleScreen(int winW, int winH, int mouseX, int mouseY);
    void renderOsd(int winW, int winH);
    void renderFastForwardBadge(int winW, int winH);
};

} // namespace ryugba::frontend
