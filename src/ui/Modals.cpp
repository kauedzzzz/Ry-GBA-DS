#include "ui/Modals.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ryugba::ui {

namespace {

bool isNdsExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".nds" || ext == ".dsi" || ext == ".srl";
}

bool hasSupportedRomExtension(const std::filesystem::path& p) {
    if (isNdsExtension(p)) return true;
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".gba" || ext == ".agb" || ext == ".bin";
}

std::string clipText(std::string_view s, std::size_t maxChars) {
    if (s.size() <= maxChars) return std::string(s);
    if (maxChars <= 3) return std::string(s.substr(0, maxChars));
    return std::string(s.substr(0, maxChars - 3)) + "...";
}

} // namespace

ModalManager::ModalManager(config::ConfigManager& cfgMgr, input::InputMapper& inputMapper)
    : m_cfgMgr(cfgMgr), m_inputMapper(inputMapper) {}

void ModalManager::open(ModalKind kind, const std::filesystem::path& defaultRomDir) {
    m_inputMapper.cancelListening();
    m_draggingVolume = false;
    m_active = kind;
    if (!defaultRomDir.empty()) {
        m_defaultRomDir = defaultRomDir;
    }

    if (kind == ModalKind::OpenRom) {
        std::filesystem::path startDir = m_cfgMgr.data().lastBrowseDirectory;
        std::error_code ec;
        if (startDir.empty() || !std::filesystem::is_directory(startDir, ec)) {
            startDir = !m_defaultRomDir.empty() && std::filesystem::is_directory(m_defaultRomDir, ec)
                ? m_defaultRomDir
                : std::filesystem::current_path(ec);
        }
        refreshDirectory(startDir);
    }
}

void ModalManager::close() {
    m_inputMapper.cancelListening();
    m_draggingVolume = false;
    m_active = ModalKind::None;
}

void ModalManager::refreshDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(dir, ec);
    const std::filesystem::path target = (!ec && std::filesystem::is_directory(canonical, ec)) ? canonical : dir;
    if (!std::filesystem::is_directory(target, ec)) return;

    m_currentDir = target;
    m_cfgMgr.data().lastBrowseDirectory = m_currentDir.string();
    m_cfgMgr.save();

    m_dirEntries.clear();
    m_fileScrollOffset = 0;

    if (m_currentDir.has_parent_path() && m_currentDir.parent_path() != m_currentDir) {
        m_dirEntries.push_back({".. (Parent Directory)", m_currentDir.parent_path(), true, false, 0});
    }

    std::vector<DirEntryItem> dirs;
    std::vector<DirEntryItem> roms;

    for (const auto& entry : std::filesystem::directory_iterator(
             m_currentDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;

        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            dirs.push_back({name, entry.path(), true, false, 0});
        } else if (entry.is_regular_file(entryEc) && hasSupportedRomExtension(entry.path())) {
            roms.push_back({name, entry.path(), false, isNdsExtension(entry.path()), entry.file_size(entryEc)});
        }
    }

    auto byName = [](const DirEntryItem& a, const DirEntryItem& b) {
        return a.displayName < b.displayName;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(roms.begin(), roms.end(), byName);

    for (auto& d : dirs) m_dirEntries.push_back(std::move(d));
    for (auto& f : roms) m_dirEntries.push_back(std::move(f));
}

SDL_Rect ModalManager::modalBox(int winW, int winH) const noexcept {
    int w = 500;
    int h = 340;
    switch (m_active) {
        case ModalKind::InputAndHotkeys: w = 556; h = 468; break;
        case ModalKind::OpenRom:         w = 548; h = 418; break;
        case ModalKind::VideoSettings:   w = 484; h = 408; break;
        case ModalKind::AudioSettings:   w = 468; h = 328; break;
        case ModalKind::About:           w = 516; h = 376; break;
        case ModalKind::GitHubLicense:   w = 508; h = 344; break;
        default: break;
    }
    w = std::min(w, std::max(220, winW - 12));
    h = std::min(h, std::max(180, winH - DragonPalette::MENUBAR_HEIGHT - 12));
    const int x = (winW - w) / 2;
    const int y = DragonPalette::MENUBAR_HEIGHT +
                  std::max(4, (winH - DragonPalette::MENUBAR_HEIGHT - h) / 2);
    return SDL_Rect{x, y, w, h};
}

bool ModalManager::handleEvent(const SDL_Event& ev, int winW, int winH, ModalCallbacks& outEffects) {
    if (m_active == ModalKind::None) return false;

    if (ev.type == SDL_MOUSEMOTION) {
        m_mouseX = ev.motion.x;
        m_mouseY = ev.motion.y;
    }

    if (m_inputMapper.isListening()) {
        return false;
    }

    if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        close();
        return true;
    }

    const SDL_Rect box = modalBox(winW, winH);

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        const HitRect closeX{SDL_Rect{box.x + box.w - 30, box.y + 5, 24, 22}};
        if (closeX.contains(ev.button.x, ev.button.y)) {
            close();
            return true;
        }
    }

    switch (m_active) {
        case ModalKind::OpenRom:
            return handleOpenRomEvent(ev, box, outEffects);
        case ModalKind::InputAndHotkeys:
            return handleInputConfigEvent(ev, box);
        case ModalKind::VideoSettings:
            return handleVideoConfigEvent(ev, box, outEffects);
        case ModalKind::AudioSettings:
            return handleAudioConfigEvent(ev, box, outEffects);
        case ModalKind::About:
        case ModalKind::GitHubLicense: {
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                const HitRect doneBtn{SDL_Rect{box.x + box.w - 104, box.y + box.h - 36, 88, 26}};
                if (doneBtn.contains(ev.button.x, ev.button.y)) {
                    close();
                }
            }
            return true;
        }
        default:
            return true;
    }
}

