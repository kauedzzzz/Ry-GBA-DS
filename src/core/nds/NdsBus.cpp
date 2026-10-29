#include "core/nds/NdsBus.hpp"
#include "core/nds/Arm9Cpu.hpp"
#include "core/nds/NdsArm7Cpu.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gba::core::nds {

namespace {

void put32Le(uint8_t* dst, std::size_t off, uint32_t v) noexcept {
    dst[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    dst[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void put16Le(uint8_t* dst, std::size_t off, uint16_t v) noexcept {
    dst[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    dst[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

[[nodiscard]] uint16_t get16Le(const uint8_t* src, std::size_t off) noexcept {
    return static_cast<uint16_t>(src[off]) | (static_cast<uint16_t>(src[off + 1]) << 8);
}

} // namespace

NdsBus::NdsBus(NdsCartridge& cart, NdsGpu& gpu, NdsSpu& spu, TouchController& touch)
    : m_cart(cart)
    , m_gpu(gpu)
    , m_spu(spu)
    , m_touch(touch)
    , m_mainRam(4 * 1024 * 1024, 0) {
    m_gpu.attachBus(this);
    m_spu.attachBus(this);
    reset();
}

bool NdsBus::loadBios9(std::span<const uint8_t> data) {
    if (data.size() < 4096) return false;
    std::memcpy(m_bios9.data(), data.data(), 4096);
    m_hasCustomBios9 = true;
    return true;
}

bool NdsBus::loadBios7(std::span<const uint8_t> data) {
    if (data.size() < 16384) return false;
    std::memcpy(m_bios7.data(), data.data(), 16384);
    m_hasCustomBios7 = true;
    return true;
}

void NdsBus::installHleBiosVectors() {
    if (!m_hasCustomBios9) {
        m_bios9.fill(0);
        // Vetores de exceção ARM9 em 0xFFFF0000:
        put32Le(m_bios9.data(), 0x00, 0xEA00004Eu); // Reset
        put32Le(m_bios9.data(), 0x08, 0xE1B0F00Eu); // SWI -> MOVS PC, LR
        put32Le(m_bios9.data(), 0x18, 0xEA000002u); // IRQ -> B 0xFFFF0028

        // Dispatcher IRQ HLE do ARM9 em 0xFFFF0028 (lê [DTCM_Base + 0x3FFC] via CP15!):
        put32Le(m_bios9.data(), 0x28, 0xE92D500Fu); // STMFD SP!, {R0-R3, R12, LR}
        put32Le(m_bios9.data(), 0x2C, 0xEE190F11u); // MRC p15, 0, R0, c9, c1, 0
        put32Le(m_bios9.data(), 0x30, 0xE3C000FFu); // BIC R0, R0, #0xFF
        put32Le(m_bios9.data(), 0x34, 0xE2800901u); // ADD R0, R0, #0x4000
        put32Le(m_bios9.data(), 0x38, 0xE28FE000u); // ADR LR, 0xFFFF0040
        put32Le(m_bios9.data(), 0x3C, 0xE510F004u); // LDR PC, [R0, #-4]
        put32Le(m_bios9.data(), 0x40, 0xE8BD500Fu); // LDMFD SP!, {R0-R3, R12, LR}
        put32Le(m_bios9.data(), 0x44, 0xE25EF004u); // SUBS PC, LR, #4

        // Handler IRQ default seguro em 0xFFFF0140: BX LR
        put32Le(m_bios9.data(), 0x140, 0xE12FFF1Eu);
    }

    if (!m_hasCustomBios7) {
        m_bios7.fill(0);
        // Vetores de exceção ARM7 em 0x00000000:
        put32Le(m_bios7.data(), 0x00, 0xEA00004Eu); // Reset
        put32Le(m_bios7.data(), 0x08, 0xE1B0F00Eu); // SWI -> MOVS PC, LR
        put32Le(m_bios7.data(), 0x18, 0xEA000002u); // IRQ -> B 0x00000028

        // Dispatcher IRQ HLE do ARM7 em 0x00000028 (lê [0x03FFFFFC] -> [0x0380FFFC]):
        put32Le(m_bios7.data(), 0x28, 0xE92D500Fu); // STMFD SP!, {R0-R3, R12, LR}
        put32Le(m_bios7.data(), 0x2C, 0xE3A00301u); // MOV R0, #0x04000000
        put32Le(m_bios7.data(), 0x30, 0xE28FE000u); // ADR LR, 0x00000038
        put32Le(m_bios7.data(), 0x34, 0xE510F004u); // LDR PC, [R0, #-4]
        put32Le(m_bios7.data(), 0x38, 0xE8BD500Fu); // LDMFD SP!, {R0-R3, R12, LR}
        put32Le(m_bios7.data(), 0x3C, 0xE25EF004u); // SUBS PC, LR, #4

        // Handler IRQ default seguro em 0x00000140: BX LR
        put32Le(m_bios7.data(), 0x140, 0xE12FFF1Eu);
    }
}

void NdsBus::reset() {
    std::fill(m_mainRam.begin(), m_mainRam.end(), 0);
    m_sharedWram.fill(0);
    m_arm7Wram.fill(0);
    m_vramA.fill(0);
    m_vramB.fill(0);
    m_vramC.fill(0);
    m_vramD.fill(0);
    m_vramE.fill(0);
    m_vramF.fill(0);
    m_vramG.fill(0);
    m_vramH.fill(0);
    m_vramI.fill(0);
    m_vramCnt.fill(0);
    m_wramCnt = 3; // 32KB Shared WRAM inicialmente alocada ao ARM7

    m_palettes.fill(0);
    m_oam.fill(0);
    m_wifiRam.fill(0);

    m_ime9 = m_ie9 = m_if9 = 0;
    m_ime7 = m_ie7 = m_if7 = 0;
    m_exMemCnt = 0x6000u;
    m_postFlg9 = 1;
    m_postFlg7 = 1;
    m_haltCnt7 = 0;
    m_rtcReg   = 0;

    m_keyInput = 0x03FFu;
    m_xyMask   = 0x0003u;
    m_keyCnt9  = 0;
    m_keyCnt7  = 0;

    m_ipcSync9     = 0;
    m_ipcSync7     = 0;
    m_ipcFifoCnt9  = 0x0101u;
    m_ipcFifoCnt7  = 0x0101u;
    m_fifo9to7.clear();
    m_fifo7to9.clear();
    m_lastFifo9to7 = 0;
    m_lastFifo7to9 = 0;

    for (auto& d : m_dma9) d = DmaChannel{};
    m_dmaFill9.fill(0);
    for (auto& d : m_dma7) d = DmaChannel{};
    for (auto& t : m_timers9) t = TimerChannel{};
    for (auto& t : m_timers7) t = TimerChannel{};

    m_divCnt = 0;
    m_divNumer = m_divDenom = m_divResult = m_divRem = 0;
    m_sqrtCnt = 0;
    m_sqrtParam = 0;
    m_sqrtResult = 0;

    installHleBiosVectors();
    // Handler IRQ default em ARM7 WRAM [0x0380FFFC]
    put32Le(m_arm7Wram.data(), 0xFFFCu, 0x00000140u);

    m_gpu.reset();
    m_spu.reset();
    m_touch.reset();
    m_cart.reset();
}

void NdsBus::setupDirectBoot() {
    reset();
    if (!m_cart.isLoaded()) return;

    const auto& hdr = m_cart.header();
    const auto rom  = m_cart.rawRom();

    if (m_arm9) m_arm9->reset();
    if (m_arm7) m_arm7->reset();

    // 1. Copia cabeçalho do cartucho (0x200 bytes) para 0x027FFE00 (espelhado em 0x023FFE00 na Main RAM 4MB)
    const std::size_t hdrCopyLen = std::min<std::size_t>(0x200u, rom.size());
    std::memcpy(&m_mainRam[0x3FFE00u], rom.data(), hdrCopyLen);

    // 2. Popula estruturas pós-firmware do Nintendo SDK em 0x027FF800..0x027FFDF0
    put32Le(m_mainRam.data(), 0x3FF800u, NdsCartridge::CHIP_ID);
    put32Le(m_mainRam.data(), 0x3FF804u, NdsCartridge::CHIP_ID);
    put16Le(m_mainRam.data(), 0x3FF808u, hdr.headerCrc16);
    put16Le(m_mainRam.data(), 0x3FF80Au, 0x5835u);
    put16Le(m_mainRam.data(), 0x3FF850u, 0x5835u);
    put32Le(m_mainRam.data(), 0x3FF868u, 0x0003FE00u);

    put32Le(m_mainRam.data(), 0x3FFC00u, NdsCartridge::CHIP_ID);
    put16Le(m_mainRam.data(), 0x3FFC08u, hdr.headerCrc16);
    put16Le(m_mainRam.data(), 0x3FFC0Au, 0x5835u);
    put16Le(m_mainRam.data(), 0x3FFC10u, 0x5835u);
    put16Le(m_mainRam.data(), 0x3FFC30u, 0xFFFFu);
    put16Le(m_mainRam.data(), 0x3FFC40u, 0x0001u); // Boot Indicator = 1 (Cartridge Boot)

    // User Settings do Firmware (incluindo calibração 1:1 do Touchscreen!) em 0x027FFC80
    m_touch.populateDefaultFirmwareUserSettings(&m_mainRam[0x3FFC80u]);

    // 3. Carrega binário ARM9 em arm9RamAddress
    if (m_arm9 && hdr.arm9RomOffset < rom.size()) {
        const std::size_t maxCopy = std::min<std::size_t>(hdr.arm9Size, rom.size() - hdr.arm9RomOffset);
        for (std::size_t i = 0; i < maxCopy; ++i) {
            m_arm9->dataWrite8(hdr.arm9RamAddress + static_cast<uint32_t>(i), rom[hdr.arm9RomOffset + i]);
        }
        // Handler IRQ default em [DTCM + 0x3FFC]
        m_arm9->dataWrite32(m_arm9->dtcmBase() + 0x3FFCu, 0xFFFF0140u);
        m_arm9->initDirectBoot(hdr.arm9EntryAddress);
    }

    // 4. Carrega binário ARM7 em arm7RamAddress
    if (m_arm7 && hdr.arm7RomOffset < rom.size()) {
        const std::size_t maxCopy = std::min<std::size_t>(hdr.arm7Size, rom.size() - hdr.arm7RomOffset);
        for (std::size_t i = 0; i < maxCopy; ++i) {
            arm7Write8(hdr.arm7RamAddress + static_cast<uint32_t>(i), rom[hdr.arm7RomOffset + i]);
        }
        m_arm7->initDirectBoot(hdr.arm7EntryAddress);
    }
}

void NdsBus::setNdsKeyState(NdsKey key, bool pressed) noexcept {
    const uint16_t raw = static_cast<uint16_t>(key);
    if (raw <= 0x0200u) {
        if (pressed) m_keyInput &= ~raw;
        else         m_keyInput |= raw;
    } else if (key == NdsKey::X) {
        if (pressed) m_xyMask &= ~0x0001u;
        else         m_xyMask |= 0x0001u;
    } else if (key == NdsKey::Y) {
        if (pressed) m_xyMask &= ~0x0002u;
        else         m_xyMask |= 0x0002u;
    }
}

uint16_t NdsBus::extKeyInReg() const noexcept {
    // Bit 0=X(0=press), Bit 1=Y(0=press), Bit 3=1(Debug), Bit 6=PenDown(0=touch,1=solto), Bit 7=Lid(1=fechada,0=aberta)
    uint16_t v = (m_xyMask & 0x0003u) | 0x000Cu;
    if (!m_touch.isTouching()) {
        v |= (1u << 6);
    }
    if (m_touch.isLidClosed()) {
        v |= (1u << 7);
    }
    return v;
}

void NdsBus::requestIrq9(uint32_t mask) noexcept {
    m_if9 |= mask;
    if ((m_ie9 & m_if9) != 0 && m_arm9) {
        m_arm9->wakeFromHalt();
    }
}

void NdsBus::requestIrq7(uint32_t mask) noexcept {
    m_if7 |= mask;
    if ((m_ie7 & m_if7) != 0 && m_arm7) {
        m_arm7->wakeFromHalt();
    }
}

// =============================================================================
// Mapeador Dinâmico dos 9 Bancos Físicos de VRAM (656 KB)
// =============================================================================
namespace {

struct VramBankDesc {
    uint8_t* ptr;
    uint32_t size;
};

} // namespace

uint8_t* NdsBus::resolveVramCpuWritePtr(bool isArm9, uint32_t addr) noexcept {
    return const_cast<uint8_t*>(resolveVramCpuReadPtr(isArm9, addr));
}

const uint8_t* NdsBus::resolveVramCpuReadPtr(bool isArm9, uint32_t addr) const noexcept {
    const std::array<std::pair<const uint8_t*, uint32_t>, 9> banks = {{
        {m_vramA.data(), 0x20000u}, {m_vramB.data(), 0x20000u},
        {m_vramC.data(), 0x20000u}, {m_vramD.data(), 0x20000u},
        {m_vramE.data(), 0x10000u}, {m_vramF.data(), 0x04000u},
        {m_vramG.data(), 0x04000u}, {m_vramH.data(), 0x08000u},
        {m_vramI.data(), 0x04000u}
    }};

    if (!isArm9) {
        // ARM7 VRAM em 0x06000000..0x0603FFFF (Bancos C e/ou D quando MST == 2)
        const uint32_t off = addr & 0x3FFFFu;
        for (int b = 2; b <= 3; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 2u) {
                const uint32_t base = ((cnt >> 3) & 1u) * 0x20000u;
                if (off >= base && off < base + 0x20000u) {
                    return banks[b].first + (off - base);
                }
            }
        }
        return nullptr;
    }

    const uint32_t region = (addr >> 20) & 0xFu; // 0=A_BG, 2=B_BG, 4=A_OBJ, 6=B_OBJ, 8..A=LCDC
    if (region >= 8) {
        // Modo LCDC (0x06800000..0x068A3FFF)
        const uint32_t lcdcOff = addr - 0x06800000u;
        static constexpr uint32_t LCDC_BASES[9] = {
            0x00000u, 0x20000u, 0x40000u, 0x60000u,
            0x80000u, 0x90000u, 0x94000u, 0x98000u, 0xA0000u
        };
        for (int b = 0; b < 9; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 0u) {
                if (lcdcOff >= LCDC_BASES[b] && lcdcOff < LCDC_BASES[b] + banks[b].second) {
                    return banks[b].first + (lcdcOff - LCDC_BASES[b]);
                }
            }
        }
        return nullptr;
    }

    if (region == 0 || region == 1) { // Engine A BG (0x06000000, até 512 KB)
        const uint32_t off = addr & 0x7FFFFu;
        for (int b = 0; b <= 3; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 1u) {
                const uint32_t base = ((cnt >> 3) & 3u) * 0x20000u;
                if (off >= base && off < base + 0x20000u) return banks[b].first + (off - base);
            }
        }
        if ((m_vramCnt[4] & 0x80u) && (m_vramCnt[4] & 0x07u) == 1u && off < 0x10000u) {
            return m_vramE.data() + off;
        }
        for (int b = 5; b <= 6; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 1u) {
                const uint32_t ofs = (cnt >> 3) & 3u;
                const uint32_t base = (ofs & 1u) * 0x4000u + ((ofs >> 1) & 1u) * 0x10000u;
                if ((off & 0x1FFFFu) >= base && (off & 0x1FFFFu) < base + 0x4000u) {
                    return banks[b].first + ((off & 0x1FFFFu) - base);
                }
            }
        }
    } else if (region == 2 || region == 3) { // Engine B BG (0x06200000, até 128 KB)
        const uint32_t off = addr & 0x1FFFFu;
        if ((m_vramCnt[2] & 0x80u) && (m_vramCnt[2] & 0x07u) == 4u) {
            return m_vramC.data() + off;
        }
        if ((m_vramCnt[7] & 0x80u) && (m_vramCnt[7] & 0x07u) == 1u && (off & 0xFFFFu) < 0x8000u) {
            return m_vramH.data() + (off & 0x7FFFu);
        }
        if ((m_vramCnt[8] & 0x80u) && (m_vramCnt[8] & 0x07u) == 1u && (off & 0xFFFFu) >= 0x8000u && (off & 0xFFFFu) < 0xC000u) {
            return m_vramI.data() + ((off & 0xFFFFu) - 0x8000u);
        }
    } else if (region == 4 || region == 5) { // Engine A OBJ (0x06400000, até 256 KB)
        const uint32_t off = addr & 0x3FFFFu;
        for (int b = 0; b <= 1; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 2u) {
                const uint32_t base = ((cnt >> 3) & 1u) * 0x20000u;
                if (off >= base && off < base + 0x20000u) return banks[b].first + (off - base);
            }
        }
        if ((m_vramCnt[4] & 0x80u) && (m_vramCnt[4] & 0x07u) == 2u && off < 0x10000u) {
            return m_vramE.data() + off;
        }
        for (int b = 5; b <= 6; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 2u) {
                const uint32_t ofs = (cnt >> 3) & 3u;
                const uint32_t base = (ofs & 1u) * 0x4000u + ((ofs >> 1) & 1u) * 0x10000u;
                if ((off & 0x1FFFFu) >= base && (off & 0x1FFFFu) < base + 0x4000u) {
                    return banks[b].first + ((off & 0x1FFFFu) - base);
                }
            }
        }
    } else if (region == 6 || region == 7) { // Engine B OBJ (0x06600000, até 128 KB)
        const uint32_t off = addr & 0x1FFFFu;
        if ((m_vramCnt[3] & 0x80u) && (m_vramCnt[3] & 0x07u) == 4u) {
            return m_vramD.data() + off;
        }
        if ((m_vramCnt[8] & 0x80u) && (m_vramCnt[8] & 0x07u) == 2u) {
            return m_vramI.data() + (off & 0x3FFFu);
        }
    }

