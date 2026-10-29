#include "input/InputMapper.hpp"
#include <cmath>

namespace ryugba::input {

namespace {
constexpr float AXIS_ACTIVATE_THRESHOLD = 0.48f;
constexpr float AXIS_CAPTURE_THRESHOLD  = 0.60f;
} // namespace

InputMapper::InputMapper(config::ConfigManager& cfgMgr)
    : m_cfgMgr(cfgMgr) {}

InputMapper::~InputMapper() {
    closeControllers();
}

void InputMapper::initControllers() {
    closeControllers();
    const int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (SDL_IsGameController(i)) {
            m_controller = SDL_GameControllerOpen(i);
            if (m_controller) {
                SDL_Joystick* j = SDL_GameControllerGetJoystick(m_controller);
                m_activeInstanceId = j ? SDL_JoystickInstanceID(j) : -1;
                return;
            }
        }
    }
    if (num > 0) {
        m_rawJoystick = SDL_JoystickOpen(0);
        if (m_rawJoystick) {
            m_activeInstanceId = SDL_JoystickInstanceID(m_rawJoystick);
        }
    }
}

void InputMapper::closeControllers() {
    if (m_controller) {
        SDL_GameControllerClose(m_controller);
        m_controller = nullptr;
    }
    if (m_rawJoystick) {
        SDL_JoystickClose(m_rawJoystick);
        m_rawJoystick = nullptr;
    }
    m_activeInstanceId = -1;
}

std::string InputMapper::activeControllerName() const {
    if (m_controller) {
        const char* name = SDL_GameControllerName(m_controller);
        return name ? std::string(name) : "SDL GameController";
    }
    if (m_rawJoystick) {
        const char* name = SDL_JoystickName(m_rawJoystick);
        return name ? std::string(name) : "USB Joystick";
    }
    return "None (Keyboard Only)";
}

void InputMapper::beginListening(bool isHotkey, std::size_t actionIdx, ListenColumn col) noexcept {
    m_listen.active   = true;
    m_listen.isHotkey = isHotkey;
    m_listen.index    = actionIdx;
    m_listen.column   = col;
}

void InputMapper::clearSlot(bool isHotkey, std::size_t actionIdx, ListenColumn col) {
    auto& cfg = m_cfgMgr.data();
    config::DualInputSlot* target = nullptr;
    if (isHotkey && actionIdx < cfg.hotkeys.size()) {
        target = &cfg.hotkeys[actionIdx];
    } else if (!isHotkey && actionIdx < cfg.controls.size()) {
        target = &cfg.controls[actionIdx];
    }
    if (!target) return;

    if (col == ListenColumn::Keyboard) {
        target->keyScancode = -1;
    } else {
        target->joyType = config::JoyBindType::None;
        target->joyCode = 0;
    }
    m_cfgMgr.save();
}

bool InputMapper::tryCaptureListeningEvent(const SDL_Event& ev) {
    if (!m_listen.active) return false;

    auto& cfg = m_cfgMgr.data();
    config::DualInputSlot* slot = nullptr;
    if (m_listen.isHotkey && m_listen.index < cfg.hotkeys.size()) {
        slot = &cfg.hotkeys[m_listen.index];
    } else if (!m_listen.isHotkey && m_listen.index < cfg.controls.size()) {
        slot = &cfg.controls[m_listen.index];
    }
    if (!slot) {
        m_listen.active = false;
        return false;
    }

    if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        const SDL_Scancode sc = ev.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) {
            m_listen.active = false;
            return true;
        }
        slot->keyScancode = static_cast<int>(sc);
        m_listen.active = false;
        m_cfgMgr.save();
        return true;
    }

    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        slot->joyType = config::JoyBindType::ControllerButton;
        slot->joyCode = ev.cbutton.button;
        m_listen.active = false;
        m_cfgMgr.save();
        return true;
    }

    if (ev.type == SDL_CONTROLLERAXISMOTION) {
        const float norm = static_cast<float>(ev.caxis.value) / 32767.0f;
        if (std::fabs(norm) >= AXIS_CAPTURE_THRESHOLD) {
            slot->joyType = (norm > 0.0f)
                ? config::JoyBindType::ControllerAxisPos
                : config::JoyBindType::ControllerAxisNeg;
            slot->joyCode = ev.caxis.axis;
            m_listen.active = false;
            m_cfgMgr.save();
            return true;
        }
    }

    if (!m_controller && ev.type == SDL_JOYBUTTONDOWN) {
        slot->joyType = config::JoyBindType::JoyButton;
        slot->joyCode = ev.jbutton.button;
        m_listen.active = false;
        m_cfgMgr.save();
        return true;
    }

    if (!m_controller && ev.type == SDL_JOYHATMOTION && ev.jhat.value != SDL_HAT_CENTERED) {
        slot->joyType = config::JoyBindType::JoyHat;
        slot->joyCode = ev.jhat.value;
        m_listen.active = false;
        m_cfgMgr.save();
        return true;
    }

    return false;
}