bool ModalManager::handleOpenRomEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects) {
    constexpr int VISIBLE_ROWS = 11;
    if (ev.type == SDL_MOUSEWHEEL) {
        m_fileScrollOffset -= ev.wheel.y * 2;
        const int maxScroll = std::max(0, static_cast<int>(m_dirEntries.size()) - VISIBLE_ROWS);
        m_fileScrollOffset = std::clamp(m_fileScrollOffset, 0, maxScroll);
        return true;
    }

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        const int mx = ev.button.x;
        const int my = ev.button.y;

        const char* homeEnv = std::getenv("HOME");
        const std::filesystem::path homeDir = homeEnv ? std::filesystem::path(homeEnv) : std::filesystem::current_path();
        const std::array<std::pair<const char*, std::filesystem::path>, 4> quickDirs = {{
            {"ROMs",      !m_defaultRomDir.empty() ? m_defaultRomDir : (homeDir / "Downloads/projetos/roms")},
            {"Project",   homeDir / "Downloads/projetos"},
            {"Downloads", homeDir / "Downloads"},
            {"Home",      homeDir}
        }};

        int qx = box.x + 16;
        for (const auto& [lbl, targetPath] : quickDirs) {
            const int bw = DragonFont::measureWidth(lbl, 1) + 20;
            if (HitRect{SDL_Rect{qx, box.y + 40, bw, 24}}.contains(mx, my)) {
                refreshDirectory(targetPath);
                return true;
            }
            qx += bw + 8;
        }

        const SDL_Rect listArea{box.x + 16, box.y + 94, box.w - 32, VISIBLE_ROWS * 24};
        if (HitRect{listArea}.contains(mx, my)) {
            const int clickedRow = (my - listArea.y) / 24;
            const int idx = m_fileScrollOffset + clickedRow;
            if (idx >= 0 && idx < static_cast<int>(m_dirEntries.size())) {
                const auto chosen = m_dirEntries[idx];
                if (chosen.isDirectory) {
                    refreshDirectory(chosen.fullPath);
                } else {
                    outEffects.romToLoad = chosen.fullPath;
                    close();
                }
            }
            return true;
        }

        if (HitRect{SDL_Rect{box.x + box.w - 104, box.y + box.h - 36, 88, 26}}.contains(mx, my)) {
            close();
            return true;
        }
    }
    return true;
}

bool ModalManager::handleInputConfigEvent(const SDL_Event& ev, const SDL_Rect& box) {
    if (ev.type != SDL_MOUSEBUTTONDOWN) return true;
    const int mx = ev.button.x;
    const int my = ev.button.y;

    if (ev.button.button == SDL_BUTTON_LEFT) {
        if (HitRect{SDL_Rect{box.x + 16, box.y + 54, 192, 24}}.contains(mx, my)) {
            m_inputTab = 0;
            return true;
        }
        if (HitRect{SDL_Rect{box.x + 214, box.y + 54, 198, 24}}.contains(mx, my)) {
            m_inputTab = 1;
            return true;
        }

        if (HitRect{SDL_Rect{box.x + box.w - 236, box.y + box.h - 36, 130, 26}}.contains(mx, my)) {
            m_cfgMgr.resetControlsToDefault();
            return true;
        }
        if (HitRect{SDL_Rect{box.x + box.w - 98, box.y + box.h - 36, 82, 26}}.contains(mx, my)) {
            close();
            return true;
        }
    }

    const bool isHotkeyTab = (m_inputTab == 1);
    const std::size_t rowCount = isHotkeyTab
        ? static_cast<std::size_t>(config::HotkeyAction::Count)
        : static_cast<std::size_t>(config::GbaAction::Count);
    const int rowStride = isHotkeyTab ? 28 : 25;

    for (std::size_t i = 0; i < rowCount; ++i) {
        const int rowY = box.y + 104 + static_cast<int>(i) * rowStride;
        const HitRect kbSlot{SDL_Rect{box.x + 174, rowY, 174, 22}};
        const HitRect joySlot{SDL_Rect{box.x + 356, rowY, 182, 22}};

        if (kbSlot.contains(mx, my)) {
            if (ev.button.button == SDL_BUTTON_LEFT) {
                m_inputMapper.beginListening(isHotkeyTab, i, input::ListenColumn::Keyboard);
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                m_inputMapper.clearSlot(isHotkeyTab, i, input::ListenColumn::Keyboard);
            }
            return true;
        }
        if (joySlot.contains(mx, my)) {
            if (ev.button.button == SDL_BUTTON_LEFT) {
                m_inputMapper.beginListening(isHotkeyTab, i, input::ListenColumn::Joystick);
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                m_inputMapper.clearSlot(isHotkeyTab, i, input::ListenColumn::Joystick);
            }
            return true;
        }
    }

    return true;
}

