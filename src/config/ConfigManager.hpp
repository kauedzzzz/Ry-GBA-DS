#pragma once
#include "core/input/InputManager.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ryugba::config {

enum class JoyBindType : uint8_t {
    None = 0,
    ControllerButton  = 1,
    ControllerAxisPos = 2,
    ControllerAxisNeg = 3,
    JoyButton         = 4,
    JoyHat            = 5
};

struct DualInputSlot {
    int         keyScancode{-1}; // SDL_Scancode (-1 = nenhum)
    JoyBindType joyType{JoyBindType::None};
    int         joyCode{0};
};

// 12 Botões de Console (10 GBA/DS + Botões X e Y exclusivos do Nintendo DS)
enum class GbaAction : std::size_t {
    A      = 0,
    B      = 1,
    X      = 2,
    Y      = 3,
    L      = 4,
    R      = 5,
    Start  = 6,
    Select = 7,
    Up     = 8,
    Down   = 9,
    Left   = 10,
    Right  = 11,
    Count  = 12
};

// 7 Atalhos e Sensores Especiais DS (Microfone, Swap Screens, Tampa/Lid)
enum class HotkeyAction : std::size_t {
    FastForward = 0,
    QuickSave   = 1,
    QuickLoad   = 2,
    Pause       = 3,
    MicBlow     = 4, // Sopro no Microfone (enquanto segurado)
    SwapScreens = 5, // Inversão instantânea de telas Top/Bottom
    ToggleLid   = 6, // Abrir/Fechar Tampa (Sleep Mode DS)
    Count       = 7
};

[[nodiscard]] constexpr bool isExtendedNdsAction(GbaAction action) noexcept {
    return action == GbaAction::X || action == GbaAction::Y;
}

[[nodiscard]] constexpr gba::core::GbaKey actionToCoreKey(GbaAction action) noexcept {
    switch (action) {
        case GbaAction::A:      return gba::core::GbaKey::A;
        case GbaAction::B:      return gba::core::GbaKey::B;
        case GbaAction::L:      return gba::core::GbaKey::L;
        case GbaAction::R:      return gba::core::GbaKey::R;
        case GbaAction::Start:  return gba::core::GbaKey::Start;
        case GbaAction::Select: return gba::core::GbaKey::Select;
        case GbaAction::Up:     return gba::core::GbaKey::Up;
        case GbaAction::Down:   return gba::core::GbaKey::Down;
        case GbaAction::Left:   return gba::core::GbaKey::Left;
        case GbaAction::Right:  return gba::core::GbaKey::Right;
        default:                return gba::core::GbaKey::A;
    }
}

[[nodiscard]] constexpr std::string_view gbaActionLabel(GbaAction action) noexcept {
    switch (action) {
        case GbaAction::A:      return "Button A";
        case GbaAction::B:      return "Button B";
        case GbaAction::X:      return "Button X (NDS)";
        case GbaAction::Y:      return "Button Y (NDS)";
        case GbaAction::L:      return "Trigger L";
        case GbaAction::R:      return "Trigger R";
        case GbaAction::Start:  return "Start";
        case GbaAction::Select: return "Select";
        case GbaAction::Up:     return "D-Pad Up";
        case GbaAction::Down:   return "D-Pad Down";
        case GbaAction::Left:   return "D-Pad Left";
        case GbaAction::Right:  return "D-Pad Right";
        default:                return "Unknown";
    }
}

[[nodiscard]] constexpr std::string_view gbaActionJsonKey(GbaAction action) noexcept {
    switch (action) {
        case GbaAction::A:      return "A";
        case GbaAction::B:      return "B";
        case GbaAction::X:      return "X";
        case GbaAction::Y:      return "Y";
        case GbaAction::L:      return "L";
        case GbaAction::R:      return "R";
        case GbaAction::Start:  return "Start";
        case GbaAction::Select: return "Select";
        case GbaAction::Up:     return "Up";
        case GbaAction::Down:   return "Down";
        case GbaAction::Left:   return "Left";
        case GbaAction::Right:  return "Right";
        default:                return "A";
    }
}

