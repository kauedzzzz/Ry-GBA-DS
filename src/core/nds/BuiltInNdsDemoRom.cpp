#include "core/nds/BuiltInNdsDemoRom.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace gba::core::nds {

namespace {

void writeLe16(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>(v >> 8);
}

void writeLe32(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t crc16Nds(const uint8_t* data, std::size_t len) noexcept {
    uint16_t crc = 0xFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001u);
            else          crc = static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

constexpr uint16_t bgr555(uint8_t r8, uint8_t g8, uint8_t b8) noexcept {
    return static_cast<uint16_t>(((r8 >> 3) & 31u) | (((g8 >> 3) & 31u) << 5) | (((b8 >> 3) & 31u) << 10));
}

// Mini-assembler ARM32 para gerar código ARM9 e ARM7 limpo e livre de erros de encoding
struct ArmEmitter {
    std::vector<uint32_t> code;
    struct PoolFixup {
        std::size_t instrIdx;
        uint32_t    rd;
        uint32_t    value;
    };
    std::vector<PoolFixup> pool;

    std::size_t pos() const noexcept { return code.size(); }

    void emit(uint32_t op) { code.push_back(op); }

    // Carrega constante arbitrária de 32 bits via literal pool relativo a PC
    void ldrConst(uint32_t rd, uint32_t value) {
        pool.push_back({code.size(), rd, value});
        code.push_back(0xE59F0000u | ((rd & 15u) << 12));
    }

    void movImm(uint32_t rd, uint8_t imm8, uint8_t rotRightEven = 0, uint8_t cond = 0xEu) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit((static_cast<uint32_t>(cond) << 28) | 0x03A00000u | ((rd & 15u) << 12) | rotField | imm8);
    }

    void movReg(uint32_t rd, uint32_t rm, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x01A00000u | ((rd & 15u) << 12) | (rm & 15u));
    }

