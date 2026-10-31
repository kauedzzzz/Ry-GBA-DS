#include "frontend/EmulatorApp.hpp"
#include "core/HybridCore.hpp"
#include "ui/DragonTheme.hpp"
#include "ui/SvgLogoRenderer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <span>

namespace ryugba::frontend {

namespace {

constexpr int GBA_WIDTH  = 240;
constexpr int GBA_HEIGHT = 160;
constexpr int NDS_WIDTH  = 256;
constexpr int NDS_HEIGHT = 192;
constexpr int NDS_DIVIDER_PX = 4;

constexpr double GBA_NATIVE_FPS = 16777216.0 / 280896.0; // ~59.7275 Hz
constexpr double NDS_NATIVE_FPS = 33513982.0 / 560190.0; // ~59.8261 Hz

bool isSupportedRomFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".nds" || ext == ".dsi" || ext == ".srl" ||
           ext == ".gba" || ext == ".agb" || ext == ".bin";
}

std::string shortenFilename(std::string_view name, std::size_t maxLen = 34) {
    if (name.size() <= maxLen) return std::string(name);
    return std::string(name.substr(0, maxLen - 3)) + "...";
}

} // namespace

EmulatorApp::EmulatorApp(gba::core::ICoreEmulator& core)
    : m_core(core)
    , m_cfgMgr()
    , m_inputMapper(m_cfgMgr)
    , m_menuBar()
    , m_modals(m_cfgMgr, m_inputMapper) {}

EmulatorApp::~EmulatorApp() {
    shutdown();
}

void EmulatorApp::locateProjectDirectories() {
    std::vector<std::filesystem::path> roots;
#ifdef GBA_PROJECT_DIR
    roots.emplace_back(GBA_PROJECT_DIR);
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        roots.push_back(cwd);
        if (cwd.has_parent_path()) roots.push_back(cwd.parent_path());
    }
    if (char* base = SDL_GetBasePath()) {
        std::filesystem::path exeDir(base);
        SDL_free(base);
        roots.push_back(exeDir);
        if (exeDir.has_parent_path()) roots.push_back(exeDir.parent_path());
    }

    for (const auto& r : roots) {
        if (m_assetsDir.empty() && std::filesystem::exists(r / "assets" / "ryu_logo.svg", ec)) {
            m_assetsDir = std::filesystem::weakly_canonical(r / "assets", ec);
        }
        if (m_defaultRomsDir.empty() && std::filesystem::is_directory(r.parent_path() / "roms", ec)) {
            m_defaultRomsDir = std::filesystem::weakly_canonical(r.parent_path() / "roms", ec);
        }
        if (m_defaultRomsDir.empty() && std::filesystem::is_directory(r / "roms", ec)) {
            m_defaultRomsDir = std::filesystem::weakly_canonical(r / "roms", ec);
        }
    }
}

void EmulatorApp::seedDiscoveredRomsIfEmpty() {
    if (!m_cfgMgr.data().recentRoms.empty()) return;

    std::error_code ec;
    std::vector<std::filesystem::path> searchDirs;
    if (!m_defaultRomsDir.empty()) searchDirs.push_back(m_defaultRomsDir);
#ifdef GBA_PROJECT_DIR
    searchDirs.push_back(std::filesystem::path(GBA_PROJECT_DIR).parent_path() / "roms");
    searchDirs.push_back(std::filesystem::path(GBA_PROJECT_DIR) / "roms");
#endif

    for (const auto& dir : searchDirs) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && isSupportedRomFile(entry.path())) {
                if (entry.path().filename() != "GBA_Core_Lab_Demo.gba") {
                    m_cfgMgr.addRecentRom(entry.path());
                }
            }
        }
        if (!m_cfgMgr.data().recentRoms.empty()) break;
    }
}

void EmulatorApp::syncNdsBootConfig() {
    auto* hybrid = dynamic_cast<gba::core::HybridCore*>(&m_core);
    if (!hybrid) return;

    const auto& b = m_cfgMgr.data().ndsBoot;
    auto resolveSystemFile = [&](const std::string& raw) -> std::filesystem::path {
        if (raw.empty()) return {};
        std::filesystem::path p(raw);
        std::error_code ec;
        if (p.is_absolute() || std::filesystem::exists(p, ec)) return p;
        if (!m_assetsDir.empty()) {
            const auto projRoot = m_assetsDir.parent_path();
            if (std::filesystem::exists(projRoot / "system" / p, ec)) return projRoot / "system" / p;
            if (std::filesystem::exists(projRoot / p, ec)) return projRoot / p;
        }
        if (!m_defaultRomsDir.empty() && std::filesystem::exists(m_defaultRomsDir / p, ec)) {
            return m_defaultRomsDir / p;
        }
        return p;
    };

    hybrid->configureNdsBootFiles(
        b.directBootHle,
        resolveSystemFile(b.bios9Path),
        resolveSystemFile(b.bios7Path),
        resolveSystemFile(b.firmwarePath)
    );
}

void EmulatorApp::refreshRomBannerTexture() {
    if (!m_renderer || !m_bannerIconTexture) return;

    if (m_core.hasBannerIcon()) {
        const auto pixels = m_core.bannerIcon32x32();
        if (pixels.size() == 32u * 32u) {
            SDL_UpdateTexture(m_bannerIconTexture, nullptr, pixels.data(), 32 * static_cast<int>(sizeof(uint32_t)));
            m_modals.setRomBannerInfo(m_bannerIconTexture, m_core.romTitle(), m_core.bannerSubtitle());
            return;
        }
    }
    m_modals.setRomBannerInfo(nullptr, {}, {});
}

bool EmulatorApp::init(const std::filesystem::path& initialRom) {
    m_cfgMgr.load();

    Uint32 sdlFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS;
    if (SDL_Init(sdlFlags) != 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
            std::cerr << "[RyūDS] SDL_Init error: " << SDL_GetError() << "\n";
            return false;
        }
    }

    locateProjectDirectories();
    seedDiscoveredRomsIfEmpty();
    syncNdsBootConfig();

    int initW = 720;
    int initH = 506;
    computeTargetWindowSize(initW, initH);

    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "ryugba");
    SDL_SetHint("SDL_APP_NAME", "RyūDS / RyūGBA");

    m_window = SDL_CreateWindow(
        "Ry\xC5\xAB" "DS / Ry\xC5\xABGBA",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        initW,
        initH,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!m_window) {
        std::cerr << "[RyūDS] SDL_CreateWindow error: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_SetWindowMinimumSize(m_window, 380, GBA_HEIGHT + ui::DragonPalette::MENUBAR_HEIGHT);

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (!m_renderer) {
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!m_renderer) {
        std::cerr << "[RyūDS] SDL_CreateRenderer error: " << SDL_GetError() << "\n";
        return false;
    }

    // Textura nativa 240x160 para GBA e par 256x192 para Nintendo DS Dual-Screen
    m_gameTexture = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, GBA_WIDTH, GBA_HEIGHT
    );
    m_ndsTopTexture = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, NDS_WIDTH, NDS_HEIGHT
    );
    m_ndsBottomTexture = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, NDS_WIDTH, NDS_HEIGHT
    );
    m_bannerIconTexture = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 32, 32
    );
    if (m_bannerIconTexture) {
        SDL_SetTextureBlendMode(m_bannerIconTexture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(m_bannerIconTexture, SDL_ScaleModeNearest);
    }

    // Carrega e rasteriza a logo vetorial SVG do Dragão
    if (!m_assetsDir.empty()) {
        const auto logoSvg = m_assetsDir / "ryu_logo.svg";
        const auto iconSvg = std::filesystem::exists(m_assetsDir / "ryu_icon.svg")
            ? (m_assetsDir / "ryu_icon.svg")
            : logoSvg;

        if (SDL_Surface* iconSurf = ui::SvgLogoRenderer::createWindowIconSurface(iconSvg, 64)) {
            SDL_SetWindowIcon(m_window, iconSurf);
            SDL_FreeSurface(iconSurf);
        }

        m_logoTexture     = ui::SvgLogoRenderer::createLogoTexture(m_renderer, logoSvg, 160);
        m_miniIconTexture = ui::SvgLogoRenderer::createLogoTexture(m_renderer, iconSvg, 36);
        m_menuBar.setBrandIconTexture(m_miniIconTexture);
        m_modals.setLogoTexture(m_logoTexture);
    }

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    m_inputMapper.initControllers();
    m_inputMapper.setHotkeyCallback([this](config::HotkeyAction hk) {
        onHotkeyTriggered(hk);
    });

    applyVideoSettings(false);
    applyAudioVolume();
    reopenAudioDevice();

    if (!initialRom.empty()) {
        loadRomFile(initialRom);
    }

    m_running = true;
    return true;
}