bool InputMapper::processEvent(const SDL_Event& ev, gba::core::ICoreEmulator& core, bool allowGameplayInput) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_JOYDEVICEADDED) {
        if (!m_controller && !m_rawJoystick) {
            initControllers();
        }
        return false;
    }
    if (ev.type == SDL_CONTROLLERDEVICEREMOVED || ev.type == SDL_JOYDEVICEREMOVED) {
        initControllers();
        return false;
    }

    if (m_listen.active) {
        return tryCaptureListeningEvent(ev);
    }

    bool stateChanged = false;
    switch (ev.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!ev.key.repeat) {
                m_keyDown[static_cast<int>(ev.key.keysym.scancode)] = (ev.type == SDL_KEYDOWN);
                stateChanged = true;
            }
            break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            m_padBtnDown[ev.cbutton.button] = (ev.type == SDL_CONTROLLERBUTTONDOWN);
            stateChanged = true;
            break;

        case SDL_CONTROLLERAXISMOTION:
            m_padAxisVal[ev.caxis.axis] = static_cast<float>(ev.caxis.value) / 32767.0f;
            stateChanged = true;
            break;

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            if (!m_controller) {
                m_rawJoyBtnDown[ev.jbutton.button] = (ev.type == SDL_JOYBUTTONDOWN);
                stateChanged = true;
            }
            break;

        case SDL_JOYHATMOTION:
            if (!m_controller) {
                m_rawHatMask = ev.jhat.value;
                stateChanged = true;
            }
            break;

        default:
            break;
    }

    if (stateChanged) {
        const SDL_Keymod mods = SDL_GetModState();
        if ((mods & (KMOD_CTRL | KMOD_ALT)) == 0) {
            if (allowGameplayInput) {
                syncCoreKeys(core);
            } else {
                releaseAllInputs(core);
            }
            // Permite que Pause / SwapScreens / Lid funcionem mesmo quando pausado!
            evaluateHotkeys();
        }
    }

    return false;
}

void InputMapper::releaseAllInputs(gba::core::ICoreEmulator& core) noexcept {
    for (std::size_t i = 0; i < static_cast<std::size_t>(config::GbaAction::Count); ++i) {
        const auto act = static_cast<config::GbaAction>(i);
        if (act == config::GbaAction::X) {
            core.setExtendedButtonX(false);
        } else if (act == config::GbaAction::Y) {
            core.setExtendedButtonY(false);
        } else {
            core.setKeyState(config::actionToCoreKey(act), false);
        }
    }
    core.setMicBlowing(false);
}

bool InputMapper::isSlotPressed(const config::DualInputSlot& slot) const noexcept {
    if (slot.keyScancode >= 0) {
        if (auto it = m_keyDown.find(slot.keyScancode); it != m_keyDown.end() && it->second) {
            return true;
        }
    }

    switch (slot.joyType) {
        case config::JoyBindType::None:
            break;
        case config::JoyBindType::ControllerButton:
            if (auto it = m_padBtnDown.find(slot.joyCode); it != m_padBtnDown.end() && it->second) return true;
            break;
        case config::JoyBindType::ControllerAxisPos:
            if (auto it = m_padAxisVal.find(slot.joyCode); it != m_padAxisVal.end() && it->second >= AXIS_ACTIVATE_THRESHOLD) return true;
            break;
        case config::JoyBindType::ControllerAxisNeg:
            if (auto it = m_padAxisVal.find(slot.joyCode); it != m_padAxisVal.end() && it->second <= -AXIS_ACTIVATE_THRESHOLD) return true;
            break;
        case config::JoyBindType::JoyButton:
            if (auto it = m_rawJoyBtnDown.find(slot.joyCode); it != m_rawJoyBtnDown.end() && it->second) return true;
            break;
        case config::JoyBindType::JoyHat:
            if ((m_rawHatMask & static_cast<uint8_t>(slot.joyCode)) != 0) return true;
            break;
    }

    return false;
}

