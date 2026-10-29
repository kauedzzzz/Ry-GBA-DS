#pragma once
#include "config/ConfigManager.hpp"
#include "core/ICoreEmulator.hpp"
#include "ui/DragonTheme.hpp"
#include <SDL2/SDL.h>
#include <optional>
#include <string>
#include <vector>

namespace ryugba::ui {

enum class MenuActionType {
    None = 0,
    OpenRomDialog,
    OpenRecentPath,
    ClearRecentRoms,
    LaunchBuiltInNdsDemo,
    TogglePause,
    ResetEmulation,
    ExitApp,
    SetSpeedPercent,
    ToggleFastForward,
    SetNdsLayout,
    ToggleSwapScreens,
    ToggleLidSleep,
    ToggleDirectBootHle,
    SaveStateSlot,
    LoadStateSlot,
    OpenInputConfig,
    OpenVideoConfig,
    OpenAudioConfig,
    OpenAboutDialog,
    OpenGitHubLicenseDialog
};

struct MenuCommand {
    MenuActionType type{MenuActionType::None};
    int            intParam{0};
    std::string    strParam{};
};

class MenuBar {
public:
    MenuBar() = default;

    void setBrandIconTexture(SDL_Texture* tex) noexcept { m_brandIconTex = tex; }

    [[nodiscard]] bool isOpen() const noexcept { return m_openTopMenu >= 0; }
    void close() noexcept {
        m_openTopMenu = -1;
        m_hoveredItem = -1;
        m_openSubMenu = -1;
        m_hoveredSubItem = -1;
    }

    bool handleEvent(
        const SDL_Event& ev,
        const config::AppConfig& cfg,
        const gba::core::ICoreEmulator& core,
        bool isPaused,
        bool isFastForward,
        std::optional<MenuCommand>& outCmd
    );

    void render(
        SDL_Renderer* r,
        int windowWidth,
        const config::AppConfig& cfg,
        const gba::core::ICoreEmulator& core,
        bool isPaused,
        bool isFastForward,
        double currentFps
    );

private:
    struct DropdownItem {
        std::string    label;
        std::string    shortcut;
        MenuActionType action{MenuActionType::None};
        int            intParam{0};
        std::string    strParam{};
        bool           checked{false};
        bool           enabled{true};
        bool           isSeparator{false};
        int            subMenuId{-1};
    };

    enum SubMenuKind : int {
        SUB_NONE        = -1,
        SUB_OPEN_RECENT = 0,
        SUB_SPEED       = 1,
        SUB_NDS_LAYOUT  = 2,
        SUB_SAVE_STATE  = 3,
        SUB_LOAD_STATE  = 4
    };

    SDL_Texture* m_brandIconTex{nullptr};
    int m_openTopMenu{-1}; // 0=File, 1=Emulation, 2=Config, 3=Help
    int m_hoveredTopMenu{-1};
    int m_hoveredItem{-1};
    int m_openSubMenu{SUB_NONE};
    int m_hoveredSubItem{-1};
    int m_mouseX{0};
    int m_mouseY{0};

    [[nodiscard]] HitRect topMenuRect(int idx) const noexcept;
    [[nodiscard]] std::vector<DropdownItem> buildPrimaryItems(
        int topIdx,
        const config::AppConfig& cfg,
        const gba::core::ICoreEmulator& core,
        bool isPaused,
        bool isFastForward
    ) const;

    [[nodiscard]] std::vector<DropdownItem> buildSubItems(
        int subMenuId,
        const config::AppConfig& cfg,
        const gba::core::ICoreEmulator& core
    ) const;

    [[nodiscard]] SDL_Rect computeMenuBounds(int x, int y, const std::vector<DropdownItem>& items) const noexcept;
    [[nodiscard]] int hitTestMenuList(const SDL_Rect& bounds, const std::vector<DropdownItem>& items, int mx, int my) const noexcept;
    [[nodiscard]] int itemYOffset(const std::vector<DropdownItem>& items, int targetIdx) const noexcept;
};

} // namespace ryugba::ui