bool ModalManager::handleVideoConfigEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects) {
    if (ev.type != SDL_MOUSEBUTTONDOWN || ev.button.button != SDL_BUTTON_LEFT) return true;
    const int mx = ev.button.x;
    const int my = ev.button.y;
    auto& v    = m_cfgMgr.data().video;
    auto& boot = m_cfgMgr.data().ndsBoot;

    // 1. Escala 1x - 4x
    for (int s = 1; s <= 4; ++s) {
        const int bx = box.x + 18 + (s - 1) * 112;
        if (HitRect{SDL_Rect{bx, box.y + 60, 104, 26}}.contains(mx, my)) {
            v.scale = s;
            m_cfgMgr.save();
            outEffects.videoLayoutDirty = true;
            return true;
        }
    }

    // 2. Layout Dual-Screen Nintendo DS: Vertical / Horizontal / SingleFocus
    constexpr std::array<config::NdsScreenLayout, 3> layouts = {
        config::NdsScreenLayout::Vertical,
        config::NdsScreenLayout::Horizontal,
        config::NdsScreenLayout::SingleFocus
    };
    for (int i = 0; i < 3; ++i) {
        const int bx = box.x + 18 + i * 152;
        if (HitRect{SDL_Rect{bx, box.y + 116, 144, 26}}.contains(mx, my)) {
            v.ndsLayout = layouts[i];
            m_cfgMgr.save();
            outEffects.videoLayoutDirty = true;
            return true;
        }
    }

    // 3. Aspect Ratio: Native / 4:3 / Stretch
    constexpr std::array<config::AspectRatioMode, 3> modes = {
        config::AspectRatioMode::Native3x2,
        config::AspectRatioMode::Ratio4x3,
        config::AspectRatioMode::Stretch
    };
    for (int i = 0; i < 3; ++i) {
        const int bx = box.x + 18 + i * 152;
        if (HitRect{SDL_Rect{bx, box.y + 172, 144, 26}}.contains(mx, my)) {
            v.aspectRatio = modes[i];
            m_cfgMgr.save();
            outEffects.videoLayoutDirty = true;
            return true;
        }
    }

    // Checkboxes
    if (HitRect{SDL_Rect{box.x + 18, box.y + 214, 420, 20}}.contains(mx, my)) {
        v.swapScreens = !v.swapScreens;
        m_cfgMgr.save();
        outEffects.videoLayoutDirty = true;
        return true;
    }
    if (HitRect{SDL_Rect{box.x + 18, box.y + 240, 420, 20}}.contains(mx, my)) {
        v.showStylusCursor = !v.showStylusCursor;
        m_cfgMgr.save();
        return true;
    }
    if (HitRect{SDL_Rect{box.x + 18, box.y + 266, 420, 20}}.contains(mx, my)) {
        v.bilinearFilter = !v.bilinearFilter;
        m_cfgMgr.save();
        outEffects.videoLayoutDirty = true;
        return true;
    }
    if (HitRect{SDL_Rect{box.x + 18, box.y + 292, 420, 20}}.contains(mx, my)) {
        v.frameLimiter60Fps = !v.frameLimiter60Fps;
        m_cfgMgr.save();
        return true;
    }
    if (HitRect{SDL_Rect{box.x + 18, box.y + 318, 440, 20}}.contains(mx, my)) {
        boot.directBootHle = !boot.directBootHle;
        m_cfgMgr.save();
        outEffects.ndsBootDirty = true;
        return true;
    }

    if (HitRect{SDL_Rect{box.x + box.w - 104, box.y + box.h - 36, 88, 26}}.contains(mx, my)) {
        close();
        return true;
    }
    return true;
}

bool ModalManager::handleAudioConfigEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects) {
    auto& a = m_cfgMgr.data().audio;
    const SDL_Rect sliderRect{box.x + 18, box.y + 68, box.w - 36, 18};

    auto updateVolumeFromX = [&](int mouseX) {
        const float ratio = std::clamp(static_cast<float>(mouseX - sliderRect.x) / static_cast<float>(sliderRect.w), 0.0f, 1.0f);
        a.masterVolume = ratio;
        outEffects.audioVolumeDirty = true;
    };

    if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        if (m_draggingVolume) {
            m_draggingVolume = false;
            m_cfgMgr.save();
        }
        return true;
    }

    if (ev.type == SDL_MOUSEMOTION && m_draggingVolume) {
        updateVolumeFromX(ev.motion.x);
        return true;
    }

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        const int mx = ev.button.x;
        const int my = ev.button.y;

        if (HitRect{sliderRect}.contains(mx, my)) {
            m_draggingVolume = true;
            updateVolumeFromX(mx);
            m_cfgMgr.save();
            return true;
        }

        if (HitRect{SDL_Rect{box.x + 18, box.y + 98, 280, 20}}.contains(mx, my)) {
            a.muted = !a.muted;
            m_cfgMgr.save();
            outEffects.audioVolumeDirty = true;
            return true;
        }

        constexpr std::array<int, 3> buffers = {512, 1024, 2048};
        for (int i = 0; i < 3; ++i) {
            const int bx = box.x + 18 + i * 144;
            if (HitRect{SDL_Rect{bx, box.y + 154, 134, 28}}.contains(mx, my)) {
                if (a.bufferSamples != buffers[i]) {
                    a.bufferSamples = buffers[i];
                    m_cfgMgr.save();
                    outEffects.audioDeviceDirty = true;
                }
                return true;
            }
        }

        constexpr std::array<int, 3> rates = {32768, 44100, 48000};
        for (int i = 0; i < 3; ++i) {
            const int bx = box.x + 18 + i * 144;
            if (HitRect{SDL_Rect{bx, box.y + 220, 134, 28}}.contains(mx, my)) {
                if (a.sampleRate != rates[i]) {
                    a.sampleRate = rates[i];
                    m_cfgMgr.save();
                    outEffects.audioDeviceDirty = true;
                }
                return true;
            }
        }

        if (HitRect{SDL_Rect{box.x + box.w - 104, box.y + box.h - 36, 88, 26}}.contains(mx, my)) {
            close();
            return true;
        }
    }
    return true;
}

