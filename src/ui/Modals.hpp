#pragma once
#include "config/ConfigManager.hpp"
#include "input/InputMapper.hpp"
#include "ui/DragonTheme.hpp"
#include <SDL2/SDL.h>
#include <filesystem>
#include <string>
#include <vector>

namespace ryugba::ui {

enum class ModalKind {
    None = 0,
    OpenRom,
    InputAndHotkeys,
    VideoSettings,
    AudioSettings,
    About,
    GitHubLicense
};

struct ModalCallbacks {
    bool videoLayoutDirty{false};
    bool audioVolumeDirty{false};
    bool audioDeviceDirty{false};
    bool ndsBootDirty{false};
    std::filesystem::path romToLoad{};
};

class ModalManager {
public:
    ModalManager(config::ConfigManager& cfgMgr, input::InputMapper& inputMapper);

    void setLogoTexture(SDL_Texture* logoTex) noexcept { m_logoTex = logoTex; }
    void setRomBannerInfo(SDL_Texture* bannerTex, std::string title, std::string subtitle) {
        m_bannerTex      = bannerTex;
        m_bannerTitle    = std::move(title);
        m_bannerSubtitle = std::move(subtitle);
    }

    void open(ModalKind kind, const std::filesystem::path& defaultRomDir = {});
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return m_active != ModalKind::None; }
    [[nodiscard]] ModalKind activeKind() const noexcept { return m_active; }

    bool handleEvent(const SDL_Event& ev, int winW, int winH, ModalCallbacks& outEffects);
    void render(SDL_Renderer* r, int winW, int winH);

private:
    struct DirEntryItem {
        std::string           displayName;
        std::filesystem::path fullPath;
        bool                  isDirectory{false};
        bool                  isNdsRom{false};
        std::uintmax_t        fileSize{0};
    };

    config::ConfigManager& m_cfgMgr;
    input::InputMapper&    m_inputMapper;
    SDL_Texture*           m_logoTex{nullptr};
    SDL_Texture*           m_bannerTex{nullptr};
    std::string            m_bannerTitle{};
    std::string            m_bannerSubtitle{};

    ModalKind m_active{ModalKind::None};
    int       m_inputTab{0}; // 0 = DS/GBA Controls (12), 1 = Hotkeys & DS Sensors (7)
    int       m_mouseX{0};
    int       m_mouseY{0};
    bool      m_draggingVolume{false};

    std::filesystem::path     m_currentDir{};
    std::filesystem::path     m_defaultRomDir{};
    std::vector<DirEntryItem> m_dirEntries{};
    int                       m_fileScrollOffset{0};

    void refreshDirectory(const std::filesystem::path& dir);
    [[nodiscard]] SDL_Rect modalBox(int winW, int winH) const noexcept;

    bool handleOpenRomEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects);
    bool handleInputConfigEvent(const SDL_Event& ev, const SDL_Rect& box);
    bool handleVideoConfigEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects);
    bool handleAudioConfigEvent(const SDL_Event& ev, const SDL_Rect& box, ModalCallbacks& outEffects);

    void renderOpenRom(SDL_Renderer* r, const SDL_Rect& box);
    void renderInputConfig(SDL_Renderer* r, const SDL_Rect& box);
    void renderVideoConfig(SDL_Renderer* r, const SDL_Rect& box);
    void renderAudioConfig(SDL_Renderer* r, const SDL_Rect& box);
    void renderAbout(SDL_Renderer* r, const SDL_Rect& box);
    void renderGitHubLicense(SDL_Renderer* r, const SDL_Rect& box);
};

} // namespace ryugba::ui
