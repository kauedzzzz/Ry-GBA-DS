#pragma once
#include "config/ConfigManager.hpp"
#include "core/ICoreEmulator.hpp"
#include <SDL2/SDL.h>
#include <array>
#include <functional>
#include <string>
#include <unordered_map>

namespace ryugba::input {

enum class ListenColumn : uint8_t {
    Keyboard = 0,
    Joystick = 1
};

struct ListeningTarget {
    bool         active{false};
    bool         isHotkey{false};
    std::size_t  index{0};
    ListenColumn column{ListenColumn::Keyboard};
};

class InputMapper {
public:
    using HotkeyCallback = std::function<void(config::HotkeyAction)>;

    explicit InputMapper(config::ConfigManager& cfgMgr);
    ~InputMapper();

    void initControllers();
    void closeControllers();

    void setHotkeyCallback(HotkeyCallback cb) { m_hotkeyCallback = std::move(cb); }

    // Modo de escuta interativa ("Listening / Pressione um botão...")
    void beginListening(bool isHotkey, std::size_t actionIdx, ListenColumn col) noexcept;
    void cancelListening() noexcept { m_listen.active = false; }
    void clearSlot(bool isHotkey, std::size_t actionIdx, ListenColumn col);
    [[nodiscard]] const ListeningTarget& listeningState() const noexcept { return m_listen; }
    [[nodiscard]] bool isListening() const noexcept { return m_listen.active; }

    // Processa eventos SDL2. Retorna true se o evento foi consumido pela captura "Listening...".
    bool processEvent(const SDL_Event& ev, gba::core::ICoreEmulator& core, bool allowGameplayInput);

    // Libera todos os botões virtuais (ex: ao abrir um modal ou pausar)
    void releaseAllInputs(gba::core::ICoreEmulator& core) noexcept;

    [[nodiscard]] std::string activeControllerName() const;
    [[nodiscard]] static std::string formatKeyName(int scancode);
    [[nodiscard]] static std::string formatJoyName(config::JoyBindType type, int code);

private:
    config::ConfigManager& m_cfgMgr;
    ListeningTarget        m_listen{};
    HotkeyCallback         m_hotkeyCallback{};

    SDL_GameController* m_controller{nullptr};
    SDL_Joystick*       m_rawJoystick{nullptr};
    SDL_JoystickID      m_activeInstanceId{-1};

    std::unordered_map<int, bool>  m_keyDown;
    std::unordered_map<int, bool>  m_padBtnDown;
    std::unordered_map<int, float> m_padAxisVal;
    std::unordered_map<int, bool>  m_rawJoyBtnDown;
    uint8_t                        m_rawHatMask{0};

    std::array<bool, static_cast<std::size_t>(config::HotkeyAction::Count)> m_hotkeyWasActive{};

    [[nodiscard]] bool isSlotPressed(const config::DualInputSlot& slot) const noexcept;
    void syncCoreKeys(gba::core::ICoreEmulator& core) noexcept;
    void evaluateHotkeys();
    bool tryCaptureListeningEvent(const SDL_Event& ev);
};

} // namespace ryugba::input