void ModalManager::render(SDL_Renderer* r, int winW, int winH) {
    if (m_active == ModalKind::None) return;

    fillRect(r, SDL_Rect{0, DragonPalette::MENUBAR_HEIGHT, winW, winH - DragonPalette::MENUBAR_HEIGHT},
             SDL_Color{0x09, 0x0A, 0x0D, 185});

    const SDL_Rect box = modalBox(winW, winH);
    drawPanel(r, box, DragonPalette::SURFACE_SCALES, DragonPalette::BORDER_SUBTLE);

    fillRect(r, SDL_Rect{box.x + 1, box.y + 1, box.w - 2, 30}, DragonPalette::BG_SLATE_DEEP);
    fillRect(r, SDL_Rect{box.x + 1, box.y + 31, box.w - 2, 2}, DragonPalette::DRAGON_RED);

    std::string_view title = "Dialog";
    switch (m_active) {
        case ModalKind::OpenRom:         title = "Open Nintendo DS / GBA ROM (.nds, .dsi, .srl, .gba)"; break;
        case ModalKind::InputAndHotkeys: title = "Input, DS Sensors and Hotkeys (melonDS Style)"; break;
        case ModalKind::VideoSettings:   title = "Video & Nintendo DS Dual-Screen Layout"; break;
        case ModalKind::AudioSettings:   title = "Audio Settings (16-Ch NDS SPU / GBA APU)"; break;
        case ModalKind::About:           title = "About Ry\xC5\xAB" "DS / Ry\xC5\xABGBA"; break;
        case ModalKind::GitHubLicense:   title = "GitHub / Licen\xC3\xA7" "a"; break;
        default: break;
    }
    DragonFont::drawText(r, title, box.x + 14, box.y + 12, 1, DragonPalette::TEXT_PRIMARY);

    const SDL_Rect closeRect{box.x + box.w - 30, box.y + 5, 24, 22};
    drawButton(r, closeRect, "X", HitRect{closeRect}.contains(m_mouseX, m_mouseY));

    switch (m_active) {
        case ModalKind::OpenRom:         renderOpenRom(r, box); break;
        case ModalKind::InputAndHotkeys: renderInputConfig(r, box); break;
        case ModalKind::VideoSettings:   renderVideoConfig(r, box); break;
        case ModalKind::AudioSettings:   renderAudioConfig(r, box); break;
        case ModalKind::About:           renderAbout(r, box); break;
        case ModalKind::GitHubLicense:   renderGitHubLicense(r, box); break;
        default: break;
    }
}

void ModalManager::renderOpenRom(SDL_Renderer* r, const SDL_Rect& box) {
    constexpr std::array<const char*, 4> quickLabels = {"ROMs", "Project", "Downloads", "Home"};
    int qx = box.x + 16;
    for (const char* lbl : quickLabels) {
        const int bw = DragonFont::measureWidth(lbl, 1) + 20;
        const SDL_Rect btn{qx, box.y + 40, bw, 24};
        drawButton(r, btn, lbl, HitRect{btn}.contains(m_mouseX, m_mouseY));
        qx += bw + 8;
    }

    const std::string dirStr = clipText(m_currentDir.string(), 62);
    DragonFont::drawText(r, dirStr, box.x + 16, box.y + 74, 1, DragonPalette::TEXT_SECONDARY);

    constexpr int VISIBLE_ROWS = 11;
    const SDL_Rect listArea{box.x + 16, box.y + 94, box.w - 32, VISIBLE_ROWS * 24};
    fillRect(r, listArea, DragonPalette::SURFACE_SUNKEN);
    strokeRect(r, listArea, DragonPalette::BORDER_SUBTLE);

    if (m_dirEntries.empty()) {
        DragonFont::drawText(r, "No .nds / .gba ROMs or folders in this directory.",
                             listArea.x + 14, listArea.y + 18, 1, DragonPalette::TEXT_MUTED);
    } else {
        for (int row = 0; row < VISIBLE_ROWS; ++row) {
            const int idx = m_fileScrollOffset + row;
            if (idx >= static_cast<int>(m_dirEntries.size())) break;

            const auto& item = m_dirEntries[idx];
            const SDL_Rect rowRect{listArea.x + 2, listArea.y + row * 24, listArea.w - 4, 24};
            const bool hov = HitRect{rowRect}.contains(m_mouseX, m_mouseY);

            if (hov) {
                fillRect(r, rowRect, DragonPalette::DRAGON_RED_DIM);
            }

            if (item.isDirectory) {
                const std::string label = "[DIR] " + clipText(item.displayName, 50);
                DragonFont::drawText(r, label, rowRect.x + 8, rowRect.y + 8, 1,
                                     hov ? DragonPalette::TEXT_PRIMARY : DragonPalette::EMERALD_SCALE);
            } else {
                const char* tag = item.isNdsRom ? "[NDS] " : "[GBA] ";
                const SDL_Color tagCol = item.isNdsRom ? DragonPalette::DRAGON_RED : DragonPalette::EMERALD_SCALE;
                DragonFont::drawText(r, tag, rowRect.x + 8, rowRect.y + 8, 1,
                                     hov ? DragonPalette::TEXT_PRIMARY : tagCol);

                const std::string label = clipText(item.displayName, 40);
                DragonFont::drawText(r, label, rowRect.x + 48, rowRect.y + 8, 1, DragonPalette::TEXT_PRIMARY);

                char sizeBuf[24];
                const double mb = static_cast<double>(item.fileSize) / (1024.0 * 1024.0);
                if (mb >= 1.0) {
                    std::snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", mb);
                } else {
                    std::snprintf(sizeBuf, sizeof(sizeBuf), "%llu KB",
                                  static_cast<unsigned long long>(item.fileSize / 1024));
                }
                const int sw = DragonFont::measureWidth(sizeBuf, 1);
                DragonFont::drawText(r, sizeBuf, rowRect.x + rowRect.w - sw - 10, rowRect.y + 8, 1,
                                     DragonPalette::TEXT_SECONDARY);
            }
        }
    }

    DragonFont::drawText(r, "Drag & Drop .nds, .dsi, .srl, .gba supported",
                         box.x + 16, box.y + box.h - 27, 1, DragonPalette::TEXT_MUTED);
    const SDL_Rect cancelBtn{box.x + box.w - 104, box.y + box.h - 36, 88, 26};
    drawButton(r, cancelBtn, "Cancel", HitRect{cancelBtn}.contains(m_mouseX, m_mouseY));
}

