#include "core/BuiltInDemoRom.hpp"
#include <cstring>

namespace gba::core {

std::vector<uint8_t> BuiltInDemoRom::build() {
    std::vector<uint8_t> rom(128 * 1024, 0x00);

    auto write16 = [&](uint32_t offset, uint16_t val) {
        if (offset + 1 < rom.size()) {
            rom[offset + 0] = static_cast<uint8_t>(val & 0xFF);
            rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        }
    };
    auto write32 = [&](uint32_t offset, uint32_t val) {
        if (offset + 3 < rom.size()) {
            rom[offset + 0] = static_cast<uint8_t>(val & 0xFF);
            rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            rom[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
            rom[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
        }
    };

    // 1. Cabeçalho Padrão GBA (0x08000000)
    write32(0x0000, 0xEA00002Eu); // B 0x080000C0
    std::memcpy(&rom[0x00A0], "GBA CORE LAB", 12);
    std::memcpy(&rom[0x00AC], "AGBE", 4);
    std::memcpy(&rom[0x00B0], "01", 2);
    rom[0x00B2] = 0x96;

    // Assinatura SDK para o SaveDetector detectar SRAM automaticamente
    std::memcpy(&rom[0x1FFE0], "SRAM_V113", 9);

    // 2. Dados pré-renderizados de fundo (240x160 RGB555) na ROM em 0x08002000
    // Desenha um painel visual de GBA estilizado na ROM para DMA3 / cópia rápida
    const uint32_t bgRomOffset = 0x2000;
    for (uint32_t y = 0; y < 160; ++y) {
        for (uint32_t x = 0; x < 240; ++x) {
            // Gradiente escuro azul-violeta tecnológico
            uint16_t r = static_cast<uint16_t>(2 + (y / 16));
            uint16_t g = static_cast<uint16_t>(3 + (y / 12));
            uint16_t b = static_cast<uint16_t>(10 + (y / 8));

            // Grade sutil a cada 16 pixels
            if ((x % 16 == 0) || (y % 16 == 0)) {
                r += 2; g += 3; b += 4;
            }

            // Moldura do corpo do Game Boy Advance no centro (x: 36..204, y: 24..136)
            if (x >= 36 && x <= 204 && y >= 24 && y <= 136) {
                if (x == 36 || x == 204 || y == 24 || y == 136) {
                    r = 28; g = 28; b = 31; // Borda clara
                } else {
                    r = 7; g = 6; b = 18;   // Carcaça Indigo GBA
                }
            }

            const uint16_t color555 = static_cast<uint16_t>((b << 10) | (g << 5) | r);
            write16(bgRomOffset + (y * 240u + x) * 2u, color555);
        }
    }

    // 3. Programa em Código de Máquina ARM7TDMI em 0x080000C0
    uint32_t pc = 0x00C0;
    auto emit = [&](uint32_t instr) {
        write32(pc, instr);
        pc += 4;
    };

    // Pool de Constantes em 0x08000900 (usado via LDR Rd, [PC, #offset])
    uint32_t litPool = 0x0900;
    std::vector<std::pair<uint32_t, uint32_t>> poolEntries;
    auto ldrImm32 = [&](uint8_t rd, uint32_t value) {
        uint32_t targetLitAddr = 0;
        bool found = false;
        for (const auto& [val, addr] : poolEntries) {
            if (val == value) {
                targetLitAddr = addr;
                found = true;
                break;
            }
        }
        if (!found) {
            targetLitAddr = litPool;
            write32(litPool, value);
            poolEntries.emplace_back(value, litPool);
            litPool += 4;
        }
        const uint32_t instrAddr = pc;
        const uint32_t offset    = targetLitAddr - (instrAddr + 8u);
        emit(0xE59F0000u | (static_cast<uint32_t>(rd) << 12) | (offset & 0xFFFu));
    };

    // Inicializa Registradores Base:
    // R4 = 0x04000000 (MMIO Base)
    // R5 = 0x06000000 (VRAM Base)
    // R6 = 0x07000000 (OAM Base)
    // R7 = 0x0E000000 (SRAM Base)
    ldrImm32(4, 0x04000000u);
    ldrImm32(5, 0x06000000u);
    ldrImm32(6, 0x07000000u);
    ldrImm32(7, 0x0E000000u);

    // Configura DISPCNT (0x04000000) = 0x1443 (Mode 3 + BG2 + OBJ + 1D OBJ Mapping)
    ldrImm32(0, 0x00001443u);
    emit(0xE1C400B0u); // STRH R0, [R4, #0]

    // Configura Áudio PSG (SOUNDCNT_X = 0x80, SOUNDCNT_L = 0xFF77, SOUNDCNT_H = 0x0002)
    ldrImm32(0, 0x00000080u);
    emit(0xE1C408B4u); // STRH R0, [R4, #0x84] (SOUNDCNT_X)
    ldrImm32(0, 0x0000FF77u);
    emit(0xE1C408B0u); // STRH R0, [R4, #0x80] (SOUNDCNT_L)
    ldrImm32(0, 0x00000002u);
    emit(0xE1C408B2u); // STRH R0, [R4, #0x82] (SOUNDCNT_H)

    // Configura Paleta de Sprite OAM (0x05000202 = Amarelo Ouro, 0x05000204 = Ciano)
    ldrImm32(1, 0x05000200u);
    ldrImm32(0, 0x000003FFu); // Amarelo vibrante RGB555
    emit(0xE1C100B2u);        // STRH R0, [R1, #2]
    ldrImm32(0, 0x00007FE0u); // Ciano vibrante RGB555
    emit(0xE1C100B4u);        // STRH R0, [R1, #4]

    // Preenche Tile #512 de Sprite (0x06014000) com um ícone 8x8 de cursor/estrela
    ldrImm32(1, 0x06014000u);
    ldrImm32(0, 0x11222211u);
    for (uint32_t i = 0; i < 8; ++i) {
        emit(0xE5810000u | (i * 4u)); // STR R0, [R1, #i*4]
    }

    // Oculta os Sprites 1..127 na OAM (attr0 = 0x0200)
    ldrImm32(0, 0x00000200u);
    for (uint32_t i = 1; i < 16; ++i) {
        emit(0xE1C600B0u | (((i * 8u) & 0xF0u) << 4) | ((i * 8u) & 0x0Fu)); // STRH R0, [R6, #i*8]
    }

    // Posição inicial do Cursor Sprite (R8 = X = 116, R9 = Y = 76, R10 = Frame Counter = 0)
    ldrImm32(8, 116u);
    ldrImm32(9, 76u);
    ldrImm32(10, 0u);

    // Dispara Nota Musical de Boot no Canal 1 (Square 1: SOUND1CNT_H=0xF780, SOUND1CNT_X=0x86D6)
    ldrImm32(0, 0x0000F780u);
    emit(0xE1C406B2u); // STRH R0, [R4, #0x62]
    ldrImm32(0, 0x000086D6u);
    emit(0xE1C406B4u); // STRH R0, [R4, #0x64]

    // =========================================================================
    // LOOP PRINCIPAL DE QUADRO (Executado pela CPU ARM7TDMI)
    // =========================================================================
    const uint32_t mainLoopAddr = pc;

    // 1. Aguarda início do VBlank (VCOUNT == 160) via SWI 0x05 (VBlankIntrWait / Halt)
    // Primeiro habilita VBlank IRQ em DISPSTAT e IE/IME para que o Halt acorde no VBlank
    ldrImm32(0, 0x00000008u);
    emit(0xE1C400B4u); // STRH R0, [R4, #0x04] (DISPSTAT VBlank IRQ Enable)
    ldrImm32(0, 0x00000001u);
    ldrImm32(1, 0x04000200u);
    emit(0xE1C100B0u); // STRH R0, [R1, #0] (IE = VBlank)
    emit(0xEF050000u); // SWI 0x05 (VBlankIntrWait)

    // 2. Copia o painel de fundo da ROM (0x08002000) para a VRAM (0x06000000) via DMA3 (19200 words de 32 bits)
    ldrImm32(0, 0x08002000u);
    emit(0xE58400D4u); // STR R0, [R4, #0xD4] (DMA3SAD)
    emit(0xE58450D8u); // STR R5, [R4, #0xD8] (DMA3DAD = 0x06000000)
    ldrImm32(0, 0x84004B00u); // Enable (bit 31) + 32-bit (bit 26) + 19200 words (0x4B00)
    emit(0xE58400DCu); // STR R0, [R4, #0xDC] (DMA3CNT)

    // 3. Lê o registrador de hardware KEYINPUT (0x04000130) para R11
    ldrImm32(1, 0x04000130u);
    emit(0xE1D1B0B0u); // LDRH R11, [R1, #0] (Bits em 0 = Pressionado!)

    // Move o sprite conforme o D-Pad (Right=bit 4, Left=bit 5, Up=bit 6, Down=bit 7)
    // Testa Right (bit 4 == 0): TST R11, #0x10 -> ADDEQ R8, R8, #2
    emit(0xE31B0010u); // TST R11, #0x10
    emit(0x02888002u); // ADDEQ R8, R8, #2
    // Testa Left (bit 5 == 0): TST R11, #0x20 -> SUBEQ R8, R8, #2
    emit(0xE31B0020u); // TST R11, #0x20
    emit(0x02488002u); // SUBEQ R8, R8, #2
    // Testa Up (bit 6 == 0): TST R11, #0x40 -> SUBEQ R9, R9, #2
    emit(0xE31B0040u); // TST R11, #0x40
    emit(0x02499002u); // SUBEQ R9, R9, #2
    // Testa Down (bit 7 == 0): TST R11, #0x80 -> ADDEQ R9, R9, #2
    emit(0xE31B0080u); // TST R11, #0x80
    emit(0x02899002u); // ADDEQ R9, R9, #2

    // Mantém R8 em [8..224] e R9 em [8..144]
    emit(0xE3580008u); // CMP R8, #8
    emit(0xB3A08008u); // MOVLT R8, #8
    emit(0xE35800E0u); // CMP R8, #224
    emit(0xC3A080E0u); // MOVGT R8, #224
    emit(0xE3590008u); // CMP R9, #8
    emit(0xB3A09008u); // MOVLT R9, #8
    emit(0xE3590090u); // CMP R9, #144
    emit(0xC3A09090u); // MOVGT R9, #144

    // Salva coordenadas atuais (R8, R9) na SRAM (0x0E000000 e 0x0E000001)
    emit(0xE5C78000u); // STRB R8, [R7, #0]
    emit(0xE5C79001u); // STRB R9, [R7, #1]

    // Atualiza OAM Sprite #0 (attr0 = Y, attr1 = X, attr2 = Tile 512)
    emit(0xE1C690B0u); // STRH R9, [R6, #0] (OAM0 Attr0 = Y)
    emit(0xE1C680B2u); // STRH R8, [R6, #2] (OAM0 Attr1 = X)
    ldrImm32(0, 0x00000200u); // Tile index 512 (0x06014000)
    emit(0xE1C600B4u); // STRH R0, [R6, #4] (OAM0 Attr2)

    // 4. Desenha indicadores luminosos na VRAM para cada um dos 10 botões de KEYINPUT
    struct ButtonRect {
        uint16_t mask;
        uint32_t x;
        uint32_t y;
        uint16_t noteFreq;
    };
    const ButtonRect buttons[10] = {
        {1 << 9,  44,  30, 0x85AC}, // L
        {1 << 8, 176,  30, 0x860C}, // R
        {1 << 6,  62,  58, 0x8642}, // UP
        {1 << 7,  62,  94, 0x8672}, // DOWN
        {1 << 5,  44,  76, 0x869E}, // LEFT
        {1 << 4,  80,  76, 0x86C8}, // RIGHT
        {1 << 2,  98, 114, 0x8706}, // SELECT
        {1 << 3, 126, 114, 0x8724}, // START
        {1 << 1, 154,  84, 0x8740}, // B
        {1 << 0, 178,  68, 0x8758}  // A
    };

    for (const auto& btn : buttons) {
        // Cor padrão solto (Cinza Azulado = 0x294A), Cor pressionado (Verde Neon = 0x03E0)
        ldrImm32(2, 0x294A294Au);
        ldrImm32(0, btn.mask);
        emit(0xE11B0000u); // TST R11, R0 (Se bit == 0, Z=1 -> botão pressionado!)
        ldrImm32(3, 0x03E003E0u);
        emit(0x01A02003u); // MOVEQ R2, R3

        // Se pressionado, toca nota correspondente no Canal 2 (Square 2)
        ldrImm32(0, 0x0000F480u);
        emit(0x01C406B8u); // STRHEQ R0, [R4, #0x68] (SOUND2CNT_L)
        ldrImm32(0, btn.noteFreq);
        emit(0x01C406BCu); // STRHEQ R0, [R4, #0x6C] (SOUND2CNT_H)

        // Preenche bloco de 16x12 pixels na VRAM em (btn.x, btn.y)
        for (uint32_t dy = 0; dy < 12; dy += 2) {
            const uint32_t vramAddr = 0x06000000u + ((btn.y + dy) * 240u + btn.x) * 2u;
            ldrImm32(1, vramAddr);
            emit(0xE5812000u); // STR R2, [R1, #0]
            emit(0xE5812004u); // STR R2, [R1, #4]
            emit(0xE5812008u); // STR R2, [R1, #8]
            emit(0xE581200Cu); // STR R2, [R1, #12]
        }
    }

    // 5. Incrementa contador de frames (R10) e toca arpeggio suave a cada 32 frames no Canal 1
    emit(0xE28AA001u); // ADD R10, R10, #1
    emit(0xE31A001Fu); // TST R10, #0x1F
    ldrImm32(0, 0x0000A580u);
    emit(0x01C406B2u); // STRHEQ R0, [R4, #0x62]
    ldrImm32(0, 0x00008680u);
    emit(0x01C406B4u); // STRHEQ R0, [R4, #0x64]

    // Salta de volta para mainLoopAddr
    const int32_t branchOffset = (static_cast<int32_t>(mainLoopAddr) - static_cast<int32_t>(pc + 8u)) >> 2;
    emit(0xEA000000u | (static_cast<uint32_t>(branchOffset) & 0x00FFFFFFu));

    return rom;
}

} // namespace gba::core
