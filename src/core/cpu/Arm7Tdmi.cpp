#include "core/cpu/Arm7Tdmi.hpp"
#include "core/memory/MemoryBus.hpp"
#include "core/ppu/Ppu.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace gba::core {

namespace {

constexpr std::string_view swiName(uint8_t comment) noexcept {
    switch (comment) {
        case 0x00: return "SoftReset";
        case 0x01: return "RegisterRamReset";
        case 0x02: return "Halt";
        case 0x03: return "Stop";
        case 0x04: return "IntrWait";
        case 0x05: return "VBlankIntrWait";
        case 0x06: return "Div";
        case 0x07: return "DivArm";
        case 0x08: return "Sqrt";
        case 0x09: return "ArcTan";
        case 0x0A: return "ArcTan2";
        case 0x0B: return "CpuSet";
        case 0x0C: return "CpuFastSet";
        case 0x0E: return "BgAffineSet";
        case 0x0F: return "ObjAffineSet";
        case 0x10: return "BitUnPack";
        case 0x11: return "LZ77UnCompWram";
        case 0x12: return "LZ77UnCompVram";
        case 0x13: return "HuffUnComp";
        case 0x14: return "RLUnCompWram";
        case 0x15: return "RLUnCompVram";
        default:   return "BIOS_SWI";
    }
}

constexpr std::string_view condSuffix(uint8_t cond) noexcept {
    constexpr std::array<std::string_view, 16> CONDS = {
        "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
        "HI", "LS", "GE", "LT", "GT", "LE", "",   "NV"
    };
    return CONDS[cond & 0x0F];
}

std::string regName(uint32_t r) {
    switch (r & 0x0F) {
        case 13: return "SP";
        case 14: return "LR";
        case 15: return "PC";
        default: return "R" + std::to_string(r & 0x0F);
    }
}

} // namespace

Arm7Tdmi::Arm7Tdmi(MemoryBus& bus)
    : m_bus(bus) {
    m_bus.attachCpu(this);
    reset(true);
}

void Arm7Tdmi::reset(bool skipBios) {
    m_regs = CpuRegisters{};
    m_halted = false;
    m_pcModified = false;
    m_pcHistory.fill(0);
    m_pcHistoryHead            = 0;
    m_pcHistoryCount           = 0;
    m_stuckLoopPcs.fill(0);
    m_stuckLoopPcCount         = 0;
    m_stuckLoopCycles          = 0;
    m_stuckLoopReportedCurrent = false;
    m_stuckLoopTotalReports    = 0;
    m_totalCycles              = 0;
    m_reportedStuckRanges.clear();
    m_swiCounts.fill(0);

    // Ponteiros de pilha padrão configurados pela BIOS do GBA
    m_regs.r[13]          = 0x03007F00u; // USR/SYS SP
    m_regs.r8_14_usr[5]   = 0x03007F00u; // USR/SYS SP no banco
    m_regs.r13_14_irq[0]  = 0x03007FA0u; // IRQ SP
    m_regs.r13_14_svc[0]  = 0x03007FE0u; // SVC SP
    m_regs.cpsr           = static_cast<uint32_t>(CpuMode::System);
    m_regs.r[15]          = skipBios ? 0x08000000u : 0x00000000u;
}

void Arm7Tdmi::flushPipeline() {
    // Alinha PC conforme o estado atual (ARM = 4 bytes, THUMB = 2 bytes)
    if (m_regs.isThumb()) {
        m_regs.r[15] &= ~1u;
    } else {
        m_regs.r[15] &= ~3u;
    }
}

void Arm7Tdmi::requestInterrupt(InterruptFlag flag) {
    m_bus.requestInterrupt(static_cast<uint16_t>(flag));
}

void Arm7Tdmi::switchMode(CpuMode newMode) {
    const CpuMode oldMode = m_regs.getMode();
    if (oldMode == newMode) return;

    // 1. Salva registradores do modo atual nos bancos
    if (oldMode == CpuMode::Fiq) {
        for (int i = 0; i < 7; ++i) {
            m_regs.r8_14_fiq[i] = m_regs.r[8 + i];
        }
        m_regs.spsr_bank[0] = m_regs.spsr;
    } else if (oldMode == CpuMode::User || oldMode == CpuMode::System) {
        for (int i = 0; i < 7; ++i) {
            m_regs.r8_14_usr[i] = m_regs.r[8 + i];
        }
    } else {
        for (int i = 0; i < 5; ++i) m_regs.r8_14_usr[i] = m_regs.r[8 + i];
        switch (oldMode) {
            case CpuMode::Irq:        m_regs.r13_14_irq = {m_regs.r[13], m_regs.r[14]}; m_regs.spsr_bank[1] = m_regs.spsr; break;
            case CpuMode::Supervisor: m_regs.r13_14_svc = {m_regs.r[13], m_regs.r[14]}; m_regs.spsr_bank[2] = m_regs.spsr; break;
            case CpuMode::Abort:      m_regs.r13_14_abt = {m_regs.r[13], m_regs.r[14]}; m_regs.spsr_bank[3] = m_regs.spsr; break;
            case CpuMode::Undefined:  m_regs.r13_14_und = {m_regs.r[13], m_regs.r[14]}; m_regs.spsr_bank[4] = m_regs.spsr; break;
            default: break;
        }
    }

    // 2. Carrega registradores do novo modo
    if (newMode == CpuMode::Fiq) {
        for (int i = 0; i < 7; ++i) {
            m_regs.r[8 + i] = m_regs.r8_14_fiq[i];
        }
        m_regs.spsr = m_regs.spsr_bank[0];
    } else if (newMode == CpuMode::User || newMode == CpuMode::System) {
        for (int i = 0; i < 7; ++i) {
            m_regs.r[8 + i] = m_regs.r8_14_usr[i];
        }
    } else {
        for (int i = 0; i < 5; ++i) m_regs.r[8 + i] = m_regs.r8_14_usr[i];
        switch (newMode) {
            case CpuMode::Irq:        m_regs.r[13] = m_regs.r13_14_irq[0]; m_regs.r[14] = m_regs.r13_14_irq[1]; m_regs.spsr = m_regs.spsr_bank[1]; break;
            case CpuMode::Supervisor: m_regs.r[13] = m_regs.r13_14_svc[0]; m_regs.r[14] = m_regs.r13_14_svc[1]; m_regs.spsr = m_regs.spsr_bank[2]; break;
            case CpuMode::Abort:      m_regs.r[13] = m_regs.r13_14_abt[0]; m_regs.r[14] = m_regs.r13_14_abt[1]; m_regs.spsr = m_regs.spsr_bank[3]; break;
            case CpuMode::Undefined:  m_regs.r[13] = m_regs.r13_14_und[0]; m_regs.r[14] = m_regs.r13_14_und[1]; m_regs.spsr = m_regs.spsr_bank[4]; break;
            default: break;
        }
    }

    m_regs.cpsr = (m_regs.cpsr & ~0x1Fu) | static_cast<uint32_t>(newMode);
}

void Arm7Tdmi::checkInterrupts() {
    const uint16_t pending = m_bus.getIe() & m_bus.getIf();
    if (pending != 0) {
        m_halted = false;
    }

    if ((m_bus.getIme() & 1u) != 0 && pending != 0 && !m_regs.isIrqDisabled()) {
        const uint32_t savedCpsr = m_regs.cpsr;
        const uint32_t nextPc    = m_regs.r[15] + 4u; // LR_irq aponta para PC + 4
        switchMode(CpuMode::Irq);
        m_regs.spsr  = savedCpsr;
        m_regs.r[14] = nextPc;
        m_regs.cpsr &= ~CpuRegisters::FLAG_T; // Entra em modo ARM (32 bits)
        m_regs.cpsr |= CpuRegisters::FLAG_I;  // Desabilita novas IRQs
        m_regs.r[15] = 0x00000018u;           // Vetor de IRQ da BIOS
    }
}

int32_t Arm7Tdmi::step() {
    checkInterrupts();

    if (m_halted) {
        m_totalCycles += 4;
        m_bus.stepTimers(4);
        return 4;
    }

    int32_t cycles = 1;
    uint32_t executedAddr = 0;
    m_pcModified = false;
    if (m_regs.isThumb()) {
        const uint32_t instrAddr = m_regs.r[15] & ~1u;
        executedAddr             = instrAddr;
        const uint16_t opcode    = m_bus.read16(instrAddr);
        m_regs.r[15] = instrAddr + 4u; // Visibilidade do pipeline THUMB: PC = instrAddr + 4
        cycles = executeThumb(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 2u;
        }
    } else {
        const uint32_t instrAddr = m_regs.r[15] & ~3u;
        executedAddr             = instrAddr;
        const uint32_t opcode    = m_bus.read32(instrAddr);
        m_regs.r[15] = instrAddr + 8u; // Visibilidade do pipeline ARM: PC = instrAddr + 8
        cycles = executeArm(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 4u;
        }
    }

    m_totalCycles += static_cast<uint64_t>(cycles);
    recordPcAndDetectStuckLoop(executedAddr, cycles);

    m_bus.stepTimers(cycles);
    return cycles;
}

