#include "core/input/InputManager.hpp"

namespace gba::core {

InputManager::InputManager() {
    loadDefaultBindings();
}

void InputManager::loadDefaultBindings() {
    std::scoped_lock lock(m_bindingMutex);
    m_bindings.clear();

    // Scancodes SDL2 padrão:
    // Setas: Up=82, Down=81, Left=80, Right=79
    // WASD:  W=26, S=22, A=4, D=7
    // Ação:  X=27 (A), Z=29 (B), Q=20 (L), E=8 (R), Enter=40 (Start), Backspace=42 (Select)
    // Gamepad SDL_GameControllerButton:
    // A=0, B=1, X=2, Y=3, Back=4, Start=6, LeftShoulder=9, RightShoulder=10, DPadUp=11, Down=12, Left=13, Right=14
    m_bindings[GbaKey::Up] = {
        {InputDeviceType::KeyboardKey, 82},
        {InputDeviceType::KeyboardKey, 26},
        {InputDeviceType::GamepadButton, 11},
        {InputDeviceType::GamepadAxisNegative, 1, 0.45f}
    };
    m_bindings[GbaKey::Down] = {
        {InputDeviceType::KeyboardKey, 81},
        {InputDeviceType::KeyboardKey, 22},
        {InputDeviceType::GamepadButton, 12},
        {InputDeviceType::GamepadAxisPositive, 1, 0.45f}
    };
    m_bindings[GbaKey::Left] = {
        {InputDeviceType::KeyboardKey, 80},
        {InputDeviceType::KeyboardKey, 4},
        {InputDeviceType::GamepadButton, 13},
        {InputDeviceType::GamepadAxisNegative, 0, 0.45f}
    };
    m_bindings[GbaKey::Right] = {
        {InputDeviceType::KeyboardKey, 79},
        {InputDeviceType::KeyboardKey, 7},
        {InputDeviceType::GamepadButton, 14},
        {InputDeviceType::GamepadAxisPositive, 0, 0.45f}
    };
    m_bindings[GbaKey::A] = {
        {InputDeviceType::KeyboardKey, 27}, // X
        {InputDeviceType::KeyboardKey, 13}, // J
        {InputDeviceType::GamepadButton, 0} // Botão Sul (A / Cross)
    };
    m_bindings[GbaKey::B] = {
        {InputDeviceType::KeyboardKey, 29}, // Z
        {InputDeviceType::KeyboardKey, 14}, // K
        {InputDeviceType::GamepadButton, 1}, // Botão Leste (B / Circle)
        {InputDeviceType::GamepadButton, 2}  // Botão Oeste (X / Square)
    };
    m_bindings[GbaKey::L] = {
        {InputDeviceType::KeyboardKey, 20}, // Q
        {InputDeviceType::KeyboardKey, 24}, // U
        {InputDeviceType::GamepadButton, 9} // LB / L1
    };
    m_bindings[GbaKey::R] = {
        {InputDeviceType::KeyboardKey, 8},  // E
        {InputDeviceType::KeyboardKey, 12}, // I
        {InputDeviceType::GamepadButton, 10} // RB / R1
    };
    m_bindings[GbaKey::Start] = {
        {InputDeviceType::KeyboardKey, 40}, // Enter
        {InputDeviceType::GamepadButton, 6} // Start
    };
    m_bindings[GbaKey::Select] = {
        {InputDeviceType::KeyboardKey, 42}, // Backspace
        {InputDeviceType::KeyboardKey, 225}, // Left Shift
        {InputDeviceType::GamepadButton, 4} // Back / Select
    };

    rebuildKeyInputRegister();
}

void InputManager::clearBindingsForKey(GbaKey targetKey) {
    std::scoped_lock lock(m_bindingMutex);
    m_bindings[targetKey].clear();
    rebuildKeyInputRegister();
}

void InputManager::bindInput(PhysicalInputBinding physical, GbaKey targetKey) {
    std::scoped_lock lock(m_bindingMutex);
    auto& list = m_bindings[targetKey];
    for (auto& existing : list) {
        if (existing == physical) {
            existing.axisThreshold = physical.axisThreshold;
            rebuildKeyInputRegister();
            return;
        }
    }
    list.push_back(physical);
    rebuildKeyInputRegister();
}

void InputManager::onKeyboardEvent(uint32_t scancode, bool pressed) {
    std::scoped_lock lock(m_bindingMutex);
    m_keyboardState[scancode] = pressed;
    rebuildKeyInputRegister();
}

void InputManager::onGamepadButtonEvent(uint32_t buttonId, bool pressed) {
    std::scoped_lock lock(m_bindingMutex);
    m_gamepadButtonState[buttonId] = pressed;
    rebuildKeyInputRegister();
}

void InputManager::onGamepadAxisEvent(uint32_t axisId, float normalizedValue) {
    std::scoped_lock lock(m_bindingMutex);
    m_gamepadAxisState[axisId] = normalizedValue;
    rebuildKeyInputRegister();
}

void InputManager::setVirtualKeyState(GbaKey key, bool pressed) noexcept {
    std::scoped_lock lock(m_bindingMutex);
    if (pressed) {
        m_directVirtualMask |= static_cast<uint16_t>(key);
    } else {
        m_directVirtualMask &= ~static_cast<uint16_t>(key);
    }
    rebuildKeyInputRegister();
}

bool InputManager::isBindingActive(const PhysicalInputBinding& binding) const noexcept {
    switch (binding.deviceType) {
        case InputDeviceType::KeyboardKey: {
            auto it = m_keyboardState.find(binding.code);
            return it != m_keyboardState.end() && it->second;
        }
        case InputDeviceType::GamepadButton: {
            auto it = m_gamepadButtonState.find(binding.code);
            return it != m_gamepadButtonState.end() && it->second;
        }
        case InputDeviceType::GamepadAxisPositive: {
            auto it = m_gamepadAxisState.find(binding.code);
            return it != m_gamepadAxisState.end() && (it->second >= binding.axisThreshold);
        }
        case InputDeviceType::GamepadAxisNegative: {
            auto it = m_gamepadAxisState.find(binding.code);
            return it != m_gamepadAxisState.end() && (it->second <= -binding.axisThreshold);
        }
    }
    return false;
}

void InputManager::rebuildKeyInputRegister() {
    uint16_t activeHighMask = m_directVirtualMask;

    for (const auto& [gbaKey, bindingList] : m_bindings) {
        for (const auto& binding : bindingList) {
            if (isBindingActive(binding)) {
                activeHighMask |= static_cast<uint16_t>(gbaKey);
                break;
            }
        }
    }

    // Impede direções opostas simultâneas no D-Pad (Up+Down ou Left+Right)
    const uint16_t upDownMask = static_cast<uint16_t>(GbaKey::Up) | static_cast<uint16_t>(GbaKey::Down);
    if ((activeHighMask & upDownMask) == upDownMask) {
        activeHighMask &= ~upDownMask;
    }
    const uint16_t leftRightMask = static_cast<uint16_t>(GbaKey::Left) | static_cast<uint16_t>(GbaKey::Right);
    if ((activeHighMask & leftRightMask) == leftRightMask) {
        activeHighMask &= ~leftRightMask;
    }

    // No GBA: bit 0 = Pressionado, bit 1 = Solto
    const uint16_t activeLowReg = (~activeHighMask) & ALL_RELEASED_MASK;
    m_keyInputRegister.store(activeLowReg, std::memory_order_relaxed);
}

bool InputManager::shouldTriggerKeypadIrq() const noexcept {
    if ((m_keyCntRegister & (1u << 14)) == 0) {
        return false;
    }

    const uint16_t watchedButtons = m_keyCntRegister & ALL_RELEASED_MASK;
    if (watchedButtons == 0) return false;

    const uint16_t pressedButtons = (~m_keyInputRegister.load(std::memory_order_relaxed)) & ALL_RELEASED_MASK;
    const bool isLogicalAndMode = (m_keyCntRegister & (1u << 15)) != 0;

    if (isLogicalAndMode) {
        return (pressedButtons & watchedButtons) == watchedButtons;
    }
    return (pressedButtons & watchedButtons) != 0;
}

} // namespace gba::core