void EmulatorApp::shutdown() {
    m_running = false;
    m_inputMapper.closeControllers();

    if (m_audioDevice != 0) {
        SDL_CloseAudioDevice(m_audioDevice);
        m_audioDevice = 0;
    }
    if (m_bannerIconTexture) {
        SDL_DestroyTexture(m_bannerIconTexture);
        m_bannerIconTexture = nullptr;
    }
    if (m_ndsBottomTexture) {
        SDL_DestroyTexture(m_ndsBottomTexture);
        m_ndsBottomTexture = nullptr;
    }
    if (m_ndsTopTexture) {
        SDL_DestroyTexture(m_ndsTopTexture);
        m_ndsTopTexture = nullptr;
    }
    if (m_miniIconTexture) {
        SDL_DestroyTexture(m_miniIconTexture);
        m_miniIconTexture = nullptr;
    }
    if (m_logoTexture) {
        SDL_DestroyTexture(m_logoTexture);
        m_logoTexture = nullptr;
    }
    if (m_gameTexture) {
        SDL_DestroyTexture(m_gameTexture);
        m_gameTexture = nullptr;
    }
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (SDL_WasInit(0) != 0) {
        SDL_Quit();
    }
}

void EmulatorApp::releaseFrontendInputs() noexcept {
    m_stylusTouching = false;
    m_inputMapper.releaseAllInputs(m_core);
}

bool EmulatorApp::loadRomFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        showOsd("ROM file not found!");
        return false;
    }

    syncNdsBootConfig();
    const bool wasDual = m_core.hasRomLoaded() && m_core.isDualScreen();

    if (!m_core.loadRomFromFile(path)) {
        showOsd("Failed to load ROM (.nds / .gba)!");
        return false;
    }

    m_paused         = false;
    m_stylusTouching = false;
    m_cfgMgr.addRecentRom(path);
    refreshRomBannerTexture();
    updateWindowTitle();

    if (m_core.isDualScreen() != wasDual || m_core.isDualScreen()) {
        applyVideoSettings(true);
    }

    const std::string title = m_core.romTitle();
    const std::string prefix = m_core.isDualScreen() ? "[NDS] " : "[GBA] ";
    showOsd(prefix + (title.empty() ? path.filename().string() : title), 3400, m_core.hasBannerIcon());
    return true;
}

void EmulatorApp::launchBuiltInNdsDemo() {
    auto* hybrid = dynamic_cast<gba::core::HybridCore*>(&m_core);
    if (!hybrid) {
        showOsd("NDS HybridCore unavailable!");
        return;
    }

    syncNdsBootConfig();
    hybrid->loadBuiltInNdsDemo();
    m_paused         = false;
    m_stylusTouching = false;
    refreshRomBannerTexture();
    updateWindowTitle();
    applyVideoSettings(true);
    showOsd("[NDS] RyūDS Dual-Core 3D + Stylus Lab", 3600, m_core.hasBannerIcon());
}

void EmulatorApp::updateWindowTitle() {
    if (!m_window) return;
    if (!m_core.hasRomLoaded()) {
        SDL_SetWindowTitle(m_window, "Ry\xC5\xAB" "DS / Ry\xC5\xABGBA");
        return;
    }
    const std::string brand = m_core.isDualScreen() ? "Ry\xC5\xAB" "DS" : "Ry\xC5\xABGBA";
    const std::string title = m_core.romTitle();
    std::string fullTitle = brand + " \xE2\x80\x94 " +
                            (title.empty() ? m_core.romPath().filename().string() : title);
    if (m_core.isLidClosed()) {
        fullTitle += " [Lid Closed]";
    }
    if (m_paused) {
        fullTitle += " [Paused]";
    }
    SDL_SetWindowTitle(m_window, fullTitle.c_str());
}

void EmulatorApp::computeTargetWindowSize(int& outW, int& outH) const noexcept {
    const auto& v = m_cfgMgr.data().video;
    const int scale = std::clamp(v.scale, 1, 4);

    int maxDesktopW = 1600;
    int maxDesktopH = 920;
    SDL_DisplayMode dm{};
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        maxDesktopW = std::max(640, dm.w - 64);
        maxDesktopH = std::max(480, dm.h - 88);
    }

    if (m_core.hasRomLoaded() && m_core.isDualScreen()) {
        int baseW = NDS_WIDTH;
        int baseH = NDS_HEIGHT * 2 + NDS_DIVIDER_PX;
        if (v.ndsLayout == config::NdsScreenLayout::Horizontal) {
            baseW = NDS_WIDTH * 2 + NDS_DIVIDER_PX;
            baseH = NDS_HEIGHT;
        } else if (v.ndsLayout == config::NdsScreenLayout::SingleFocus) {
            baseW = NDS_WIDTH;
            baseH = NDS_HEIGHT;
        }

        int contentW = baseW * scale;
        int contentH = baseH * scale;

        if (contentH + ui::DragonPalette::MENUBAR_HEIGHT > maxDesktopH) {
            contentH = maxDesktopH - ui::DragonPalette::MENUBAR_HEIGHT;
            contentW = (contentH * baseW) / std::max(1, baseH);
        }
        if (contentW > maxDesktopW) {
            contentW = maxDesktopW;
            contentH = (contentW * baseH) / std::max(1, baseW);
        }

        outW = std::max(384, contentW);
        outH = std::max(NDS_HEIGHT + ui::DragonPalette::MENUBAR_HEIGHT,
                        contentH + ui::DragonPalette::MENUBAR_HEIGHT);
        return;
    }

    outW = std::min(maxDesktopW, std::max(380, GBA_WIDTH * scale));
    outH = std::min(maxDesktopH, GBA_HEIGHT * scale + ui::DragonPalette::MENUBAR_HEIGHT);
}