void ModalManager::renderInputConfig(SDL_Renderer* r, const SDL_Rect& box) {
    const std::string padName = m_inputMapper.activeControllerName();
    const bool hasPad = (padName.find("None") == std::string::npos);
    const std::string padLine = "Active Device: " + clipText(padName, 46);
    DragonFont::drawText(r, padLine, box.x + 16, box.y + 39, 1,
                         hasPad ? DragonPalette::EMERALD_SCALE : DragonPalette::TEXT_SECONDARY);

    const SDL_Rect tabGba{box.x + 16, box.y + 54, 192, 24};
    const SDL_Rect tabHk{box.x + 214, box.y + 54, 198, 24};
    drawButton(r, tabGba, "DS / GBA Controls (12)", HitRect{tabGba}.contains(m_mouseX, m_mouseY), m_inputTab == 0);
    drawButton(r, tabHk,  "Hotkeys & Sensors (7)",  HitRect{tabHk}.contains(m_mouseX, m_mouseY),  m_inputTab == 1);

    DragonFont::drawText(r, "Button / Action",    box.x + 18,  box.y + 86, 1, DragonPalette::TEXT_SECONDARY);
    DragonFont::drawText(r, "Keyboard",           box.x + 176, box.y + 86, 1, DragonPalette::TEXT_SECONDARY);
    DragonFont::drawText(r, "Gamepad / Joystick", box.x + 358, box.y + 86, 1, DragonPalette::TEXT_SECONDARY);
    fillRect(r, SDL_Rect{box.x + 16, box.y + 98, box.w - 32, 1}, DragonPalette::BORDER_SUBTLE);

    const bool isHotkeyTab = (m_inputTab == 1);
    const std::size_t rowCount = isHotkeyTab
        ? static_cast<std::size_t>(config::HotkeyAction::Count)
        : static_cast<std::size_t>(config::GbaAction::Count);
    const int rowStride = isHotkeyTab ? 28 : 25;

    const auto& listen = m_inputMapper.listeningState();

    for (std::size_t i = 0; i < rowCount; ++i) {
        const int rowY = box.y + 104 + static_cast<int>(i) * rowStride;
        const std::string_view rowLabel = isHotkeyTab
            ? config::hotkeyActionLabel(static_cast<config::HotkeyAction>(i))
            : config::gbaActionLabel(static_cast<config::GbaAction>(i));

        const config::DualInputSlot& slot = isHotkeyTab
            ? m_cfgMgr.data().hotkeys[i]
            : m_cfgMgr.data().controls[i];

        const bool isNdsSpecial = (!isHotkeyTab && config::isExtendedNdsAction(static_cast<config::GbaAction>(i))) ||
                                  (isHotkeyTab && i >= static_cast<std::size_t>(config::HotkeyAction::MicBlow));
        DragonFont::drawText(r, rowLabel, box.x + 18, rowY + 7, 1,
                             isNdsSpecial ? DragonPalette::EMERALD_SCALE : DragonPalette::TEXT_PRIMARY);

        const SDL_Rect kbRect{box.x + 174, rowY, 174, 22};
        const bool kbListening = listen.active && listen.isHotkey == isHotkeyTab &&
                                 listen.index == i && listen.column == input::ListenColumn::Keyboard;
        const bool kbHover = HitRect{kbRect}.contains(m_mouseX, m_mouseY);

        if (kbListening) {
            fillRect(r, kbRect, DragonPalette::DRAGON_RED);
            strokeRect(r, kbRect, DragonPalette::TEXT_PRIMARY);
            DragonFont::drawText(r, "Listening... [Key]", kbRect.x + 14, kbRect.y + 7, 1, DragonPalette::TEXT_PRIMARY);
        } else {
            fillRect(r, kbRect, kbHover ? DragonPalette::SURFACE_HOVER : DragonPalette::SURFACE_SUNKEN);
            strokeRect(r, kbRect, kbHover ? DragonPalette::DRAGON_RED : DragonPalette::BORDER_SUBTLE);
            const std::string keyTxt = clipText(input::InputMapper::formatKeyName(slot.keyScancode), 19);
            const SDL_Color col = (slot.keyScancode >= 0) ? DragonPalette::TEXT_PRIMARY : DragonPalette::TEXT_MUTED;
            const int tw = DragonFont::measureWidth(keyTxt, 1);
            DragonFont::drawText(r, keyTxt, kbRect.x + (kbRect.w - tw) / 2, kbRect.y + 7, 1, col);
        }

        const SDL_Rect joyRect{box.x + 356, rowY, 182, 22};
        const bool joyListening = listen.active && listen.isHotkey == isHotkeyTab &&
                                  listen.index == i && listen.column == input::ListenColumn::Joystick;
        const bool joyHover = HitRect{joyRect}.contains(m_mouseX, m_mouseY);

        if (joyListening) {
            fillRect(r, joyRect, DragonPalette::DRAGON_RED);
            strokeRect(r, joyRect, DragonPalette::TEXT_PRIMARY);
            DragonFont::drawText(r, "Listening... [Pad]", joyRect.x + 16, joyRect.y + 7, 1, DragonPalette::TEXT_PRIMARY);
        } else {
            fillRect(r, joyRect, joyHover ? DragonPalette::SURFACE_HOVER : DragonPalette::SURFACE_SUNKEN);
            strokeRect(r, joyRect, joyHover ? DragonPalette::EMERALD_SCALE : DragonPalette::BORDER_SUBTLE);
            const std::string joyTxt = clipText(input::InputMapper::formatJoyName(slot.joyType, slot.joyCode), 20);
            const SDL_Color col = (slot.joyType != config::JoyBindType::None)
                ? DragonPalette::EMERALD_SCALE
                : DragonPalette::TEXT_MUTED;
            const int tw = DragonFont::measureWidth(joyTxt, 1);
            DragonFont::drawText(r, joyTxt, joyRect.x + (joyRect.w - tw) / 2, joyRect.y + 7, 1, col);
        }
    }

    DragonFont::drawText(r, "Mouse L-Drag on DS Bottom Screen = Stylus Touch",
                         box.x + 16, box.y + box.h - 27, 1, DragonPalette::TEXT_MUTED);

    const SDL_Rect resetBtn{box.x + box.w - 236, box.y + box.h - 36, 130, 26};
    const SDL_Rect doneBtn{box.x + box.w - 98, box.y + box.h - 36, 82, 26};
    drawButton(r, resetBtn, "Reset Defaults", HitRect{resetBtn}.contains(m_mouseX, m_mouseY));
    drawButton(r, doneBtn,  "Done",           HitRect{doneBtn}.contains(m_mouseX, m_mouseY), true);
}