    void movLslImm(uint32_t rd, uint32_t rm, uint8_t shift, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x01A00000u | ((rd & 15u) << 12) | ((shift & 31u) << 7) | (rm & 15u));
    }

    void movLsrImm(uint32_t rd, uint32_t rm, uint8_t shift, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x01A00020u | ((rd & 15u) << 12) | ((shift & 31u) << 7) | (rm & 15u));
    }

    void addImm(uint32_t rd, uint32_t rn, uint8_t imm8, uint8_t rotRightEven = 0, uint8_t cond = 0xEu) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit((static_cast<uint32_t>(cond) << 28) | 0x02800000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | rotField | imm8);
    }

    void subImm(uint32_t rd, uint32_t rn, uint8_t imm8, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x02400000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | imm8);
    }

    void addReg(uint32_t rd, uint32_t rn, uint32_t rm, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x00800000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (rm & 15u));
    }

    void andImm(uint32_t rd, uint32_t rn, uint8_t imm8, uint8_t rotRightEven = 0, uint8_t cond = 0xEu) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit((static_cast<uint32_t>(cond) << 28) | 0x02000000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | rotField | imm8);
    }

    void orrRegLsl(uint32_t rd, uint32_t rn, uint32_t rm, uint8_t shift, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x01800000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | ((shift & 31u) << 7) | (rm & 15u));
    }

    void orrImm(uint32_t rd, uint32_t rn, uint8_t imm8, uint8_t rotRightEven = 0, uint8_t cond = 0xEu) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit((static_cast<uint32_t>(cond) << 28) | 0x03800000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | rotField | imm8);
    }

    void cmpImm(uint32_t rn, uint8_t imm8, uint8_t rotRightEven = 0) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit(0xE3500000u | ((rn & 15u) << 16) | rotField | imm8);
    }

    void cmpReg(uint32_t rn, uint32_t rm) {
        emit(0xE1500000u | ((rn & 15u) << 16) | (rm & 15u));
    }

    void tstImm(uint32_t rn, uint8_t imm8, uint8_t rotRightEven = 0) {
        const uint32_t rotField = ((rotRightEven / 2u) & 0x0Fu) << 8;
        emit(0xE3100000u | ((rn & 15u) << 16) | rotField | imm8);
    }

    void str32(uint32_t rd, uint32_t rn, uint16_t off12 = 0, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x05800000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (off12 & 0xFFFu));
    }

    void ldr32(uint32_t rd, uint32_t rn, uint16_t off12 = 0, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x05900000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (off12 & 0xFFFu));
    }

    void strb(uint32_t rd, uint32_t rn, uint16_t off12 = 0, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x05C00000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (off12 & 0xFFFu));
    }

    void ldrb(uint32_t rd, uint32_t rn, uint16_t off12 = 0, uint8_t cond = 0xEu) {
        emit((static_cast<uint32_t>(cond) << 28) | 0x05D00000u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (off12 & 0xFFFu));
    }

    void strh(uint32_t rd, uint32_t rn, uint8_t off8 = 0, uint8_t cond = 0xEu) {
        const uint32_t hi = (off8 >> 4) & 0x0Fu;
        const uint32_t lo = off8 & 0x0Fu;
        emit((static_cast<uint32_t>(cond) << 28) | 0x01C000B0u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (hi << 8) | lo);
    }

    void ldrh(uint32_t rd, uint32_t rn, uint8_t off8 = 0, uint8_t cond = 0xEu) {
        const uint32_t hi = (off8 >> 4) & 0x0Fu;
        const uint32_t lo = off8 & 0x0Fu;
        emit((static_cast<uint32_t>(cond) << 28) | 0x01D000B0u | ((rn & 15u) << 16) | ((rd & 15u) << 12) | (hi << 8) | lo);
    }

    std::size_t branchPlaceholder(uint8_t cond = 0xEu, bool link = false) {
        const std::size_t idx = code.size();
        emit((static_cast<uint32_t>(cond) << 28) | (link ? 0x0B000000u : 0x0A000000u));
        return idx;
    }

    void branchTo(std::size_t targetIdx, uint8_t cond = 0xEu, bool link = false) {
        const int32_t rel = static_cast<int32_t>(targetIdx) - static_cast<int32_t>(code.size() + 2u);
        emit((static_cast<uint32_t>(cond) << 28) | (link ? 0x0B000000u : 0x0A000000u) |
             (static_cast<uint32_t>(rel) & 0x00FFFFFFu));
    }

    void patchBranch(std::size_t branchIdx, std::size_t targetIdx) {
        const int32_t rel = static_cast<int32_t>(targetIdx) - static_cast<int32_t>(branchIdx + 2u);
        code[branchIdx] = (code[branchIdx] & 0xFF000000u) | (static_cast<uint32_t>(rel) & 0x00FFFFFFu);
    }

    std::vector<uint8_t> finalize() {
        for (const auto& f : pool) {
            const std::size_t litIdx = code.size();
            code.push_back(f.value);
            const int32_t byteDiff = static_cast<int32_t>((litIdx - (f.instrIdx + 2u)) * 4u);
            if (byteDiff >= 0) {
                code[f.instrIdx] = 0xE59F0000u | (f.rd << 12) | (static_cast<uint32_t>(byteDiff) & 0xFFFu);
            } else {
                code[f.instrIdx] = 0xE51F0000u | (f.rd << 12) | (static_cast<uint32_t>(-byteDiff) & 0xFFFu);
            }
        }
        std::vector<uint8_t> bytes(code.size() * 4u);
        for (std::size_t i = 0; i < code.size(); ++i) {
            writeLe32(&bytes[i * 4u], code[i]);
        }
        return bytes;
    }
};