    return nullptr;
}

uint8_t NdsBus::readBgVram8(int engine, uint32_t offset) const noexcept {
    const uint32_t base = (engine == 0) ? 0x06000000u : 0x06200000u;
    const uint8_t* p = resolveVramCpuReadPtr(true, base + offset);
    return p ? *p : 0;
}

uint16_t NdsBus::readBgVram16(int engine, uint32_t offset) const noexcept {
    offset &= ~1u;
    return static_cast<uint16_t>(readBgVram8(engine, offset)) |
           (static_cast<uint16_t>(readBgVram8(engine, offset + 1)) << 8);
}

uint8_t NdsBus::readObjVram8(int engine, uint32_t offset) const noexcept {
    const uint32_t base = (engine == 0) ? 0x06400000u : 0x06600000u;
    const uint8_t* p = resolveVramCpuReadPtr(true, base + offset);
    return p ? *p : 0;
}

uint16_t NdsBus::readObjVram16(int engine, uint32_t offset) const noexcept {
    offset &= ~1u;
    return static_cast<uint16_t>(readObjVram8(engine, offset)) |
           (static_cast<uint16_t>(readObjVram8(engine, offset + 1)) << 8);
}

uint16_t NdsBus::readLcdcBank16(int bankIdx, uint32_t byteOffset) const noexcept {
    byteOffset &= 0x1FFFEu;
    switch (bankIdx & 3) {
        case 0: return get16Le(m_vramA.data(), byteOffset);
        case 1: return get16Le(m_vramB.data(), byteOffset);
        case 2: return get16Le(m_vramC.data(), byteOffset);
        case 3: return get16Le(m_vramD.data(), byteOffset);
    }
    return 0;
}

