#include "core/memory/MemoryBus.hpp"
#include "core/apu/Apu.hpp"
#include "core/cpu/Arm7Tdmi.hpp"
#include "core/input/InputManager.hpp"
#include "core/memory/save/SaveDetector.hpp"
#include "core/ppu/Ppu.hpp"
#include <algorithm>
#include <cstring>

namespace gba::core {

MemoryBus::MemoryBus(Ppu& ppu, Apu& apu, InputManager& input)
    : m_ppu(ppu)
    , m_apu(apu)
    , m_input(input) {
    reset();
}

void MemoryBus::reset() {
    m_bios.fill(0);
    m_ewram.fill(0);
    m_iwram.fill(0);
    m_io.fill(0);
    m_ie      = 0;
    m_if      = 0;
    m_ime     = 0;
    m_waitcnt = 0;
    m_sioCyclesRemaining = 0;
    m_saveReadCount  = 0;
    m_saveWriteCount = 0;
    m_dma.fill(DmaChannel{});
    m_timers.fill(TimerChannel{});
    installBuiltInBiosVectors();
}

void MemoryBus::installBuiltInBiosVectors() {
    auto writeBios32 = [this](uint32_t addr, uint32_t instr) {
        m_bios[addr + 0] = static_cast<uint8_t>(instr & 0xFF);
        m_bios[addr + 1] = static_cast<uint8_t>((instr >> 8) & 0xFF);
        m_bios[addr + 2] = static_cast<uint8_t>((instr >> 16) & 0xFF);
        m_bios[addr + 3] = static_cast<uint8_t>((instr >> 24) & 0xFF);
    };

    // 0x00000000: Reset Vector -> B 0x08000000 (tratado em reset)
    writeBios32(0x00, 0xEA000004u);

    // 0x00000018: IRQ Vector -> B 0x00000128 (Rotina de dispatch de IRQ da BIOS GBA)
    writeBios32(0x18, 0xEA000042u);

    // 0x00000128: Rotina HLE de IRQ compatível com o ponteiro de callback 0x03FFFFFC:
    // STMDB sp!, {r0-r3, r12, lr}
    // MOV   r0, #0x04000000
    // ADD   lr, pc, #0
    // LDR   pc, [r0, #-4]       ; Salta para [0x03FFFFFC] (handler da ROM)
    // LDMIA sp!, {r0-r3, r12, lr}
    // SUBS  pc, lr, #4          ; Retorna da interrupção restaurando CPSR = SPSR_irq
    writeBios32(0x128, 0xE92D500Fu);
    writeBios32(0x12C, 0xE3A00301u);
    writeBios32(0x130, 0xE28FE000u);
    writeBios32(0x134, 0xE510F004u);
    writeBios32(0x138, 0xE8BD500Fu);
    writeBios32(0x13C, 0xE25EF004u);

    // 0x00000140: Handler padrao seguro caso a ROM nao configure [0x03FFFFFC]:
    // MOV  r0, #0x04000000
    // ADD  r0, r0, #0x200
    // LDR  r1, [r0]             ; r1 = IE | (IF << 16)
    // MOV  r1, r1, LSR #16      ; r1 = IF
    // STRH r1, [r0, #2]         ; Acknowledge IF (0x04000202)
    // LDRH r2, [r0, #-0x208]    ; r2 = INTR_CHECK (0x03FFFFF8)
    // ORR  r2, r2, r1
    // STRH r2, [r0, #-0x208]    ; Atualiza INTR_CHECK (0x03FFFFF8)
    // BX   lr
    writeBios32(0x140, 0xE3A00301u);
    writeBios32(0x144, 0xE2800C02u);
    writeBios32(0x148, 0xE5901000u);
    writeBios32(0x14C, 0xE1A01821u);
    writeBios32(0x150, 0xE1C010B2u);
    writeBios32(0x154, 0xE15022B8u);
    writeBios32(0x158, 0xE1822001u);
    writeBios32(0x15C, 0xE14022B8u);
    writeBios32(0x160, 0xE12FFF1Eu);

    // Inicializa [0x03FFFFFC] (m_iwram[0x7FFC..0x7FFF]) apontando para 0x00000140
    m_iwram[0x7FFC] = 0x40;
    m_iwram[0x7FFD] = 0x01;
    m_iwram[0x7FFE] = 0x00;
    m_iwram[0x7FFF] = 0x00;
}

bool MemoryBus::loadBios(std::span<const uint8_t> biosData) {
    if (biosData.empty() || biosData.size() > m_bios.size()) return false;
    std::copy(biosData.begin(), biosData.end(), m_bios.begin());
    return true;
}

bool MemoryBus::loadRom(std::span<const uint8_t> romData, SaveType forcedSaveType) {
    if (romData.empty()) return false;
    reset();

    m_rom.assign(romData.begin(), romData.end());

    // Extrai o título da ROM do cabeçalho GBA (0xA0..0xAB)
    if (m_rom.size() >= 0xAC) {
        m_romTitle.clear();
        for (std::size_t i = 0xA0; i < 0xAC; ++i) {
            const char c = static_cast<char>(m_rom[i]);
            if (c == '\0') break;
            if (c >= 32 && c <= 126) m_romTitle.push_back(c);
        }
        if (m_romTitle.empty()) m_romTitle = "GBA ROM";
    }

    const SaveType detected = (forcedSaveType == SaveType::AutoDetect)
        ? SaveDetector::detect(m_rom)
        : forcedSaveType;

    m_backupMedia = SaveDetector::createMedia(detected);
    if (m_backupMedia) {
        m_backupMedia->setTraceEnabled(m_traceEnabled);
    }
    return true;
}

bool MemoryBus::loadSaveFile(const std::filesystem::path& path) {
    return m_backupMedia ? m_backupMedia->loadFromFile(path) : false;
}

bool MemoryBus::flushSaveFile(const std::filesystem::path& path) {
    if (!m_backupMedia) return false;
    const bool ok = m_backupMedia->saveToFile(path);
    if (ok) m_backupMedia->clearDirty();
    return ok;
}

uint8_t MemoryBus::read8(uint32_t addr) {
    const uint8_t region = static_cast<uint8_t>((addr >> 24) & 0xFF);
    switch (region) {
        case 0x00:
            if (addr < m_bios.size()) return m_bios[addr];
            return 0;
        case 0x02:
            return m_ewram[addr & 0x3FFFF];
        case 0x03:
            return m_iwram[addr & 0x7FFF];
        case 0x04:
            return readIo8(addr & 0xFFFFFF);
        case 0x05:
            return m_ppu.readPalette(addr & 0x3FF);
        case 0x06:
            return m_ppu.readVram(addr & 0x1FFFF);
        case 0x07:
            return m_ppu.readOam(addr & 0x3FF);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C: {
            const uint32_t offset = addr & 0x01FFFFFF;
            if (offset < m_rom.size()) return m_rom[offset];
            return static_cast<uint8_t>((offset >> 1) & 0xFF); // Open-bus / prefetch
        }
        case 0x0D: {
            if (m_backupMedia &&
                (m_backupMedia->getType() == SaveType::Eeprom512B ||
                 m_backupMedia->getType() == SaveType::Eeprom8K)) {
                return m_backupMedia->readByte(addr);
            }
            const uint32_t offset = addr & 0x01FFFFFF;
            if (offset < m_rom.size()) return m_rom[offset];
            return 0xFF;
        }
        case 0x0E:
        case 0x0F:
            ++m_saveReadCount;
            return m_backupMedia ? m_backupMedia->readByte(addr & 0xFFFF) : 0xFF;
        default:
            return 0;
    }
}

uint16_t MemoryBus::read16(uint32_t addr) {
    const uint32_t aligned = addr & ~1u;
    if (((aligned >> 24) & 0xFF) == 0x0D && m_backupMedia &&
        (m_backupMedia->getType() == SaveType::Eeprom512B ||
         m_backupMedia->getType() == SaveType::Eeprom8K)) {
        return m_backupMedia->readHalfWord(aligned);
    }
    return static_cast<uint16_t>(read8(aligned) | (static_cast<uint16_t>(read8(aligned + 1)) << 8));
}

uint32_t MemoryBus::read32(uint32_t addr) {
    const uint32_t aligned = addr & ~3u;
    return static_cast<uint32_t>(read8(aligned)) |
           (static_cast<uint32_t>(read8(aligned + 1)) << 8) |
           (static_cast<uint32_t>(read8(aligned + 2)) << 16) |
           (static_cast<uint32_t>(read8(aligned + 3)) << 24);
}

void MemoryBus::write8(uint32_t addr, uint8_t val) {
    const uint8_t region = static_cast<uint8_t>((addr >> 24) & 0xFF);
    switch (region) {
        case 0x02:
            m_ewram[addr & 0x3FFFF] = val;
            break;
        case 0x03:
            m_iwram[addr & 0x7FFF] = val;
            break;
        case 0x04:
            writeIo8(addr & 0xFFFFFF, val);
            break;
        case 0x05: {
            // Escrita de 8 bits na Palette RAM duplica o byte nos 16 bits do halfword
            const uint32_t aligned = addr & ~1u;
            m_ppu.writePalette(aligned, val);
            m_ppu.writePalette(aligned + 1, val);
            break;
        }
        case 0x06: {
            const uint32_t offset = addr & 0x1FFFF;
            if (offset < 0x10000) {
                const uint32_t aligned = offset & ~1u;
                m_ppu.writeVram(aligned, val);
                m_ppu.writeVram(aligned + 1, val);
            }
            break;
        }
        case 0x0D:
            if (m_backupMedia &&
                (m_backupMedia->getType() == SaveType::Eeprom512B ||
                 m_backupMedia->getType() == SaveType::Eeprom8K)) {
                m_backupMedia->writeByte(addr, val);
            }
            break;
        case 0x0E:
        case 0x0F:
            ++m_saveWriteCount;
            if (m_backupMedia) m_backupMedia->writeByte(addr & 0xFFFF, val);
            break;
        default:
            break;
    }
}

void MemoryBus::write16(uint32_t addr, uint16_t val) {
    const uint32_t aligned = addr & ~1u;
    const uint8_t  region  = static_cast<uint8_t>((aligned >> 24) & 0xFF);

    if (region == 0x05) {
        m_ppu.writePalette(aligned & 0x3FF, static_cast<uint8_t>(val & 0xFF));
        m_ppu.writePalette((aligned + 1) & 0x3FF, static_cast<uint8_t>((val >> 8) & 0xFF));
        return;
    }
    if (region == 0x06) {
        m_ppu.writeVram(aligned & 0x1FFFF, static_cast<uint8_t>(val & 0xFF));
        m_ppu.writeVram((aligned + 1) & 0x1FFFF, static_cast<uint8_t>((val >> 8) & 0xFF));
        return;
    }
    if (region == 0x07) {
        m_ppu.writeOam(aligned & 0x3FF, static_cast<uint8_t>(val & 0xFF));
        m_ppu.writeOam((aligned + 1) & 0x3FF, static_cast<uint8_t>((val >> 8) & 0xFF));
        return;
    }
    if (region == 0x0D && m_backupMedia &&
        (m_backupMedia->getType() == SaveType::Eeprom512B ||
         m_backupMedia->getType() == SaveType::Eeprom8K)) {
        m_backupMedia->writeHalfWord(aligned, val);
        return;
    }

    write8(aligned, static_cast<uint8_t>(val & 0xFF));
    write8(aligned + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
}

void MemoryBus::write32(uint32_t addr, uint32_t val) {
    const uint32_t aligned = addr & ~3u;
    if (aligned == 0x040000A0) {
        m_apu.writeFifoA(val);
        return;
    }
    if (aligned == 0x040000A4) {
        m_apu.writeFifoB(val);
        return;
    }
    write16(aligned, static_cast<uint16_t>(val & 0xFFFF));
    write16(aligned + 2, static_cast<uint16_t>((val >> 16) & 0xFFFF));
}

uint8_t MemoryBus::readIo8(uint32_t offset) {
    if (offset >= 0x400) return 0;

    if (offset < 0x60) {
        return m_ppu.readRegister(offset);
    }
    if (offset >= 0x60 && offset < 0xB0) {
        return m_apu.readRegister(offset);
    }

    switch (offset) {
        // Timers 0-3
        case 0x100: return static_cast<uint8_t>(m_timers[0].counter & 0xFF);
        case 0x101: return static_cast<uint8_t>((m_timers[0].counter >> 8) & 0xFF);
        case 0x102: return static_cast<uint8_t>(m_timers[0].control & 0xFF);
        case 0x104: return static_cast<uint8_t>(m_timers[1].counter & 0xFF);
        case 0x105: return static_cast<uint8_t>((m_timers[1].counter >> 8) & 0xFF);
        case 0x106: return static_cast<uint8_t>(m_timers[1].control & 0xFF);
        case 0x108: return static_cast<uint8_t>(m_timers[2].counter & 0xFF);
        case 0x109: return static_cast<uint8_t>((m_timers[2].counter >> 8) & 0xFF);
        case 0x10A: return static_cast<uint8_t>(m_timers[2].control & 0xFF);
        case 0x10C: return static_cast<uint8_t>(m_timers[3].counter & 0xFF);
        case 0x10D: return static_cast<uint8_t>((m_timers[3].counter >> 8) & 0xFF);
        case 0x10E: return static_cast<uint8_t>(m_timers[3].control & 0xFF);

        // KEYINPUT (0x04000130) & KEYCNT (0x04000132)
        case 0x130: return static_cast<uint8_t>(m_input.readKeyInput() & 0xFF);
        case 0x131: return static_cast<uint8_t>((m_input.readKeyInput() >> 8) & 0xFF);
        case 0x132: return static_cast<uint8_t>(m_input.readKeyCnt() & 0xFF);
        case 0x133: return static_cast<uint8_t>((m_input.readKeyCnt() >> 8) & 0xFF);

        // Interrupts
        case 0x200: return static_cast<uint8_t>(m_ie & 0xFF);
        case 0x201: return static_cast<uint8_t>((m_ie >> 8) & 0xFF);
        case 0x202: return static_cast<uint8_t>(m_if & 0xFF);
        case 0x203: return static_cast<uint8_t>((m_if >> 8) & 0xFF);
        case 0x204: return static_cast<uint8_t>(m_waitcnt & 0xFF);
        case 0x205: return static_cast<uint8_t>((m_waitcnt >> 8) & 0xFF);
        case 0x208: return static_cast<uint8_t>(m_ime & 0xFF);
        case 0x209: return static_cast<uint8_t>((m_ime >> 8) & 0xFF);
        default:
            return m_io[offset];
    }
}

void MemoryBus::writeIo8(uint32_t offset, uint8_t val) {
    if (offset >= 0x400) return;

    if (offset < 0x60) {
        m_ppu.writeRegister(offset, val);
        return;
    }
    if (offset >= 0x60 && offset < 0xB0) {
        m_apu.writeRegister(offset, val);
        return;
    }

    const uint8_t prevVal = m_io[offset];
    m_io[offset] = val;

    auto io16 = [this](uint32_t base) -> uint16_t {
        return static_cast<uint16_t>(m_io[base] | (m_io[base + 1] << 8));
    };
    auto io32 = [this](uint32_t base) -> uint32_t {
        return static_cast<uint32_t>(m_io[base]) |
               (static_cast<uint32_t>(m_io[base + 1]) << 8) |
               (static_cast<uint32_t>(m_io[base + 2]) << 16) |
               (static_cast<uint32_t>(m_io[base + 3]) << 24);
    };

    // DMA 0..3 (0x0B0 .. 0x0DF)
    if (offset >= 0xB0 && offset < 0xE0) {
        const uint8_t  ch   = static_cast<uint8_t>((offset - 0xB0) / 12);
        const uint32_t base = 0xB0 + ch * 12u;
        const uint32_t reg  = offset - base;

        if (reg < 4) {
            m_dma[ch].sad = io32(base);
        } else if (reg < 8) {
            m_dma[ch].dad = io32(base + 4);
        } else if (reg < 10) {
            m_dma[ch].count = io16(base + 8);
        } else if (reg == 11) {
            const uint16_t oldCtrl = static_cast<uint16_t>(m_io[base + 10] | (prevVal << 8));
            const uint16_t newCtrl = io16(base + 10);
            m_dma[ch].control = newCtrl;
            onDmaControlWrite(ch, oldCtrl, newCtrl);
        }
        return;
    }

    // Timers 0..3 (0x100 .. 0x10F)
    if (offset >= 0x100 && offset < 0x110) {
        const uint8_t  t    = static_cast<uint8_t>((offset - 0x100) / 4);
        const uint32_t base = 0x100 + t * 4u;
        const uint32_t reg  = offset - base;
        if (reg < 2) {
            m_timers[t].reload = io16(base);
        } else if (reg == 2) {
            const bool wasEnabled = (m_timers[t].control & (1u << 7)) != 0;
            const bool nowEnabled = (val & (1u << 7)) != 0;
            m_timers[t].control = val;
            if (!wasEnabled && nowEnabled) {
                m_timers[t].counter     = m_timers[t].reload;
                m_timers[t].cyclesAccum = 0;
            }
        }
        return;
    }

    switch (offset) {
        case 0x128:
        case 0x129:
            // SIOCNT (Serial Control): se bit 7 de 0x128 (Start/Busy) estiver ativo, agenda conclusão da transferência
            if (m_io[0x128] & 0x80u) {
                m_sioCyclesRemaining = 256;
            }
            break;
        case 0x132:
        case 0x133:
            m_input.writeKeyCnt(io16(0x132));
            break;
        case 0x200:
        case 0x201:
            m_ie = io16(0x200);
            break;
        case 0x202:
            m_if &= ~static_cast<uint16_t>(val);
            break;
        case 0x203:
            m_if &= ~static_cast<uint16_t>(static_cast<uint16_t>(val) << 8);
            break;
        case 0x204:
        case 0x205:
            m_waitcnt = io16(0x204);
            break;
        case 0x208:
        case 0x209:
            m_ime = io16(0x208) & 1u;
            break;
        case 0x301:
            if ((val & 0x80) == 0 && m_cpu) {
                m_cpu->halt();
            }
            break;
        default:
            break;
    }
}

void MemoryBus::onDmaControlWrite(uint8_t ch, uint16_t oldCtrl, uint16_t newCtrl) {
    const bool wasEnabled = (oldCtrl & (1u << 15)) != 0;
    const bool nowEnabled = (newCtrl & (1u << 15)) != 0;

    if (!wasEnabled && nowEnabled) {
        m_dma[ch].internalSrc   = m_dma[ch].sad;
        m_dma[ch].internalDst   = m_dma[ch].dad;
        m_dma[ch].internalCount = m_dma[ch].count ? m_dma[ch].count : (ch == 3 ? 0x10000u : 0x4000u);

        const uint8_t startTiming = (newCtrl >> 12) & 0x03;
        if (startTiming == 0) {
            executeDma(ch); // Immediate DMA
        }
    }
}

void MemoryBus::executeDma(uint8_t ch) {
    auto& d = m_dma[ch];
    const uint16_t ctrl = d.control;
    const bool is32Bit  = (ctrl & (1u << 10)) != 0;
    const uint8_t dstAdj = (ctrl >> 5) & 0x03;
    const uint8_t srcAdj = (ctrl >> 7) & 0x03;
    const uint8_t timing = (ctrl >> 12) & 0x03;

    const int32_t stepSize = is32Bit ? 4 : 2;
    const int32_t srcStep  = (srcAdj == 0 || srcAdj == 3) ? stepSize : (srcAdj == 1 ? -stepSize : 0);
    const int32_t dstStep  = (dstAdj == 0 || dstAdj == 3) ? stepSize : (dstAdj == 1 ? -stepSize : 0);

    // DirectSound FIFO DMA (DMA1 ou DMA2 com timing Special=3) transfere 4 words de 32 bits fixos
    if (timing == 3 && (ch == 1 || ch == 2)) {
        for (int i = 0; i < 4; ++i) {
            const uint32_t word = read32(d.internalSrc);
            if (d.internalDst == 0x040000A0) m_apu.writeFifoA(word);
            else if (d.internalDst == 0x040000A4) m_apu.writeFifoB(word);
            d.internalSrc = static_cast<uint32_t>(static_cast<int32_t>(d.internalSrc) + srcStep);
        }
    } else {
        const uint32_t count = d.internalCount ? d.internalCount : (ch == 3 ? 0x10000u : 0x4000u);
        for (uint32_t i = 0; i < count; ++i) {
            if (is32Bit) {
                write32(d.internalDst, read32(d.internalSrc));
            } else {
                write16(d.internalDst, read16(d.internalSrc));
            }
            d.internalSrc = static_cast<uint32_t>(static_cast<int32_t>(d.internalSrc) + srcStep);
            d.internalDst = static_cast<uint32_t>(static_cast<int32_t>(d.internalDst) + dstStep);
        }
    }

    if (ctrl & (1u << 14)) {
        requestInterrupt(static_cast<uint16_t>(1u << (8 + ch))); // DMA0..DMA3 IRQ
    }

    const bool repeat = (ctrl & (1u << 9)) != 0 && timing != 0;
    if (repeat) {
        d.internalCount = d.count ? d.count : (ch == 3 ? 0x10000u : 0x4000u);
        if (dstAdj == 3) d.internalDst = d.dad;
    } else {
        d.control &= ~(1u << 15);
        m_io[0xB0 + ch * 12u + 11] &= ~0x80u;
    }
}

void MemoryBus::triggerDmaVBlank() {
    for (uint8_t ch = 0; ch < 4; ++ch) {
        if ((m_dma[ch].control & (1u << 15)) && (((m_dma[ch].control >> 12) & 3u) == 1u)) {
            executeDma(ch);
        }
    }
}

void MemoryBus::triggerDmaHBlank() {
    for (uint8_t ch = 0; ch < 4; ++ch) {
        if ((m_dma[ch].control & (1u << 15)) && (((m_dma[ch].control >> 12) & 3u) == 2u)) {
            executeDma(ch);
        }
    }
}

void MemoryBus::triggerDmaFifo(uint8_t fifoIndex) {
    const uint8_t ch = (fifoIndex == 0) ? 1 : 2;
    if ((m_dma[ch].control & (1u << 15)) && (((m_dma[ch].control >> 12) & 3u) == 3u)) {
        executeDma(ch);
    }
}

void MemoryBus::stepTimers(int32_t cycles) {
    if (m_sioCyclesRemaining > 0) {
        m_sioCyclesRemaining -= cycles;
        if (m_sioCyclesRemaining <= 0) {
            m_sioCyclesRemaining = 0;
            m_io[0x128] &= ~0x80u; // Limpa bit Start/Busy de SIOCNT
            m_io[0x120] = m_io[0x121] = m_io[0x122] = m_io[0x123] = 0xFF;
            m_io[0x12A] = m_io[0x12B] = 0xFF;
            if (m_io[0x129] & 0x40u) { // Bit 14 de SIOCNT: Serial IRQ Enable
                requestInterrupt(1u << 7); // InterruptFlag::Serial
            }
        }
    }

    static constexpr int32_t PRESCALERS[4] = {1, 64, 256, 1024};

    for (uint8_t i = 0; i < 4; ++i) {
        auto& t = m_timers[i];
        if ((t.control & (1u << 7)) == 0) continue; // Timer desligado
        if ((t.control & (1u << 2)) != 0 && i > 0) continue; // Count-up (Cascade) tratado no overflow do anterior

        const int32_t prescaler = PRESCALERS[t.control & 0x03];
        t.cyclesAccum += cycles;

        while (t.cyclesAccum >= prescaler) {
            t.cyclesAccum -= prescaler;
            if (t.counter == 0xFFFF) {
                t.counter = t.reload;
                onTimerOverflow(i);
            } else {
                ++t.counter;
            }
        }
    }
}

void MemoryBus::onTimerOverflow(uint8_t timerIdx) {
    if (timerIdx < 2) {
        m_apu.onTimerOverflow(timerIdx);
    }

    if (m_timers[timerIdx].control & (1u << 6)) {
        requestInterrupt(static_cast<uint16_t>(1u << (3 + timerIdx))); // Timer0..3 IRQ
    }

    // Cascade para o próximo Timer se Count-Up (bit 2) estiver ativo
    if (timerIdx < 3) {
        auto& next = m_timers[timerIdx + 1];
        if ((next.control & (1u << 7)) && (next.control & (1u << 2))) {
            if (next.counter == 0xFFFF) {
                next.counter = next.reload;
                onTimerOverflow(timerIdx + 1);
            } else {
                ++next.counter;
            }
        }
    }
}

void MemoryBus::serializeState(SaveStateWriter& out) const {
    out.writePod(m_ewram);
    out.writePod(m_iwram);
    out.writePod(m_io);
    out.writePod(m_ie);
    out.writePod(m_if);
    out.writePod(m_ime);
    out.writePod(m_waitcnt);
    out.writePod(m_sioCyclesRemaining);
    out.writePod(m_dma);
    out.writePod(m_timers);

    const uint8_t hasMedia = m_backupMedia ? 1u : 0u;
    out.writePod(hasMedia);
    if (m_backupMedia) {
        const auto mediaType = static_cast<uint8_t>(m_backupMedia->getType());
        out.writePod(mediaType);
        m_backupMedia->serializeState(out);
    }
}

bool MemoryBus::deserializeState(SaveStateReader& in) {
    if (!in.readPod(m_ewram) ||
        !in.readPod(m_iwram) ||
        !in.readPod(m_io) ||
        !in.readPod(m_ie) ||
        !in.readPod(m_if) ||
        !in.readPod(m_ime) ||
        !in.readPod(m_waitcnt) ||
        !in.readPod(m_sioCyclesRemaining) ||
        !in.readPod(m_dma) ||
        !in.readPod(m_timers)) {
        return false;
    }

    uint8_t hasMedia = 0;
    if (!in.readPod(hasMedia)) return false;
    if (hasMedia != 0) {
        uint8_t mediaType = 0;
        if (!in.readPod(mediaType)) return false;
        const auto expected = static_cast<SaveType>(mediaType);
        if (!m_backupMedia || m_backupMedia->getType() != expected) {
            m_backupMedia = SaveDetector::createMedia(expected);
            if (m_backupMedia) m_backupMedia->setTraceEnabled(m_traceEnabled);
        }
        if (m_backupMedia && !m_backupMedia->deserializeState(in)) {
            return false;
        }
    }
    return in.good();
}

} // namespace gba::core