std::vector<uint8_t> buildArm7Binary() {
    ArmEmitter a;
    // R4 = Base I/O 0x04000000
    a.ldrConst(4, 0x04000000u);

    // 1. Habilita IPCFIFOCNT (0x04000184) = 0x8008 (Enable + Clear Send FIFO)
    a.ldrConst(0, 0x00008008u);
    a.ldrConst(1, 0x04000184u);
    a.strh(0, 1, 0);

    // 2. Habilita SPU 16 Canais: SOUNDCNT (0x04000500) = 0x807F
    a.ldrConst(1, 0x04000500u);
    a.ldrConst(0, 0x0000807Fu);
    a.strh(0, 1, 0);

    // Canal 8 (PSG Square 440Hz, Pan=40, Duty=50%): 0x04000480
    a.ldrConst(1, 0x04000480u);
    a.ldrConst(0, 0xFFF4u); // TMR = 65536 - (16756991 / (8 * 440)) ~= 0xED00
    a.ldrConst(0, 0xED00u);
    a.strh(0, 1, 8);
    a.ldrConst(0, 0xE4280030u); // Enable(31) | PSG(29..30=3) | Duty=4(24..26) | Pan=40(16..22) | Vol=48
    a.str32(0, 1, 0);

    // Canal 9 (PSG Square 659Hz, Pan=88, Duty=37.5%): 0x04000490
    a.ldrConst(1, 0x04000490u);
    a.ldrConst(0, 0xF350u);
    a.strh(0, 1, 8);
    a.ldrConst(0, 0xE3580028u);
    a.str32(0, 1, 0);

    // Loop principal ARM7: Lê EXTKEYIN, Touchscreen TSC2046 (X, Y) e Microfone via SPI e envia por IPCFIFO
    const std::size_t loopTop = a.pos();

    // R5 = EXTKEYIN (0x04000136)
    a.ldrConst(1, 0x04000136u);
    a.ldrh(5, 1, 0);

    // Lê TSC2046 Canal 5 (X-Position = cmd 0xD0) em R6 (0..255)
    a.ldrConst(1, 0x040001C0u); // SPICNT / SPIDATA (+2)
    a.ldrConst(0, 0x8A00u);     // Enable(15) | Device=2 Touch(8..9) | Hold CS(11)
    a.strh(0, 1, 0);
    a.movImm(0, 0xD0u);
    a.strb(0, 1, 2);
    a.movImm(0, 0x00u);
    a.strb(0, 1, 2);
    a.ldrb(6, 1, 2);            // High byte (bits 11..5 em 6..0)
    a.ldrConst(0, 0x8200u);     // Solta Hold CS no último byte
    a.strh(0, 1, 0);
    a.movImm(0, 0x00u);
    a.strb(0, 1, 2);
    a.ldrb(2, 1, 2);            // Low byte (bits 4..0 em 7..3)
    // adc12 = (R6 << 5) | (R2 >> 3); pixelX = adc12 >> 4 = (R6 << 1) | (R2 >> 7)
    a.movLslImm(6, 6, 1);
    a.movLsrImm(2, 2, 7);
    a.orrRegLsl(6, 6, 2, 0);
    a.andImm(6, 6, 0xFFu);

    // Lê TSC2046 Canal 1 (Y-Position = cmd 0x90) em R7 (0..191)
    a.ldrConst(0, 0x8A00u);
    a.strh(0, 1, 0);
    a.movImm(0, 0x90u);
    a.strb(0, 1, 2);
    a.movImm(0, 0x00u);
    a.strb(0, 1, 2);
    a.ldrb(7, 1, 2);
    a.ldrConst(0, 0x8200u);
    a.strh(0, 1, 0);
    a.movImm(0, 0x00u);
    a.strb(0, 1, 2);
    a.ldrb(2, 1, 2);
    a.movLslImm(7, 7, 1);
    a.movLsrImm(2, 2, 7);
    a.orrRegLsl(7, 7, 2, 0);
    a.andImm(7, 7, 0xFFu);

    // Lê TSC2046 Canal 6 (Microfone AUX = cmd 0xE0) em R8
    a.ldrConst(0, 0x8A00u);
    a.strh(0, 1, 0);
    a.movImm(0, 0xE0u);
    a.strb(0, 1, 2);
    a.ldrConst(0, 0x8200u);
    a.strh(0, 1, 0);
    a.movImm(0, 0x00u);
    a.strb(0, 1, 2);
    a.ldrb(8, 1, 2); // Em silêncio = 0x40 (0x800 >> 5); sopro oscila longe de 0x40

    // Monta pacote IPC: R9 = (R5 << 16) | (R7 << 8) | R6; bit 31 = Sopro no Microfone
    a.movReg(9, 6);
    a.orrRegLsl(9, 9, 7, 8);
    a.orrRegLsl(9, 9, 5, 16);
    a.cmpImm(8, 0x40u);
    a.orrImm(9, 9, 0x80u, 8, 0x1u); // cond NE (0x1): orr R9, R9, #0x80000000

    // Envia R9 para IPCFIFOSEND (0x04000188) se Send FIFO não estiver cheia (bit 1 de IPCFIFOCNT == 0)
    a.ldrConst(1, 0x04000184u);
    a.ldrh(0, 1, 0);
    a.tstImm(0, 0x02u);
    a.str32(9, 1, 4, 0x0u); // cond EQ (0x0): str R9, [0x04000188]

    // Aguarda próxima scanline VBlank (VCOUNT == 192) para cadenciar ~60 pacotes/s sem saturar a FIFO
    a.ldrConst(1, 0x04000006u);
    const std::size_t waitNot192 = a.pos();
    a.ldrh(0, 1, 0);
    a.cmpImm(0, 192u);
    a.branchTo(waitNot192, 0x0u); // BEQ waitNot192

    const std::size_t wait192 = a.pos();
    a.ldrh(0, 1, 0);
    a.cmpImm(0, 192u);
    a.branchTo(wait192, 0x1u); // BNE wait192

    a.branchTo(loopTop);
    return a.finalize();
}

