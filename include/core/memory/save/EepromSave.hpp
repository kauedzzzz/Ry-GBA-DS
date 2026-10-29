#pragma once
#include "core/memory/save/IBackupMedia.hpp"
#include <vector>

namespace gba::core {

class EepromSave final : public IBackupMedia {
public:
    explicit EepromSave(bool is8KB);

    uint8_t readByte(uint32_t address) override;
    void writeByte(uint32_t address, uint8_t value) override;
    uint16_t readHalfWord(uint32_t address) override;
    void writeHalfWord(uint32_t address, uint16_t value) override;

    bool loadFromFile(const std::filesystem::path& path) override;
    bool saveToFile(const std::filesystem::path& path) const override;

    [[nodiscard]] SaveType getType() const noexcept override {
        return m_is8KB ? SaveType::Eeprom8K : SaveType::Eeprom512B;
    }
    [[nodiscard]] bool isDirty() const noexcept override { return m_dirty; }
    void clearDirty() noexcept override { m_dirty = false; }

    void serializeState(SaveStateWriter& out) const override;
    bool deserializeState(SaveStateReader& in) override;

private:
    bool m_is8KB{false};
    bool m_dirty{false};
    uint8_t m_addressBits{6}; // 6 bits para 512B (64 blocos de 8B) ou 14 bits para 8KB (1024 blocos de 8B)

    std::vector<uint8_t> m_bitBuffer;
    uint16_t m_currentBlockAddress{0};
    uint8_t  m_readBitsRemaining{0};
    uint64_t m_readShiftReg{0};

    std::vector<uint8_t> m_data;

    void processBitstream();
};

} // namespace gba::core

