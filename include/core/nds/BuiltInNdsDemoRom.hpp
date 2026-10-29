#pragma once
#include <cstdint>
#include <vector>

namespace gba::core::nds {

// =============================================================================
// Gerador de ROM .nds Embutida (RyūDS Dual-Screen 2D + 3D + Stylus + IPC + SPU)
// Permite validar e demonstrar todo o pipeline Nintendo DS sem arquivos externos.
// =============================================================================
class BuiltInNdsDemoRom {
public:
    [[nodiscard]] static std::vector<uint8_t> build();
};

} // namespace gba::core::nds