std::vector<uint8_t> buildArm9Binary() {
    ArmEmitter a;

    // 1. POWCNT1 (0x04000304) = 0x820F (LCDs + Engine A + 3D + Engine B ativos; Engine A na tela superior)
    a.ldrConst(1, 0x04000304u);
    a.ldrConst(0, 0x0000820Fu);
    a.strh(0, 1, 0);

    // 2. Mapeia VRAM_A -> Engine A BG (0x81) e VRAM_C -> Engine B BG 0x06200000 (0x84)
    a.ldrConst(1, 0x04000240u);
    a.movImm(0, 0x81u);
    a.strb(0, 1, 0); // VRAMCNT_A
    a.movImm(0, 0x84u);
    a.strb(0, 1, 2); // VRAMCNT_C

    // 3. Habilita IPCFIFOCNT (0x04000184) = 0x8008
    a.ldrConst(1, 0x04000184u);
    a.ldrConst(0, 0x00008008u);
    a.strh(0, 1, 0);

    // 4. Configura Engine A (Tela Superior: BG0 3D Hardware Rasterizer!)
    // DISPCNT_A (0x04000000) = 0x00010108 (Mode 0, Display Mode 1, BG0=3D bit 3, BG0 Enable bit 8)
    a.ldrConst(1, 0x04000000u);
    a.ldrConst(0, 0x00010108u);
    a.str32(0, 1, 0);

    // CLEAR_COLOR (0x04000350) = 0x001F1042 (Obsidiana Dracônica) & VIEWPORT (0x04000580) = 0xBFFF0000
    a.ldrConst(1, 0x04000350u);
    a.ldrConst(0, 0x001F1042u);
    a.str32(0, 1, 0);
    a.ldrConst(1, 0x04000580u);
    a.ldrConst(0, 0xBFFF0000u);
    a.str32(0, 1, 0);

    // Inicializa matrizes 3D como Identidade (Projection mode=0, Position mode=1)
    a.ldrConst(1, 0x04000440u); // GX_MTX_MODE (0x440), GX_MTX_IDENTITY (0x454)
    a.movImm(0, 0u);
    a.str32(0, 1, 0x00);
    a.str32(0, 1, 0x14);
    a.movImm(0, 1u);
    a.str32(0, 1, 0x00);
    a.str32(0, 1, 0x14);

    // 5. Configura Engine B (Tela Inferior Touchscreen: Direct Color Bitmap 256x192 15-bit em 0x06200000)
    a.ldrConst(1, 0x04001000u);
    a.ldrConst(0, 0x00010803u); // Display Mode 1, Mode 3, BG3 Enable
    a.str32(0, 1, 0);
    a.ldrConst(0, 0x4084u);     // BG3CNT_B: Size=256x256(1<<14) | DirectColor 15-bit(bit7+bit2)
    a.strh(0, 1, 0x0E);
    a.ldrConst(0, 0x0100u);
    a.strh(0, 1, 0x30);         // BG3PA_B = 1.0
    a.strh(0, 1, 0x36);         // BG3PD_B = 1.0
    a.movImm(0, 0u);
    a.strh(0, 1, 0x32);         // BG3PB_B = 0
    a.strh(0, 1, 0x34);         // BG3PC_B = 0

    // Preenche o canvas inicial da Tela Inferior (256 * 192 pixels em stride de 256 pixels = 0x06200000)
    a.ldrConst(2, 0x06200000u);
    a.ldrConst(3, 256u * 192u);
    a.ldrConst(0, static_cast<uint32_t>(bgr555(22, 25, 34)) | 0x8000u);
    const std::size_t fillLoop = a.pos();
    a.strh(0, 2, 0);
    a.addImm(2, 2, 2u);
    a.subImm(3, 3, 1u);
    a.cmpImm(3, 0u);
    a.branchTo(fillLoop, 0x1u); // BNE fillLoop

    // Desenha faixa decorativa Dragon Carmesim (#e63946) no topo e Esmeralda (#10b981) na base da Tela Inferior
    a.ldrConst(2, 0x06200000u);
    a.ldrConst(3, 256u * 4u);
    a.ldrConst(0, static_cast<uint32_t>(bgr555(230, 57, 70)) | 0x8000u);
    const std::size_t topBarLoop = a.pos();
    a.strh(0, 2, 0);
    a.addImm(2, 2, 2u);
    a.subImm(3, 3, 1u);
    a.cmpImm(3, 0u);
    a.branchTo(topBarLoop, 0x1u);

    a.ldrConst(2, 0x06200000u + (188u * 256u * 2u));
    a.ldrConst(3, 256u * 4u);
    a.ldrConst(0, static_cast<uint32_t>(bgr555(16, 185, 129)) | 0x8000u);
    const std::size_t botBarLoop = a.pos();
    a.strh(0, 2, 0);
    a.addImm(2, 2, 2u);
    a.subImm(3, 3, 1u);
    a.cmpImm(3, 0u);
    a.branchTo(botBarLoop, 0x1u);

    // R10 = último pacote IPC recebido (inicia com Pen solta = 0x007F6080)
    a.ldrConst(10, 0x007F6080u);
    // R11 = contador de quadros para animação 3D
    a.movImm(11, 0u);

    // ===================== LOOP DE QUADRO ARM9 =====================
    const std::size_t frameLoop = a.pos();
    a.addImm(11, 11, 1u);

    // 1. Drena todos os pacotes disponíveis em IPCFIFORECV (0x04100000) enquanto bit 8 de IPCFIFOCNT == 0
    a.ldrConst(1, 0x04000184u);
    a.ldrConst(2, 0x04100000u);
    const std::size_t fifoDrain = a.pos();
    a.ldrh(0, 1, 0);
    a.tstImm(0, 0x01u, 24); // Testa bit 8 (Recv FIFO Empty) -> 0x01 rotRight 24 = 0x100
    const std::size_t fifoDoneBranch = a.branchPlaceholder(0x1u); // BNE fifoDone
    a.ldr32(10, 2, 0);      // R10 = novo pacote IPC do ARM7!
    // Espelha o último pacote IPC em Main RAM 0x02003F00 para inspeção em testes automatizados
    a.ldrConst(3, 0x02003F00u);
    a.str32(10, 3, 0);
    a.branchTo(fifoDrain);
    a.patchBranch(fifoDoneBranch, a.pos());

    // 2. Se a caneta Stylus estiver tocando a tela (bit 22 de R10 == EXTKEYIN bit 6 == 0), pinta pincel 3x3!
    a.tstImm(10, 0x40u, 16); // 0x40 rotRight 16 = 0x00400000 (EXTKEYIN bit 6 Pen Down, active-low)
    const std::size_t skipPaint = a.branchPlaceholder(0x1u); // BNE skipPaint (não está tocando)

    // Extrai X = R10 & 0xFF (R6), Y = (R10 >> 8) & 0xFF (R7)
    a.andImm(6, 10, 0xFFu);
    a.movLsrImm(7, 10, 8);
    a.andImm(7, 7, 0xFFu);
    // Garante Y <= 190
    a.cmpImm(7, 190u);
    a.movImm(7, 190u, 0, 0x8u); // cond HI: mov R7, #190

    // Endereço do pixel em Engine B VRAM: 0x06200000 + ((Y * 256 + X) * 2)
    a.ldrConst(2, 0x06200000u);
    a.movLslImm(3, 7, 9); // Y * 512
    a.addReg(2, 2, 3);
    a.movLslImm(3, 6, 1); // X * 2
    a.addReg(2, 2, 3);

    // Cor do pincel: Esmeralda (#10b981) se botão X pressionado ou Sopro no Mic, senão Fogo Dracônico (#e63946)
    a.ldrConst(0, static_cast<uint32_t>(bgr555(230, 57, 70)) | 0x8000u);
    a.tstImm(10, 0x80u, 8); // Bit 31 (Sopro no Microfone)
    a.ldrConst(3, static_cast<uint32_t>(bgr555(16, 185, 129)) | 0x8000u);
    a.movReg(0, 3, 0x1u);   // cond NE -> Esmeralda

    // Pinta cruz/bloco 2x2 no ponto tocado pela Stylus
    a.strh(0, 2, 0);
    a.strh(0, 2, 2);
    a.ldrConst(3, 512u);
    a.addReg(2, 2, 3);
    a.strh(0, 2, 0);
    a.strh(0, 2, 2);

    a.patchBranch(skipPaint, a.pos());

    // 3. Renderiza Prisma 3D Dracônico na Tela Superior via registradores GX 3D!
    // POLY_ATTR (0x040004A4) = Alpha 31 + Back/Front visíveis (0x001F00C0)
    a.ldrConst(1, 0x040004A4u);
    a.ldrConst(0, 0x001F00C0u);
    a.str32(0, 1, 0);

    // GX_BEGIN_VTXS (0x04000500) = 0 (Triangles)
    a.ldrConst(1, 0x04000500u);
    a.movImm(0, 0u);
    a.str32(0, 1, 0);

    // Lê KEYINPUT (0x04000130) para variar o brilho do vértice superior com A/B/X/Y
    a.ldrConst(2, 0x04000130u);
    a.ldrh(5, 2, 0);

    // Vértice 0 (Topo do Cristal do Dragão: Carmesim #e63946 ou Dourado se X/Y/A pressionado)
    a.ldrConst(1, 0x04000480u); // GX_COLOR (0x480), GX_VTX_16 (0x48C)
    a.ldrConst(0, bgr555(235, 60, 75));
    a.tstImm(10, 0x03u, 16); // EXTKEYIN X/Y (0 = pressionado)
    a.cmpImm(5, 0xFFu);
    a.str32(0, 1, 0x00);
    // Posição V0: X = oscilação suave ((frame & 63) - 32) * 32, Y = +0.65 (0x0A80), Z = 0
    a.andImm(3, 11, 0x3Fu);
    a.subImm(3, 3, 32u);
    a.movLslImm(3, 3, 5);
    a.ldrConst(4, 0x0000FFFFu);
    a.emit(0xE0033004u); // AND R3, R3, R4 (isola 16 bits inferiores de X)
    a.ldrConst(0, 0x0A800000u); // Y = +0x0A80 nos 16 bits superiores
    a.orrRegLsl(0, 0, 3, 0);
    a.str32(0, 1, 0x0C); // GX_VTX_16 param 1 (X, Y)
    a.movImm(0, 0u);
    a.str32(0, 1, 0x0C); // GX_VTX_16 param 2 (Z = 0)

    // Vértice 1 (Base Esquerda: Esmeralda #10b981)
    a.ldrConst(0, bgr555(16, 195, 135));
    a.str32(0, 1, 0x00);
    a.ldrConst(0, 0xF600F500u); // X = -0x0B00, Y = -0x0A00
    a.str32(0, 1, 0x0C);
    a.movImm(0, 0u);
    a.str32(0, 1, 0x0C);

    // Vértice 2 (Base Direita: Âmbar Dourado #fbbf24)
    a.ldrConst(0, bgr555(250, 190, 40));
    a.str32(0, 1, 0x00);
    a.ldrConst(0, 0xF6000B00u); // X = +0x0B00, Y = -0x0A00
    a.str32(0, 1, 0x0C);
    a.movImm(0, 0u);
    a.str32(0, 1, 0x0C);

    // GX_END_VTXS (0x04000504) & GX_SWAP_BUFFERS (0x04000540)
    a.ldrConst(1, 0x04000504u);
    a.movImm(0, 0u);
    a.str32(0, 1, 0);
    a.ldrConst(1, 0x04000540u);
    a.str32(0, 1, 0);

    // 4. Sincroniza no VBlank (VCOUNT == 192)
    a.ldrConst(1, 0x04000006u);
    const std::size_t waitNot192 = a.pos();
    a.ldrh(0, 1, 0);
    a.cmpImm(0, 192u);
    a.branchTo(waitNot192, 0x0u);

    const std::size_t wait192 = a.pos();
    a.ldrh(0, 1, 0);
    a.cmpImm(0, 192u);
    a.branchTo(wait192, 0x1u);

    a.branchTo(frameLoop);
    return a.finalize();
}

