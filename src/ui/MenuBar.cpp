#include "ui/MenuBar.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

namespace ryugba::ui {

namespace {

constexpr std::array<std::string_view, 4> TOP_LABELS = {
    "File", "Emulation", "Config", "Help"
};

constexpr int ITEM_ROW_H = 24;
constexpr int SEP_ROW_H  = 9;
constexpr int MENU_PAD_Y = 4;

std::string truncateMiddle(std::string_view text, std::size_t maxChars = 34) {
    if (text.size() <= maxChars) return std::string(text);
    const std::size_t keepFront = (maxChars - 3) / 2 + 2;
    const std::size_t keepBack  = maxChars - 3 - keepFront;
    return std::string(text.substr(0, keepFront)) + "..." +
           std::string(text.substr(text.size() - keepBack));
}

} // namespace

HitRect MenuBar::topMenuRect(int idx) const noexcept {
    int curX = 30;
    for (int i = 0; i < 4; ++i) {
        const int w = DragonFont::measureWidth(TOP_LABELS[i], 1) + 20;
        if (i == idx) {
            return HitRect{SDL_Rect{curX, 0, w, DragonPalette::MENUBAR_HEIGHT}};
        }
        curX += w + 2;
    }
    return HitRect{};
}

std::vector<MenuBar::DropdownItem> MenuBar::buildPrimaryItems(
    int topIdx,
    const config::AppConfig& cfg,
    const gba::core::ICoreEmulator& core,
    bool isPaused,
    bool isFastForward
) const {
    std::vector<DropdownItem> list;
    const bool hasRom = core.hasRomLoaded();
    const bool isNds  = core.isDualScreen();

    switch (topIdx) {
        case 0: { // File
            list.push_back({"Open ROM (.nds / .gba)...", "Ctrl+O", MenuActionType::OpenRomDialog});
            DropdownItem recent{};
            recent.label     = "Open Recent";
            recent.subMenuId = SUB_OPEN_RECENT;
            list.push_back(std::move(recent));

            list.push_back({"Launch Built-in Ry\xC5\xAB" "DS Demo", "", MenuActionType::LaunchBuiltInNdsDemo});

            DropdownItem sep1{}; sep1.isSeparator = true;
            list.push_back(sep1);

            DropdownItem pauseItem{"Pause", "P", MenuActionType::TogglePause};
            pauseItem.checked = isPaused && hasRom;
            pauseItem.enabled = hasRom;
            list.push_back(std::move(pauseItem));

            DropdownItem resetItem{"Reset", "Ctrl+R", MenuActionType::ResetEmulation};
            resetItem.enabled = hasRom;
            list.push_back(std::move(resetItem));

            DropdownItem sep2{}; sep2.isSeparator = true;
            list.push_back(sep2);

            list.push_back({"Exit", "Ctrl+Q", MenuActionType::ExitApp});
            break;
        }
        case 1: { // Emulation
            DropdownItem speedItem{};
            speedItem.label     = "Speed";
            speedItem.subMenuId = SUB_SPEED;
            list.push_back(std::move(speedItem));

            DropdownItem ffSub{};
            ffSub.label     = "Fast Forward";
            ffSub.subMenuId = SUB_FAST_FORWARD;
            list.push_back(std::move(ffSub));

            DropdownItem ffItem{"Fast Forward On/Off", "Tab", MenuActionType::ToggleFastForward};
            ffItem.checked = isFastForward;
            list.push_back(std::move(ffItem));

            DropdownItem sep1{}; sep1.isSeparator = true;
            list.push_back(sep1);

            DropdownItem layoutSub{};
            layoutSub.label     = "DS Screen Layout";
            layoutSub.subMenuId = SUB_NDS_LAYOUT;
            list.push_back(std::move(layoutSub));

            DropdownItem swapItem{"Swap Top/Bottom Screens", "F6", MenuActionType::ToggleSwapScreens};
            swapItem.checked = cfg.video.swapScreens;
            list.push_back(std::move(swapItem));

            DropdownItem lidItem{"Close DS Lid (Sleep Mode)", "F7", MenuActionType::ToggleLidSleep};
            lidItem.checked = isNds && core.isLidClosed();
            lidItem.enabled = isNds;
            list.push_back(std::move(lidItem));

            DropdownItem hleItem{"Direct Boot / HLE (No BIOS)", "", MenuActionType::ToggleDirectBootHle};
            hleItem.checked = cfg.ndsBoot.directBootHle;
            list.push_back(std::move(hleItem));

            DropdownItem sep2{}; sep2.isSeparator = true;
            list.push_back(sep2);

            DropdownItem saveSub{};
            saveSub.label     = "Save State";
            saveSub.subMenuId = SUB_SAVE_STATE;
            saveSub.enabled   = hasRom;
            list.push_back(std::move(saveSub));

            DropdownItem loadSub{};
            loadSub.label     = "Load State";
            loadSub.subMenuId = SUB_LOAD_STATE;
            loadSub.enabled   = hasRom;
            list.push_back(std::move(loadSub));
            break;
        }
        case 2: { // Config
            list.push_back({"Input and Hotkeys...", "", MenuActionType::OpenInputConfig});
            list.push_back({"Video & DS Layout...", "", MenuActionType::OpenVideoConfig});
            list.push_back({"Audio Settings...",    "", MenuActionType::OpenAudioConfig});
            break;
        }
        case 3: { // Help
            list.push_back({"About Ry\xC5\xAB" "DS / Ry\xC5\xABGBA...", "", MenuActionType::OpenAboutDialog});
            list.push_back({"GitHub / Licen\xC3\xA7" "a...",            "", MenuActionType::OpenGitHubLicenseDialog});
            break;
        }
        default:
            break;
    }
    return list;
}

std::vector<MenuBar::DropdownItem> MenuBar::buildSubItems(
    int subMenuId,
    const config::AppConfig& cfg,
    const gba::core::ICoreEmulator& core
) const {
    std::vector<DropdownItem> list;
    switch (subMenuId) {
        case SUB_OPEN_RECENT: {
            if (cfg.recentRoms.empty()) {
                DropdownItem empty{"(No recent ROMs)", "", MenuActionType::None};
                empty.enabled = false;
                list.push_back(std::move(empty));
            } else {
                for (const auto& pathStr : cfg.recentRoms) {
                    const std::string fname = std::filesystem::path(pathStr).filename().string();
                    DropdownItem item{};
                    item.label    = truncateMiddle(fname, 34);
                    item.action   = MenuActionType::OpenRecentPath;
                    item.strParam = pathStr;
                    list.push_back(std::move(item));
                }
                DropdownItem sep{}; sep.isSeparator = true;
                list.push_back(sep);
                list.push_back({"Clear Recent List", "", MenuActionType::ClearRecentRoms});
            }
            break;
        }
        case SUB_SPEED: {
            constexpr std::array<std::pair<int, const char*>, 5> speeds = {{
                {100, "100% (Normal 60 FPS)"},
                {150, "150% (90 FPS)"},
                {200, "200% (120 FPS)"},
                {300, "300% (180 FPS)"},
                {0,   "Uncapped (Max Speed)"}
            }};
            for (const auto& [pct, lbl] : speeds) {
                DropdownItem item{lbl, "", MenuActionType::SetSpeedPercent, pct};
                item.checked = (cfg.speedPercent == pct);
                list.push_back(std::move(item));
            }
            break;
        }
        case SUB_FAST_FORWARD: {
            DropdownItem modeItem{
                (cfg.fastForwardMode == config::FastForwardMode::Hold)
                    ? "Mode: Hold (Hold Tab to speed up)"
                    : "Mode: Toggle (Press Tab to toggle)",
                "",
                MenuActionType::SetFastForwardMode,
                (cfg.fastForwardMode == config::FastForwardMode::Hold) ? 1 : 0
            };
            list.push_back(std::move(modeItem));

            DropdownItem sep{}; sep.isSeparator = true;
            list.push_back(sep);

            constexpr std::array<std::pair<int, const char*>, 4> ffSpeeds = {{
                {2, "Multiplier: 2x (120 FPS)"},
                {4, "Multiplier: 4x (240 FPS)"},
                {8, "Multiplier: 8x (480 FPS)"},
                {0, "Multiplier: Uncapped (Max)"}
            }};
            for (const auto& [spd, lbl] : ffSpeeds) {
                DropdownItem item{lbl, "", MenuActionType::SetFastForwardSpeed, spd};
                item.checked = (static_cast<int>(cfg.fastForwardSpeed) == spd);
                list.push_back(std::move(item));
            }
            break;
        }
        case SUB_NDS_LAYOUT: {
            constexpr std::array<std::pair<config::NdsScreenLayout, const char*>, 3> layouts = {{
                {config::NdsScreenLayout::Vertical,    "Vertical (Padrao DS)"},
                {config::NdsScreenLayout::Horizontal,  "Horizontal (Lado a Lado)"},
                {config::NdsScreenLayout::SingleFocus, "Foco Unico (Single + PiP)"}
            }};
            for (const auto& [lay, lbl] : layouts) {
                DropdownItem item{lbl, "", MenuActionType::SetNdsLayout, static_cast<int>(lay)};
                item.checked = (cfg.video.ndsLayout == lay);
                list.push_back(std::move(item));
            }
            DropdownItem sep{}; sep.isSeparator = true;
            list.push_back(sep);
            DropdownItem swap{"Inverter Telas (Swap)", "F6", MenuActionType::ToggleSwapScreens};
            swap.checked = cfg.video.swapScreens;
            list.push_back(std::move(swap));
            break;
        }
        case SUB_SAVE_STATE: {
            for (int slot = 1; slot <= 9; ++slot) {
                const bool exists = core.hasRomLoaded() && core.hasStateSlot(slot);
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "Slot %d%s", slot, exists ? " [Saved]" : "");
                DropdownItem item{lbl, slot == 1 ? "F5" : "", MenuActionType::SaveStateSlot, slot};
                item.enabled = core.hasRomLoaded();
                list.push_back(std::move(item));
            }
            break;
        }
        case SUB_LOAD_STATE: {
            for (int slot = 1; slot <= 9; ++slot) {
                const bool exists = core.hasRomLoaded() && core.hasStateSlot(slot);
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "Slot %d%s", slot, exists ? " [Saved]" : " (Empty)");
                DropdownItem item{lbl, slot == 1 ? "F8" : "", MenuActionType::LoadStateSlot, slot};
                item.enabled = exists;
                list.push_back(std::move(item));
            }
            break;
        }
        default:
            break;
    }
    return list;
}