void EmulatorApp::applyVideoSettings(bool resizeWindow) {
    const auto& v = m_cfgMgr.data().video;
    const SDL_ScaleMode scaleMode = v.bilinearFilter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
    if (m_gameTexture)      SDL_SetTextureScaleMode(m_gameTexture, scaleMode);
    if (m_ndsTopTexture)    SDL_SetTextureScaleMode(m_ndsTopTexture, scaleMode);
    if (m_ndsBottomTexture) SDL_SetTextureScaleMode(m_ndsBottomTexture, scaleMode);

    if (resizeWindow && m_window && !m_modals.isOpen()) {
        int targetW = 720, targetH = 506;
        computeTargetWindowSize(targetW, targetH);
        SDL_SetWindowSize(m_window, targetW, targetH);
        m_windowExpandedForModal = false;
    }
}

void EmulatorApp::ensureModalWindowSize() {
    if (!m_window) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    constexpr int MIN_MODAL_W = 580;
    constexpr int MIN_MODAL_H = 516;
    if (w < MIN_MODAL_W || h < MIN_MODAL_H) {
        SDL_SetWindowSize(m_window, std::max(w, MIN_MODAL_W), std::max(h, MIN_MODAL_H));
        m_windowExpandedForModal = true;
    }
}

void EmulatorApp::restorePostModalWindowSize() {
    if (m_windowExpandedForModal && !m_modals.isOpen() && m_window) {
        int targetW = 720, targetH = 506;
        computeTargetWindowSize(targetW, targetH);
        SDL_SetWindowSize(m_window, targetW, targetH);
        m_windowExpandedForModal = false;
    }
}

SDL_Rect EmulatorApp::computeGameViewport(int winW, int winH) const noexcept {
    const int availW = std::max(1, winW);
    const int availH = std::max(1, winH - ui::DragonPalette::MENUBAR_HEIGHT);
    const auto mode  = m_cfgMgr.data().video.aspectRatio;

    if (mode == config::AspectRatioMode::Stretch) {
        return SDL_Rect{0, ui::DragonPalette::MENUBAR_HEIGHT, availW, availH};
    }

    const double targetAspect = (mode == config::AspectRatioMode::Ratio4x3)
        ? (4.0 / 3.0)
        : (3.0 / 2.0); // 240:160 nativo GBA

    int dstW = availW;
    int dstH = static_cast<int>(static_cast<double>(dstW) / targetAspect + 0.5);
    if (dstH > availH) {
        dstH = availH;
        dstW = static_cast<int>(static_cast<double>(dstH) * targetAspect + 0.5);
    }

    const int dstX = (availW - dstW) / 2;
    const int dstY = ui::DragonPalette::MENUBAR_HEIGHT + (availH - dstH) / 2;
    return SDL_Rect{dstX, dstY, dstW, dstH};
}

EmulatorApp::NdsViewports EmulatorApp::computeNdsViewports(int winW, int winH) const noexcept {
    NdsViewports vp{};
    const int topBar = ui::DragonPalette::MENUBAR_HEIGHT;
    const int availW = std::max(2, winW);
    const int availH = std::max(2, winH - topBar);
    const auto& v    = m_cfgMgr.data().video;
    const bool stretch = (v.aspectRatio == config::AspectRatioMode::Stretch);

    SDL_Rect primary{};
    SDL_Rect secondary{};

    if (v.ndsLayout == config::NdsScreenLayout::Vertical) {
        vp.hasDivider = true;
        if (stretch) {
            const int slotH = std::max(1, (availH - NDS_DIVIDER_PX) / 2);
            primary         = SDL_Rect{0, topBar, availW, slotH};
            vp.dividerRect  = SDL_Rect{0, topBar + slotH, availW, NDS_DIVIDER_PX};
            secondary       = SDL_Rect{0, topBar + slotH + NDS_DIVIDER_PX, availW,
                                       std::max(1, availH - slotH - NDS_DIVIDER_PX)};
        } else {
            int scrH = std::max(1, (availH - NDS_DIVIDER_PX) / 2);
            int scrW = (scrH * 4) / 3;
            if (scrW > availW) {
                scrW = availW;
                scrH = std::max(1, (scrW * 3) / 4);
            }
            const int totalH = scrH * 2 + NDS_DIVIDER_PX;
            const int startX = (availW - scrW) / 2;
            const int startY = topBar + (availH - totalH) / 2;
            primary         = SDL_Rect{startX, startY, scrW, scrH};
            vp.dividerRect  = SDL_Rect{startX, startY + scrH, scrW, NDS_DIVIDER_PX};
            secondary       = SDL_Rect{startX, startY + scrH + NDS_DIVIDER_PX, scrW, scrH};
        }
    } else if (v.ndsLayout == config::NdsScreenLayout::Horizontal) {
        vp.hasDivider = true;
        if (stretch) {
            const int slotW = std::max(1, (availW - NDS_DIVIDER_PX) / 2);
            primary         = SDL_Rect{0, topBar, slotW, availH};
            vp.dividerRect  = SDL_Rect{slotW, topBar, NDS_DIVIDER_PX, availH};
            secondary       = SDL_Rect{slotW + NDS_DIVIDER_PX, topBar,
                                       std::max(1, availW - slotW - NDS_DIVIDER_PX), availH};
        } else {
            int scrW = std::max(1, (availW - NDS_DIVIDER_PX) / 2);
            int scrH = (scrW * 3) / 4;
            if (scrH > availH) {
                scrH = availH;
                scrW = std::max(1, (scrH * 4) / 3);
            }
            const int totalW = scrW * 2 + NDS_DIVIDER_PX;
            const int startX = (availW - totalW) / 2;
            const int startY = topBar + (availH - scrH) / 2;
            primary         = SDL_Rect{startX, startY, scrW, scrH};
            vp.dividerRect  = SDL_Rect{startX + scrW, startY, NDS_DIVIDER_PX, scrH};
            secondary       = SDL_Rect{startX + scrW + NDS_DIVIDER_PX, startY, scrW, scrH};
        }
    } else {
        // Foco Único (Single Focus) + miniatura PiP clicável da tela secundária
        vp.hasDivider = false;
        if (stretch) {
            primary = SDL_Rect{0, topBar, availW, availH};
        } else {
            int scrW = availW;
            int scrH = (scrW * 3) / 4;
            if (scrH > availH) {
                scrH = availH;
                scrW = std::max(1, (scrH * 4) / 3);
            }
            primary = SDL_Rect{(availW - scrW) / 2, topBar + (availH - scrH) / 2, scrW, scrH};
        }

        const int pipW = std::clamp((primary.w * 28) / 100, 96, 224);
        const int pipH = (pipW * 3) / 4;
        secondary = SDL_Rect{
            std::max(8, winW - pipW - 10),
            std::max(topBar + 8, winH - pipH - 10),
            pipW,
            pipH
        };
    }

    if (!v.swapScreens) {
        vp.topRect     = primary;
        vp.bottomRect  = secondary;
        vp.bottomIsPiP = (v.ndsLayout == config::NdsScreenLayout::SingleFocus);
    } else {
        vp.bottomRect = primary;
        vp.topRect    = secondary;
        vp.topIsPiP   = (v.ndsLayout == config::NdsScreenLayout::SingleFocus);
    }

    return vp;
}

bool EmulatorApp::mapWindowToStylus(
    int mx, int my, const SDL_Rect& bottomRect, int& outDsX, int& outDsY, bool clampOutside
) const noexcept {
    if (bottomRect.w <= 0 || bottomRect.h <= 0) return false;

    const bool inside = (mx >= bottomRect.x && mx < bottomRect.x + bottomRect.w &&
                         my >= bottomRect.y && my < bottomRect.y + bottomRect.h);
    if (!inside && !clampOutside) {
        return false;
    }

    const int relX = std::clamp(mx - bottomRect.x, 0, bottomRect.w - 1);
    const int relY = std::clamp(my - bottomRect.y, 0, bottomRect.h - 1);
    outDsX = std::clamp((relX * NDS_WIDTH) / bottomRect.w, 0, NDS_WIDTH - 1);
    outDsY = std::clamp((relY * NDS_HEIGHT) / bottomRect.h, 0, NDS_HEIGHT - 1);
    return true;
}