void populateDragonBanner(uint8_t* banner) noexcept {
    // Versão 0x0001 do Banner NDS (0x840 bytes)
    writeLe16(banner + 0x000, 0x0001u);

    // Paleta de 16 cores BGR555 em 0x220..0x23F (Tema Dragon: Obsidiana, Carmesim, Esmeralda, Ouro)
    const std::array<uint16_t, 16> pal = {
        0x0000u,                  // 0: Transparente
        bgr555(18, 19, 22),       // 1: Obsidiana Profunda (#121316)
        bgr555(34, 37, 46),       // 2: Escama Metálica (#22252e)
        bgr555(230, 57, 70),      // 3: Fogo Dracônico (#e63946)
        bgr555(185, 28, 45),      // 4: Carmesim Escuro
        bgr555(16, 185, 129),     // 5: Escama Esmeralda (#10b981)
        bgr555(251, 191, 36),     // 6: Pupila / Ouro (#fbbf24)
        bgr555(243, 244, 246),    // 7: Branco Pérola
        bgr555(55, 65, 81),
        bgr555(99, 102, 241),
        bgr555(239, 68, 68),
        bgr555(52, 211, 153),
        bgr555(245, 158, 11),
        bgr555(209, 213, 219),
        bgr555(31, 41, 55),
        bgr555(255, 255, 255)
    };
    for (std::size_t i = 0; i < 16; ++i) {
        writeLe16(banner + 0x220 + i * 2u, pal[i]);
    }

    // Desenha silhueta 32x32 do Olho / Brasão de Dragão e converte para 4x4 tiles 4bpp em 0x020..0x21F
    std::array<uint8_t, 32 * 32> pixels{};
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            uint8_t c = 1; // Fundo Obsidiana
            if (x == 0 || x == 31 || y == 0 || y == 31) {
                c = 2; // Moldura Escama Metálica
            } else if (x == 1 || x == 30 || y == 1 || y == 30) {
                c = 3; // Moldura Carmesim Dracônica
            } else {
                const int dx = x - 16;
                const int dy = y - 16;
                const int dist2 = dx * dx + dy * dy;
                // Losango / Olho de Dragão
                if (std::abs(dx) + std::abs(dy) <= 12) {
                    c = 3; // Íris Carmesim
                    if (std::abs(dx) + std::abs(dy) <= 9) {
                        c = 5; // Anel Esmeralda
                    }
                    if (std::abs(dx) <= 2 && std::abs(dy) <= 7) {
                        c = 6; // Fenda vertical dourada do olho de dragão!
                    }
                    if (dx == 0 && std::abs(dy) <= 5) {
                        c = 1; // Pupila reptiliana central
                    }
                } else if (dist2 <= 160) {
                    c = 2;
                }
            }
            pixels[static_cast<std::size_t>(y * 32 + x)] = c;
        }
    }

    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            const uint32_t tileOff = 0x020u + static_cast<uint32_t>((ty * 4 + tx) * 32);
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; px += 2) {
                    const uint8_t p0 = pixels[static_cast<std::size_t>((ty * 8 + py) * 32 + (tx * 8 + px))];
                    const uint8_t p1 = pixels[static_cast<std::size_t>((ty * 8 + py) * 32 + (tx * 8 + px + 1))];
                    banner[tileOff + static_cast<uint32_t>(py * 4 + (px / 2))] =
                        static_cast<uint8_t>((p0 & 0x0Fu) | ((p1 & 0x0Fu) << 4));
                }
            }
        }
    }

    // 6 Títulos UTF-16LE localizados (0x240..0x83F, 0x100 bytes cada)
    constexpr std::string_view TITLE_ASCII = "RyuDS Dragon Edition\nDual-Screen 2D+3D Tech Demo";
    for (int lang = 0; lang < 6; ++lang) {
        uint8_t* dst = banner + 0x240 + lang * 0x100;
        for (std::size_t i = 0; i < TITLE_ASCII.size() && i < 127; ++i) {
            writeLe16(dst + i * 2u, static_cast<uint16_t>(static_cast<uint8_t>(TITLE_ASCII[i])));
        }
    }

    // CRC16 do Banner (calculado sobre 0x020..0x83F) gravado em 0x002
    writeLe16(banner + 0x002, crc16Nds(banner + 0x020, 0x820));
}

} // namespace

