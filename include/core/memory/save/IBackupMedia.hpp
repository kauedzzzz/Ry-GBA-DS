#pragma once
#include "core/SaveStateStream.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace gba::core {

enum class SaveType {
    AutoDetect,
    None,
    Sram32K,       // 32 KB (0x0E000000 - 0x0E007FFF)
    Eeprom512B,    // 512 Bytes (Endereçamento serial de 6 bits via DMA3)
    Eeprom8K,      // 8 KB (Endereçamento serial de 14 bits via DMA3)
    Flash64K,      // 64 KB (1 banco, Macronix/Panasonic)
    Flash128K      // 128 KB (2 bancos de 64 KB, Sanyo/Macronix - Pokémon Emerald/FireRed)
};

constexpr std::string_view saveTypeToString(SaveType type) noexcept {
    switch (type) {
        case SaveType::AutoDetect: return "AutoDetect";
        case SaveType::None:       return "None";
        case SaveType::Sram32K:    return "SRAM 32KB";
        case SaveType::Eeprom512B: return "EEPROM 512B";
        case SaveType::Eeprom8K:   return "EEPROM 8KB";
        case SaveType::Flash64K:   return "FLASH 64KB";
        case SaveType::Flash128K:  return "FLASH 128KB";
    }
    return "Unknown";
}

class IBackupMedia {
public:
    virtual ~IBackupMedia() = default;

    virtual uint8_t readByte(uint32_t address) = 0;
    virtual void writeByte(uint32_t address, uint8_t value) = 0;

    // Usado pela EEPROM (transferência serial bit-a-bit via DMA3 em 16 bits)
    virtual uint16_t readHalfWord(uint32_t address) { return readByte(address); }
    virtual void writeHalfWord(uint32_t address, uint16_t value) {
        writeByte(address, static_cast<uint8_t>(value & 0xFF));
    }

    virtual bool loadFromFile(const std::filesystem::path& path) = 0;
    virtual bool saveToFile(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual SaveType getType() const noexcept = 0;
    [[nodiscard]] virtual bool isDirty() const noexcept = 0;
    virtual void clearDirty() noexcept = 0;
    virtual void setTraceEnabled(bool /*enabled*/) noexcept {}
    virtual void serializeState(SaveStateWriter& /*out*/) const {}
    virtual bool deserializeState(SaveStateReader& /*in*/) { return true; }
};

} // namespace gba::core