void InputMapper::syncCoreKeys(gba::core::ICoreEmulator& core) noexcept {
    const auto& controls = m_cfgMgr.data().controls;
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const auto act = static_cast<config::GbaAction>(i);
        const bool pressed = isSlotPressed(controls[i]);
        if (act == config::GbaAction::X) {
            core.setExtendedButtonX(pressed);
        } else if (act == config::GbaAction::Y) {
            core.setExtendedButtonY(pressed);
        } else {
            core.setKeyState(config::actionToCoreKey(act), pressed);
        }
    }

    // Sopro no microfone DS: ativo continuamente enquanto a tecla/botão MicBlow estiver segurada
    const auto& hotkeys = m_cfgMgr.data().hotkeys;
    core.setMicBlowing(isSlotPressed(hotkeys[static_cast<std::size_t>(config::HotkeyAction::MicBlow)]));
}

void InputMapper::evaluateHotkeys() {
    const auto& hotkeys = m_cfgMgr.data().hotkeys;
    for (std::size_t i = 0; i < hotkeys.size(); ++i) {
        const bool nowActive = isSlotPressed(hotkeys[i]);
        if (nowActive && !m_hotkeyWasActive[i]) {
            if (m_hotkeyCallback) {
                m_hotkeyCallback(static_cast<config::HotkeyAction>(i));
            }
        }
        m_hotkeyWasActive[i] = nowActive;
    }
}

std::string InputMapper::formatKeyName(int scancode) {
    if (scancode < 0) return "[None]";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    if (!name || name[0] == '\0') {
        return "Key #" + std::to_string(scancode);
    }
    return std::string(name);
}

std::string InputMapper::formatJoyName(config::JoyBindType type, int code) {
    switch (type) {
        case config::JoyBindType::None:
            return "[None]";
        case config::JoyBindType::ControllerButton: {
            switch (static_cast<SDL_GameControllerButton>(code)) {
                case SDL_CONTROLLER_BUTTON_A:             return "Pad A / Cross";
                case SDL_CONTROLLER_BUTTON_B:             return "Pad B / Circle";
                case SDL_CONTROLLER_BUTTON_X:             return "Pad X / Square";
                case SDL_CONTROLLER_BUTTON_Y:             return "Pad Y / Triangle";
                case SDL_CONTROLLER_BUTTON_BACK:          return "Pad Back / Select";
                case SDL_CONTROLLER_BUTTON_START:         return "Pad Start";
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "Pad L1 / LB";
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Pad R1 / RB";
                case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "Pad L3 (Stick)";
                case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "Pad R3 (Stick)";
                case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "Pad D-Up";
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "Pad D-Down";
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "Pad D-Left";
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "Pad D-Right";
                default:                                  return "Pad Btn " + std::to_string(code);
            }
        }
        case config::JoyBindType::ControllerAxisPos:
        case config::JoyBindType::ControllerAxisNeg: {
            const char* sign = (type == config::JoyBindType::ControllerAxisPos) ? "+" : "-";
            switch (static_cast<SDL_GameControllerAxis>(code)) {
                case SDL_CONTROLLER_AXIS_LEFTX:        return std::string("L-Stick X") + sign;
                case SDL_CONTROLLER_AXIS_LEFTY:        return std::string("L-Stick Y") + sign;
                case SDL_CONTROLLER_AXIS_RIGHTX:       return std::string("R-Stick X") + sign;
                case SDL_CONTROLLER_AXIS_RIGHTY:       return std::string("R-Stick Y") + sign;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return "Pad L2 / LT";
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Pad R2 / RT";
                default:                               return "Axis " + std::to_string(code) + sign;
            }
        }
        case config::JoyBindType::JoyButton:
            return "Joy Btn " + std::to_string(code);
        case config::JoyBindType::JoyHat:
            if (code & SDL_HAT_UP)    return "Hat Up";
            if (code & SDL_HAT_DOWN)  return "Hat Down";
            if (code & SDL_HAT_LEFT)  return "Hat Left";
            if (code & SDL_HAT_RIGHT) return "Hat Right";
            return "Hat " + std::to_string(code);
    }
    return "[None]";
}

} // namespace ryugba::input
