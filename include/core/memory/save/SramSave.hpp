#pragma once
#include "core/memory/save/IBackupMedia.hpp"
#include <array>

namespace gba::core {

class SramSave final : public IBackupMedia {
public:
    static constexpr std::size_t SRAM_SIZE = 32 * 1024; // 32 KB

    SramSave();

    uint8_t readByte(uint32_t address) override;
    void writeByte(uint32_t address, uint8_t value) override;
    bool loadFromFile(const std::filesystem::path& path) override;
    bool saveToFile(const std::filesystem::path& path) const override;

    [[nodiscard]] SaveType getType() const noexcept override { return SaveType::Sram32K; }
    [[nodiscard]] bool isDirty() const noexcept override { return m_dirty; }
    void clearDirty() noexcept override { m_dirty = false; }

    void serializeState(SaveStateWriter& out) const override;
    bool deserializeState(SaveStateReader& in) override;

private:
    std::array<uint8_t, SRAM_SIZE> m_data{};
    bool m_dirty{false};
};

} // namespace gba::core