void ModalManager::renderVideoConfig(SDL_Renderer* r, const SDL_Rect& box) {
    const auto& v    = m_cfgMgr.data().video;
    const auto& boot = m_cfgMgr.data().ndsBoot;

    DragonFont::drawText(r, "Window Scale (DS 256x192 / GBA 240x160):", box.x + 18, box.y + 44, 1, DragonPalette::TEXT_SECONDARY);
    constexpr std::array<const char*, 4> scaleLabels = {"1x Native", "2x (2x)", "3x (HD)", "4x (Ultra)"};
    for (int s = 1; s <= 4; ++s) {
        const SDL_Rect btn{box.x + 18 + (s - 1) * 112, box.y + 60, 104, 26};
        drawButton(r, btn, scaleLabels[s - 1], HitRect{btn}.contains(m_mouseX, m_mouseY), v.scale == s);
    }

    DragonFont::drawText(r, "Nintendo DS Dual-Screen Layout:", box.x + 18, box.y + 100, 1, DragonPalette::TEXT_SECONDARY);
    constexpr std::array<std::pair<config::NdsScreenLayout, const char*>, 3> layouts = {{
        {config::NdsScreenLayout::Vertical,    "Vertical (DS)"},
        {config::NdsScreenLayout::Horizontal,  "Horizontal"},
        {config::NdsScreenLayout::SingleFocus, "Single + PiP"}
    }};
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect btn{box.x + 18 + i * 152, box.y + 116, 144, 26};
        drawButton(r, btn, layouts[i].second, HitRect{btn}.contains(m_mouseX, m_mouseY), v.ndsLayout == layouts[i].first);
    }

    DragonFont::drawText(r, "Aspect Ratio:", box.x + 18, box.y + 156, 1, DragonPalette::TEXT_SECONDARY);
    constexpr std::array<std::pair<config::AspectRatioMode, const char*>, 3> ratios = {{
        {config::AspectRatioMode::Native3x2, "Pixel-Exact"},
        {config::AspectRatioMode::Ratio4x3,  "4:3 Classic"},
        {config::AspectRatioMode::Stretch,   "Stretch"}
    }};
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect btn{box.x + 18 + i * 152, box.y + 172, 144, 26};
        drawButton(r, btn, ratios[i].second, HitRect{btn}.contains(m_mouseX, m_mouseY), v.aspectRatio == ratios[i].first);
    }

    drawCheckbox(r, SDL_Rect{box.x + 18, box.y + 214, 18, 18}, v.swapScreens,
                 "Swap Top / Bottom Screens (Inverter Telas - F6)",
                 HitRect{SDL_Rect{box.x + 18, box.y + 214, 420, 20}}.contains(m_mouseX, m_mouseY));

    drawCheckbox(r, SDL_Rect{box.x + 18, box.y + 240, 18, 18}, v.showStylusCursor,
                 "Show Dragon Stylus Touch Indicator on Bottom Screen",
                 HitRect{SDL_Rect{box.x + 18, box.y + 240, 420, 20}}.contains(m_mouseX, m_mouseY));

    drawCheckbox(r, SDL_Rect{box.x + 18, box.y + 266, 18, 18}, v.bilinearFilter,
                 "Bilinear Interpolation (Smooth scaling)",
                 HitRect{SDL_Rect{box.x + 18, box.y + 266, 420, 20}}.contains(m_mouseX, m_mouseY));

    drawCheckbox(r, SDL_Rect{box.x + 18, box.y + 292, 18, 18}, v.frameLimiter60Fps,
                 "60 FPS Framerate Limiter",
                 HitRect{SDL_Rect{box.x + 18, box.y + 292, 420, 20}}.contains(m_mouseX, m_mouseY));

    drawCheckbox(r, SDL_Rect{box.x + 18, box.y + 318, 18, 18}, boot.directBootHle,
                 "NDS Direct Boot / HLE (Uncheck for bios9/bios7.bin)",
                 HitRect{SDL_Rect{box.x + 18, box.y + 318, 440, 20}}.contains(m_mouseX, m_mouseY));

    const SDL_Rect doneBtn{box.x + box.w - 104, box.y + box.h - 36, 88, 26};
    drawButton(r, doneBtn, "Done", HitRect{doneBtn}.contains(m_mouseX, m_mouseY), true);
}