void EmulatorApp::applyAudioVolume() noexcept {
    const auto& a = m_cfgMgr.data().audio;
    m_core.setVolume(a.masterVolume);
    m_core.setMuted(a.muted);
}

void EmulatorApp::reopenAudioDevice() {
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) return;

    if (m_audioDevice != 0) {
        SDL_CloseAudioDevice(m_audioDevice);
        m_audioDevice = 0;
    }

    const auto& a = m_cfgMgr.data().audio;
    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};
    desired.freq     = std::clamp(a.sampleRate, 32768, 48000);
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = static_cast<Uint16>(std::clamp(a.bufferSamples, 512, 4096));
    desired.callback = &EmulatorApp::sdlAudioCallback;
    desired.userdata = this;

    m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (m_audioDevice != 0) {
        m_core.setAudioSampleRate(static_cast<uint32_t>(obtained.freq > 0 ? obtained.freq : desired.freq));
        SDL_PauseAudioDevice(m_audioDevice, 0);
    } else {
        m_core.setAudioSampleRate(static_cast<uint32_t>(desired.freq));
    }
}

void EmulatorApp::sdlAudioCallback(void* userdata, Uint8* stream, int len) noexcept {
    auto* app = static_cast<EmulatorApp*>(userdata);
    auto* out = reinterpret_cast<int16_t*>(stream);
    const std::size_t totalSamples = static_cast<std::size_t>(len) / sizeof(int16_t);

    if (!app->m_core.hasRomLoaded() || app->m_paused || app->m_modals.isOpen() || app->m_core.isLidClosed()) {
        std::fill_n(out, totalSamples, int16_t{0});
        app->m_lastAudioL = 0;
        app->m_lastAudioR = 0;
        return;
    }

    const std::size_t pulled = app->m_core.pullAudioSamples(std::span<int16_t>(out, totalSamples));
    if (pulled >= 2) {
        app->m_lastAudioL = out[pulled - 2];
        app->m_lastAudioR = out[pulled - 1];
    }

    for (std::size_t i = pulled; i + 1 < totalSamples; i += 2) {
        app->m_lastAudioL = static_cast<int16_t>((app->m_lastAudioL * 15) / 16);
        app->m_lastAudioR = static_cast<int16_t>((app->m_lastAudioR * 15) / 16);
        out[i]     = app->m_lastAudioL;
        out[i + 1] = app->m_lastAudioR;
    }
}

void EmulatorApp::showOsd(std::string msg, uint32_t durationMs, bool withBanner) {
    const uint32_t now       = SDL_GetTicks();
    m_osdMessage             = std::move(msg);
    m_osdExpireTicks         = now + durationMs;
    m_osdBannerExpireTicks   = withBanner ? (now + durationMs) : 0;
}

void EmulatorApp::onHotkeyTriggered(config::HotkeyAction hk) {
    switch (hk) {
        case config::HotkeyAction::FastForward:
            if (m_cfgMgr.data().fastForwardMode == config::FastForwardMode::Toggle) {
                m_fastForward = !m_fastForward;
                showOsd(m_fastForward ? "Fast Forward: ON" : "Fast Forward: OFF");
            }
            break;
        case config::HotkeyAction::QuickSave:
            if (m_core.hasRomLoaded()) {
                if (m_core.saveStateSlot(1)) showOsd("State Saved -> Slot 1");
                else showOsd("Save State Failed!");
            }
            break;
        case config::HotkeyAction::QuickLoad:
            if (m_core.hasRomLoaded()) {
                if (m_core.loadStateSlot(1)) showOsd("State Loaded <- Slot 1");
                else showOsd("Slot 1 is Empty!");
            }
            break;
        case config::HotkeyAction::Pause:
            if (m_core.hasRomLoaded()) {
                m_paused = !m_paused;
                releaseFrontendInputs();
                updateWindowTitle();
                showOsd(m_paused ? "Emulation Paused" : "Emulation Resumed");
            }
            break;
        case config::HotkeyAction::MicBlow:
            if (m_core.isDualScreen()) {
                showOsd("DS Microphone: Blowing (White Noise)", 900);
            }
            break;
        case config::HotkeyAction::SwapScreens:
            m_cfgMgr.data().video.swapScreens = !m_cfgMgr.data().video.swapScreens;
            m_cfgMgr.save();
            showOsd(m_cfgMgr.data().video.swapScreens
                        ? "DS Screens: Swapped (Bottom Primary)"
                        : "DS Screens: Normal (Top Primary)");
            break;
        case config::HotkeyAction::ToggleLid:
            if (m_core.hasRomLoaded() && m_core.isDualScreen()) {
                const bool nextClosed = !m_core.isLidClosed();
                releaseFrontendInputs();
                m_core.setLidClosed(nextClosed);
                updateWindowTitle();
                showOsd(nextClosed ? "DS Lid Closed (Sleep Mode)" : "DS Lid Opened");
            }
            break;
        default:
            break;
    }
}