void Arm7Tdmi::recordPcAndDetectStuckLoop(uint32_t instrAddr, int32_t cycles) {
    m_pcHistory[m_pcHistoryHead] = instrAddr;
    m_pcHistoryHead = (m_pcHistoryHead + 1u) & 0x0Fu;
    if (m_pcHistoryCount < 16u) {
        ++m_pcHistoryCount;
        return;
    }

    std::array<uint32_t, 16> sorted = m_pcHistory;
    std::sort(sorted.begin(), sorted.end());
    std::size_t uniqueCount = 0;
    std::array<uint32_t, 16> uniquePcs{};
    for (std::size_t i = 0; i < 16u; ++i) {
        if (i == 0 || sorted[i] != sorted[i - 1]) {
            uniquePcs[uniqueCount++] = sorted[i];
        }
    }

    // Verifica se os mesmos 2 a 4 endereços de PC estão se repetindo continuamente
    if (uniqueCount >= 2u && uniqueCount <= 4u) {
        bool sameSet = (uniqueCount == m_stuckLoopPcCount);
        if (sameSet) {
            for (std::size_t i = 0; i < uniqueCount; ++i) {
                if (uniquePcs[i] != m_stuckLoopPcs[i]) {
                    sameSet = false;
                    break;
                }
            }
        }

        if (sameSet) {
            m_stuckLoopCycles += static_cast<uint64_t>(cycles);
        } else {
            m_stuckLoopPcCount = uniqueCount;
            for (std::size_t i = 0; i < uniqueCount; ++i) {
                m_stuckLoopPcs[i] = uniquePcs[i];
            }
            m_stuckLoopCycles          = static_cast<uint64_t>(cycles);
            m_stuckLoopReportedCurrent = false;
        }

        if (m_stuckLoopCycles > 50000u && !m_stuckLoopReportedCurrent) {
            m_stuckLoopReportedCurrent = true;
            ++m_stuckLoopTotalReports;
            const uint32_t minPc = uniquePcs[0];
            const uint32_t maxPc = uniquePcs[uniqueCount - 1];

            if (m_traceEnabled) {
                bool alreadyPrinted = false;
                for (const auto& [lo, hi] : m_reportedStuckRanges) {
                    if (lo == minPc && hi == maxPc) {
                        alreadyPrinted = true;
                        break;
                    }
                }
                if (!alreadyPrinted) {
                    m_reportedStuckRanges.emplace_back(minPc, maxPc);
                    printStuckLoopDiagnostic(minPc, maxPc);
                }
            }
        }
    } else {
        m_stuckLoopPcCount         = 0;
        m_stuckLoopCycles          = 0;
        m_stuckLoopReportedCurrent = false;
    }
}

void Arm7Tdmi::printStuckLoopDiagnostic(uint32_t minPc, uint32_t maxPc) {
    const bool thumb = m_regs.isThumb();
    std::cout << "\n==================== [DIAGNOSTICO: STUCK LOOP DETECTADO] ====================\n"
              << "  Ciclos continuos no loop : " << std::dec << m_stuckLoopCycles << " (> 50.000 ciclos)\n"
              << "  Ciclos totais de CPU     : " << m_totalCycles
              << " (~Quadro " << (m_totalCycles / 280896ull) << ", Scanline " << m_bus.ppu().getVCount() << ")\n"
              << "  Estado da CPU            : " << (thumb ? "THUMB (16-bit)" : "ARM (32-bit)")
              << " | HALT = " << (m_halted ? "SIM (true)" : "NAO (false)") << "\n"
              << "  Intervalo de PC travado  : 0x" << std::hex << std::setw(8) << std::setfill('0') << minPc
              << " .. 0x" << std::setw(8) << maxPc << " (" << std::dec << m_stuckLoopPcCount << " instrucoes)\n";

    std::cout << "\n  --- Desensamblado / Mnemonico do Intervalo Travado ---\n";
    const uint32_t stepSize = thumb ? 2u : 4u;
    for (uint32_t addr = minPc; addr <= maxPc; addr += stepSize) {
        if (thumb) {
            const uint16_t op = m_bus.read16(addr);
            std::cout << "    0x" << std::hex << std::setw(8) << std::setfill('0') << addr
                      << ": [0x" << std::setw(4) << op << "]  "
                      << disassembleThumb(addr, op) << "\n";
        } else {
            const uint32_t op = m_bus.read32(addr);
            std::cout << "    0x" << std::hex << std::setw(8) << std::setfill('0') << addr
                      << ": [0x" << std::setw(8) << op << "]  "
                      << disassembleArm(addr, op) << "\n";
        }
    }

    std::cout << "\n  --- Historico Circular dos Ultimos 16 PCs ---\n    ";
    for (std::size_t i = 0; i < 16u; ++i) {
        const std::size_t idx = (m_pcHistoryHead + i) & 0x0Fu;
        std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0') << m_pcHistory[idx]
                  << (i == 15u ? "" : " -> ");
        if ((i & 3u) == 3u && i != 15u) std::cout << "\n    ";
    }
    std::cout << "\n";

    std::cout << "\n  --- Estado dos Registradores (R0-R15 e CPSR) ---\n";
    for (int i = 0; i < 16; ++i) {
        std::cout << "    R" << std::dec << std::setw(2) << std::setfill(' ') << i << ": 0x"
                  << std::hex << std::setw(8) << std::setfill('0') << m_regs.r[i];
        if ((i & 3) == 3) std::cout << "\n";
    }
    std::cout << "    CPSR: 0x" << std::hex << std::setw(8) << std::setfill('0') << m_regs.cpsr
              << " [Flags: "
              << (m_regs.getFlag(CpuRegisters::FLAG_N) ? 'N' : '-')
              << (m_regs.getFlag(CpuRegisters::FLAG_Z) ? 'Z' : '-')
              << (m_regs.getFlag(CpuRegisters::FLAG_C) ? 'C' : '-')
              << (m_regs.getFlag(CpuRegisters::FLAG_V) ? 'V' : '-')
              << " | IRQ_Disable(I)=" << (m_regs.getFlag(CpuRegisters::FLAG_I) ? '1' : '0')
              << " | FIQ_Disable(F)=" << (m_regs.getFlag(CpuRegisters::FLAG_F) ? '1' : '0')
              << " | Thumb(T)=" << (m_regs.getFlag(CpuRegisters::FLAG_T) ? '1' : '0')
              << " | Mode=0x" << std::hex << (m_regs.cpsr & 0x1Fu) << std::dec << "]\n";

    const uint16_t ime      = m_bus.getIme();
    const uint16_t ie       = m_bus.getIe();
    const uint16_t iflag    = m_bus.getIf();
    const uint16_t dispstat = m_bus.ppu().getDispStat();
    std::cout << "\n  --- Registradores de Interrupcao e Video ---\n"
              << "    IME      (0x04000208) : 0x" << std::hex << std::setw(4) << std::setfill('0') << ime
              << " (" << (ime & 1u ? "HABILITADO" : "DESABILITADO") << ")\n"
              << "    IE       (0x04000200) : 0x" << std::setw(4) << ie
              << " [VBlank=" << ((ie & 1u) ? 1 : 0)
              << " HBlank=" << ((ie & 2u) ? 1 : 0)
              << " VCount=" << ((ie & 4u) ? 1 : 0)
              << " Timer=" << ((ie >> 3) & 0xFu)
              << " Serial=" << ((ie & 0x80u) ? 1 : 0) << "]\n"
              << "    IF       (0x04000202) : 0x" << std::setw(4) << iflag
              << " | INTR_CHECK (0x03007FF8) = 0x" << std::setw(4) << m_bus.read16(0x03007FF8u)
              << " | IRQ_HANDLER (0x03007FFC) = 0x" << std::setw(8) << m_bus.read32(0x03007FFCu) << "\n"
              << "    DISPSTAT (0x04000004) : 0x" << std::setw(4) << dispstat
              << " [VBlankFlag=" << (dispstat & 1u)
              << " HBlankFlag=" << ((dispstat >> 1) & 1u)
              << " VCountFlag=" << ((dispstat >> 2) & 1u)
              << " VBlankIRQ_En=" << ((dispstat >> 3) & 1u)
              << " HBlankIRQ_En=" << ((dispstat >> 4) & 1u)
              << " VCountIRQ_En=" << ((dispstat >> 5) & 1u)
              << " LYC=" << std::dec << ((dispstat >> 8) & 0xFFu) << "]\n"
              << "    CPU em estado HALT    : " << (m_halted ? "SIM" : "NAO") << "\n"
              << "=============================================================================\n\n";
}

bool Arm7Tdmi::checkCondition(uint8_t cond) const noexcept {
    const bool n = m_regs.getFlag(CpuRegisters::FLAG_N);
    const bool z = m_regs.getFlag(CpuRegisters::FLAG_Z);
    const bool c = m_regs.getFlag(CpuRegisters::FLAG_C);
    const bool v = m_regs.getFlag(CpuRegisters::FLAG_V);

    switch (cond & 0x0F) {
        case 0x0: return z;               // EQ
        case 0x1: return !z;              // NE
        case 0x2: return c;               // CS / HS
        case 0x3: return !c;              // CC / LO
        case 0x4: return n;               // MI
        case 0x5: return !n;              // PL
        case 0x6: return v;               // VS
        case 0x7: return !v;              // VC
        case 0x8: return c && !z;         // HI
        case 0x9: return !c || z;         // LS
        case 0xA: return n == v;          // GE
        case 0xB: return n != v;          // LT
        case 0xC: return !z && (n == v);  // GT
        case 0xD: return z || (n != v);   // LE
        case 0xE: return true;            // AL
        default:  return true;
    }
}

uint32_t Arm7Tdmi::barrelShift(uint32_t val, uint8_t shiftType, uint8_t amount, bool& carryOut, bool immediateShift) const noexcept {
    switch (shiftType & 3u) {
        case 0: // LSL
            if (amount == 0) return val;
            if (amount < 32) {
                carryOut = ((val >> (32 - amount)) & 1u) != 0;
                return val << amount;
            }
            carryOut = (amount == 32) ? ((val & 1u) != 0) : false;
            return 0;

        case 1: // LSR
            if (amount == 0) {
                if (!immediateShift) return val;
                amount = 32;
            }
            if (amount < 32) {
                carryOut = ((val >> (amount - 1)) & 1u) != 0;
                return val >> amount;
            }
            carryOut = (amount == 32) ? ((val >> 31) != 0) : false;
            return 0;

        case 2: // ASR
            if (amount == 0) {
                if (!immediateShift) return val;
                amount = 32;
            }
            if (amount < 32) {
                carryOut = ((val >> (amount - 1)) & 1u) != 0;
                return static_cast<uint32_t>(static_cast<int32_t>(val) >> amount);
            }
            carryOut = (val >> 31) != 0;
            return carryOut ? 0xFFFFFFFFu : 0u;

        case 3: // ROR / RRX
            if (amount == 0) {
                if (!immediateShift) return val;
                // RRX (Rotate Right Extended por 1 bit através do Carry)
                const uint32_t cIn = carryOut ? (1u << 31) : 0u;
                carryOut = (val & 1u) != 0;
                return (val >> 1) | cIn;
            }
            amount &= 31u;
            if (amount == 0) {
                carryOut = (val >> 31) != 0;
                return val;
            }
            val = std::rotr(val, amount);
            carryOut = (val >> 31) != 0;
            return val;
    }
    return val;
}