void ModalManager::renderAudioConfig(SDL_Renderer* r, const SDL_Rect& box) {
    const auto& a = m_cfgMgr.data().audio;

    char volLabel[48];
    std::snprintf(volLabel, sizeof(volLabel), "Master Volume: %d%%%s",
                  static_cast<int>(a.masterVolume * 100.0f + 0.5f),
                  a.muted ? " (MUTED)" : "");
    DragonFont::drawText(r, volLabel, box.x + 18, box.y + 48, 1,
                         a.muted ? DragonPalette::DRAGON_RED : DragonPalette::TEXT_PRIMARY);

    const SDL_Rect sliderBg{box.x + 18, box.y + 68, box.w - 36, 18};
    fillRect(r, sliderBg, DragonPalette::SURFACE_SUNKEN);
    const int fillW = static_cast<int>(static_cast<float>(sliderBg.w - 4) * std::clamp(a.masterVolume, 0.0f, 1.0f));
    if (fillW > 0) {
        fillRect(r, SDL_Rect{sliderBg.x + 2, sliderBg.y + 2, fillW, sliderBg.h - 4},
                 a.muted ? DragonPalette::DRAGON_RED_DIM : DragonPalette::EMERALD_SCALE);
    }
    strokeRect(r, sliderBg, HitRect{sliderBg}.contains(m_mouseX, m_mouseY)
                            ? DragonPalette::EMERALD_SCALE
                            : DragonPalette::BORDER_SUBTLE);

    const SDL_Rect chkMute{box.x + 18, box.y + 98, 18, 18};
    drawCheckbox(r, chkMute, a.muted, "Mute Audio Output (Lock-Free 16-Ch SPU / APU)",
                 HitRect{SDL_Rect{box.x + 18, box.y + 98, 340, 20}}.contains(m_mouseX, m_mouseY));

    DragonFont::drawText(r, "Audio Buffer Size (Samples):", box.x + 18, box.y + 136, 1, DragonPalette::TEXT_SECONDARY);
    constexpr std::array<std::pair<int, const char*>, 3> buffers = {{
        {512,  "512 (Low Lat)"},
        {1024, "1024 (Normal)"},
        {2048, "2048 (Safe)"}
    }};
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect btn{box.x + 18 + i * 144, box.y + 154, 134, 28};
        drawButton(r, btn, buffers[i].second, HitRect{btn}.contains(m_mouseX, m_mouseY), a.bufferSamples == buffers[i].first);
    }

    DragonFont::drawText(r, "Output Sample Rate:", box.x + 18, box.y + 202, 1, DragonPalette::TEXT_SECONDARY);
    constexpr std::array<std::pair<int, const char*>, 3> rates = {{
        {32768, "32768 Hz"},
        {44100, "44100 Hz"},
        {48000, "48000 Hz"}
    }};
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect btn{box.x + 18 + i * 144, box.y + 220, 134, 28};
        drawButton(r, btn, rates[i].second, HitRect{btn}.contains(m_mouseX, m_mouseY), a.sampleRate == rates[i].first);
    }

    const SDL_Rect doneBtn{box.x + box.w - 104, box.y + box.h - 36, 88, 26};
    drawButton(r, doneBtn, "Done", HitRect{doneBtn}.contains(m_mouseX, m_mouseY), true);
}