void EmulatorApp::executeMenuCommand(const ui::MenuCommand& cmd) {
    switch (cmd.type) {
        case ui::MenuActionType::OpenRomDialog:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::OpenRom, m_defaultRomsDir);
            break;
        case ui::MenuActionType::OpenRecentPath:
            loadRomFile(cmd.strParam);
            break;
        case ui::MenuActionType::ClearRecentRoms:
            m_cfgMgr.clearRecentRoms();
            showOsd("Cleared Recent ROMs");
            break;
        case ui::MenuActionType::LaunchBuiltInNdsDemo:
            launchBuiltInNdsDemo();
            break;
        case ui::MenuActionType::TogglePause:
            if (m_core.hasRomLoaded()) {
                m_paused = !m_paused;
                releaseFrontendInputs();
                updateWindowTitle();
                showOsd(m_paused ? "Emulation Paused" : "Emulation Resumed");
            }
            break;
        case ui::MenuActionType::ResetEmulation:
            if (m_core.hasRomLoaded()) {
                releaseFrontendInputs();
                m_core.reset();
                m_paused = false;
                updateWindowTitle();
                showOsd("System Reset");
            }
            break;
        case ui::MenuActionType::ExitApp:
            m_running = false;
            break;
        case ui::MenuActionType::SetSpeedPercent:
            m_cfgMgr.data().speedPercent = cmd.intParam;
            m_cfgMgr.save();
            if (cmd.intParam == 0) showOsd("Speed: Uncapped");
            else showOsd("Speed: " + std::to_string(cmd.intParam) + "%");
            break;
        case ui::MenuActionType::ToggleFastForward:
            m_fastForward = !m_fastForward;
            showOsd(m_fastForward ? "Fast Forward: ON" : "Fast Forward: OFF");
            break;
        case ui::MenuActionType::SetFastForwardSpeed:
            m_cfgMgr.data().fastForwardSpeed = static_cast<config::FastForwardSpeed>(cmd.intParam);
            m_cfgMgr.save();
            if (cmd.intParam == 0) showOsd("Fast Forward Speed: Uncapped");
            else showOsd("Fast Forward Speed: " + std::to_string(cmd.intParam) + "x");
            break;
        case ui::MenuActionType::SetFastForwardMode:
            m_cfgMgr.data().fastForwardMode = static_cast<config::FastForwardMode>(cmd.intParam);
            m_cfgMgr.save();
            showOsd(cmd.intParam == 0 ? "Fast Forward Mode: Hold (Tab)" : "Fast Forward Mode: Toggle (Tab)");
            break;
        case ui::MenuActionType::SetNdsLayout:
            m_cfgMgr.data().video.ndsLayout = static_cast<config::NdsScreenLayout>(std::clamp(cmd.intParam, 0, 2));
            m_cfgMgr.save();
            applyVideoSettings(true);
            switch (m_cfgMgr.data().video.ndsLayout) {
                case config::NdsScreenLayout::Vertical:
                    showOsd("DS Layout: Vertical (Standard)");
                    break;
                case config::NdsScreenLayout::Horizontal:
                    showOsd("DS Layout: Horizontal (Side-by-Side)");
                    break;
                case config::NdsScreenLayout::SingleFocus:
                    showOsd("DS Layout: Single Focus + PiP");
                    break;
            }
            break;
        case ui::MenuActionType::ToggleSwapScreens:
            m_cfgMgr.data().video.swapScreens = !m_cfgMgr.data().video.swapScreens;
            m_cfgMgr.save();
            showOsd(m_cfgMgr.data().video.swapScreens
                        ? "DS Screens: Swapped (Bottom Primary)"
                        : "DS Screens: Normal (Top Primary)");
            break;
        case ui::MenuActionType::ToggleLidSleep:
            if (m_core.hasRomLoaded() && m_core.isDualScreen()) {
                const bool nextClosed = !m_core.isLidClosed();
                releaseFrontendInputs();
                m_core.setLidClosed(nextClosed);
                updateWindowTitle();
                showOsd(nextClosed ? "DS Lid Closed (Sleep Mode)" : "DS Lid Opened");
            }
            break;
        case ui::MenuActionType::ToggleDirectBootHle:
            m_cfgMgr.data().ndsBoot.directBootHle = !m_cfgMgr.data().ndsBoot.directBootHle;
            m_cfgMgr.save();
            syncNdsBootConfig();
            showOsd(m_cfgMgr.data().ndsBoot.directBootHle
                        ? "NDS Boot: Direct Boot / HLE Enabled"
                        : "NDS Boot: External BIOS/Firmware Mode");
            break;
        case ui::MenuActionType::SaveStateSlot:
            if (m_core.hasRomLoaded()) {
                if (m_core.saveStateSlot(cmd.intParam)) {
                    showOsd("Saved State -> Slot " + std::to_string(cmd.intParam));
                } else {
                    showOsd("Save State Failed!");
                }
            }
            break;
        case ui::MenuActionType::LoadStateSlot:
            if (m_core.hasRomLoaded()) {
                if (m_core.loadStateSlot(cmd.intParam)) {
                    showOsd("Loaded State <- Slot " + std::to_string(cmd.intParam));
                } else {
                    showOsd("Slot " + std::to_string(cmd.intParam) + " is Empty!");
                }
            }
            break;
        case ui::MenuActionType::OpenInputConfig:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::InputAndHotkeys);
            break;
        case ui::MenuActionType::OpenVideoConfig:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::VideoSettings);
            break;
        case ui::MenuActionType::OpenAudioConfig:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::AudioSettings);
            break;
        case ui::MenuActionType::OpenAboutDialog:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::About);
            break;
        case ui::MenuActionType::OpenGitHubLicenseDialog:
            releaseFrontendInputs();
            ensureModalWindowSize();
            m_modals.open(ui::ModalKind::GitHubLicense);
            break;
        default:
            break;
    }
}

void EmulatorApp::renderNdsScreens(int winW, int winH, int mouseX, int mouseY) {
    const NdsViewports vp = computeNdsViewports(winW, winH);

    auto drawPiPFrame = [&](const SDL_Rect& rect, bool isTouchscreen) {
        const SDL_Rect shadow{rect.x - 2, rect.y - 2, rect.w + 4, rect.h + 4};
        ui::fillRect(m_renderer, shadow, SDL_Color{0x12, 0x13, 0x16, 235});
        ui::strokeRect(m_renderer, shadow,
                       isTouchscreen ? ui::DragonPalette::DRAGON_RED : ui::DragonPalette::EMERALD_SCALE);
    };

    // Desenha primeiro a tela principal e por cima a miniatura PiP (caso SingleFocus)
    if (vp.bottomIsPiP) {
        SDL_RenderCopy(m_renderer, m_ndsTopTexture, nullptr, &vp.topRect);
        drawPiPFrame(vp.bottomRect, true);
        SDL_RenderCopy(m_renderer, m_ndsBottomTexture, nullptr, &vp.bottomRect);
    } else if (vp.topIsPiP) {
        SDL_RenderCopy(m_renderer, m_ndsBottomTexture, nullptr, &vp.bottomRect);
        drawPiPFrame(vp.topRect, false);
        SDL_RenderCopy(m_renderer, m_ndsTopTexture, nullptr, &vp.topRect);
    } else {
        SDL_RenderCopy(m_renderer, m_ndsTopTexture, nullptr, &vp.topRect);
        SDL_RenderCopy(m_renderer, m_ndsBottomTexture, nullptr, &vp.bottomRect);
    }

    // Divisória temática Dragon entre as duas telas (Vertical ou Horizontal)
    if (vp.hasDivider) {
        ui::fillRect(m_renderer, vp.dividerRect, ui::DragonPalette::SURFACE_SCALES);
        if (vp.dividerRect.w >= vp.dividerRect.h) {
            const int accentW = std::max(24, vp.dividerRect.w / 3);
            const SDL_Rect accent{
                vp.dividerRect.x + (vp.dividerRect.w - accentW) / 2,
                vp.dividerRect.y + 1,
                accentW,
                std::max(1, vp.dividerRect.h - 2)
            };
            ui::fillRect(m_renderer, accent,
                         m_stylusTouching ? ui::DragonPalette::EMERALD_SCALE : ui::DragonPalette::DRAGON_RED);
        } else {
            const int accentH = std::max(24, vp.dividerRect.h / 3);
            const SDL_Rect accent{
                vp.dividerRect.x + 1,
                vp.dividerRect.y + (vp.dividerRect.h - accentH) / 2,
                std::max(1, vp.dividerRect.w - 2),
                accentH
            };
            ui::fillRect(m_renderer, accent,
                         m_stylusTouching ? ui::DragonPalette::EMERALD_SCALE : ui::DragonPalette::DRAGON_RED);
        }
    }

    renderStylusCursor(vp.bottomRect, mouseX, mouseY);
}