void Arm7Tdmi::setNzFlags(uint32_t result) noexcept {
    m_regs.setFlag(CpuRegisters::FLAG_N, (result & 0x80000000u) != 0);
    m_regs.setFlag(CpuRegisters::FLAG_Z, result == 0);
}

uint32_t Arm7Tdmi::addWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
    const uint32_t res = static_cast<uint32_t>(sum);
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, sum > 0xFFFFFFFFull);
        m_regs.setFlag(CpuRegisters::FLAG_V, ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t Arm7Tdmi::adcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
    const uint64_t c   = carryIn ? 1ull : 0ull;
    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + c;
    const uint32_t res = static_cast<uint32_t>(sum);
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, sum > 0xFFFFFFFFull);
        m_regs.setFlag(CpuRegisters::FLAG_V, ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t Arm7Tdmi::subWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint32_t res = a - b;
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, a >= b);
        m_regs.setFlag(CpuRegisters::FLAG_V, (((a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t Arm7Tdmi::sbcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
    const uint64_t borrow = carryIn ? 0ull : 1ull;
    const uint64_t rhs    = static_cast<uint64_t>(b) + borrow;
    const uint32_t res    = static_cast<uint32_t>(static_cast<uint64_t>(a) - rhs);
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, static_cast<uint64_t>(a) >= rhs);
        m_regs.setFlag(CpuRegisters::FLAG_V, (((a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

void Arm7Tdmi::handleSwi(uint8_t comment) {
    const uint64_t callNum = ++m_swiCounts[comment];
    if (m_traceEnabled) {
        const bool isIntrWait = (comment == 0x04 || comment == 0x05);
        const bool shouldLog  = isIntrWait
            ? (callNum <= 10 || callNum % 50 == 0)
            : (callNum <= 10);
        if (shouldLog) {
            const uint32_t callerPc = m_regs.isThumb() ? (m_regs.r[15] - 4u) : (m_regs.r[15] - 8u);
            const uint16_t dispstat = m_bus.ppu().getDispStat();
            const uint16_t ie       = m_bus.getIe();
            const uint16_t iflag    = m_bus.getIf();
            const uint16_t ime      = m_bus.getIme();
            std::cout << "[TRACE SWI] SWI 0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<uint32_t>(comment) << " (" << swiName(comment) << ") #"
                      << std::dec << callNum
                      << " em PC=0x" << std::hex << std::setw(8) << std::setfill('0') << callerPc
                      << " (LR=0x" << std::setw(8) << m_regs.r[14] << ")"
                      << " | R0=0x" << std::setw(8) << m_regs.r[0]
                      << " R1=0x" << std::setw(8) << m_regs.r[1]
                      << " | IME=" << std::dec << ime
                      << " IE=0x" << std::hex << std::setw(4) << ie
                      << " IF=0x" << std::setw(4) << iflag
                      << " DISPSTAT=0x" << std::setw(4) << dispstat
                      << " (PPU VBlankIRQ_En=" << ((dispstat & 0x08u) ? "SIM" : "NAO")
                      << ", VBlankIRQs_Disparadas=" << std::dec << m_bus.ppu().getVBlankIrqCount() << ")\n";
        }
    }

    // Emulação de Alto Nível (HLE BIOS) para rotinas vitais do GBA
    switch (comment) {
        case 0x00: // SoftReset
            reset(true);
            m_pcModified = true;
            break;
        case 0x01: { // RegisterRamReset
            const uint32_t flags = m_regs.r[0];
            if (flags & (1u << 0)) {
                for (uint32_t a = 0; a < 0x40000; a += 4) m_bus.write32(0x02000000u + a, 0);
            }
            if (flags & (1u << 1)) {
                // Preserva 0x03007E00..0x03007FFF (pilha e ponteiro IRQ 0x03007FFC)
                for (uint32_t a = 0; a < 0x7E00; a += 4) m_bus.write32(0x03000000u + a, 0);
            }
            if (flags & (1u << 2)) {
                for (uint32_t a = 0; a < 0x400; a += 4) m_bus.write32(0x05000000u + a, 0);
            }
            if (flags & (1u << 3)) {
                for (uint32_t a = 0; a < 0x18000; a += 4) m_bus.write32(0x06000000u + a, 0);
            }
            if (flags & (1u << 4)) {
                for (uint32_t a = 0; a < 0x400; a += 4) m_bus.write32(0x07000000u + a, 0);
            }
            break;
        }
        case 0x02: // Halt
            m_halted = true;
            break;
        case 0x04: // IntrWait
        case 0x05: // VBlankIntrWait
            m_bus.write16(0x04000208u, 1u); // Garante IME = 1 durante VBlankIntrWait
            m_halted = true;
            break;
        case 0x06: { // Div (R0 / R1 -> R0=Quociente, R1=Resto, R3=|Quociente|)
            const int32_t num = static_cast<int32_t>(m_regs.r[0]);
            const int32_t den = static_cast<int32_t>(m_regs.r[1]);
            if (den != 0) {
                const int32_t q = (num == INT32_MIN && den == -1) ? INT32_MIN : (num / den);
                const int32_t r = (num == INT32_MIN && den == -1) ? 0 : (num % den);
                m_regs.r[0] = static_cast<uint32_t>(q);
                m_regs.r[1] = static_cast<uint32_t>(r);
                m_regs.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            break;
        }
        case 0x07: { // DivArm (R1 / R0 -> R0=Quociente, R1=Resto, R3=|Quociente|)
            const int32_t den = static_cast<int32_t>(m_regs.r[0]);
            const int32_t num = static_cast<int32_t>(m_regs.r[1]);
            if (den != 0) {
                const int32_t q = (num == INT32_MIN && den == -1) ? INT32_MIN : (num / den);
                const int32_t r = (num == INT32_MIN && den == -1) ? 0 : (num % den);
                m_regs.r[0] = static_cast<uint32_t>(q);
                m_regs.r[1] = static_cast<uint32_t>(r);
                m_regs.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            break;
        }
        case 0x08: { // Sqrt (sqrt(R0) -> R0)
            m_regs.r[0] = static_cast<uint32_t>(std::sqrt(static_cast<double>(m_regs.r[0])));
            break;
        }
        case 0x09: { // ArcTan
            const double t = static_cast<double>(static_cast<int16_t>(m_regs.r[0] & 0xFFFFu)) / 16384.0;
            const double rad = std::atan(t);
            m_regs.r[0] = static_cast<uint32_t>(static_cast<int32_t>(rad * 32768.0 / 3.14159265358979323846));
            break;
        }
        case 0x0A: { // ArcTan2
            const double x = static_cast<double>(static_cast<int16_t>(m_regs.r[0] & 0xFFFFu));
            const double y = static_cast<double>(static_cast<int16_t>(m_regs.r[1] & 0xFFFFu));
            double rad = std::atan2(y, x);
            if (rad < 0) rad += 2.0 * 3.14159265358979323846;
            m_regs.r[0] = static_cast<uint32_t>(rad * 32768.0 / 3.14159265358979323846) & 0xFFFFu;
            break;
        }
        case 0x0B: { // CpuSet (R0=src, R1=dst, R2=len/mode)
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t ctrl = m_regs.r[2];
            const uint32_t count = ctrl & 0x1FFFFFu;
            const bool fixedSrc = (ctrl & (1u << 24)) != 0;
            const bool is32Bit  = (ctrl & (1u << 26)) != 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (is32Bit) {
                    m_bus.write32(dst, m_bus.read32(src));
                    dst += 4;
                    if (!fixedSrc) src += 4;
                } else {
                    m_bus.write16(dst, m_bus.read16(src));
                    dst += 2;
                    if (!fixedSrc) src += 2;
                }
            }
            break;
        }
        case 0x0C: { // CpuFastSet
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t ctrl  = m_regs.r[2];
            const uint32_t count = (ctrl & 0x1FFFFFu);
            const bool fixedSrc  = (ctrl & (1u << 24)) != 0;
            for (uint32_t i = 0; i < count; ++i) {
                m_bus.write32(dst, m_bus.read32(src));
                dst += 4;
                if (!fixedSrc) src += 4;
            }
            break;
        }
        case 0x0E: { // BgAffineSet
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t count = m_regs.r[2];
            for (uint32_t i = 0; i < count; ++i) {
                const int32_t origX = static_cast<int32_t>(m_bus.read32(src + 0));
                const int32_t origY = static_cast<int32_t>(m_bus.read32(src + 4));
                const int16_t dispX = static_cast<int16_t>(m_bus.read16(src + 8));
                const int16_t dispY = static_cast<int16_t>(m_bus.read16(src + 10));
                const int16_t scaleX = static_cast<int16_t>(m_bus.read16(src + 12));
                const int16_t scaleY = static_cast<int16_t>(m_bus.read16(src + 14));
                const uint16_t angle = m_bus.read16(src + 16);
                const double rad = static_cast<double>(angle >> 8) * (2.0 * 3.14159265358979323846 / 256.0);
                const int16_t pa = static_cast<int16_t>(std::cos(rad) * scaleX);
                const int16_t pb = static_cast<int16_t>(-std::sin(rad) * scaleX);
                const int16_t pc = static_cast<int16_t>(std::sin(rad) * scaleY);
                const int16_t pd = static_cast<int16_t>(std::cos(rad) * scaleY);
                const int32_t startX = origX - (pa * dispX + pb * dispY);
                const int32_t startY = origY - (pc * dispX + pd * dispY);
                m_bus.write16(dst + 0, static_cast<uint16_t>(pa));
                m_bus.write16(dst + 2, static_cast<uint16_t>(pb));
                m_bus.write16(dst + 4, static_cast<uint16_t>(pc));
                m_bus.write16(dst + 6, static_cast<uint16_t>(pd));
                m_bus.write32(dst + 8, static_cast<uint32_t>(startX));
                m_bus.write32(dst + 12, static_cast<uint32_t>(startY));
                src += 20;
                dst += 16;
            }
            break;
        }
        case 0x0F: { // ObjAffineSet
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t count  = m_regs.r[2];
            const uint32_t stride = m_regs.r[3];
            for (uint32_t i = 0; i < count; ++i) {
                const int16_t scaleX = static_cast<int16_t>(m_bus.read16(src + 0));
                const int16_t scaleY = static_cast<int16_t>(m_bus.read16(src + 2));
                const uint16_t angle = m_bus.read16(src + 4);
                const double rad = static_cast<double>(angle >> 8) * (2.0 * 3.14159265358979323846 / 256.0);
                const int16_t pa = static_cast<int16_t>(std::cos(rad) * scaleX);
                const int16_t pb = static_cast<int16_t>(-std::sin(rad) * scaleX);
                const int16_t pc = static_cast<int16_t>(std::sin(rad) * scaleY);
                const int16_t pd = static_cast<int16_t>(std::cos(rad) * scaleY);
                m_bus.write16(dst + stride * 0, static_cast<uint16_t>(pa));
                m_bus.write16(dst + stride * 1, static_cast<uint16_t>(pb));
                m_bus.write16(dst + stride * 2, static_cast<uint16_t>(pc));
                m_bus.write16(dst + stride * 3, static_cast<uint16_t>(pd));
                src += 8;
                dst += stride * 4;
            }
            break;
        }
        case 0x10: { // BitUnPack (Essencial para fontes de texto de Pokémon FireRed!)
            uint32_t src  = m_regs.r[0];
            uint32_t dst  = m_regs.r[1];
            uint32_t info = m_regs.r[2];
            const uint16_t srcLen  = m_bus.read16(info + 0);
            const uint8_t  srcBits = m_bus.read8(info + 2);
            const uint8_t  dstBits = m_bus.read8(info + 3);
            const uint32_t dataOffset = m_bus.read32(info + 4);
            const bool zeroDataOffset = (dataOffset & (1u << 31)) != 0;
            const uint32_t addOffset  = dataOffset & 0x7FFFFFFFu;

            if (srcBits > 0 && dstBits > 0) {
                const uint32_t mask = (1u << srcBits) - 1u;
                uint32_t outWord = 0;
                uint32_t outBitPos = 0;
                for (uint16_t i = 0; i < srcLen; ++i) {
                    const uint8_t byte = m_bus.read8(src++);
                    for (uint8_t b = 0; b < 8; b += srcBits) {
                        uint32_t unit = (byte >> b) & mask;
                        if (unit != 0 || zeroDataOffset) {
                            unit += addOffset;
                        }
                        outWord |= (unit << outBitPos);
                        outBitPos += dstBits;
                        if (outBitPos >= 32) {
                            m_bus.write32(dst, outWord);
                            dst += 4;
                            outWord = 0;
                            outBitPos = 0;
                        }
                    }
                }
            }
            break;
        }
        case 0x11:   // LZ77UnCompWram (8-bit writes)
        case 0x12: { // LZ77UnCompVram (16-bit writes - gráficos de Pokémon FireRed!)
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t header = m_bus.read32(src);
            src += 4;
            uint32_t remaining = header >> 8;
            if (remaining == 0 || remaining > 0x40000u) break;

            std::vector<uint8_t> decompressed;
            decompressed.reserve(remaining);

            while (decompressed.size() < remaining) {
                const uint8_t flags = m_bus.read8(src++);
                for (int bit = 7; bit >= 0 && decompressed.size() < remaining; --bit) {
                    if ((flags >> bit) & 1u) {
                        const uint8_t b1 = m_bus.read8(src++);
                        const uint8_t b2 = m_bus.read8(src++);
                        const uint32_t count = (b1 >> 4) + 3u;
                        const uint32_t disp  = (static_cast<uint32_t>(b1 & 0x0Fu) << 8) | b2;
                        for (uint32_t k = 0; k < count && decompressed.size() < remaining; ++k) {
                            const std::size_t backIdx = decompressed.size() - (disp + 1u);
                            const uint8_t val = (backIdx < decompressed.size()) ? decompressed[backIdx] : 0;
                            decompressed.push_back(val);
                        }
                    } else {
                        decompressed.push_back(m_bus.read8(src++));
                    }
                }
            }

            if (comment == 0x12) {
                for (std::size_t i = 0; i + 1 < decompressed.size(); i += 2) {
                    const uint16_t hw = static_cast<uint16_t>(decompressed[i] | (decompressed[i + 1] << 8));
                    m_bus.write16(dst + static_cast<uint32_t>(i), hw);
                }
            } else {
                for (std::size_t i = 0; i < decompressed.size(); ++i) {
                    m_bus.write8(dst + static_cast<uint32_t>(i), decompressed[i]);
                }
            }
            break;
        }
        case 0x14:   // RLUnCompWram
        case 0x15: { // RLUnCompVram
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t header = m_bus.read32(src);
            src += 4;
            const uint32_t remaining = header >> 8;
            if (remaining == 0 || remaining > 0x40000u) break;

            std::vector<uint8_t> decompressed;
            decompressed.reserve(remaining);

            while (decompressed.size() < remaining) {
                const uint8_t flag = m_bus.read8(src++);
                if (flag & 0x80u) {
                    const uint32_t len = (flag & 0x7Fu) + 3u;
                    const uint8_t  val = m_bus.read8(src++);
                    for (uint32_t k = 0; k < len && decompressed.size() < remaining; ++k) {
                        decompressed.push_back(val);
                    }
                } else {
                    const uint32_t len = (flag & 0x7Fu) + 1u;
                    for (uint32_t k = 0; k < len && decompressed.size() < remaining; ++k) {
                        decompressed.push_back(m_bus.read8(src++));
                    }
                }
            }

            if (comment == 0x15) {
                for (std::size_t i = 0; i + 1 < decompressed.size(); i += 2) {
                    const uint16_t hw = static_cast<uint16_t>(decompressed[i] | (decompressed[i + 1] << 8));
                    m_bus.write16(dst + static_cast<uint32_t>(i), hw);
                }
            } else {
                for (std::size_t i = 0; i < decompressed.size(); ++i) {
                    m_bus.write8(dst + static_cast<uint32_t>(i), decompressed[i]);
                }
            }
            break;
        }
        default:
            break;
    }
}

int32_t Arm7Tdmi::executeArm(uint32_t opcode) {
    const uint8_t cond = static_cast<uint8_t>((opcode >> 28) & 0x0F);
    if (!checkCondition(cond)) {
        return 1;
    }

    // 1. Branch and Exchange (BX Rn): 0001 0010 1111 1111 1111 0001 Rn
    if ((opcode & 0x0FFFFFF0u) == 0x012FFF10u) {
        const uint32_t rn  = opcode & 0x0Fu;
        const uint32_t dst = m_regs.r[rn];
        m_regs.setFlag(CpuRegisters::FLAG_T, (dst & 1u) != 0);
        m_regs.r[15] = dst & (m_regs.isThumb() ? ~1u : ~3u);
        m_pcModified = true;
        return 3;
    }

    // 2. Software Interrupt (SWI): 1111 xxxx ...
    if ((opcode & 0x0F000000u) == 0x0F000000u) {
        handleSwi(static_cast<uint8_t>((opcode >> 16) & 0xFF));
        return 3;
    }

    // 3. Branch / Branch with Link (B / BL): 101L offset24
    if ((opcode & 0x0E000000u) == 0x0A000000u) {
        const bool link = (opcode & (1u << 24)) != 0;
        int32_t offset = static_cast<int32_t>(opcode & 0x00FFFFFFu);
        if (offset & 0x00800000) offset |= ~0x00FFFFFF; // Sign-extend 24-bit
        offset <<= 2;

        if (link) {
            m_regs.r[14] = m_regs.r[15] - 4u;
        }
        m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + offset) & ~3u;
        m_pcModified = true;
        return 3;
    }

    // 4. Block Data Transfer (LDM / STM): 100P USWL Rn reglist
    if ((opcode & 0x0E000000u) == 0x08000000u) {
        const bool preIndex  = (opcode & (1u << 24)) != 0;
        const bool up        = (opcode & (1u << 23)) != 0;
        const bool sBit      = (opcode & (1u << 22)) != 0;
        const bool writeBack = (opcode & (1u << 21)) != 0;
        const bool isLoad    = (opcode & (1u << 20)) != 0;
        const uint32_t rn    = (opcode >> 16) & 0x0Fu;
        const uint16_t list  = static_cast<uint16_t>(opcode & 0xFFFFu);

        const uint32_t count = static_cast<uint32_t>(std::popcount(list));
        uint32_t base = m_regs.r[rn];
        uint32_t addr = base;

        if (!up) {
            addr -= count * 4u;
            if (!preIndex) addr += 4u;
        } else if (preIndex) {
            addr += 4u;
        }

        const uint32_t finalBase = up ? (base + count * 4u) : (base - count * 4u);

        for (uint32_t reg = 0; reg < 16; ++reg) {
            if ((list & (1u << reg)) == 0) continue;
            if (isLoad) {
                uint32_t val = m_bus.read32(addr);
                if (reg == 15) {
                    if (sBit) {
                        const uint32_t savedSpsr = m_regs.spsr;
                        switchMode(static_cast<CpuMode>(savedSpsr & 0x1Fu));
                        m_regs.cpsr = savedSpsr;
                    }
                    m_regs.r[15] = val & (m_regs.isThumb() ? ~1u : ~3u);
                    m_pcModified = true;
                } else {
                    m_regs.r[reg] = val;
                }
            } else {
                uint32_t val = m_regs.r[reg];
                if (reg == 15) val += 4u;
                m_bus.write32(addr, val);
            }
            addr += 4u;
        }

        if (writeBack && !(isLoad && (list & (1u << rn)))) {
            m_regs.r[rn] = finalBase;
        }
        return static_cast<int32_t>(count + 2u);
    }

    // 5. Single Data Transfer (LDR / STR / LDRB / STRB): 01IP UBWL Rn Rd offset12
    if ((opcode & 0x0C000000u) == 0x04000000u) {
        const bool regOffset = (opcode & (1u << 25)) != 0;
        const bool preIndex  = (opcode & (1u << 24)) != 0;
        const bool up        = (opcode & (1u << 23)) != 0;
        const bool isByte    = (opcode & (1u << 22)) != 0;
        const bool writeBack = (opcode & (1u << 21)) != 0;
        const bool isLoad    = (opcode & (1u << 20)) != 0;
        const uint32_t rn    = (opcode >> 16) & 0x0Fu;
        const uint32_t rd    = (opcode >> 12) & 0x0Fu;

        uint32_t offset = 0;
        if (!regOffset) {
            offset = opcode & 0xFFFu;
        } else {
            const uint32_t rm = opcode & 0x0Fu;
            const uint8_t shiftType = static_cast<uint8_t>((opcode >> 5) & 3u);
            const uint8_t shiftAmt  = static_cast<uint8_t>((opcode >> 7) & 0x1Fu);
            bool carry = m_regs.getFlag(CpuRegisters::FLAG_C);
            offset = barrelShift(m_regs.r[rm], shiftType, shiftAmt, carry, true);
        }

        const uint32_t base = m_regs.r[rn];
        const uint32_t offsetAddr = up ? (base + offset) : (base - offset);
        const uint32_t addr = preIndex ? offsetAddr : base;

        if (isLoad) {
            if (isByte) {
                m_regs.r[rd] = m_bus.read8(addr);
            } else {
                uint32_t val = m_bus.read32(addr & ~3u);
                const uint32_t rot = (addr & 3u) * 8u;
                if (rot != 0) val = std::rotr(val, rot);
                if (rd == 15) {
                    m_regs.r[15] = val & ~3u;
                    m_pcModified = true;
                } else {
                    m_regs.r[rd] = val;
                }
            }
        } else {
            uint32_t val = m_regs.r[rd];
            if (rd == 15) val += 4u;
            if (isByte) {
                m_bus.write8(addr, static_cast<uint8_t>(val & 0xFF));
            } else {
                m_bus.write32(addr & ~3u, val);
            }
        }

        if ((!preIndex || writeBack) && !(isLoad && rd == rn)) {
            m_regs.r[rn] = offsetAddr;
        }
        return isLoad ? 3 : 2;
    }

    // 6. Multiply & Multiply Long
    if ((opcode & 0x0FC000F0u) == 0x00000090u) {
        const bool accumulate = (opcode & (1u << 21)) != 0;
        const bool setFlags   = (opcode & (1u << 20)) != 0;
        const uint32_t rd = (opcode >> 16) & 0x0Fu;
        const uint32_t rn = (opcode >> 12) & 0x0Fu;
        const uint32_t rs = (opcode >> 8) & 0x0Fu;
        const uint32_t rm = opcode & 0x0Fu;

        uint32_t res = m_regs.r[rm] * m_regs.r[rs];
        if (accumulate) res += m_regs.r[rn];
        m_regs.r[rd] = res;
        if (setFlags) setNzFlags(res);
        return 3;
    }
    if ((opcode & 0x0F8000F0u) == 0x00800090u) {
        const bool isSigned   = (opcode & (1u << 22)) != 0;
        const bool accumulate = (opcode & (1u << 21)) != 0;
        const bool setFlags   = (opcode & (1u << 20)) != 0;
        const uint32_t rdHi = (opcode >> 16) & 0x0Fu;
        const uint32_t rdLo = (opcode >> 12) & 0x0Fu;
        const uint32_t rs   = (opcode >> 8) & 0x0Fu;
        const uint32_t rm   = opcode & 0x0Fu;

        uint64_t prod = isSigned
            ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(m_regs.r[rm])) *
                                    static_cast<int64_t>(static_cast<int32_t>(m_regs.r[rs])))
            : (static_cast<uint64_t>(m_regs.r[rm]) * static_cast<uint64_t>(m_regs.r[rs]));
        if (accumulate) {
            const uint64_t acc = (static_cast<uint64_t>(m_regs.r[rdHi]) << 32) | m_regs.r[rdLo];
            prod += acc;
        }
        m_regs.r[rdLo] = static_cast<uint32_t>(prod & 0xFFFFFFFFull);
        m_regs.r[rdHi] = static_cast<uint32_t>(prod >> 32);
        if (setFlags) {
            m_regs.setFlag(CpuRegisters::FLAG_N, (m_regs.r[rdHi] & 0x80000000u) != 0);
            m_regs.setFlag(CpuRegisters::FLAG_Z, prod == 0);
        }
        return 4;
    }

    // 7. Single Data Swap (SWP / SWPB)
    if ((opcode & 0x0FB00FF0u) == 0x01000090u) {
        const bool isByte = (opcode & (1u << 22)) != 0;
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        const uint32_t rm = opcode & 0x0Fu;
        const uint32_t addr = m_regs.r[rn];
        if (isByte) {
            const uint8_t oldVal = m_bus.read8(addr);
            m_bus.write8(addr, static_cast<uint8_t>(m_regs.r[rm] & 0xFF));
            m_regs.r[rd] = oldVal;
        } else {
            const uint32_t oldVal = m_bus.read32(addr & ~3u);
            m_bus.write32(addr & ~3u, m_regs.r[rm]);
            m_regs.r[rd] = oldVal;
        }
        return 4;
    }

    // 8. Halfword / Signed Data Transfer (LDRH / STRH / LDRSB / LDRSH)
    if ((opcode & 0x0E000090u) == 0x00000090u && ((opcode >> 5) & 3u) != 0) {
        const bool preIndex  = (opcode & (1u << 24)) != 0;
        const bool up        = (opcode & (1u << 23)) != 0;
        const bool immOffset = (opcode & (1u << 22)) != 0;
        const bool writeBack = (opcode & (1u << 21)) != 0;
        const bool isLoad    = (opcode & (1u << 20)) != 0;
        const uint32_t rn    = (opcode >> 16) & 0x0Fu;
        const uint32_t rd    = (opcode >> 12) & 0x0Fu;
        const uint8_t  sh    = static_cast<uint8_t>((opcode >> 5) & 3u);

        const uint32_t offset = immOffset
            ? (((opcode >> 4) & 0xF0u) | (opcode & 0x0Fu))
            : m_regs.r[opcode & 0x0Fu];

        const uint32_t base = m_regs.r[rn];
        const uint32_t offsetAddr = up ? (base + offset) : (base - offset);
        const uint32_t addr = preIndex ? offsetAddr : base;

        if (isLoad) {
            switch (sh) {
                case 1: // LDRH
                    m_regs.r[rd] = m_bus.read16(addr & ~1u);
                    break;
                case 2: // LDRSB
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.read8(addr))));
                    break;
                case 3: // LDRSH
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(m_bus.read16(addr & ~1u))));
                    break;
            }
        } else if (sh == 1) { // STRH
            m_bus.write16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu));
        }

        if ((!preIndex || writeBack) && !(isLoad && rd == rn)) {
            m_regs.r[rn] = offsetAddr;
        }
        return isLoad ? 3 : 2;
    }

    // 9. MRS / MSR (Transferência CPSR / SPSR)
    if ((opcode & 0x0FBF0FFFu) == 0x010F0000u) { // MRS Rd, Psr
        const bool useSpsr = (opcode & (1u << 22)) != 0;
        const uint32_t rd  = (opcode >> 12) & 0x0Fu;
        m_regs.r[rd] = useSpsr ? m_regs.spsr : m_regs.cpsr;
        return 1;
    }
    if ((opcode & 0x0DB0F000u) == 0x0120F000u) { // MSR Psr, Op
        const bool isImm   = (opcode & (1u << 25)) != 0;
        const bool useSpsr = (opcode & (1u << 22)) != 0;
        uint32_t operand = 0;
        if (isImm) {
            const uint32_t imm = opcode & 0xFFu;
            const uint32_t rot = ((opcode >> 8) & 0x0Fu) * 2u;
            operand = std::rotr(imm, rot);
        } else {
            operand = m_regs.r[opcode & 0x0Fu];
        }

        uint32_t mask = 0;
        if (opcode & (1u << 19)) mask |= 0xFF000000u; // Flags
        if (opcode & (1u << 16)) mask |= 0x000000FFu; // Control

        if (useSpsr) {
            m_regs.spsr = (m_regs.spsr & ~mask) | (operand & mask);
        } else {
            if (mask & 0xFFu) {
                switchMode(static_cast<CpuMode>(operand & 0x1Fu));
            }
            m_regs.cpsr = (m_regs.cpsr & ~mask) | (operand & mask);
        }
        return 1;
    }

    // 10. Data Processing (ALU)
    if ((opcode & 0x0C000000u) == 0x00000000u) {
        const bool isImm    = (opcode & (1u << 25)) != 0;
        const uint8_t aluOp = static_cast<uint8_t>((opcode >> 21) & 0x0Fu);
        const bool setFlags = (opcode & (1u << 20)) != 0;
        const uint32_t rn   = (opcode >> 16) & 0x0Fu;
        const uint32_t rd   = (opcode >> 12) & 0x0Fu;

        bool shifterCarry = m_regs.getFlag(CpuRegisters::FLAG_C);
        uint32_t op1 = m_regs.r[rn];
        uint32_t op2 = 0;

        if (isImm) {
            const uint32_t imm = opcode & 0xFFu;
            const uint32_t rot = ((opcode >> 8) & 0x0Fu) * 2u;
            op2 = std::rotr(imm, rot);
            if (rot != 0) shifterCarry = (op2 >> 31) != 0;
        } else {
            const uint32_t rm = opcode & 0x0Fu;
            const uint8_t shiftType = static_cast<uint8_t>((opcode >> 5) & 3u);
            const bool shiftByReg = (opcode & (1u << 4)) != 0;
            uint32_t rmVal = m_regs.r[rm];
            if ( shiftByReg && rm == 15) rmVal += 4u;
            if ( shiftByReg && rn == 15) op1   += 4u;
            const uint8_t shiftAmt = shiftByReg
                ? static_cast<uint8_t>(m_regs.r[(opcode >> 8) & 0x0Fu] & 0xFFu)
                : static_cast<uint8_t>((opcode >> 7) & 0x1Fu);
            op2 = barrelShift(rmVal, shiftType, shiftAmt, shifterCarry, !shiftByReg);
        }

        const bool updateAluFlags = setFlags && (rd != 15);
        uint32_t res = 0;
        bool writeResult = true;

        switch (aluOp) {
            case 0x0: res = op1 & op2; if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // AND
            case 0x1: res = op1 ^ op2; if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // EOR
            case 0x2: res = subWithFlags(op1, op2, updateAluFlags); break; // SUB
            case 0x3: res = subWithFlags(op2, op1, updateAluFlags); break; // RSB
            case 0x4: res = addWithFlags(op1, op2, updateAluFlags); break; // ADD
            case 0x5: res = adcWithFlags(op1, op2, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break; // ADC
            case 0x6: res = sbcWithFlags(op1, op2, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break; // SBC
            case 0x7: res = sbcWithFlags(op2, op1, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break; // RSC
            case 0x8: res = op1 & op2; setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); writeResult = false; break; // TST
            case 0x9: res = op1 ^ op2; setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); writeResult = false; break; // TEQ
            case 0xA: subWithFlags(op1, op2, true); writeResult = false; break; // CMP
            case 0xB: addWithFlags(op1, op2, true); writeResult = false; break; // CMN
            case 0xC: res = op1 | op2; if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // ORR
            case 0xD: res = op2;       if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // MOV
            case 0xE: res = op1 & ~op2; if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // BIC
            case 0xF: res = ~op2;      if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break; // MVN
        }

        if (writeResult) {
            if (rd == 15) {
                if (setFlags) {
                    const uint32_t savedSpsr = m_regs.spsr;
                    switchMode(static_cast<CpuMode>(savedSpsr & 0x1Fu));
                    m_regs.cpsr = savedSpsr;
                }
                m_regs.r[15] = res & (m_regs.isThumb() ? ~1u : ~3u);
                m_pcModified = true;
            } else {
                m_regs.r[rd] = res;
            }
        }
        return 1;
    }

    return 1;
}