SDL_Rect MenuBar::computeMenuBounds(int x, int y, const std::vector<DropdownItem>& items) const noexcept {
    int maxW = 190;
    int totalH = MENU_PAD_Y * 2;
    for (const auto& it : items) {
        if (it.isSeparator) {
            totalH += SEP_ROW_H;
        } else {
            totalH += ITEM_ROW_H;
            const int lw = DragonFont::measureWidth(it.label, 1);
            const int sw = it.subMenuId >= 0 ? 18 : (it.shortcut.empty() ? 0 : DragonFont::measureWidth(it.shortcut, 1) + 18);
            maxW = std::max(maxW, 26 + lw + sw + 18);
        }
    }
    return SDL_Rect{x, y, maxW, totalH};
}

int MenuBar::hitTestMenuList(const SDL_Rect& bounds, const std::vector<DropdownItem>& items, int mx, int my) const noexcept {
    if (mx < bounds.x || mx >= bounds.x + bounds.w || my < bounds.y || my >= bounds.y + bounds.h) {
        return -1;
    }
    int curY = bounds.y + MENU_PAD_Y;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int h = items[i].isSeparator ? SEP_ROW_H : ITEM_ROW_H;
        if (!items[i].isSeparator && my >= curY && my < curY + h) {
            return static_cast<int>(i);
        }
        curY += h;
    }
    return -1;
}