void ModalManager::renderAbout(SDL_Renderer* r, const SDL_Rect& box) {
    if (m_logoTex) {
        const SDL_Rect logoDst{box.x + 20, box.y + 44, 84, 84};
        SDL_RenderCopy(r, m_logoTex, nullptr, &logoDst);
    }

    DragonFont::drawText(r, "Ry\xC5\xAB" "DS / Ry\xC5\xABGBA", box.x + 118, box.y + 48, 2, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "Dragon Edition v2.0 — Hybrid NDS + GBA", box.x + 118, box.y + 72, 1, DragonPalette::DRAGON_RED);
    DragonFont::drawText(r, "Dual-Core Nintendo DS & GBA Emulator (melonDS UX)", box.x + 118, box.y + 90, 1, DragonPalette::EMERALD_SCALE);
    DragonFont::drawText(r, "Decoupled C++20 Core + SDL2 Hardware Frontend", box.x + 118, box.y + 108, 1, DragonPalette::TEXT_SECONDARY);

    // Exibe o Ícone 32x32 Extraído do Banner da ROM .nds carregada (se disponível)
    int specTop = box.y + 138;
    if (m_bannerTex) {
        const SDL_Rect bannerCard{box.x + 20, box.y + 134, box.w - 40, 46};
        fillRect(r, bannerCard, DragonPalette::SURFACE_SUNKEN);
        strokeRect(r, bannerCard, DragonPalette::EMERALD_SCALE);

        const SDL_Rect iconDst{bannerCard.x + 7, bannerCard.y + 7, 32, 32};
        SDL_RenderCopy(r, m_bannerTex, nullptr, &iconDst);
        strokeRect(r, iconDst, DragonPalette::BORDER_SUBTLE);

        const std::string line1 = "Active NDS Cartridge: " + clipText(m_bannerTitle, 34);
        DragonFont::drawText(r, line1, bannerCard.x + 48, bannerCard.y + 10, 1, DragonPalette::TEXT_PRIMARY);

        std::string flatSub = m_bannerSubtitle;
        std::replace(flatSub.begin(), flatSub.end(), '\n', ' ');
        DragonFont::drawText(r, clipText(flatSub.empty() ? "32x32 16-Color Banner Extracted" : flatSub, 48),
                             bannerCard.x + 48, bannerCard.y + 26, 1, DragonPalette::EMERALD_SCALE);
        specTop = box.y + 188;
    }

    const SDL_Rect specBox{box.x + 20, specTop, box.w - 40, box.y + box.h - 46 - specTop};
    fillRect(r, specBox, DragonPalette::SURFACE_SUNKEN);
    strokeRect(r, specBox, DragonPalette::BORDER_SUBTLE);

    DragonFont::drawText(r, "NDS + GBA Hardware Subsystems:", specBox.x + 12, specBox.y + 10, 1, DragonPalette::DRAGON_RED);
    DragonFont::drawText(r, "- NDS Dual-CPU: ARM946E-S (66MHz TCM/CP15/DSP) + ARM7 (33MHz)", specBox.x + 12, specBox.y + 28, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "- NDS Dual-GPU: 2D Engines A+B (256x192) + 3D GX Rasterizer", specBox.x + 12, specBox.y + 46, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "- NDS Audio/SPI: 16-Ch SPU (PCM/ADPCM/PSG) + TSC2046 Stylus/Mic", specBox.x + 12, specBox.y + 64, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "- GBA Core: ARM7TDMI + Scanline PPU + DirectSound + Flash/SRAM", specBox.x + 12, specBox.y + 82, 1, DragonPalette::TEXT_PRIMARY);
    if (specBox.h >= 114) {
        DragonFont::drawText(r, "- Boot & Saves: Direct Boot HLE / Optional BIOS + Auto .sav", specBox.x + 12, specBox.y + 100, 1, DragonPalette::TEXT_PRIMARY);
    }

    const SDL_Rect doneBtn{box.x + box.w - 104, box.y + box.h - 36, 88, 26};
    drawButton(r, doneBtn, "Close", HitRect{doneBtn}.contains(m_mouseX, m_mouseY), true);
}

void ModalManager::renderGitHubLicense(SDL_Renderer* r, const SDL_Rect& box) {
    DragonFont::drawText(r, "Ry\xC5\xAB" "DS / Ry\xC5\xABGBA — Open Source Architecture", box.x + 20, box.y + 48, 1, DragonPalette::EMERALD_SCALE);
    DragonFont::drawText(r, "Modules: core/nds/ | core/cpu/ | frontend/ | ui/ | input/ | config/", box.x + 20, box.y + 68, 1, DragonPalette::TEXT_SECONDARY);

    const std::string cfgLine = "Config JSON: " + clipText(m_cfgMgr.configPath().string(), 46);
    DragonFont::drawText(r, cfgLine, box.x + 20, box.y + 92, 1, DragonPalette::TEXT_PRIMARY);

    const SDL_Rect licBox{box.x + 20, box.y + 118, box.w - 40, 172};
    fillRect(r, licBox, DragonPalette::SURFACE_SUNKEN);
    strokeRect(r, licBox, DragonPalette::BORDER_SUBTLE);

    DragonFont::drawText(r, "MIT License — Copyright (c) 2026 Ry\xC5\xAB" "DS Project", licBox.x + 12, licBox.y + 14, 1, DragonPalette::DRAGON_RED);
    DragonFont::drawText(r, "Permission is hereby granted, free of charge, to any", licBox.x + 12, licBox.y + 38, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "person obtaining a copy of this software to use, copy,", licBox.x + 12, licBox.y + 56, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "modify, merge, publish, and distribute copies of the", licBox.x + 12, licBox.y + 74, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "Software, inspired by the clean UX of melonDS.", licBox.x + 12, licBox.y + 92, 1, DragonPalette::TEXT_PRIMARY);
    DragonFont::drawText(r, "Nintendo DS and Game Boy Advance are trademarks of Nintendo.", licBox.x + 12, licBox.y + 128, 1, DragonPalette::TEXT_MUTED);

    const SDL_Rect doneBtn{box.x + box.w - 104, box.y + box.h - 36, 88, 26};
    drawButton(r, doneBtn, "Close", HitRect{doneBtn}.contains(m_mouseX, m_mouseY), true);
}

} // namespace ryugba::ui