int32_t Arm7Tdmi::executeThumb(uint16_t opcode) {
    // Formato 2: Add / Subtract (00011 I Op Rn Rs Rd)
    if ((opcode >> 11) == 0b00011) {
        const bool isImm = (opcode & (1u << 10)) != 0;
        const bool isSub = (opcode & (1u << 9)) != 0;
        const uint32_t rnImm = (opcode >> 6) & 7u;
        const uint32_t rs    = (opcode >> 3) & 7u;
        const uint32_t rd    = opcode & 7u;
        const uint32_t op2   = isImm ? rnImm : m_regs.r[rnImm];
        m_regs.r[rd] = isSub
            ? subWithFlags(m_regs.r[rs], op2, true)
            : addWithFlags(m_regs.r[rs], op2, true);
        return 1;
    }

    // Formato 1: Move shifted register (000 Op Offset5 Rs Rd)
    if ((opcode >> 13) == 0b000) {
        const uint8_t  op     = (opcode >> 11) & 3u;
        const uint8_t  offset = (opcode >> 6) & 0x1Fu;
        const uint32_t rs     = (opcode >> 3) & 7u;
        const uint32_t rd     = opcode & 7u;
        bool carry = m_regs.getFlag(CpuRegisters::FLAG_C);
        const uint32_t res = barrelShift(m_regs.r[rs], op, offset, carry, true);
        m_regs.r[rd] = res;
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, carry);
        return 1;
    }

    // Formato 3: Move / Compare / Add / Subtract Immediate (001 Op Rd Offset8)
    if ((opcode >> 13) == 0b001) {
        const uint8_t  op  = (opcode >> 11) & 3u;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t imm = opcode & 0xFFu;
        switch (op) {
            case 0: m_regs.r[rd] = imm; setNzFlags(imm); break;                     // MOV
            case 1: subWithFlags(m_regs.r[rd], imm, true); break;                   // CMP
            case 2: m_regs.r[rd] = addWithFlags(m_regs.r[rd], imm, true); break;    // ADD
            case 3: m_regs.r[rd] = subWithFlags(m_regs.r[rd], imm, true); break;    // SUB
        }
        return 1;
    }

    // Formato 4: ALU Operations (010000 Op Rs Rd)
    if ((opcode >> 10) == 0b010000) {
        const uint8_t  op = (opcode >> 6) & 0x0Fu;
        const uint32_t rs = (opcode >> 3) & 7u;
        const uint32_t rd = opcode & 7u;
        bool carry = m_regs.getFlag(CpuRegisters::FLAG_C);
        switch (op) {
            case 0x0: m_regs.r[rd] &= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // AND
            case 0x1: m_regs.r[rd] ^= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // EOR
            case 0x2: m_regs.r[rd] = barrelShift(m_regs.r[rd], 0, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break; // LSL
            case 0x3: m_regs.r[rd] = barrelShift(m_regs.r[rd], 1, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break; // LSR
            case 0x4: m_regs.r[rd] = barrelShift(m_regs.r[rd], 2, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break; // ASR
            case 0x5: m_regs.r[rd] = adcWithFlags(m_regs.r[rd], m_regs.r[rs], carry, true); break; // ADC
            case 0x6: m_regs.r[rd] = sbcWithFlags(m_regs.r[rd], m_regs.r[rs], carry, true); break; // SBC
            case 0x7: m_regs.r[rd] = barrelShift(m_regs.r[rd], 3, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break; // ROR
            case 0x8: setNzFlags(m_regs.r[rd] & m_regs.r[rs]); break; // TST
            case 0x9: m_regs.r[rd] = subWithFlags(0, m_regs.r[rs], true); break; // NEG
            case 0xA: subWithFlags(m_regs.r[rd], m_regs.r[rs], true); break; // CMP
            case 0xB: addWithFlags(m_regs.r[rd], m_regs.r[rs], true); break; // CMN
            case 0xC: m_regs.r[rd] |= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // ORR
            case 0xD: m_regs.r[rd] *= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // MUL
            case 0xE: m_regs.r[rd] &= ~m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // BIC
            case 0xF: m_regs.r[rd] = ~m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break; // MVN
        }
        return 1;
    }

    // Formato 5: Hi Register Operations / Branch Exchange (010001 Op H1 H2 Rs Rd)
    if ((opcode >> 10) == 0b010001) {
        const uint8_t  op = (opcode >> 8) & 3u;
        const uint32_t rd = (opcode & 7u) | ((opcode >> 4) & 8u);
        const uint32_t rs = ((opcode >> 3) & 7u) | ((opcode >> 3) & 8u);
        const uint32_t srcVal = m_regs.r[rs];

        switch (op) {
            case 0: // ADD
                m_regs.r[rd] += srcVal;
                if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; }
                break;
            case 1: // CMP
                subWithFlags(m_regs.r[rd], srcVal, true);
                break;
            case 2: // MOV
                m_regs.r[rd] = srcVal;
                if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; }
                break;
            case 3: // BX
                m_regs.setFlag(CpuRegisters::FLAG_T, (srcVal & 1u) != 0);
                m_regs.r[15] = srcVal & (m_regs.isThumb() ? ~1u : ~3u);
                m_pcModified = true;
                break;
        }
        return 1;
    }

    // Formato 6: PC-relative Load (01001 Rd Word8)
    if ((opcode >> 11) == 0b01001) {
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t imm  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[15] & ~3u) + imm;
        m_regs.r[rd] = m_bus.read32(addr);
        return 3;
    }

    // Formato 7 & 8: Load/Store com Offset de Registrador (0101 xx Ro Rb Rd)
    if ((opcode >> 12) == 0b0101) {
        const uint32_t ro   = (opcode >> 6) & 7u;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + m_regs.r[ro];

        if ((opcode & (1u << 9)) == 0) {
            const uint8_t op = (opcode >> 10) & 3u;
            switch (op) {
                case 0: m_bus.write32(addr & ~3u, m_regs.r[rd]); break; // STR
                case 1: m_bus.write8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFF)); break; // STRB
                case 2: m_regs.r[rd] = std::rotr(m_bus.read32(addr & ~3u), (addr & 3u) * 8u); break; // LDR
                case 3: m_regs.r[rd] = m_bus.read8(addr); break;        // LDRB
            }
        } else {
            const uint8_t op = (opcode >> 10) & 3u;
            switch (op) {
                case 0: m_bus.write16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFF)); break; // STRH
                case 1: m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.read8(addr)))); break; // LDSB
                case 2: m_regs.r[rd] = std::rotr(static_cast<uint32_t>(m_bus.read16(addr & ~1u)), (addr & 1u) * 8u); break; // LDRH
                case 3:
                    if (addr & 1u) {
                        m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.read8(addr))));
                    } else {
                        m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(m_bus.read16(addr))));
                    }
                    break; // LDSH
            }
        }
        return 2;
    }

    // Formato 9: Load/Store com Offset Imediato (011 B L Offset5 Rb Rd)
    if ((opcode >> 13) == 0b011) {
        const bool isByte   = (opcode & (1u << 12)) != 0;
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t off  = (opcode >> 6) & 0x1Fu;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + (isByte ? off : (off * 4u));

        if (isLoad) {
            m_regs.r[rd] = isByte ? m_bus.read8(addr) : std::rotr(m_bus.read32(addr & ~3u), (addr & 3u) * 8u);
        } else {
            if (isByte) m_bus.write8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFF));
            else        m_bus.write32(addr & ~3u, m_regs.r[rd]);
        }
        return 2;
    }

    // Formato 10: Load/Store Halfword (1000 L Offset5 Rb Rd)
    if ((opcode >> 12) == 0b1000) {
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t off  = ((opcode >> 6) & 0x1Fu) * 2u;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t rawAddr = m_regs.r[rb] + off;
        if (isLoad) m_regs.r[rd] = std::rotr(static_cast<uint32_t>(m_bus.read16(rawAddr & ~1u)), (rawAddr & 1u) * 8u);
        else        m_bus.write16(rawAddr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFF));
        return 2;
    }

    // Formato 11: SP-relative Load/Store (1001 L Rd Word8)
    if ((opcode >> 12) == 0b1001) {
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t off  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[13] + off) & ~3u;
        if (isLoad) m_regs.r[rd] = m_bus.read32(addr);
        else        m_bus.write32(addr, m_regs.r[rd]);
        return 2;
    }

    // Formato 12: Load Address (1010 SP Rd Word8)
    if ((opcode >> 12) == 0b1010) {
        const bool useSp  = (opcode & (1u << 11)) != 0;
        const uint32_t rd = (opcode >> 8) & 7u;
        const uint32_t off = (opcode & 0xFFu) * 4u;
        m_regs.r[rd] = (useSp ? m_regs.r[13] : (m_regs.r[15] & ~3u)) + off;
        return 1;
    }

    // Formato 13: Add Offset to Stack Pointer (1011 0000 S Word7)
    if ((opcode >> 8) == 0b10110000) {
        const uint32_t off = (opcode & 0x7Fu) * 4u;
        if (opcode & 0x80u) m_regs.r[13] -= off;
        else                m_regs.r[13] += off;
        return 1;
    }

    // Formato 14: Push / Pop Registers (1011 L 10 R Rlist)
    if ((opcode & 0xF600u) == 0xB400u) {
        const bool isPop = (opcode & (1u << 11)) != 0;
        const bool pclr  = (opcode & (1u << 8)) != 0;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);

        if (isPop) {
            for (uint32_t i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    m_regs.r[i] = m_bus.read32(m_regs.r[13]);
                    m_regs.r[13] += 4u;
                }
            }
            if (pclr) {
                m_regs.r[15] = m_bus.read32(m_regs.r[13]) & ~1u;
                m_regs.r[13] += 4u;
                m_pcModified = true;
            }
        } else {
            if (pclr) {
                m_regs.r[13] -= 4u;
                m_bus.write32(m_regs.r[13], m_regs.r[14]);
            }
            for (int i = 7; i >= 0; --i) {
                if (list & (1u << i)) {
                    m_regs.r[13] -= 4u;
                    m_bus.write32(m_regs.r[13], m_regs.r[i]);
                }
            }
        }
        return 3;
    }

    // Formato 15: Multiple Load/Store (1100 L Rb Rlist)
    if ((opcode >> 12) == 0b1100) {
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t rb  = (opcode >> 8) & 7u;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);
        uint32_t addr = m_regs.r[rb];
        for (uint32_t i = 0; i < 8; ++i) {
            if (list & (1u << i)) {
                if (isLoad) m_regs.r[i] = m_bus.read32(addr);
                else        m_bus.write32(addr, m_regs.r[i]);
                addr += 4u;
            }
        }
        if (!(isLoad && (list & (1u << rb)))) {
            m_regs.r[rb] = addr;
        }
        return 3;
    }

    // Formato 16 & 17: Conditional Branch / SWI (1101 Cond Offset8)
    if ((opcode >> 12) == 0b1101) {
        const uint8_t cond = (opcode >> 8) & 0x0Fu;
        if (cond == 0x0F) {
            handleSwi(static_cast<uint8_t>(opcode & 0xFFu));
            return 3;
        }
        if (checkCondition(cond)) {
            const int32_t off = static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFFu)) * 2;
            m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + off) & ~1u;
            m_pcModified = true;
            return 3;
        }
        return 1;
    }

    // Formato 18: Unconditional Branch (11100 Offset11)
    if ((opcode >> 11) == 0b11100) {
        int32_t off = opcode & 0x7FFu;
        if (off & 0x400) off |= ~0x7FF;
        off <<= 1;
        m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + off) & ~1u;
        m_pcModified = true;
        return 3;
    }

    // Formato 19: Long Branch with Link (1111 H Offset11)
    if ((opcode >> 12) == 0b1111) {
        const bool isSecondHalf = (opcode & (1u << 11)) != 0;
        int32_t off = opcode & 0x7FFu;
        if (!isSecondHalf) {
            if (off & 0x400) off |= ~0x7FF;
            m_regs.r[14] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + (off << 12));
        } else {
            const uint32_t nextInstr = (m_regs.r[15] - 2u) | 1u;
            m_regs.r[15] = (m_regs.r[14] + (static_cast<uint32_t>(off) << 1)) & ~1u;
            m_regs.r[14] = nextInstr;
            m_pcModified = true;
        }
        return 2;
    }

    return 1;
}

