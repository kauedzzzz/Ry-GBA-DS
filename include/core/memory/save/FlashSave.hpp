#pragma once
#include "core/memory/save/IBackupMedia.hpp"
#include <vector>

namespace gba::core {

class FlashSave final : public IBackupMedia {
public:
    explicit FlashSave(bool is128KB);

    uint8_t readByte(uint32_t address) override;
    void writeByte(uint32_t address, uint8_t value) override;
    bool loadFromFile(const std::filesystem::path& path) override;
    bool saveToFile(const std::filesystem::path& path) const override;

    [[nodiscard]] SaveType getType() const noexcept override {
        return m_is128KB ? SaveType::Flash128K : SaveType::Flash64K;
    }
    [[nodiscard]] bool isDirty() const noexcept override { return m_dirty; }
    void clearDirty() noexcept override { m_dirty = false; }

    void setTraceEnabled(bool enabled) noexcept override { m_traceEnabled = enabled; }
    [[nodiscard]] bool isChipIdMode() const noexcept { return m_chipIdMode; }
    [[nodiscard]] uint8_t currentBank() const noexcept { return m_currentBank; }

    void serializeState(SaveStateWriter& out) const override;
    bool deserializeState(SaveStateReader& in) override;

private:
    enum class State {
        Ready,
        Cmd1,
        Cmd2,
        EraseCmd1,
        EraseCmd2,
        WriteSingleByte,
        BankSwitch
    };

    bool m_is128KB{false};
    bool m_chipIdMode{false};
    bool m_dirty{false};
    bool m_traceEnabled{false};
    uint8_t m_currentBank{0};          // 0 ou 1 (para Flash 128KB via comando 0xB0)
    State m_state{State::Ready};
    uint16_t m_manufacturerDeviceId{0}; // 0x1CC2 (64KB Macronix) ou 0x1362 (128KB Sanyo)
    std::vector<uint8_t> m_data;
};

} // namespace gba::core