std::vector<uint8_t> BuiltInNdsDemoRom::build() {
    const auto arm9Bin = buildArm9Binary();
    const auto arm7Bin = buildArm7Binary();

    std::vector<uint8_t> rom(0x10000u, 0x00u); // Imagem compacta de 64 KB (.nds válido)

    // 1. Cabeçalho Padrão Nintendo DS (0x000..0x1FF)
    std::memcpy(&rom[0x000], "RYUDS DEMO\0\0", 12);
    std::memcpy(&rom[0x00C], "ADRE", 4);
    std::memcpy(&rom[0x010], "RY", 2);
    rom[0x012] = 0x00u; // Unit Code: NDS
    rom[0x014] = 0x00u; // Device Capacity: 128 KB

    writeLe32(&rom[0x020], 0x00004000u); // ARM9 ROM Offset
    writeLe32(&rom[0x024], 0x02000000u); // ARM9 Entry Address
    writeLe32(&rom[0x028], 0x02000000u); // ARM9 RAM Load Address
    writeLe32(&rom[0x02C], static_cast<uint32_t>(arm9Bin.size()));

    writeLe32(&rom[0x030], 0x00008000u); // ARM7 ROM Offset
    writeLe32(&rom[0x034], 0x03800000u); // ARM7 Entry Address
    writeLe32(&rom[0x038], 0x03800000u); // ARM7 RAM Load Address
    writeLe32(&rom[0x03C], static_cast<uint32_t>(arm7Bin.size()));

    writeLe32(&rom[0x068], 0x00001000u); // Icon/Banner Offset
    writeLe32(&rom[0x080], static_cast<uint32_t>(rom.size()));
    writeLe32(&rom[0x084], 0x00004000u); // Header Size

    // Assinatura Nintendo Logo CRC (0xCF56 em 0x15C) + Header CRC16 (em 0x15E)
    writeLe16(&rom[0x15C], 0xCF56u);
    writeLe16(&rom[0x15E], crc16Nds(rom.data(), 0x15Eu));

    // 2. Banner 32x32 16-color + Títulos UTF-16LE em 0x1000
    populateDragonBanner(&rom[0x1000]);

    // 3. Binários Executáveis ARM9 (0x4000) e ARM7 (0x8000)
    std::memcpy(&rom[0x4000], arm9Bin.data(), arm9Bin.size());
    std::memcpy(&rom[0x8000], arm7Bin.data(), arm7Bin.size());

    return rom;
}

} // namespace gba::core::nds