uint16_t NdsBus::readPalette16(int engine, uint32_t byteOffset) const noexcept {
    const uint32_t base = (engine == 0) ? 0u : 0x400u;
    return get16Le(m_palettes.data(), base + (byteOffset & 0x3FEu));
}

uint16_t NdsBus::readExtBgPalette16(int engine, int slot, uint32_t colorIdx) const noexcept {
    const uint32_t off = (static_cast<uint32_t>(slot & 3) * 0x2000u) + ((colorIdx & 0xFFFu) * 2u);
    if (engine == 0) {
        if ((m_vramCnt[4] & 0x80u) && (m_vramCnt[4] & 0x07u) == 4u && off < 0x8000u) {
            return get16Le(m_vramE.data(), off);
        }
        for (int b = 5; b <= 6; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 4u) {
                const uint32_t base = ((cnt >> 3) & 1u) * 0x4000u;
                if (off >= base && off < base + 0x4000u) {
                    const uint8_t* ptr = (b == 5) ? m_vramF.data() : m_vramG.data();
                    return get16Le(ptr, off - base);
                }
            }
        }
    } else {
        if ((m_vramCnt[7] & 0x80u) && (m_vramCnt[7] & 0x07u) == 2u && off < 0x8000u) {
            return get16Le(m_vramH.data(), off);
        }
    }
    return 0;
}

uint16_t NdsBus::readExtObjPalette16(int engine, uint32_t colorIdx) const noexcept {
    const uint32_t off = (colorIdx & 0xFFFu) * 2u;
    if (engine == 0) {
        for (int b = 5; b <= 6; ++b) {
            const uint8_t cnt = m_vramCnt[b];
            if ((cnt & 0x80u) && (cnt & 0x07u) == 5u && off < 0x2000u) {
                const uint8_t* ptr = (b == 5) ? m_vramF.data() : m_vramG.data();
                return get16Le(ptr, off);
            }
        }
    } else {
        if ((m_vramCnt[8] & 0x80u) && (m_vramCnt[8] & 0x07u) == 3u && off < 0x2000u) {
            return get16Le(m_vramI.data(), off);
        }
    }
    return 0;
}

uint16_t NdsBus::readOam16(int engine, uint32_t byteOffset) const noexcept {
    const uint32_t base = (engine == 0) ? 0u : 0x400u;
    return get16Le(m_oam.data(), base + (byteOffset & 0x3FEu));
}

uint8_t NdsBus::readTexVram8(uint32_t texOffset) const noexcept {
    const uint32_t slot = (texOffset >> 17) & 3u;
    const uint32_t inSlot = texOffset & 0x1FFFFu;
    const uint8_t* banks[4] = {m_vramA.data(), m_vramB.data(), m_vramC.data(), m_vramD.data()};
    for (int b = 0; b < 4; ++b) {
        const uint8_t cnt = m_vramCnt[b];
        if ((cnt & 0x80u) && (cnt & 0x07u) == 3u && ((cnt >> 3) & 3u) == slot) {
            return banks[b][inSlot];
        }
    }
    return 0;
}

uint16_t NdsBus::readTexVram16(uint32_t texOffset) const noexcept {
    texOffset &= ~1u;
    return static_cast<uint16_t>(readTexVram8(texOffset)) |
           (static_cast<uint16_t>(readTexVram8(texOffset + 1)) << 8);
}

uint16_t NdsBus::readTexPalette16(uint32_t byteOffset) const noexcept {
    byteOffset &= 0x1FFFEu;
    if ((m_vramCnt[4] & 0x80u) && (m_vramCnt[4] & 0x07u) == 3u && byteOffset < 0x10000u) {
        return get16Le(m_vramE.data(), byteOffset);
    }
    for (int b = 5; b <= 6; ++b) {
        const uint8_t cnt = m_vramCnt[b];
        if ((cnt & 0x80u) && (cnt & 0x07u) == 3u) {
            const uint32_t ofs = (cnt >> 3) & 3u;
            const uint32_t slot = (ofs & 1u) + ((ofs & 2u) * 2u);
            const uint32_t base = slot * 0x4000u;
            if (byteOffset >= base && byteOffset < base + 0x4000u) {
                const uint8_t* ptr = (b == 5) ? m_vramF.data() : m_vramG.data();
                return get16Le(ptr, byteOffset - base);
            }
        }
    }
    return 0;
}

// =============================================================================
// Coprocessador Matemático Hardware ARM9 (DIV & SQRT)
// =============================================================================
void NdsBus::recomputeDiv() noexcept {
    m_divCnt &= ~((1u << 15) | (1u << 14)); // Busy = 0
    const uint8_t mode = m_divCnt & 0x3u;
    const int64_t num  = (mode == 0) ? static_cast<int64_t>(static_cast<int32_t>(m_divNumer)) : m_divNumer;
    const int64_t den  = (mode == 2) ? m_divDenom : static_cast<int64_t>(static_cast<int32_t>(m_divDenom));

    if (m_divDenom == 0) {
        m_divCnt |= (1u << 14); // Division by zero flag
    }
    if (den == 0) {
        m_divResult = (num >= 0) ? -1 : 1;
        m_divRem    = num;
    } else if (num == INT64_MIN && den == -1) {
        m_divResult = INT64_MIN;
        m_divRem    = 0;
    } else {
        m_divResult = num / den;
        m_divRem    = num % den;
    }
}

void NdsBus::recomputeSqrt() noexcept {
    m_sqrtCnt &= ~(1u << 15);
    const uint64_t val = (m_sqrtCnt & 1u) ? m_sqrtParam : (m_sqrtParam & 0xFFFFFFFFull);
    if (val == 0) {
        m_sqrtResult = 0;
        return;
    }
    uint64_t res = static_cast<uint64_t>(std::sqrt(static_cast<long double>(val)));
    while ((res + 1) * (res + 1) <= val && res < 0xFFFFFFFFull) ++res;
    while (res * res > val) --res;
    m_sqrtResult = static_cast<uint32_t>(res);
}

// =============================================================================
// Sincronização IPC e FIFOs Bidirecionais (ARM9 <-> ARM7)
// =============================================================================
uint16_t NdsBus::buildIpcFifoCnt(bool isArm9) const noexcept {
    const auto& sendQ = isArm9 ? m_fifo9to7 : m_fifo7to9;
    const auto& recvQ = isArm9 ? m_fifo7to9 : m_fifo9to7;
    const uint16_t lat = isArm9 ? m_ipcFifoCnt9 : m_ipcFifoCnt7;

    uint16_t out = lat & 0xC404u; // Preserva Enable (15), Error (14), RecvIRQ (10), SendIRQ (2)
    if (sendQ.empty())     out |= (1u << 0);
    if (sendQ.size() >= 16) out |= (1u << 1);
    if (recvQ.empty())     out |= (1u << 8);
    if (recvQ.size() >= 16) out |= (1u << 9);
    return out;
}

