#pragma once
#include <cstdint>
#include <vector>

namespace gba::core {

class BuiltInDemoRom {
public:
    // Gera uma ROM GBA funcional completa em código de máquina ARM7TDMI (32-bit ARM + 16-bit THUMB)
    // que exercita a CPU, PPU (Modo 3 + OAM Sprites), APU (PSG Canais 1, 2 e 4),
    // KEYINPUT (0x04000130) para todos os 10 botões e Save RAM (0x0E000000).
    static std::vector<uint8_t> build();
};

} // namespace gba::core

