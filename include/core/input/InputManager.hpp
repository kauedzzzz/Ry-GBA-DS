#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gba::core {

enum class GbaKey : uint16_t {
    A      = 1 << 0, // Bit 0
    B      = 1 << 1, // Bit 1
    Select = 1 << 2, // Bit 2
    Start  = 1 << 3, // Bit 3
    Right  = 1 << 4, // Bit 4
    Left   = 1 << 5, // Bit 5
    Up     = 1 << 6, // Bit 6
    Down   = 1 << 7, // Bit 7
    R      = 1 << 8, // Bit 8
    L      = 1 << 9  // Bit 9
};

constexpr std::string_view gbaKeyToString(GbaKey key) noexcept {
    switch (key) {
        case GbaKey::A:      return "A";
        case GbaKey::B:      return "B";
        case GbaKey::Select: return "SELECT";
        case GbaKey::Start:  return "START";
        case GbaKey::Right:  return "RIGHT";
        case GbaKey::Left:   return "LEFT";
        case GbaKey::Up:     return "UP";
        case GbaKey::Down:   return "DOWN";
        case GbaKey::R:      return "R";
        case GbaKey::L:      return "L";
    }
    return "?";
}

enum class InputDeviceType : uint8_t {
    KeyboardKey,
    GamepadButton,
    GamepadAxisPositive,
    GamepadAxisNegative
};

struct PhysicalInputBinding {
    InputDeviceType deviceType{InputDeviceType::KeyboardKey};
    uint32_t        code{0};
    float           axisThreshold{0.45f};

    bool operator==(const PhysicalInputBinding& other) const noexcept {
        return deviceType == other.deviceType && code == other.code;
    }
};

class InputManager {
public:
    static constexpr uint16_t ALL_RELEASED_MASK = 0x03FF;

    InputManager();

    void loadDefaultBindings();
    void clearBindingsForKey(GbaKey targetKey);
    void bindInput(PhysicalInputBinding physical, GbaKey targetKey);

    void onKeyboardEvent(uint32_t scancode, bool pressed);
    void onGamepadButtonEvent(uint32_t buttonId, bool pressed);
    void onGamepadAxisEvent(uint32_t axisId, float normalizedValue);
    void setVirtualKeyState(GbaKey key, bool pressed) noexcept;

    // Retorna o valor de KEYINPUT (0x04000130) onde bit 0 = Pressionado e bit 1 = Solto
    [[nodiscard]] uint16_t readKeyInput() const noexcept {
        return m_keyInputRegister.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isKeyPressed(GbaKey key) const noexcept {
        return (readKeyInput() & static_cast<uint16_t>(key)) == 0;
    }

    [[nodiscard]] uint16_t readKeyCnt() const noexcept { return m_keyCntRegister; }
    void writeKeyCnt(uint16_t value) noexcept { m_keyCntRegister = value; }

    [[nodiscard]] bool shouldTriggerKeypadIrq() const noexcept;

private:
    mutable std::mutex m_bindingMutex;
    std::unordered_map<GbaKey, std::vector<PhysicalInputBinding>> m_bindings;

    std::unordered_map<uint32_t, bool>  m_keyboardState;
    std::unordered_map<uint32_t, bool>  m_gamepadButtonState;
    std::unordered_map<uint32_t, float> m_gamepadAxisState;

    uint16_t m_directVirtualMask{0};
    std::atomic<uint16_t> m_keyInputRegister{ALL_RELEASED_MASK};
    uint16_t m_keyCntRegister{0};

    void rebuildKeyInputRegister();
    [[nodiscard]] bool isBindingActive(const PhysicalInputBinding& binding) const noexcept;
};

} // namespace gba::core