void NdsBus::writeIpcFifoCnt(bool isArm9, uint16_t val) noexcept {
    uint16_t& lat = isArm9 ? m_ipcFifoCnt9 : m_ipcFifoCnt7;
    auto& sendQ   = isArm9 ? m_fifo9to7 : m_fifo7to9;

    const bool prevSendIrq = (lat & (1u << 2)) != 0;
    const bool prevRecvIrq = (lat & (1u << 10)) != 0;

    if (val & (1u << 3)) { // Send FIFO Clear
        sendQ.clear();
        if (isArm9) m_lastFifo9to7 = 0;
        else        m_lastFifo7to9 = 0;
    }
    if (val & (1u << 14)) { // Acknowledge Error bit
        lat &= ~(1u << 14);
    }

    lat = static_cast<uint16_t>((lat & (1u << 14)) | (val & 0x8404u));

    const bool nowSendIrq = (lat & (1u << 2)) != 0;
    const bool nowRecvIrq = (lat & (1u << 10)) != 0;
    const auto& recvQ     = isArm9 ? m_fifo7to9 : m_fifo9to7;

    if (!prevSendIrq && nowSendIrq && sendQ.empty()) {
        if (isArm9) requestIrq9(NdsIrq::IpcSendEmpty);
        else        requestIrq7(NdsIrq::IpcSendEmpty);
    }
    if (!prevRecvIrq && nowRecvIrq && !recvQ.empty()) {
        if (isArm9) requestIrq9(NdsIrq::IpcRecvNotEmpty);
        else        requestIrq7(NdsIrq::IpcRecvNotEmpty);
    }
}

void NdsBus::pushIpcFifo(bool fromArm9, uint32_t word) noexcept {
    uint16_t& senderCnt   = fromArm9 ? m_ipcFifoCnt9 : m_ipcFifoCnt7;
    const uint16_t rcvCnt = fromArm9 ? m_ipcFifoCnt7 : m_ipcFifoCnt9;
    if ((senderCnt & (1u << 15)) == 0) return;

    auto& q = fromArm9 ? m_fifo9to7 : m_fifo7to9;
    if (q.size() >= 16) {
        senderCnt |= (1u << 14); // Overflow Error
        return;
    }

    const bool wasEmpty = q.empty();
    q.push_back(word);

    if (wasEmpty && (rcvCnt & (1u << 10))) {
        if (fromArm9) requestIrq7(NdsIrq::IpcRecvNotEmpty);
        else          requestIrq9(NdsIrq::IpcRecvNotEmpty);
    }
}

uint32_t NdsBus::popIpcFifo(bool forArm9) noexcept {
    uint16_t& rcvCnt      = forArm9 ? m_ipcFifoCnt9 : m_ipcFifoCnt7;
    const uint16_t sndCnt = forArm9 ? m_ipcFifoCnt7 : m_ipcFifoCnt9;
    auto& q               = forArm9 ? m_fifo7to9 : m_fifo9to7;
    uint32_t& lastVal     = forArm9 ? m_lastFifo7to9 : m_lastFifo9to7;

    if ((rcvCnt & (1u << 15)) == 0) {
        return q.empty() ? lastVal : q.front();
    }
    if (q.empty()) {
        rcvCnt |= (1u << 14); // Underflow Error
        return lastVal;
    }

    lastVal = q.front();
    q.pop_front();

    if (q.empty() && (sndCnt & (1u << 2))) {
        if (forArm9) requestIrq7(NdsIrq::IpcSendEmpty);
        else         requestIrq9(NdsIrq::IpcSendEmpty);
    }
    return lastVal;
}

// =============================================================================
// Barramento de Memória ARM9 (66.66 MHz)
// =============================================================================
uint8_t NdsBus::arm9Read8(uint32_t addr) {
    if ((addr & 0xFFFFF000u) == 0xFFFF0000u) {
        return m_bios9[addr & 0xFFFu];
    }

    switch ((addr >> 24) & 0xFFu) {
        case 0x02:
            return m_mainRam[addr & 0x3FFFFFu];
        case 0x03: {
            switch (m_wramCnt & 3u) {
                case 0: return m_sharedWram[addr & 0x7FFFu];
                case 1: return m_sharedWram[0x4000u + (addr & 0x3FFFu)];
                case 2: return m_sharedWram[addr & 0x3FFFu];
                default: return 0; // Alocada 100% ao ARM7
            }
        }
        case 0x04:
            return readIo9_8(addr);
        case 0x05:
            return m_palettes[addr & 0x7FFu];
        case 0x06: {
            const uint8_t* p = resolveVramCpuReadPtr(true, addr);
            return p ? *p : 0;
        }
        case 0x07:
            return m_oam[addr & 0x7FFu];
        case 0x08:
        case 0x09:
            return 0xFFu; // GBA Slot vazio
        default:
            return 0;
    }
}

uint16_t NdsBus::arm9Read16(uint32_t addr) {
    addr &= ~1u;
    if ((addr >> 24) == 0x04u) return readIo9_16(addr);
    return static_cast<uint16_t>(arm9Read8(addr)) |
           (static_cast<uint16_t>(arm9Read8(addr + 1)) << 8);
}

uint32_t NdsBus::arm9Read32(uint32_t addr) {
    addr &= ~3u;
    if ((addr >> 24) == 0x04u) return readIo9_32(addr);
    if ((addr >> 24) == 0x02u) {
        const uint32_t off = addr & 0x3FFFFCu;
        return static_cast<uint32_t>(m_mainRam[off]) |
               (static_cast<uint32_t>(m_mainRam[off + 1]) << 8) |
               (static_cast<uint32_t>(m_mainRam[off + 2]) << 16) |
               (static_cast<uint32_t>(m_mainRam[off + 3]) << 24);
    }
    return static_cast<uint32_t>(arm9Read16(addr)) |
           (static_cast<uint32_t>(arm9Read16(addr + 2)) << 16);
}

void NdsBus::arm9Write8(uint32_t addr, uint8_t val) {
    switch ((addr >> 24) & 0xFFu) {
        case 0x02:
            m_mainRam[addr & 0x3FFFFFu] = val;
            break;
        case 0x03:
            switch (m_wramCnt & 3u) {
                case 0: m_sharedWram[addr & 0x7FFFu] = val; break;
                case 1: m_sharedWram[0x4000u + (addr & 0x3FFFu)] = val; break;
                case 2: m_sharedWram[addr & 0x3FFFu] = val; break;
                default: break;
            }
            break;
        case 0x04:
            writeIo9_8(addr, val);
            break;
        case 0x05: {
            const uint32_t off = addr & 0x7FEu;
            m_palettes[off] = m_palettes[off + 1] = val;
            break;
        }
        case 0x06: {
            if (uint8_t* p = resolveVramCpuWritePtr(true, addr & ~1u)) {
                p[0] = p[1] = val;
            }
            break;
        }
        default:
            break;
    }
}

void NdsBus::arm9Write16(uint32_t addr, uint16_t val) {
    addr &= ~1u;
    switch ((addr >> 24) & 0xFFu) {
        case 0x02:
            m_mainRam[addr & 0x3FFFFFu]       = static_cast<uint8_t>(val & 0xFFu);
            m_mainRam[(addr + 1) & 0x3FFFFFu] = static_cast<uint8_t>((val >> 8) & 0xFFu);
            break;
        case 0x04:
            writeIo9_16(addr, val);
            break;
        case 0x05:
            m_palettes[addr & 0x7FEu]       = static_cast<uint8_t>(val & 0xFFu);
            m_palettes[(addr & 0x7FEu) + 1] = static_cast<uint8_t>((val >> 8) & 0xFFu);
            break;
        case 0x06:
            if (uint8_t* p = resolveVramCpuWritePtr(true, addr)) {
                p[0] = static_cast<uint8_t>(val & 0xFFu);
                p[1] = static_cast<uint8_t>((val >> 8) & 0xFFu);
            }
            break;
        case 0x07:
            m_oam[addr & 0x7FEu]       = static_cast<uint8_t>(val & 0xFFu);
            m_oam[(addr & 0x7FEu) + 1] = static_cast<uint8_t>((val >> 8) & 0xFFu);
            break;
        default:
            arm9Write8(addr, static_cast<uint8_t>(val & 0xFFu));
            arm9Write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
            break;
    }
}