void EmulatorApp::renderStylusCursor(const SDL_Rect& bottomRect, int mouseX, int mouseY) {
    if (!m_cfgMgr.data().video.showStylusCursor || m_menuBar.isOpen() || m_modals.isOpen()) {
        return;
    }

    const bool hovered = (mouseX >= bottomRect.x && mouseX < bottomRect.x + bottomRect.w &&
                          mouseY >= bottomRect.y && mouseY < bottomRect.y + bottomRect.h);
    if (!m_stylusTouching && !hovered) {
        return;
    }

    const int cx = std::clamp(mouseX, bottomRect.x, std::max(bottomRect.x, bottomRect.x + bottomRect.w - 1));
    const int cy = std::clamp(mouseY, bottomRect.y, std::max(bottomRect.y, bottomRect.y + bottomRect.h - 1));

    const SDL_Color outerColor = m_stylusTouching
        ? ui::DragonPalette::DRAGON_RED
        : SDL_Color{0x10, 0xB9, 0x81, 185};
    const SDL_Color innerColor = m_stylusTouching
        ? ui::DragonPalette::EMERALD_SCALE
        : ui::DragonPalette::TEXT_PRIMARY;

    // Retículo geométrico Dragon Stylus (cruz + núcleo luminoso)
    ui::fillRect(m_renderer, SDL_Rect{cx - 6, cy, 4, 1}, outerColor);
    ui::fillRect(m_renderer, SDL_Rect{cx + 3, cy, 4, 1}, outerColor);
    ui::fillRect(m_renderer, SDL_Rect{cx, cy - 6, 1, 4}, outerColor);
    ui::fillRect(m_renderer, SDL_Rect{cx, cy + 3, 1, 4}, outerColor);

    if (m_stylusTouching) {
        ui::strokeRect(m_renderer, SDL_Rect{cx - 4, cy - 4, 9, 9}, outerColor);
        ui::fillRect(m_renderer, SDL_Rect{cx - 1, cy - 1, 3, 3}, innerColor);
    } else {
        ui::fillRect(m_renderer, SDL_Rect{cx, cy, 1, 1}, innerColor);
    }
}

void EmulatorApp::renderIdleScreen(int winW, int winH, int mouseX, int mouseY) {
    const int contentTop = ui::DragonPalette::MENUBAR_HEIGHT;
    const int availH     = std::max(1, winH - contentTop);
    const int centerX    = winW / 2;

    const bool compact = (availH < 270);
    const int logoSize = compact ? 72 : 124;
    const int logoY    = contentTop + std::max(8, (availH - (logoSize + (compact ? 74 : 150))) / 2);

    if (m_logoTexture) {
        const SDL_Rect dst{centerX - logoSize / 2, logoY, logoSize, logoSize};
        SDL_RenderCopy(m_renderer, m_logoTexture, nullptr, &dst);
    }

    const int titleScale = compact ? 1 : 2;
    const std::string_view brand = "Ry\xC5\xAB" "DS / Ry\xC5\xABGBA";
    const int brandW = ui::DragonFont::measureWidth(brand, titleScale);
    const int brandY = logoY + logoSize + (compact ? 6 : 10);
    ui::DragonFont::drawText(m_renderer, brand, centerX - brandW / 2, brandY, titleScale, ui::DragonPalette::TEXT_PRIMARY);

    if (!compact) {
        constexpr std::string_view sub = "File -> Open ROM... (Ctrl+O) or Drag & Drop .nds / .gba";
        const int subW = ui::DragonFont::measureWidth(sub, 1);
        ui::DragonFont::drawText(m_renderer, sub, centerX - subW / 2, brandY + 24, 1, ui::DragonPalette::TEXT_SECONDARY);
    }

    const int btnY = brandY + (compact ? 18 : 44);
    if (!m_cfgMgr.data().recentRoms.empty()) {
        const std::filesystem::path recentPath(m_cfgMgr.data().recentRoms.front());
        std::string ext = recentPath.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string tag = (ext == ".nds" || ext == ".dsi" || ext == ".srl") ? "[NDS] " : "[GBA] ";

        const std::string recentName = shortenFilename(recentPath.stem().string(), compact ? 20 : 28);
        const std::string launchLbl  = "Play: " + tag + recentName;
        const int launchW = std::min(winW - 24, std::max(190, ui::DragonFont::measureWidth(launchLbl, 1) + 28));
        const SDL_Rect launchBtn{centerX - launchW / 2, btnY, launchW, 26};
        ui::drawButton(m_renderer, launchBtn, launchLbl, ui::HitRect{launchBtn}.contains(mouseX, mouseY), true);

        if (!compact) {
            const SDL_Rect openBtn{centerX - 156, btnY + 34, 148, 24};
            ui::drawButton(m_renderer, openBtn, "Open ROM...", ui::HitRect{openBtn}.contains(mouseX, mouseY), false);

            const SDL_Rect demoBtn{centerX + 4, btnY + 34, 152, 24};
            ui::drawButton(m_renderer, demoBtn, "Ry\xC5\xAB" "DS 3D Demo", ui::HitRect{demoBtn}.contains(mouseX, mouseY), false);
        }
    } else {
        const SDL_Rect openBtn{centerX - 156, btnY, 148, 26};
        ui::drawButton(m_renderer, openBtn, "Open ROM...", ui::HitRect{openBtn}.contains(mouseX, mouseY), true);

        const SDL_Rect demoBtn{centerX + 4, btnY, 152, 26};
        ui::drawButton(m_renderer, demoBtn, "Ry\xC5\xAB" "DS 3D Demo", ui::HitRect{demoBtn}.contains(mouseX, mouseY), false);
    }
}

void EmulatorApp::renderOsd(int /*winW*/, int winH) {
    if (m_osdMessage.empty()) return;
    const uint32_t now = SDL_GetTicks();
    if (now >= m_osdExpireTicks) {
        m_osdMessage.clear();
        return;
    }

    const bool showBanner = (now < m_osdBannerExpireTicks) && m_core.hasBannerIcon() && (m_bannerIconTexture != nullptr);
    const int tw    = ui::DragonFont::measureWidth(m_osdMessage, 1);
    const int iconW = showBanner ? 38 : 0;
    const int pillH = showBanner ? 40 : 24;
    const SDL_Rect pill{12, winH - pillH - 12, tw + 22 + iconW, pillH};

    ui::fillRect(m_renderer, pill, SDL_Color{0x1A, 0x1C, 0x23, 236});
    ui::strokeRect(m_renderer, pill, ui::DragonPalette::EMERALD_SCALE);
    ui::fillRect(m_renderer, SDL_Rect{pill.x, pill.y, 3, pill.h}, ui::DragonPalette::DRAGON_RED);

    int textX = pill.x + 12;
    if (showBanner) {
        const SDL_Rect iconDst{pill.x + 8, pill.y + 4, 32, 32};
        ui::fillRect(m_renderer, SDL_Rect{iconDst.x - 1, iconDst.y - 1, 34, 34}, ui::DragonPalette::BG_OBSIDIAN);
        SDL_RenderCopy(m_renderer, m_bannerIconTexture, nullptr, &iconDst);
        ui::strokeRect(m_renderer, SDL_Rect{iconDst.x - 1, iconDst.y - 1, 34, 34}, ui::DragonPalette::BORDER_SUBTLE);
        textX = iconDst.x + 38;
    }

    ui::DragonFont::drawText(
        m_renderer, m_osdMessage, textX, pill.y + (pill.h - 8) / 2, 1, ui::DragonPalette::TEXT_PRIMARY
    );
}