int MenuBar::itemYOffset(const std::vector<DropdownItem>& items, int targetIdx) const noexcept {
    int y = MENU_PAD_Y;
    for (int i = 0; i < targetIdx && i < static_cast<int>(items.size()); ++i) {
        y += items[i].isSeparator ? SEP_ROW_H : ITEM_ROW_H;
    }
    return y;
}

bool MenuBar::handleEvent(
    const SDL_Event& ev,
    const config::AppConfig& cfg,
    const gba::core::ICoreEmulator& core,
    bool isPaused,
    bool isFastForward,
    std::optional<MenuCommand>& outCmd
) {
    if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        const bool ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
        if (m_openTopMenu >= 0 && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            close();
            return true;
        }
        if (ctrl && ev.key.keysym.scancode == SDL_SCANCODE_O) {
            close();
            outCmd = MenuCommand{MenuActionType::OpenRomDialog};
            return true;
        }
        if (ctrl && ev.key.keysym.scancode == SDL_SCANCODE_R && core.hasRomLoaded()) {
            close();
            outCmd = MenuCommand{MenuActionType::ResetEmulation};
            return true;
        }
        if (ctrl && ev.key.keysym.scancode == SDL_SCANCODE_Q) {
            close();
            outCmd = MenuCommand{MenuActionType::ExitApp};
            return true;
        }
    }

    if (ev.type == SDL_MOUSEMOTION) {
        m_mouseX = ev.motion.x;
        m_mouseY = ev.motion.y;
        m_hoveredTopMenu = -1;
        for (int i = 0; i < 4; ++i) {
            if (topMenuRect(i).contains(m_mouseX, m_mouseY)) {
                m_hoveredTopMenu = i;
                break;
            }
        }

        if (m_openTopMenu >= 0) {
            if (m_hoveredTopMenu >= 0 && m_hoveredTopMenu != m_openTopMenu) {
                m_openTopMenu = m_hoveredTopMenu;
                m_hoveredItem = -1;
                m_openSubMenu = SUB_NONE;
                m_hoveredSubItem = -1;
            }

            const auto primary = buildPrimaryItems(m_openTopMenu, cfg, core, isPaused, isFastForward);
            const SDL_Rect pBounds = computeMenuBounds(topMenuRect(m_openTopMenu).rect.x, DragonPalette::MENUBAR_HEIGHT, primary);
            const int hitPrimary = hitTestMenuList(pBounds, primary, m_mouseX, m_mouseY);

            if (hitPrimary >= 0) {
                m_hoveredItem = hitPrimary;
                m_hoveredSubItem = -1;
                if (primary[hitPrimary].enabled && primary[hitPrimary].subMenuId >= 0) {
                    m_openSubMenu = primary[hitPrimary].subMenuId;
                } else {
                    m_openSubMenu = SUB_NONE;
                }
            } else if (m_openSubMenu != SUB_NONE && m_hoveredItem >= 0 && m_hoveredItem < static_cast<int>(primary.size())) {
                const auto subItems = buildSubItems(m_openSubMenu, cfg, core);
                const int subY = pBounds.y + itemYOffset(primary, m_hoveredItem) - MENU_PAD_Y;
                int subX = pBounds.x + pBounds.w - 2;
                int winW = 720;
                if (SDL_Window* win = SDL_GetWindowFromID(ev.motion.windowID)) {
                    SDL_GetWindowSize(win, &winW, nullptr);
                }
                SDL_Rect sBounds = computeMenuBounds(subX, subY, subItems);
                if (sBounds.x + sBounds.w > winW) {
                    sBounds.x = std::max(0, pBounds.x - sBounds.w + 2);
                }
                m_hoveredSubItem = hitTestMenuList(sBounds, subItems, m_mouseX, m_mouseY);
            } else {
                m_hoveredItem = -1;
            }
            return true;
        }
        return m_mouseY < DragonPalette::MENUBAR_HEIGHT;
    }

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        const int mx = ev.button.x;
        const int my = ev.button.y;

        for (int i = 0; i < 4; ++i) {
            if (topMenuRect(i).contains(mx, my)) {
                if (m_openTopMenu == i) {
                    close();
                } else {
                    m_openTopMenu = i;
                    m_hoveredItem = -1;
                    m_openSubMenu = SUB_NONE;
                    m_hoveredSubItem = -1;
                }
                return true;
            }
        }

        if (m_openTopMenu >= 0) {
            const auto primary = buildPrimaryItems(m_openTopMenu, cfg, core, isPaused, isFastForward);
            const SDL_Rect pBounds = computeMenuBounds(topMenuRect(m_openTopMenu).rect.x, DragonPalette::MENUBAR_HEIGHT, primary);

            if (m_openSubMenu != SUB_NONE && m_hoveredItem >= 0 && m_hoveredItem < static_cast<int>(primary.size())) {
                const auto subItems = buildSubItems(m_openSubMenu, cfg, core);
                const int subY = pBounds.y + itemYOffset(primary, m_hoveredItem) - MENU_PAD_Y;
                int subX = pBounds.x + pBounds.w - 2;
                int winW = 720;
                if (SDL_Window* win = SDL_GetWindowFromID(ev.button.windowID)) {
                    SDL_GetWindowSize(win, &winW, nullptr);
                }
                SDL_Rect sBounds = computeMenuBounds(subX, subY, subItems);
                if (sBounds.x + sBounds.w > winW) {
                    sBounds.x = std::max(0, pBounds.x - sBounds.w + 2);
                }
                const int hitSub = hitTestMenuList(sBounds, subItems, mx, my);
                if (hitSub >= 0) {
                    const auto& chosen = subItems[hitSub];
                    if (chosen.enabled && chosen.action != MenuActionType::None) {
                        outCmd = MenuCommand{chosen.action, chosen.intParam, chosen.strParam};
                        close();
                    }
                    return true;
                }
            }

            const int hitPrimary = hitTestMenuList(pBounds, primary, mx, my);
            if (hitPrimary >= 0) {
                const auto& chosen = primary[hitPrimary];
                if (chosen.enabled) {
                    if (chosen.subMenuId >= 0) {
                        m_hoveredItem = hitPrimary;
                        m_openSubMenu = chosen.subMenuId;
                    } else if (chosen.action != MenuActionType::None) {
                        outCmd = MenuCommand{chosen.action, chosen.intParam, chosen.strParam};
                        close();
                    }
                }
                return true;
            }

            close();
            return true;
        }

        if (my < DragonPalette::MENUBAR_HEIGHT) {
            return true;
        }
    }

    return false;
}