std::string Arm7Tdmi::disassembleArm(uint32_t addr, uint32_t opcode) {
    std::ostringstream oss;
    const uint8_t cond = static_cast<uint8_t>((opcode >> 28) & 0x0F);
    const std::string_view cs = condSuffix(cond);

    if ((opcode & 0x0FFFFFF0u) == 0x012FFF10u) {
        oss << "BX" << cs << " " << regName(opcode & 0x0Fu);
        return oss.str();
    }
    if ((opcode & 0x0F000000u) == 0x0F000000u) {
        const uint8_t swi = static_cast<uint8_t>((opcode >> 16) & 0xFFu);
        oss << "SWI" << cs << " 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<uint32_t>(swi) << " (" << swiName(swi) << ")";
        return oss.str();
    }
    if ((opcode & 0x0E000000u) == 0x0A000000u) {
        const bool link = (opcode & (1u << 24)) != 0;
        int32_t off = static_cast<int32_t>(opcode & 0x00FFFFFFu);
        if (off & 0x00800000) off |= ~0x00FFFFFF;
        off <<= 2;
        const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(addr + 8u) + off);
        oss << (link ? "BL" : "B") << cs << " 0x" << std::hex << std::setw(8) << std::setfill('0') << target;
        return oss.str();
    }
    if ((opcode & 0x0C000000u) == 0x04000000u) {
        const bool isLoad = (opcode & (1u << 20)) != 0;
        const bool isByte = (opcode & (1u << 22)) != 0;
        const bool up     = (opcode & (1u << 23)) != 0;
        const bool imm    = (opcode & (1u << 25)) == 0;
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        oss << (isLoad ? "LDR" : "STR") << (isByte ? "B" : "") << cs << " " << regName(rd)
            << ", [" << regName(rn);
        if (imm) {
            const uint32_t off = opcode & 0xFFFu;
            if (off != 0) oss << ", #" << (up ? "+" : "-") << "0x" << std::hex << off;
        } else {
            oss << ", " << (up ? "+" : "-") << regName(opcode & 0x0Fu);
        }
        oss << "]";
        return oss.str();
    }
    if ((opcode & 0x0E000090u) == 0x00000090u && ((opcode >> 5) & 3u) != 0) {
        const bool isLoad = (opcode & (1u << 20)) != 0;
        const bool up     = (opcode & (1u << 23)) != 0;
        const bool imm    = (opcode & (1u << 22)) != 0;
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        const uint8_t  sh = static_cast<uint8_t>((opcode >> 5) & 3u);
        const char* mn    = isLoad ? (sh == 1 ? "LDRH" : (sh == 2 ? "LDRSB" : "LDRSH")) : "STRH";
        oss << mn << cs << " " << regName(rd) << ", [" << regName(rn);
        if (imm) {
            const uint32_t off = ((opcode >> 4) & 0xF0u) | (opcode & 0x0Fu);
            if (off != 0) oss << ", #" << (up ? "+" : "-") << "0x" << std::hex << off;
        } else {
            oss << ", " << (up ? "+" : "-") << regName(opcode & 0x0Fu);
        }
        oss << "]";
        return oss.str();
    }
    if ((opcode & 0x0E000000u) == 0x08000000u) {
        const bool isLoad = (opcode & (1u << 20)) != 0;
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        oss << (isLoad ? "LDM" : "STM") << cs << " " << regName(rn)
            << ((opcode & (1u << 21)) ? "!" : "") << ", {reglist=0x"
            << std::hex << (opcode & 0xFFFFu) << "}";
        return oss.str();
    }
    if ((opcode & 0x0C000000u) == 0x00000000u) {
        static constexpr std::array<std::string_view, 16> ALU_NAMES = {
            "AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
            "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"
        };
        const uint8_t aluOp = static_cast<uint8_t>((opcode >> 21) & 0x0Fu);
        const bool setFlags = (opcode & (1u << 20)) != 0;
        const bool isImm    = (opcode & (1u << 25)) != 0;
        const uint32_t rn   = (opcode >> 16) & 0x0Fu;
        const uint32_t rd   = (opcode >> 12) & 0x0Fu;

        oss << ALU_NAMES[aluOp] << cs << (setFlags && (aluOp < 8 || aluOp > 11) ? "S" : "") << " ";
        if (aluOp >= 8 && aluOp <= 11) {
            oss << regName(rn) << ", ";
        } else if (aluOp == 13 || aluOp == 15) {
            oss << regName(rd) << ", ";
        } else {
            oss << regName(rd) << ", " << regName(rn) << ", ";
        }

        if (isImm) {
            const uint32_t imm = opcode & 0xFFu;
            const uint32_t rot = ((opcode >> 8) & 0x0Fu) * 2u;
            const uint32_t val = std::rotr(imm, rot);
            oss << "#0x" << std::hex << val;
        } else {
            oss << regName(opcode & 0x0Fu);
        }
        return oss.str();
    }

    oss << "ARM_OP 0x" << std::hex << std::setw(8) << std::setfill('0') << opcode;
    return oss.str();
}