[[nodiscard]] constexpr std::string_view hotkeyActionLabel(HotkeyAction hk) noexcept {
    switch (hk) {
        case HotkeyAction::FastForward: return "Fast Forward";
        case HotkeyAction::QuickSave:   return "Quick Save (Slot 1)";
        case HotkeyAction::QuickLoad:   return "Quick Load (Slot 1)";
        case HotkeyAction::Pause:       return "Pause / Resume";
        case HotkeyAction::MicBlow:     return "Mic Blow (Sopro DS)";
        case HotkeyAction::SwapScreens: return "Swap Screens (DS)";
        case HotkeyAction::ToggleLid:   return "Lid Sleep (Tampa DS)";
        default:                        return "Hotkey";
    }
}

[[nodiscard]] constexpr std::string_view hotkeyActionJsonKey(HotkeyAction hk) noexcept {
    switch (hk) {
        case HotkeyAction::FastForward: return "FastForward";
        case HotkeyAction::QuickSave:   return "QuickSave";
        case HotkeyAction::QuickLoad:   return "QuickLoad";
        case HotkeyAction::Pause:       return "Pause";
        case HotkeyAction::MicBlow:     return "MicBlow";
        case HotkeyAction::SwapScreens: return "SwapScreens";
        case HotkeyAction::ToggleLid:   return "ToggleLid";
        default:                        return "FastForward";
    }
}

enum class AspectRatioMode : int {
    Native3x2 = 0, // Nativo (4:3 por tela no DS / 3:2 no GBA)
    Ratio4x3  = 1, // 4:3 clássico
    Stretch   = 2  // Preencher janela
};

// Modos de Layout Dual-Screen do Nintendo DS (256x192 x 2)
enum class NdsScreenLayout : int {
    Vertical    = 0, // Padrão DS (Superior em cima, Inferior embaixo com divisória Dragon)
    Horizontal  = 1, // Lado a Lado (Widescreen)
    SingleFocus = 2  // Foco Único (Tela principal grande + secundária PiP / alternável via F6)
};

struct VideoConfig {
    int             scale{3}; // 1x..4x
    AspectRatioMode aspectRatio{AspectRatioMode::Native3x2};
    NdsScreenLayout ndsLayout{NdsScreenLayout::Vertical};
    bool            swapScreens{false};      // Inversão de Telas Top <-> Bottom
    bool            showStylusCursor{true};  // Indicador temático Dragon no toque da Stylus
    bool            bilinearFilter{false};
    bool            frameLimiter60Fps{true};
};

struct AudioConfig {
    float masterVolume{0.80f};
    bool  muted{false};
    int   bufferSamples{1024}; // 512, 1024, 2048
    int   sampleRate{48000};   // 32768, 44100, 48000
};

struct NdsBootConfig {
    bool        directBootHle{true}; // Direct Boot / HLE pós-firmware sem exigir BIOS externa
    std::string bios9Path{"bios9.bin"};
    std::string bios7Path{"bios7.bin"};
    std::string firmwarePath{"firmware.bin"};
};

struct AppConfig {
    VideoConfig   video{};
    AudioConfig   audio{};
    NdsBootConfig ndsBoot{};
    int           speedPercent{100}; // 100, 150, 200, 300, 0 (Uncapped)

    std::array<DualInputSlot, static_cast<std::size_t>(GbaAction::Count)>    controls{};
    std::array<DualInputSlot, static_cast<std::size_t>(HotkeyAction::Count)> hotkeys{};
    std::vector<std::string> recentRoms{};
    std::string lastBrowseDirectory{};
};

class ConfigManager {
public:
    ConfigManager();

    static AppConfig makeDefaults();
    [[nodiscard]] static std::filesystem::path defaultConfigPath();

    bool load();
    bool loadFrom(const std::filesystem::path& path);
    bool save() const;
    bool saveTo(const std::filesystem::path& path) const;

    void resetControlsToDefault();

    void addRecentRom(const std::filesystem::path& romPath);
    void clearRecentRoms();

    [[nodiscard]] AppConfig& data() noexcept { return m_config; }
    [[nodiscard]] const AppConfig& data() const noexcept { return m_config; }
    [[nodiscard]] const std::filesystem::path& configPath() const noexcept { return m_configPath; }

private:
    AppConfig             m_config;
    std::filesystem::path m_configPath;
};

} // namespace ryugba::config
