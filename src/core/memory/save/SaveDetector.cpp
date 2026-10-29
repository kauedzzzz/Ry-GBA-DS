#include "core/memory/save/SaveDetector.hpp"
#include "core/memory/save/EepromSave.hpp"
#include "core/memory/save/FlashSave.hpp"
#include "core/memory/save/SramSave.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace gba::core {

namespace {
bool containsSignature(std::span<const uint8_t> data, std::string_view sig) noexcept {
    if (data.size() < sig.size()) return false;
    const auto it = std::search(
        data.begin(), data.end(),
        reinterpret_cast<const uint8_t*>(sig.data()),
        reinterpret_cast<const uint8_t*>(sig.data()) + sig.size()
    );
    return it != data.end();
}
} // namespace

SaveType SaveDetector::detect(std::span<const uint8_t> romData) noexcept {
    if (containsSignature(romData, "FLASH1M_V"))  return SaveType::Flash128K;
    if (containsSignature(romData, "FLASH512_V")) return SaveType::Flash64K;
    if (containsSignature(romData, "FLASH_V"))    return SaveType::Flash64K;
    if (containsSignature(romData, "SRAM_F_V"))   return SaveType::Sram32K;
    if (containsSignature(romData, "SRAM_V"))     return SaveType::Sram32K;
    if (containsSignature(romData, "EEPROM_V"))   return SaveType::Eeprom8K;
    // Fallback padrão para máxima compatibilidade com homebrews e ROMs sem string SDK
    return SaveType::Sram32K;
}

std::unique_ptr<IBackupMedia> SaveDetector::createMedia(SaveType type) {
    switch (type) {
        case SaveType::Sram32K:    return std::make_unique<SramSave>();
        case SaveType::Flash64K:   return std::make_unique<FlashSave>(false);
        case SaveType::Flash128K:  return std::make_unique<FlashSave>(true);
        case SaveType::Eeprom512B: return std::make_unique<EepromSave>(false);
        case SaveType::Eeprom8K:   return std::make_unique<EepromSave>(true);
        default:                   return std::make_unique<SramSave>();
    }
}

} // namespace gba::core