std::string Arm7Tdmi::disassembleThumb(uint32_t addr, uint16_t opcode) {
    std::ostringstream oss;

    if ((opcode >> 11) == 0b00011) {
        const bool isImm = (opcode & (1u << 10)) != 0;
        const bool isSub = (opcode & (1u << 9)) != 0;
        const uint32_t rnImm = (opcode >> 6) & 7u;
        const uint32_t rs    = (opcode >> 3) & 7u;
        const uint32_t rd    = opcode & 7u;
        oss << (isSub ? "SUB" : "ADD") << " R" << rd << ", R" << rs << ", ";
        if (isImm) oss << "#0x" << std::hex << rnImm;
        else       oss << "R" << rnImm;
        return oss.str();
    }
    if ((opcode >> 13) == 0b000) {
        static constexpr std::array<std::string_view, 4> SHIFT_NAMES = {"LSL", "LSR", "ASR", "ROR"};
        const uint8_t  op     = (opcode >> 11) & 3u;
        const uint8_t  offset = (opcode >> 6) & 0x1Fu;
        const uint32_t rs     = (opcode >> 3) & 7u;
        const uint32_t rd     = opcode & 7u;
        oss << SHIFT_NAMES[op] << " R" << rd << ", R" << rs << ", #" << static_cast<uint32_t>(offset);
        return oss.str();
    }
    if ((opcode >> 13) == 0b001) {
        static constexpr std::array<std::string_view, 4> OP_NAMES = {"MOV", "CMP", "ADD", "SUB"};
        const uint8_t  op  = (opcode >> 11) & 3u;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t imm = opcode & 0xFFu;
        oss << OP_NAMES[op] << " R" << rd << ", #0x" << std::hex << imm;
        return oss.str();
    }
    if ((opcode >> 10) == 0b010000) {
        static constexpr std::array<std::string_view, 16> ALU_NAMES = {
            "AND", "EOR", "LSL", "LSR", "ASR", "ADC", "SBC", "ROR",
            "TST", "NEG", "CMP", "CMN", "ORR", "MUL", "BIC", "MVN"
        };
        const uint8_t  op = (opcode >> 6) & 0x0Fu;
        const uint32_t rs = (opcode >> 3) & 7u;
        const uint32_t rd = opcode & 7u;
        oss << ALU_NAMES[op] << " R" << rd << ", R" << rs;
        return oss.str();
    }
    if ((opcode >> 10) == 0b010001) {
        const uint8_t  op = (opcode >> 8) & 3u;
        const uint32_t rd = (opcode & 7u) | ((opcode >> 4) & 8u);
        const uint32_t rs = ((opcode >> 3) & 7u) | ((opcode >> 3) & 8u);
        switch (op) {
            case 0: oss << "ADD " << regName(rd) << ", " << regName(rs); break;
            case 1: oss << "CMP " << regName(rd) << ", " << regName(rs); break;
            case 2: oss << "MOV " << regName(rd) << ", " << regName(rs); break;
            case 3: oss << "BX " << regName(rs); break;
        }
        return oss.str();
    }
    if ((opcode >> 11) == 0b01001) {
        const uint32_t rd     = (opcode >> 8) & 7u;
        const uint32_t imm    = (opcode & 0xFFu) * 4u;
        const uint32_t target = ((addr + 4u) & ~3u) + imm;
        oss << "LDR R" << rd << ", [PC, #0x" << std::hex << imm << "] ; =0x" << std::setw(8) << std::setfill('0') << target;
        return oss.str();
    }
    if ((opcode >> 12) == 0b0101) {
        const uint32_t ro = (opcode >> 6) & 7u;
        const uint32_t rb = (opcode >> 3) & 7u;
        const uint32_t rd = opcode & 7u;
        if ((opcode & (1u << 9)) == 0) {
            static constexpr std::array<std::string_view, 4> OPS = {"STR", "STRB", "LDR", "LDRB"};
            oss << OPS[(opcode >> 10) & 3u] << " R" << rd << ", [R" << rb << ", R" << ro << "]";
        } else {
            static constexpr std::array<std::string_view, 4> OPS = {"STRH", "LDSB", "LDRH", "LDSH"};
            oss << OPS[(opcode >> 10) & 3u] << " R" << rd << ", [R" << rb << ", R" << ro << "]";
        }
        return oss.str();
    }
    if ((opcode >> 13) == 0b011) {
        const bool isByte  = (opcode & (1u << 12)) != 0;
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t off = ((opcode >> 6) & 0x1Fu) * (isByte ? 1u : 4u);
        const uint32_t rb  = (opcode >> 3) & 7u;
        const uint32_t rd  = opcode & 7u;
        oss << (isLoad ? "LDR" : "STR") << (isByte ? "B" : "")
            << " R" << rd << ", [R" << rb << ", #0x" << std::hex << off << "]";
        return oss.str();
    }
    if ((opcode >> 12) == 0b1000) {
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t off = ((opcode >> 6) & 0x1Fu) * 2u;
        const uint32_t rb  = (opcode >> 3) & 7u;
        const uint32_t rd  = opcode & 7u;
        oss << (isLoad ? "LDRH" : "STRH") << " R" << rd << ", [R" << rb << ", #0x" << std::hex << off << "]";
        return oss.str();
    }
    if ((opcode >> 12) == 0b1001) {
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t off = (opcode & 0xFFu) * 4u;
        oss << (isLoad ? "LDR" : "STR") << " R" << rd << ", [SP, #0x" << std::hex << off << "]";
        return oss.str();
    }
    if ((opcode >> 12) == 0b1010) {
        const bool useSp   = (opcode & (1u << 11)) != 0;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t off = (opcode & 0xFFu) * 4u;
        oss << "ADD R" << rd << ", " << (useSp ? "SP" : "PC") << ", #0x" << std::hex << off;
        return oss.str();
    }
    if ((opcode >> 8) == 0b10110000) {
        const uint32_t off = (opcode & 0x7Fu) * 4u;
        oss << "ADD SP, #" << ((opcode & 0x80u) ? "-" : "+") << "0x" << std::hex << off;
        return oss.str();
    }
    if ((opcode & 0xF600u) == 0xB400u) {
        const bool isPop = (opcode & (1u << 11)) != 0;
        const bool pclr  = (opcode & (1u << 8)) != 0;
        oss << (isPop ? "POP" : "PUSH") << " {reglist=0x" << std::hex << (opcode & 0xFFu)
            << (pclr ? (isPop ? ", PC" : ", LR") : "") << "}";
        return oss.str();
    }
    if ((opcode >> 12) == 0b1100) {
        const bool isLoad = (opcode & (1u << 11)) != 0;
        const uint32_t rb = (opcode >> 8) & 7u;
        oss << (isLoad ? "LDMIA" : "STMIA") << " R" << rb << "!, {reglist=0x" << std::hex << (opcode & 0xFFu) << "}";
        return oss.str();
    }
    if ((opcode >> 12) == 0b1101) {
        const uint8_t cond = (opcode >> 8) & 0x0Fu;
        if (cond == 0x0F) {
            const uint8_t swi = static_cast<uint8_t>(opcode & 0xFFu);
            oss << "SWI 0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<uint32_t>(swi) << " (" << swiName(swi) << ")";
            return oss.str();
        }
        const int32_t off = static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFFu)) * 2;
        const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(addr + 4u) + off);
        oss << "B" << condSuffix(cond) << " 0x" << std::hex << std::setw(8) << std::setfill('0') << target;
        return oss.str();
    }
    if ((opcode >> 11) == 0b11100) {
        int32_t off = opcode & 0x7FFu;
        if (off & 0x400) off |= ~0x7FF;
        off <<= 1;
        const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(addr + 4u) + off);
        oss << "B 0x" << std::hex << std::setw(8) << std::setfill('0') << target;
        return oss.str();
    }
    if ((opcode >> 12) == 0b1111) {
        const bool isSecondHalf = (opcode & (1u << 11)) != 0;
        oss << (isSecondHalf ? "BL_LO" : "BL_HI") << " offset=0x" << std::hex << (opcode & 0x7FFu);
        return oss.str();
    }

    oss << "THUMB_OP 0x" << std::hex << std::setw(4) << std::setfill('0') << opcode;
    return oss.str();
}

void Arm7Tdmi::serializeState(SaveStateWriter& out) const {
    out.writePod(m_regs);
    out.writePod(m_halted);
    out.writePod(m_pcModified);
}

bool Arm7Tdmi::deserializeState(SaveStateReader& in) {
    m_pcHistory.fill(0);
    m_pcHistoryHead            = 0;
    m_pcHistoryCount           = 0;
    m_stuckLoopPcCount         = 0;
    m_stuckLoopCycles          = 0;
    m_stuckLoopReportedCurrent = false;
    return in.readPod(m_regs) &&
           in.readPod(m_halted) &&
           in.readPod(m_pcModified);
}

} // namespace gba::core
