#pragma once
#include "core/memory/save/IBackupMedia.hpp"
#include <memory>
#include <span>

namespace gba::core {

class SaveDetector {
public:
    static SaveType detect(std::span<const uint8_t> romData) noexcept;
    static std::unique_ptr<IBackupMedia> createMedia(SaveType type);
};

} // namespace gba::core