void MenuBar::render(
    SDL_Renderer* r,
    int windowWidth,
    const config::AppConfig& cfg,
    const gba::core::ICoreEmulator& core,
    bool isPaused,
    bool isFastForward,
    double currentFps
) {
    fillRect(r, SDL_Rect{0, 0, windowWidth, DragonPalette::MENUBAR_HEIGHT}, DragonPalette::BG_SLATE_DEEP);
    fillRect(r, SDL_Rect{0, DragonPalette::MENUBAR_HEIGHT - 1, windowWidth, 1}, DragonPalette::BORDER_SUBTLE);

    if (m_brandIconTex) {
        const SDL_Rect iconDst{7, 4, 18, 18};
        SDL_RenderCopy(r, m_brandIconTex, nullptr, &iconDst);
    }

    for (int i = 0; i < 4; ++i) {
        const SDL_Rect tab = topMenuRect(i).rect;
        const bool isActive = (m_openTopMenu == i);
        const bool isHover  = (m_hoveredTopMenu == i);

        if (isActive) {
            fillRect(r, SDL_Rect{tab.x, 1, tab.w, tab.h - 2}, DragonPalette::SURFACE_SCALES);
            fillRect(r, SDL_Rect{tab.x, tab.h - 2, tab.w, 2}, DragonPalette::DRAGON_RED);
        } else if (isHover) {
            fillRect(r, SDL_Rect{tab.x, 2, tab.w, tab.h - 4}, DragonPalette::SURFACE_HOVER);
        }

        const int tx = tab.x + 10;
        const int ty = (DragonPalette::MENUBAR_HEIGHT - 8) / 2;
        DragonFont::drawText(r, TOP_LABELS[i], tx, ty, 1, DragonPalette::TEXT_PRIMARY);
    }

    if (windowWidth >= 340) {
        char statusBuf[64];
        SDL_Color statusCol = DragonPalette::TEXT_SECONDARY;
        const char* sysTag  = core.isDualScreen() ? "NDS" : "GBA";
        if (!core.hasRomLoaded()) {
            std::snprintf(statusBuf, sizeof(statusBuf), "NDS / GBA IDLE");
            statusCol = DragonPalette::TEXT_MUTED;
        } else if (core.isDualScreen() && core.isLidClosed()) {
            std::snprintf(statusBuf, sizeof(statusBuf), "[NDS] SLEEP (LID)");
            statusCol = DragonPalette::DRAGON_RED;
        } else if (isPaused) {
            std::snprintf(statusBuf, sizeof(statusBuf), "[%s] PAUSED", sysTag);
            statusCol = DragonPalette::DRAGON_RED;
        } else if (isFastForward) {
            const double fpsDisplay = (currentFps > 0.0) ? currentFps : 480.0;
            std::snprintf(statusBuf, sizeof(statusBuf), "[%s] \xF0\x9F\x94\xA5 8x (~%.0f FPS)", sysTag, fpsDisplay);
            statusCol = DragonPalette::DRAGON_RED;
        } else {
            const double fpsDisplay = (currentFps >= 58.0 && currentFps <= 62.0) ? 60.0 : ((currentFps > 0.0) ? currentFps : 60.0);
            std::snprintf(statusBuf, sizeof(statusBuf), "[%s] %.1f FPS", sysTag, fpsDisplay);
            statusCol = DragonPalette::EMERALD_SCALE;
        }
        const int sw = DragonFont::measureWidth(statusBuf, 1);
        DragonFont::drawText(r, statusBuf, windowWidth - sw - 10, (DragonPalette::MENUBAR_HEIGHT - 8) / 2, 1, statusCol);
    }

    if (m_openTopMenu < 0) return;

    auto drawMenuBox = [&](const SDL_Rect& bounds, const std::vector<DropdownItem>& items, int hoveredIdx) {
        drawPanel(r, bounds, DragonPalette::SURFACE_SCALES, DragonPalette::BORDER_SUBTLE);
        int curY = bounds.y + MENU_PAD_Y;

        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.isSeparator) {
                fillRect(r, SDL_Rect{bounds.x + 8, curY + SEP_ROW_H / 2, bounds.w - 16, 1}, DragonPalette::BORDER_SUBTLE);
                curY += SEP_ROW_H;
                continue;
            }

            const bool hov = (static_cast<int>(i) == hoveredIdx) && it.enabled;
            if (hov) {
                fillRect(r, SDL_Rect{bounds.x + 3, curY, bounds.w - 6, ITEM_ROW_H}, DragonPalette::DRAGON_RED);
            }

            const SDL_Color txtCol = !it.enabled ? DragonPalette::TEXT_MUTED : DragonPalette::TEXT_PRIMARY;

            if (it.checked) {
                const SDL_Color chkCol = hov ? DragonPalette::TEXT_PRIMARY : DragonPalette::EMERALD_SCALE;
                DragonFont::drawText(r, "\xE2\x9C\x93", bounds.x + 9, curY + (ITEM_ROW_H - 8) / 2, 1, chkCol);
            }

            DragonFont::drawText(r, it.label, bounds.x + 24, curY + (ITEM_ROW_H - 8) / 2, 1, txtCol);

            if (it.subMenuId >= 0) {
                DragonFont::drawText(r, "\xE2\x96\xB6", bounds.x + bounds.w - 16, curY + (ITEM_ROW_H - 8) / 2, 1,
                                     hov ? DragonPalette::TEXT_PRIMARY : DragonPalette::TEXT_SECONDARY);
            } else if (!it.shortcut.empty()) {
                const int scW = DragonFont::measureWidth(it.shortcut, 1);
                DragonFont::drawText(r, it.shortcut, bounds.x + bounds.w - scW - 10, curY + (ITEM_ROW_H - 8) / 2, 1,
                                     hov ? DragonPalette::TEXT_PRIMARY : DragonPalette::TEXT_SECONDARY);
            }

            curY += ITEM_ROW_H;
        }
    };

    const auto primary = buildPrimaryItems(m_openTopMenu, cfg, core, isPaused, isFastForward);
    SDL_Rect pBounds = computeMenuBounds(topMenuRect(m_openTopMenu).rect.x, DragonPalette::MENUBAR_HEIGHT, primary);
    if (pBounds.x + pBounds.w > windowWidth) {
        pBounds.x = std::max(0, windowWidth - pBounds.w);
    }
    drawMenuBox(pBounds, primary, m_hoveredItem);

    if (m_openSubMenu != SUB_NONE && m_hoveredItem >= 0 && m_hoveredItem < static_cast<int>(primary.size())) {
        const auto subItems = buildSubItems(m_openSubMenu, cfg, core);
        const int subY = pBounds.y + itemYOffset(primary, m_hoveredItem) - MENU_PAD_Y;
        SDL_Rect sBounds = computeMenuBounds(pBounds.x + pBounds.w - 2, subY, subItems);
        if (sBounds.x + sBounds.w > windowWidth) {
            sBounds.x = std::max(0, pBounds.x - sBounds.w + 2);
        }
        drawMenuBox(sBounds, subItems, m_hoveredSubItem);
    }
}

} // namespace ryugba::ui