void EmulatorApp::renderFastForwardBadge(int winW, int /*winH*/) {
    if (!m_fastForward || !m_core.hasRomLoaded()) return;

    const auto& cfg = m_cfgMgr.data();
    std::string badgeText = "FF";
    switch (cfg.fastForwardSpeed) {
        case config::FastForwardSpeed::Speed2x:  badgeText = "FF 2x"; break;
        case config::FastForwardSpeed::Speed4x:  badgeText = "FF 4x"; break;
        case config::FastForwardSpeed::Speed8x:  badgeText = "FF 8x"; break;
        case config::FastForwardSpeed::Uncapped: badgeText = "FF MAX"; break;
    }

    const int tw = ui::DragonFont::measureWidth(badgeText, 1);
    const int badgeW = tw + 18;
    const int badgeH = 22;
    const SDL_Rect badgeRect{winW - badgeW - 14, 30, badgeW, badgeH};

    ui::fillRect(m_renderer, badgeRect, SDL_Color{0x1A, 0x1C, 0x23, 235});
    ui::strokeRect(m_renderer, badgeRect, ui::DragonPalette::DRAGON_RED);
    ui::fillRect(m_renderer, SDL_Rect{badgeRect.x, badgeRect.y, 3, badgeRect.h}, ui::DragonPalette::DRAGON_RED);
    ui::DragonFont::drawText(
        m_renderer,
        badgeText,
        badgeRect.x + 9,
        badgeRect.y + (badgeH - 8) / 2,
        1,
        SDL_Color{0xFF, 0x7B, 0x7B, 255}
    );
}