void NdsBus::arm9Write32(uint32_t addr, uint32_t val) {
    addr &= ~3u;
    if ((addr >> 24) == 0x04u) {
        writeIo9_32(addr, val);
        return;
    }
    if ((addr >> 24) == 0x02u) {
        const uint32_t off = addr & 0x3FFFFCu;
        m_mainRam[off + 0] = static_cast<uint8_t>(val & 0xFFu);
        m_mainRam[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFFu);
        m_mainRam[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFFu);
        m_mainRam[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFFu);
        return;
    }
    arm9Write16(addr, static_cast<uint16_t>(val & 0xFFFFu));
    arm9Write16(addr + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

// =============================================================================
// Barramento de Memória ARM7 (33.51 MHz)
// =============================================================================
uint8_t NdsBus::arm7Read8(uint32_t addr) {
    switch ((addr >> 24) & 0xFFu) {
        case 0x00:
            return (addr < 0x4000u) ? m_bios7[addr] : 0;
        case 0x02:
            return m_mainRam[addr & 0x3FFFFFu];
        case 0x03: {
            if (addr >= 0x03800000u) {
                return m_arm7Wram[addr & 0xFFFFu];
            }
            switch (m_wramCnt & 3u) {
                case 0: return m_arm7Wram[addr & 0xFFFFu]; // Espelha ARM7 WRAM quando Shared=100% ARM9
                case 1: return m_sharedWram[addr & 0x3FFFu];
                case 2: return m_sharedWram[0x4000u + (addr & 0x3FFFu)];
                default: return m_sharedWram[addr & 0x7FFFu];
            }
        }
        case 0x04:
            return readIo7_8(addr);
        case 0x06: {
            const uint8_t* p = resolveVramCpuReadPtr(false, addr);
            return p ? *p : 0;
        }
        case 0x08:
        case 0x09:
            return 0xFFu;
        default:
            return 0;
    }
}

uint16_t NdsBus::arm7Read16(uint32_t addr) {
    addr &= ~1u;
    if ((addr >> 24) == 0x04u) return readIo7_16(addr);
    return static_cast<uint16_t>(arm7Read8(addr)) |
           (static_cast<uint16_t>(arm7Read8(addr + 1)) << 8);
}

uint32_t NdsBus::arm7Read32(uint32_t addr) {
    addr &= ~3u;
    if ((addr >> 24) == 0x04u) return readIo7_32(addr);
    return static_cast<uint32_t>(arm7Read16(addr)) |
           (static_cast<uint32_t>(arm7Read16(addr + 2)) << 16);
}

void NdsBus::arm7Write8(uint32_t addr, uint8_t val) {
    switch ((addr >> 24) & 0xFFu) {
        case 0x02:
            m_mainRam[addr & 0x3FFFFFu] = val;
            break;
        case 0x03:
            if (addr >= 0x03800000u) {
                m_arm7Wram[addr & 0xFFFFu] = val;
            } else {
                switch (m_wramCnt & 3u) {
                    case 0: m_arm7Wram[addr & 0xFFFFu] = val; break;
                    case 1: m_sharedWram[addr & 0x3FFFu] = val; break;
                    case 2: m_sharedWram[0x4000u + (addr & 0x3FFFu)] = val; break;
                    default: m_sharedWram[addr & 0x7FFFu] = val; break;
                }
            }
            break;
        case 0x04:
            writeIo7_8(addr, val);
            break;
        case 0x06:
            if (uint8_t* p = resolveVramCpuWritePtr(false, addr)) {
                *p = val;
            }
            break;
        default:
            break;
    }
}

void NdsBus::arm7Write16(uint32_t addr, uint16_t val) {
    addr &= ~1u;
    if ((addr >> 24) == 0x04u) {
        writeIo7_16(addr, val);
        return;
    }
    arm7Write8(addr, static_cast<uint8_t>(val & 0xFFu));
    arm7Write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsBus::arm7Write32(uint32_t addr, uint32_t val) {
    addr &= ~3u;
    if ((addr >> 24) == 0x04u) {
        writeIo7_32(addr, val);
        return;
    }
    arm7Write16(addr, static_cast<uint16_t>(val & 0xFFFFu));
    arm7Write16(addr + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

// =============================================================================
// Registradores MMIO do ARM9 (0x04000000 .. 0x04100010)
// =============================================================================
uint8_t NdsBus::readIo9_8(uint32_t addr) {
    const uint32_t off = addr & 0x00FFFFFFu;
    if (off < 0x070u) return m_gpu.readEngine2D8(0, off);
    if (off >= 0x1000u && off < 0x1070u) return m_gpu.readEngine2D8(1, off - 0x1000u);
    if (off >= 0x320u && off < 0x700u) return m_gpu.read3D8(off);

    const uint32_t w32 = readIo9_32(addr & ~3u);
    return static_cast<uint8_t>((w32 >> ((addr & 3u) * 8)) & 0xFFu);
}

uint16_t NdsBus::readIo9_16(uint32_t addr) {
    const uint32_t off = addr & 0x00FFFFFEu;
    if (off < 0x070u) return m_gpu.readEngine2D16(0, off);
    if (off >= 0x1000u && off < 0x1070u) return m_gpu.readEngine2D16(1, off - 0x1000u);
    if (off >= 0x320u && off < 0x700u) return m_gpu.read3D16(off);

    switch (off) {
        case 0x130: return m_keyInput;
        case 0x132: return m_keyCnt9;
        case 0x180: return static_cast<uint16_t>((m_ipcSync9 & 0x4F00u) | ((m_ipcSync7 >> 8) & 0x0Fu));
        case 0x184: return buildIpcFifoCnt(true);
        case 0x1A0: return m_cart.readAuxSpiCnt();
        case 0x1A2: return m_cart.readAuxSpiData();
        case 0x204: return m_exMemCnt;
        case 0x208: return static_cast<uint16_t>(m_ime9 & 1u);
        case 0x247: return m_wramCnt;
        case 0x280: return m_divCnt;
        case 0x2B0: return m_sqrtCnt;
        case 0x300: return m_postFlg9;
        case 0x304: return m_gpu.powCnt1();
    }

    if (off >= 0x100u && off < 0x110u) {
        const std::size_t t = (off - 0x100u) >> 2;
        return (off & 2u) ? m_timers9[t].control : m_timers9[t].counter;
    }

    const uint32_t w32 = readIo9_32(addr & ~3u);
    return static_cast<uint16_t>((w32 >> ((addr & 2u) * 8)) & 0xFFFFu);
}

uint32_t NdsBus::readIo9_32(uint32_t addr) {
    const uint32_t off = addr & 0x00FFFFFCu;
    if (off == 0x100000u) return popIpcFifo(true);
    if (off == 0x100010u) {
        bool moreReady = false, done = false;
        const uint32_t w = m_cart.readCardData(moreReady, done);
        if (done) requestIrq9(NdsIrq::CardDataReady);
        return w;
    }
    if (off < 0x070u) return m_gpu.readEngine2D32(0, off);
    if (off >= 0x1000u && off < 0x1070u) return m_gpu.readEngine2D32(1, off - 0x1000u);
    if (off >= 0x320u && off < 0x700u) return m_gpu.read3D32(off);

    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t r     = (off - 0x0B0u) % 12u;
        if (r == 0) return m_dma9[ch].sad;
        if (r == 4) return m_dma9[ch].dad;
        return m_dma9[ch].cnt;
    }
    if (off >= 0x0E0u && off < 0x0F0u) {
        return m_dmaFill9[(off - 0x0E0u) >> 2];
    }

    switch (off) {
        case 0x1A4: return m_cart.readRomCtrl();
        case 0x208: return m_ime9 & 1u;
        case 0x210: return m_ie9;
        case 0x214: return m_if9;
        case 0x290: return static_cast<uint32_t>(m_divNumer & 0xFFFFFFFFull);
        case 0x294: return static_cast<uint32_t>((static_cast<uint64_t>(m_divNumer) >> 32) & 0xFFFFFFFFull);
        case 0x298: return static_cast<uint32_t>(m_divDenom & 0xFFFFFFFFull);
        case 0x29C: return static_cast<uint32_t>((static_cast<uint64_t>(m_divDenom) >> 32) & 0xFFFFFFFFull);
        case 0x2A0: return static_cast<uint32_t>(m_divResult & 0xFFFFFFFFull);
        case 0x2A4: return static_cast<uint32_t>((static_cast<uint64_t>(m_divResult) >> 32) & 0xFFFFFFFFull);
        case 0x2A8: return static_cast<uint32_t>(m_divRem & 0xFFFFFFFFull);
        case 0x2AC: return static_cast<uint32_t>((static_cast<uint64_t>(m_divRem) >> 32) & 0xFFFFFFFFull);
        case 0x2B4: return m_sqrtResult;
        case 0x2B8: return static_cast<uint32_t>(m_sqrtParam & 0xFFFFFFFFull);
        case 0x2BC: return static_cast<uint32_t>((m_sqrtParam >> 32) & 0xFFFFFFFFull);
    }

    return static_cast<uint32_t>(readIo9_16(addr)) |
           (static_cast<uint32_t>(readIo9_16(addr + 2)) << 16);
}

void NdsBus::writeIo9_8(uint32_t addr, uint8_t val) {
    const uint32_t off = addr & 0x00FFFFFFu;
    if (off < 0x070u) { m_gpu.writeEngine2D8(0, off, val); return; }
    if (off >= 0x1000u && off < 0x1070u) { m_gpu.writeEngine2D8(1, off - 0x1000u, val); return; }
    if (off >= 0x320u && off < 0x700u) { m_gpu.write3D8(off, val); return; }

    if (off >= 0x1A8u && off < 0x1B0u) {
        m_cart.writeCardCmd(static_cast<uint8_t>(off - 0x1A8u), val);
        return;
    }
    if (off == 0x1A2u) {
        m_cart.writeAuxSpiData(val);
        return;
    }

    // VRAMCNT_A..G (0x240..0x246), WRAMCNT (0x247), VRAMCNT_H (0x248), VRAMCNT_I (0x249)
    if (off >= 0x240u && off <= 0x246u) {
        m_vramCnt[off - 0x240u] = val;
        return;
    }
    if (off == 0x247u) {
        m_wramCnt = val & 0x3u;
        return;
    }
    if (off == 0x248u) { m_vramCnt[7] = val; return; }
    if (off == 0x249u) { m_vramCnt[8] = val; return; }
    if (off == 0x300u) { m_postFlg9 |= (val & 1u); return; }
}

void NdsBus::writeIo9_16(uint32_t addr, uint16_t val) {
    const uint32_t off = addr & 0x00FFFFFEu;
    if (off < 0x070u) { m_gpu.writeEngine2D16(0, off, val); return; }
    if (off >= 0x1000u && off < 0x1070u) { m_gpu.writeEngine2D16(1, off - 0x1000u, val); return; }
    if (off >= 0x320u && off < 0x700u) { m_gpu.write3D16(off, val); return; }

    if (off >= 0x100u && off < 0x110u) {
        const std::size_t t = (off - 0x100u) >> 2;
        if ((off & 2u) == 0) {
            m_timers9[t].reload = val;
        } else {
            const bool wasEn = (m_timers9[t].control & 0x80u) != 0;
            const bool nowEn = (val & 0x80u) != 0;
            m_timers9[t].control = val;
            if (!wasEn && nowEn) {
                m_timers9[t].counter     = m_timers9[t].reload;
                m_timers9[t].cyclesAccum = 0;
            }
        }
        return;
    }

    switch (off) {
        case 0x132: m_keyCnt9 = val; return;
        case 0x180: { // IPCSYNC ARM9
            m_ipcSync9 = val & 0x4F00u;
            if ((val & (1u << 13)) && (m_ipcSync7 & (1u << 14))) {
                requestIrq7(NdsIrq::IpcSync);
            }
            return;
        }
        case 0x184: writeIpcFifoCnt(true, val); return;
        case 0x1A0: m_cart.writeAuxSpiCnt(val); return;
        case 0x1A2: m_cart.writeAuxSpiData(static_cast<uint8_t>(val & 0xFFu)); return;
        case 0x204: m_exMemCnt = val; return;
        case 0x208: m_ime9 = val & 1u; return;
        case 0x280: m_divCnt = val & 0x0003u; recomputeDiv(); return;
        case 0x2B0: m_sqrtCnt = val & 0x0001u; recomputeSqrt(); return;
        case 0x304: m_gpu.writePowCnt1(val); return;
    }

    // DMA9 halfword writes (ex: escrita apenas em DMAxCNT_H em +0x0A!)
    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t sub   = (off - 0x0B0u) % 12u;
        if (sub == 8) {
            m_dma9[ch].cnt = (m_dma9[ch].cnt & 0xFFFF0000u) | val;
        } else if (sub == 10) {
            const uint32_t oldCnt = m_dma9[ch].cnt;
            const uint32_t newCnt = (oldCnt & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
            onDmaWrite9(ch, oldCnt, newCnt);
        } else if (sub == 0) m_dma9[ch].sad = (m_dma9[ch].sad & 0xFFFF0000u) | val;
        else if (sub == 2)    m_dma9[ch].sad = (m_dma9[ch].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
        else if (sub == 4)    m_dma9[ch].dad = (m_dma9[ch].dad & 0xFFFF0000u) | val;
        else if (sub == 6)    m_dma9[ch].dad = (m_dma9[ch].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
        return;
    }

    writeIo9_8(addr, static_cast<uint8_t>(val & 0xFFu));
    writeIo9_8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsBus::writeIo9_32(uint32_t addr, uint32_t val) {
    const uint32_t off = addr & 0x00FFFFFCu;
    if (off < 0x070u) { m_gpu.writeEngine2D32(0, off, val); return; }
    if (off >= 0x1000u && off < 0x1070u) { m_gpu.writeEngine2D32(1, off - 0x1000u, val); return; }
    if (off >= 0x320u && off < 0x700u) { m_gpu.write3D32(off, val); return; }

    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t r     = (off - 0x0B0u) % 12u;
        if (r == 0)      m_dma9[ch].sad = val;
        else if (r == 4) m_dma9[ch].dad = val;
        else             onDmaWrite9(ch, m_dma9[ch].cnt, val);
        return;
    }
    if (off >= 0x0E0u && off < 0x0F0u) {
        m_dmaFill9[(off - 0x0E0u) >> 2] = val;
        return;
    }

    switch (off) {
        case 0x188: pushIpcFifo(true, val); return;
        case 0x1A4: {
            bool dataReady = false, done = false;
            m_cart.writeRomCtrl(val, dataReady, done);
            if (dataReady) triggerDmaCardData(true);
            if (done)      requestIrq9(NdsIrq::CardDataReady);
            return;
        }
        case 0x208: m_ime9 = val & 1u; return;
        case 0x210: m_ie9  = val; return;
        case 0x214: m_if9 &= ~val; return; // Write-1-to-clear IF9!
        case 0x290:
            m_divNumer = static_cast<int64_t>((static_cast<uint64_t>(m_divNumer) & 0xFFFFFFFF00000000ull) | val);
            recomputeDiv();
            return;
        case 0x294:
            m_divNumer = static_cast<int64_t>((static_cast<uint64_t>(m_divNumer) & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32));
            recomputeDiv();
            return;
        case 0x298:
            m_divDenom = static_cast<int64_t>((static_cast<uint64_t>(m_divDenom) & 0xFFFFFFFF00000000ull) | val);
            recomputeDiv();
            return;
        case 0x29C:
            m_divDenom = static_cast<int64_t>((static_cast<uint64_t>(m_divDenom) & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32));
            recomputeDiv();
            return;
        case 0x2B8:
            m_sqrtParam = (m_sqrtParam & 0xFFFFFFFF00000000ull) | val;
            recomputeSqrt();
            return;
        case 0x2BC:
            m_sqrtParam = (m_sqrtParam & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
            recomputeSqrt();
            return;
    }

    writeIo9_16(addr, static_cast<uint16_t>(val & 0xFFFFu));
    writeIo9_16(addr + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

// =============================================================================
// Registradores MMIO do ARM7 (0x04000000 .. 0x0480FFFF)
// =============================================================================
uint8_t NdsBus::readIo7_8(uint32_t addr) {
    if (addr >= 0x04800000u && addr < 0x04810000u) {
        return m_wifiRam[addr & 0x7FFFu];
    }
    const uint32_t off = addr & 0x00FFFFFFu;
    if (off >= 0x400u && off < 0x520u) {
        return m_spu.read8(off - 0x400u);
    }
    if (off == 0x138u) return m_rtcReg;
    if (off == 0x240u) {
        // VRAMSTAT no ARM7: bit 0 = Bank C no ARM7, bit 1 = Bank D no ARM7
        uint8_t st = 0;
        if ((m_vramCnt[2] & 0x80u) && (m_vramCnt[2] & 0x07u) == 2u) st |= 1u;
        if ((m_vramCnt[3] & 0x80u) && (m_vramCnt[3] & 0x07u) == 2u) st |= 2u;
        return st;
    }
    if (off == 0x241u) return m_wramCnt;
    if (off == 0x300u) return m_postFlg7;
    if (off == 0x301u) return m_haltCnt7;

    const uint32_t w32 = readIo7_32(addr & ~3u);
    return static_cast<uint8_t>((w32 >> ((addr & 3u) * 8)) & 0xFFu);
}

uint16_t NdsBus::readIo7_16(uint32_t addr) {
    if (addr >= 0x04800000u && addr < 0x04810000u) {
        return get16Le(m_wifiRam.data(), addr & 0x7FFEu);
    }
    const uint32_t off = addr & 0x00FFFFFEu;
    if (off >= 0x400u && off < 0x520u) {
        return m_spu.read16(off - 0x400u);
    }
    if (off >= 0x100u && off < 0x110u) {
        const std::size_t t = (off - 0x100u) >> 2;
        return (off & 2u) ? m_timers7[t].control : m_timers7[t].counter;
    }

    switch (off) {
        case 0x004: return m_gpu.dispStat7();
        case 0x006: return m_gpu.vcount();
        case 0x130: return m_keyInput;
        case 0x132: return m_keyCnt7;
        case 0x136: return extKeyInReg();
        case 0x180: return static_cast<uint16_t>((m_ipcSync7 & 0x4F00u) | ((m_ipcSync9 >> 8) & 0x0Fu));
        case 0x184: return buildIpcFifoCnt(false);
        case 0x1A0: return m_cart.readAuxSpiCnt();
        case 0x1A2: return m_cart.readAuxSpiData();
        case 0x1C0: return m_touch.readSpiCnt();
        case 0x1C2: return m_touch.readSpiData();
        case 0x204: return m_exMemCnt;
        case 0x208: return static_cast<uint16_t>(m_ime7 & 1u);
    }

    const uint32_t w32 = readIo7_32(addr & ~3u);
    return static_cast<uint16_t>((w32 >> ((addr & 2u) * 8)) & 0xFFFFu);
}

uint32_t NdsBus::readIo7_32(uint32_t addr) {
    const uint32_t off = addr & 0x00FFFFFCu;
    if (off == 0x100000u) return popIpcFifo(false);
    if (off == 0x100010u) {
        bool moreReady = false, done = false;
        const uint32_t w = m_cart.readCardData(moreReady, done);
        if (done) requestIrq7(NdsIrq::CardDataReady);
        return w;
    }
    if (off >= 0x400u && off < 0x520u) {
        return m_spu.read32(off - 0x400u);
    }
    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t r     = (off - 0x0B0u) % 12u;
        if (r == 0) return m_dma7[ch].sad;
        if (r == 4) return m_dma7[ch].dad;
        return m_dma7[ch].cnt;
    }
    switch (off) {
        case 0x1A4: return m_cart.readRomCtrl();
        case 0x208: return m_ime7 & 1u;
        case 0x210: return m_ie7;
        case 0x214: return m_if7;
    }
    return static_cast<uint32_t>(readIo7_16(addr)) |
           (static_cast<uint32_t>(readIo7_16(addr + 2)) << 16);
}

void NdsBus::writeIo7_8(uint32_t addr, uint8_t val) {
    if (addr >= 0x04800000u && addr < 0x04810000u) {
        m_wifiRam[addr & 0x7FFFu] = val;
        return;
    }
    const uint32_t off = addr & 0x00FFFFFFu;
    if (off >= 0x400u && off < 0x520u) {
        m_spu.write8(off - 0x400u, val);
        return;
    }
    if (off >= 0x1A8u && off < 0x1B0u) {
        m_cart.writeCardCmd(static_cast<uint8_t>(off - 0x1A8u), val);
        return;
    }
    if (off == 0x138u) { m_rtcReg = val; return; }
    if (off == 0x1A2u) { m_cart.writeAuxSpiData(val); return; }
    if (off == 0x1C2u) {
        m_touch.writeSpiData(val);
        if (m_touch.readSpiCnt() & (1u << 14)) {
            requestIrq7(NdsIrq::SpiDataReady);
        }
        return;
    }
    if (off == 0x300u) { m_postFlg7 |= (val & 1u); return; }
    if (off == 0x301u) {
        m_haltCnt7 = val;
        if (((val >> 6) & 3u) == 2u && m_arm7) {
            m_arm7->halt(); // HALTCNT = Halt CPU até próxima IRQ
        }
        return;
    }
}

void NdsBus::writeIo7_16(uint32_t addr, uint16_t val) {
    if (addr >= 0x04800000u && addr < 0x04810000u) {
        put16Le(m_wifiRam.data(), addr & 0x7FFEu, val);
        return;
    }
    const uint32_t off = addr & 0x00FFFFFEu;
    if (off >= 0x400u && off < 0x520u) {
        m_spu.write16(off - 0x400u, val);
        return;
    }
    if (off >= 0x100u && off < 0x110u) {
        const std::size_t t = (off - 0x100u) >> 2;
        if ((off & 2u) == 0) {
            m_timers7[t].reload = val;
        } else {
            const bool wasEn = (m_timers7[t].control & 0x80u) != 0;
            const bool nowEn = (val & 0x80u) != 0;
            m_timers7[t].control = val;
            if (!wasEn && nowEn) {
                m_timers7[t].counter     = m_timers7[t].reload;
                m_timers7[t].cyclesAccum = 0;
            }
        }
        return;
    }

    switch (off) {
        case 0x004: m_gpu.writeDispStat7(val); return;
        case 0x132: m_keyCnt7 = val; return;
        case 0x180: { // IPCSYNC ARM7
            m_ipcSync7 = val & 0x4F00u;
            if ((val & (1u << 13)) && (m_ipcSync9 & (1u << 14))) {
                requestIrq9(NdsIrq::IpcSync);
            }
            return;
        }
        case 0x184: writeIpcFifoCnt(false, val); return;
        case 0x1A0: m_cart.writeAuxSpiCnt(val); return;
        case 0x1A2: m_cart.writeAuxSpiData(static_cast<uint8_t>(val & 0xFFu)); return;
        case 0x1C0: m_touch.writeSpiCnt(val); return;
        case 0x1C2:
            m_touch.writeSpiData(static_cast<uint8_t>(val & 0xFFu));
            if (m_touch.readSpiCnt() & (1u << 14)) {
                requestIrq7(NdsIrq::SpiDataReady);
            }
            return;
        case 0x208: m_ime7 = val & 1u; return;
    }

    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t sub   = (off - 0x0B0u) % 12u;
        if (sub == 8) {
            m_dma7[ch].cnt = (m_dma7[ch].cnt & 0xFFFF0000u) | val;
        } else if (sub == 10) {
            const uint32_t oldCnt = m_dma7[ch].cnt;
            const uint32_t newCnt = (oldCnt & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
            onDmaWrite7(ch, oldCnt, newCnt);
        } else if (sub == 0) m_dma7[ch].sad = (m_dma7[ch].sad & 0xFFFF0000u) | val;
        else if (sub == 2)    m_dma7[ch].sad = (m_dma7[ch].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
        else if (sub == 4)    m_dma7[ch].dad = (m_dma7[ch].dad & 0xFFFF0000u) | val;
        else if (sub == 6)    m_dma7[ch].dad = (m_dma7[ch].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16);
        return;
    }

    writeIo7_8(addr, static_cast<uint8_t>(val & 0xFFu));
    writeIo7_8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsBus::writeIo7_32(uint32_t addr, uint32_t val) {
    const uint32_t off = addr & 0x00FFFFFCu;
    if (off >= 0x400u && off < 0x520u) {
        m_spu.write32(off - 0x400u, val);
        return;
    }
    if (off >= 0x0B0u && off < 0x0E0u) {
        const std::size_t ch = (off - 0x0B0u) / 12u;
        const uint32_t r     = (off - 0x0B0u) % 12u;
        if (r == 0)      m_dma7[ch].sad = val;
        else if (r == 4) m_dma7[ch].dad = val;
        else             onDmaWrite7(ch, m_dma7[ch].cnt, val);
        return;
    }
    switch (off) {
        case 0x188: pushIpcFifo(false, val); return;
        case 0x1A4: {
            bool dataReady = false, done = false;
            m_cart.writeRomCtrl(val, dataReady, done);
            if (dataReady) triggerDmaCardData(false);
            if (done)      requestIrq7(NdsIrq::CardDataReady);
            return;
        }
        case 0x208: m_ime7 = val & 1u; return;
        case 0x210: m_ie7  = val; return;
        case 0x214: m_if7 &= ~val; return; // Write-1-to-clear IF7!
    }
    writeIo7_16(addr, static_cast<uint16_t>(val & 0xFFFFu));
    writeIo7_16(addr + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

// =============================================================================
// DMA ARM9 & ARM7 + Timers
// =============================================================================
void NdsBus::onDmaWrite9(std::size_t ch, uint32_t oldCnt, uint32_t newCnt) {
    auto& d = m_dma9[ch];
    d.cnt   = newCnt;
    const bool wasEn = (oldCnt & (1u << 31)) != 0;
    const bool nowEn = (newCnt & (1u << 31)) != 0;
    if (!wasEn && nowEn) {
        d.internalSrc   = d.sad;
        d.internalDst   = d.dad;
        d.internalCount = newCnt & 0x1FFFFFu;
        if (d.internalCount == 0) d.internalCount = 0x200000u;

        const uint8_t mode = (newCnt >> 27) & 0x7u;
        if (mode == 0 || (mode == 7 && m_gpu.isGxFifoLessThanHalfFull())) {
            executeDma9(ch);
        }
    }
}

void NdsBus::executeDma9(std::size_t ch) {
    auto& d = m_dma9[ch];
    const bool is32       = (d.cnt & (1u << 26)) != 0;
    const uint32_t step   = is32 ? 4u : 2u;
    const uint8_t dstCtrl = (d.cnt >> 21) & 0x3u;
    const uint8_t srcCtrl = (d.cnt >> 23) & 0x3u;
    const uint32_t count  = (d.internalCount == 0) ? 0x200000u : d.internalCount;

    for (uint32_t i = 0; i < count; ++i) {
        if (is32) {
            const uint32_t v = m_arm9 ? m_arm9->dataRead32(d.internalSrc) : arm9Read32(d.internalSrc);
            if (m_arm9) m_arm9->dataWrite32(d.internalDst, v);
            else        arm9Write32(d.internalDst, v);
        } else {
            const uint16_t v = m_arm9 ? m_arm9->dataRead16(d.internalSrc) : arm9Read16(d.internalSrc);
            if (m_arm9) m_arm9->dataWrite16(d.internalDst, v);
            else        arm9Write16(d.internalDst, v);
        }

        if (srcCtrl == 0 || srcCtrl == 3) d.internalSrc += step;
        else if (srcCtrl == 1)            d.internalSrc -= step;

        if (dstCtrl == 0 || dstCtrl == 3) d.internalDst += step;
        else if (dstCtrl == 1)            d.internalDst -= step;
    }

    if (dstCtrl == 3) d.internalDst = d.dad;

    if ((d.cnt & (1u << 25)) == 0) {
        d.cnt &= ~(1u << 31);
    } else {
        d.internalCount = d.cnt & 0x1FFFFFu;
    }

    if (d.cnt & (1u << 30)) {
        requestIrq9(NdsIrq::Dma0 << ch);
    }
}

void NdsBus::onDmaWrite7(std::size_t ch, uint32_t oldCnt, uint32_t newCnt) {
    auto& d = m_dma7[ch];
    d.cnt   = newCnt;
    const bool wasEn = (oldCnt & (1u << 31)) != 0;
    const bool nowEn = (newCnt & (1u << 31)) != 0;
    if (!wasEn && nowEn) {
        d.internalSrc   = d.sad;
        d.internalDst   = d.dad;
        const uint32_t mask = (ch == 3) ? 0xFFFFu : 0x3FFFu;
        d.internalCount = newCnt & mask;
        if (d.internalCount == 0) d.internalCount = mask + 1u;

        const uint8_t mode = (newCnt >> 28) & 0x3u;
        if (mode == 0) {
            executeDma7(ch);
        }
    }
}

void NdsBus::executeDma7(std::size_t ch) {
    auto& d = m_dma7[ch];
    const bool is32       = (d.cnt & (1u << 26)) != 0;
    const uint32_t step   = is32 ? 4u : 2u;
    const uint8_t dstCtrl = (d.cnt >> 21) & 0x3u;
    const uint8_t srcCtrl = (d.cnt >> 23) & 0x3u;

    for (uint32_t i = 0; i < d.internalCount; ++i) {
        if (is32) arm7Write32(d.internalDst, arm7Read32(d.internalSrc));
        else      arm7Write16(d.internalDst, arm7Read16(d.internalSrc));

        if (srcCtrl == 0 || srcCtrl == 3) d.internalSrc += step;
        else if (srcCtrl == 1)            d.internalSrc -= step;

        if (dstCtrl == 0 || dstCtrl == 3) d.internalDst += step;
        else if (dstCtrl == 1)            d.internalDst -= step;
    }

    if (dstCtrl == 3) d.internalDst = d.dad;
    if ((d.cnt & (1u << 25)) == 0) d.cnt &= ~(1u << 31);
    if (d.cnt & (1u << 30)) requestIrq7(NdsIrq::Dma0 << ch);
}

void NdsBus::triggerDmaVBlank() {
    for (std::size_t ch = 0; ch < 4; ++ch) {
        if ((m_dma9[ch].cnt & (1u << 31)) && (((m_dma9[ch].cnt >> 27) & 7u) == 1u)) executeDma9(ch);
        if ((m_dma7[ch].cnt & (1u << 31)) && (((m_dma7[ch].cnt >> 28) & 3u) == 1u)) executeDma7(ch);
    }
}

void NdsBus::triggerDmaHBlank() {
    for (std::size_t ch = 0; ch < 4; ++ch) {
        if ((m_dma9[ch].cnt & (1u << 31)) && (((m_dma9[ch].cnt >> 27) & 7u) == 2u)) executeDma9(ch);
    }
}

void NdsBus::triggerDmaGxFifo() {
    for (std::size_t ch = 0; ch < 4; ++ch) {
        if ((m_dma9[ch].cnt & (1u << 31)) && (((m_dma9[ch].cnt >> 27) & 7u) == 7u)) {
            executeDma9(ch);
        }
    }
}

void NdsBus::triggerDmaCardData(bool forArm9) {
    if (forArm9) {
        for (std::size_t ch = 0; ch < 4; ++ch) {
            if ((m_dma9[ch].cnt & (1u << 31)) && (((m_dma9[ch].cnt >> 27) & 7u) == 5u)) {
                while (m_cart.readRomCtrl() & (1u << 23)) {
                    executeDma9(ch);
                    if ((m_dma9[ch].cnt & (1u << 31)) == 0) break;
                }
            }
        }
    } else {
        for (std::size_t ch = 0; ch < 4; ++ch) {
            if ((m_dma7[ch].cnt & (1u << 31)) && (((m_dma7[ch].cnt >> 28) & 3u) == 2u)) {
                while (m_cart.readRomCtrl() & (1u << 23)) {
                    executeDma7(ch);
                    if ((m_dma7[ch].cnt & (1u << 31)) == 0) break;
                }
            }
        }
    }
}

void NdsBus::stepTimersArm7Cycles(int32_t arm7Cycles) {
    // Base do clock de timers no NDS = 33.513982 MHz (igual ao clock do ARM7!)
    static constexpr int32_t PRESCALERS[4] = {1, 64, 256, 1024};

    auto stepBank = [&](std::array<TimerChannel, 4>& bank, bool isArm9) {
        bool prevOverflow = false;
        for (std::size_t i = 0; i < 4; ++i) {
            auto& t = bank[i];
            if ((t.control & 0x80u) == 0) {
                prevOverflow = false;
                continue;
            }
            const bool cascade = (i > 0) && ((t.control & 0x04u) != 0);
            uint32_t ticks = 0;
            if (cascade) {
                ticks = prevOverflow ? 1u : 0u;
            } else {
                const int32_t pre = PRESCALERS[t.control & 3u];
                t.cyclesAccum += arm7Cycles;
                ticks = static_cast<uint32_t>(t.cyclesAccum / pre);
                t.cyclesAccum %= pre;
            }

            prevOverflow = false;
            if (ticks > 0) {
                uint32_t sum = static_cast<uint32_t>(t.counter) + ticks;
                while (sum > 0xFFFFu) {
                    prevOverflow = true;
                    sum = static_cast<uint32_t>(t.reload) + (sum - 0x10000u);
                    if (t.control & 0x40u) {
                        if (isArm9) requestIrq9(NdsIrq::Timer0 << i);
                        else        requestIrq7(NdsIrq::Timer0 << i);
                    }
                    if (t.reload == 0xFFFFu) {
                        sum = 0xFFFFu;
                        break;
                    }
                }
                t.counter = static_cast<uint16_t>(sum);
            }
        }
    };

    stepBank(m_timers9, true);
    stepBank(m_timers7, false);
}

void NdsBus::serializeState(SaveStateWriter& out) const {
    out.writeBytes(m_mainRam);
    out.writeArray(m_sharedWram);
    out.writeArray(m_arm7Wram);
    out.writeArray(m_vramA);
    out.writeArray(m_vramB);
    out.writeArray(m_vramC);
    out.writeArray(m_vramD);
    out.writeArray(m_vramE);
    out.writeArray(m_vramF);
    out.writeArray(m_vramG);
    out.writeArray(m_vramH);
    out.writeArray(m_vramI);
    out.writeArray(m_vramCnt);
    out.writePod(m_wramCnt);
    out.writeArray(m_palettes);
    out.writeArray(m_oam);
    out.writePod(m_ime9); out.writePod(m_ie9); out.writePod(m_if9);
    out.writePod(m_ime7); out.writePod(m_ie7); out.writePod(m_if7);
    out.writePod(m_exMemCnt);
    out.writePod(m_ipcSync9); out.writePod(m_ipcSync7);
    out.writePod(m_ipcFifoCnt9); out.writePod(m_ipcFifoCnt7);
    out.writeArray(m_dma9);
    out.writeArray(m_dmaFill9);
    out.writeArray(m_dma7);
    out.writeArray(m_timers9);
    out.writeArray(m_timers7);
    out.writePod(m_divCnt); out.writePod(m_divNumer); out.writePod(m_divDenom);
    out.writePod(m_divResult); out.writePod(m_divRem);
    out.writePod(m_sqrtCnt); out.writePod(m_sqrtParam); out.writePod(m_sqrtResult);
}

bool NdsBus::deserializeState(SaveStateReader& in) {
    m_fifo9to7.clear();
    m_fifo7to9.clear();
    return in.readBytes(m_mainRam) &&
           in.readArray(m_sharedWram) && in.readArray(m_arm7Wram) &&
           in.readArray(m_vramA) && in.readArray(m_vramB) &&
           in.readArray(m_vramC) && in.readArray(m_vramD) &&
           in.readArray(m_vramE) && in.readArray(m_vramF) &&
           in.readArray(m_vramG) && in.readArray(m_vramH) &&
           in.readArray(m_vramI) && in.readArray(m_vramCnt) &&
           in.readPod(m_wramCnt) && in.readArray(m_palettes) && in.readArray(m_oam) &&
           in.readPod(m_ime9) && in.readPod(m_ie9) && in.readPod(m_if9) &&
           in.readPod(m_ime7) && in.readPod(m_ie7) && in.readPod(m_if7) &&
           in.readPod(m_exMemCnt) &&
           in.readPod(m_ipcSync9) && in.readPod(m_ipcSync7) &&
           in.readPod(m_ipcFifoCnt9) && in.readPod(m_ipcFifoCnt7) &&
           in.readArray(m_dma9) && in.readArray(m_dmaFill9) && in.readArray(m_dma7) &&
           in.readArray(m_timers9) && in.readArray(m_timers7) &&
           in.readPod(m_divCnt) && in.readPod(m_divNumer) && in.readPod(m_divDenom) &&
           in.readPod(m_divResult) && in.readPod(m_divRem) &&
           in.readPod(m_sqrtCnt) && in.readPod(m_sqrtParam) && in.readPod(m_sqrtResult);
}

} // namespace gba::core::nds