int EmulatorApp::run() {
    using Clock = std::chrono::steady_clock;
    auto nextFrameDeadline = Clock::now();
    auto fpsTimerStart     = Clock::now();
    int  emulatedFramesAcc = 0;

    int mouseX = 0;
    int mouseY = 0;

    while (m_running) {
        int winW = 720, winH = 506;
        SDL_GetWindowSize(m_window, &winW, &winH);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                m_running = false;
                break;
            }

            if (ev.type == SDL_MOUSEMOTION) {
                mouseX = ev.motion.x;
                mouseY = ev.motion.y;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                mouseX = ev.button.x;
                mouseY = ev.button.y;
            }

            if (ev.type == SDL_DROPFILE) {
                if (ev.drop.file) {
                    const std::filesystem::path droppedPath(ev.drop.file);
                    SDL_free(ev.drop.file);
                    m_modals.close();
                    m_menuBar.close();
                    restorePostModalWindowSize();
                    loadRomFile(droppedPath);
                }
                continue;
            }

            // 1. Se o InputMapper está em estado "Listening...", captura imediatamente a próxima tecla/botão
            if (m_inputMapper.isListening()) {
                m_inputMapper.processEvent(ev, m_core, false);
                continue;
            }

            // 2. Se um modal de configuração/diálogo está aberto, ele consome eventos de UI
            if (m_modals.isOpen()) {
                if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED ||
                    ev.type == SDL_JOYDEVICEADDED || ev.type == SDL_JOYDEVICEREMOVED) {
                    m_inputMapper.processEvent(ev, m_core, false);
                }

                ui::ModalCallbacks effects{};
                const bool wasOpen = m_modals.isOpen();
                m_modals.handleEvent(ev, winW, winH, effects);

                if (effects.ndsBootDirty) {
                    syncNdsBootConfig();
                }
                if (effects.videoLayoutDirty) {
                    applyVideoSettings(false);
                }
                if (effects.audioVolumeDirty) {
                    applyAudioVolume();
                }
                if (effects.audioDeviceDirty) {
                    reopenAudioDevice();
                }
                if (!effects.romToLoad.empty()) {
                    restorePostModalWindowSize();
                    loadRomFile(effects.romToLoad);
                } else if (wasOpen && !m_modals.isOpen()) {
                    m_windowExpandedForModal = true;
                    restorePostModalWindowSize();
                }
                continue;
            }

            // 3. Barra de menu clássica no topo (melonDS MenuBar)
            std::optional<ui::MenuCommand> menuCmd;
            const bool consumedByMenu = m_menuBar.handleEvent(
                ev, m_cfgMgr.data(), m_core, m_paused, m_fastForward, menuCmd
            );
            if (menuCmd.has_value()) {
                executeMenuCommand(*menuCmd);
                nextFrameDeadline = Clock::now();
                continue;
            }
            if (consumedByMenu) {
                if (m_stylusTouching) {
                    m_stylusTouching = false;
                    m_core.setStylusState(m_stylusDsX, m_stylusDsY, false);
                }
                continue;
            }

            // 4. Interações rápidas da Tela Inicial (Idle Screen) quando nenhuma ROM está carregada
            if (!m_core.hasRomLoaded()) {
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                    ev.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                    !m_cfgMgr.data().recentRoms.empty()) {
                    loadRomFile(m_cfgMgr.data().recentRoms.front());
                    nextFrameDeadline = Clock::now();
                    continue;
                }
                if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    const int contentTop = ui::DragonPalette::MENUBAR_HEIGHT;
                    const int availH     = std::max(1, winH - contentTop);
                    const bool compact   = (availH < 270);
                    const int logoSize   = compact ? 72 : 124;
                    const int logoY      = contentTop + std::max(8, (availH - (logoSize + (compact ? 74 : 150))) / 2);
                    const int brandY     = logoY + logoSize + (compact ? 6 : 10);
                    const int btnY       = brandY + (compact ? 18 : 44);
                    const int centerX    = winW / 2;

                    if (!m_cfgMgr.data().recentRoms.empty()) {
                        const std::filesystem::path recentPath(m_cfgMgr.data().recentRoms.front());
                        std::string ext = recentPath.extension().string();
                        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        const std::string tag = (ext == ".nds" || ext == ".dsi" || ext == ".srl") ? "[NDS] " : "[GBA] ";
                        const std::string recentName = shortenFilename(recentPath.stem().string(), compact ? 20 : 28);
                        const std::string launchLbl  = "Play: " + tag + recentName;
                        const int launchW = std::min(winW - 24, std::max(190, ui::DragonFont::measureWidth(launchLbl, 1) + 28));
                        const ui::HitRect launchHit{SDL_Rect{centerX - launchW / 2, btnY, launchW, 26}};
                        if (launchHit.contains(ev.button.x, ev.button.y)) {
                            loadRomFile(recentPath);
                            nextFrameDeadline = Clock::now();
                            continue;
                        }
                        if (!compact) {
                            const ui::HitRect openHit{SDL_Rect{centerX - 156, btnY + 34, 148, 24}};
                            if (openHit.contains(ev.button.x, ev.button.y)) {
                                executeMenuCommand(ui::MenuCommand{ui::MenuActionType::OpenRomDialog});
                                continue;
                            }
                            const ui::HitRect demoHit{SDL_Rect{centerX + 4, btnY + 34, 152, 24}};
                            if (demoHit.contains(ev.button.x, ev.button.y)) {
                                launchBuiltInNdsDemo();
                                nextFrameDeadline = Clock::now();
                                continue;
                            }
                        }
                    } else {
                        const ui::HitRect openHit{SDL_Rect{centerX - 156, btnY, 148, 26}};
                        if (openHit.contains(ev.button.x, ev.button.y)) {
                            executeMenuCommand(ui::MenuCommand{ui::MenuActionType::OpenRomDialog});
                            continue;
                        }
                        const ui::HitRect demoHit{SDL_Rect{centerX + 4, btnY, 152, 26}};
                        if (demoHit.contains(ev.button.x, ev.button.y)) {
                            launchBuiltInNdsDemo();
                            nextFrameDeadline = Clock::now();
                            continue;
                        }
                    }
                }
            }

            // 5. Touchscreen (Stylus) do Nintendo DS — mapeamento preciso 0..255 x 0..191 em qualquer layout
            const bool allowGameplay = m_core.hasRomLoaded() && !m_paused && !m_menuBar.isOpen();
            if (allowGameplay && m_core.isDualScreen() && !m_core.isLidClosed()) {
                if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    const NdsViewports vp = computeNdsViewports(winW, winH);
                    int dsX = 0, dsY = 0;
                    if (mapWindowToStylus(ev.button.x, ev.button.y, vp.bottomRect, dsX, dsY, false)) {
                        m_stylusTouching = true;
                        m_stylusDsX      = dsX;
                        m_stylusDsY      = dsY;
                        m_core.setStylusState(dsX, dsY, true);
                    }
                } else if (ev.type == SDL_MOUSEMOTION && m_stylusTouching) {
                    const NdsViewports vp = computeNdsViewports(winW, winH);
                    int dsX = 0, dsY = 0;
                    if (mapWindowToStylus(ev.motion.x, ev.motion.y, vp.bottomRect, dsX, dsY, true)) {
                        m_stylusDsX = dsX;
                        m_stylusDsY = dsY;
                        m_core.setStylusState(dsX, dsY, true);
                    }
                } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT && m_stylusTouching) {
                    m_stylusTouching = false;
                    m_core.setStylusState(m_stylusDsX, m_stylusDsY, false);
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT && m_stylusTouching) {
                m_stylusTouching = false;
                m_core.setStylusState(m_stylusDsX, m_stylusDsY, false);
            }

            // 6. Encaminha eventos de gameplay (12 botões DS/GBA) e hotkeys ao InputMapper híbrido
            m_inputMapper.processEvent(ev, m_core, allowGameplay);
        }

        if (!m_running) break;

        const auto& cfg = m_cfgMgr.data();
        if (cfg.fastForwardMode == config::FastForwardMode::Hold) {
            m_fastForward = m_inputMapper.isHotkeyActive(config::HotkeyAction::FastForward);
        }

        // =====================================================================
        // EXECUÇÃO DO CORE HÍBRIDO NDS / GBA (Desacoplado)
        // =====================================================================
        const bool activelyEmulating = m_core.hasRomLoaded() && !m_paused && !m_modals.isOpen();
        if (activelyEmulating) {
            int fastForwardMultiplier = 1;
            if (m_fastForward) {
                switch (cfg.fastForwardSpeed) {
                    case config::FastForwardSpeed::Speed2x:  fastForwardMultiplier = 2; break;
                    case config::FastForwardSpeed::Speed4x:  fastForwardMultiplier = 4; break;
                    case config::FastForwardSpeed::Speed8x:  fastForwardMultiplier = 8; break;
                    case config::FastForwardSpeed::Uncapped: fastForwardMultiplier = 8; break;
                    default: fastForwardMultiplier = 8; break;
                }
            }

            for (int f = 0; f < fastForwardMultiplier; ++f) {
                const bool skipRendering = (f < fastForwardMultiplier - 1);
                m_core.runFrame(skipRendering);
                ++emulatedFramesAcc;
            }

            // Desacoplamento de áudio: drena amostras excedentes durante Fast Forward para eliminar backpressure
            if (m_fastForward) {
                std::array<int16_t, 2048> drain{};
                while (m_core.pullAudioSamples(drain) > 0) {}
            }

            if (m_core.isDualScreen()) {
                const auto topFb = m_core.topFramebufferSpan();
                const auto botFb = m_core.bottomFramebufferSpan();
                if (topFb.size() == static_cast<std::size_t>(NDS_WIDTH * NDS_HEIGHT)) {
                    SDL_UpdateTexture(
                        m_ndsTopTexture, nullptr, topFb.data(), NDS_WIDTH * static_cast<int>(sizeof(uint32_t))
                    );
                }
                if (botFb.size() == static_cast<std::size_t>(NDS_WIDTH * NDS_HEIGHT)) {
                    SDL_UpdateTexture(
                        m_ndsBottomTexture, nullptr, botFb.data(), NDS_WIDTH * static_cast<int>(sizeof(uint32_t))
                    );
                }
            } else {
                const auto fb = m_core.framebuffer();
                SDL_UpdateTexture(
                    m_gameTexture, nullptr, fb.data(), GBA_WIDTH * static_cast<int>(sizeof(uint32_t))
                );
            }
        }

        // Cálculo de FPS real emulado a cada 500ms
        const auto now = Clock::now();
        const double elapsedSec = std::chrono::duration<double>(now - fpsTimerStart).count();
        if (elapsedSec >= 0.5) {
            m_currentFps      = activelyEmulating ? (static_cast<double>(emulatedFramesAcc) / elapsedSec) : 0.0;
            emulatedFramesAcc = 0;
            fpsTimerStart     = now;
        }

        // =====================================================================
        // COMPOSIÇÃO VISUAL (Tema Dragon / Minimalismo melonDS)
        // =====================================================================
        SDL_GetWindowSize(m_window, &winW, &winH);
        SDL_SetRenderDrawColor(
            m_renderer,
            ui::DragonPalette::BG_OBSIDIAN.r,
            ui::DragonPalette::BG_OBSIDIAN.g,
            ui::DragonPalette::BG_OBSIDIAN.b,
            255
        );
        SDL_RenderClear(m_renderer);

        if (m_core.hasRomLoaded()) {
            if (m_core.isDualScreen()) {
                renderNdsScreens(winW, winH, mouseX, mouseY);
            } else {
                const SDL_Rect vp = computeGameViewport(winW, winH);
                SDL_RenderCopy(m_renderer, m_gameTexture, nullptr, &vp);
            }
        } else {
            renderIdleScreen(winW, winH, mouseX, mouseY);
        }

        renderOsd(winW, winH);
        renderFastForwardBadge(winW, winH);

        // Barra de menu superior (e dropdowns por cima do viewport)
        m_menuBar.render(m_renderer, winW, m_cfgMgr.data(), m_core, m_paused, m_fastForward, m_currentFps);

        // Diálogos modais (Input and Hotkeys, Video, Audio, Open ROM, About)
        m_modals.render(m_renderer, winW, winH);

        SDL_RenderPresent(m_renderer);

        // =====================================================================
        // SINCRONIA E LIMITADOR DE FRAMERATE (NDS 59.826 Hz / GBA 59.727 Hz)
        // =====================================================================
        if (!activelyEmulating) {
            SDL_Delay(12);
            nextFrameDeadline = Clock::now();
        } else {
            // Durante Fast Forward, desativa o limitador rígido para rodar na velocidade máxima do hardware
            const bool limitActive = cfg.video.frameLimiter60Fps && !m_fastForward && (cfg.speedPercent > 0);
            if (!limitActive) {
                nextFrameDeadline = Clock::now();
            } else {
                const double baseFps = m_core.isDualScreen() ? NDS_NATIVE_FPS : GBA_NATIVE_FPS;
                const double targetFps = baseFps * (static_cast<double>(cfg.speedPercent) / 100.0);
                const auto frameDuration = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / targetFps)
                );
                nextFrameDeadline += frameDuration;
                const auto afterRender = Clock::now();
                if (nextFrameDeadline > afterRender) {
                    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        nextFrameDeadline - afterRender
                    ).count();
                    if (remainingMs > 2) {
                        SDL_Delay(static_cast<Uint32>(remainingMs - 1));
                    }
                    while (Clock::now() < nextFrameDeadline) {
                        // Espera fina sub-milissegundo
                    }
                } else if (afterRender - nextFrameDeadline > std::chrono::milliseconds(100)) {
                    nextFrameDeadline = afterRender;
                }
            }
        }
    }

    return 0;
}

} // namespace ryugba::frontend
