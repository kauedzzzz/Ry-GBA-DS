#include "core/nds/Arm9Cpu.hpp"
#include "core/nds/NdsBus.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace gba::core::nds {

namespace {

int32_t saturate32(int64_t val, bool& saturated) noexcept {
    if (val > static_cast<int64_t>(INT32_MAX)) {
        saturated = true;
        return INT32_MAX;
    }
    if (val < static_cast<int64_t>(INT32_MIN)) {
        saturated = true;
        return INT32_MIN;
    }
    return static_cast<int32_t>(val);
}

uint16_t computeCrc16Step(uint16_t crc, uint8_t byte) noexcept {
    static constexpr std::array<uint16_t, 8> XOR_VAL = {
        0xC0C1u, 0xC181u, 0xC301u, 0xC601u,
        0xCC01u, 0xD801u, 0xF001u, 0xA001u
    };
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        const bool carry = (crc & 1u) != 0;
        crc >>= 1;
        if (carry) {
            crc ^= static_cast<uint16_t>(XOR_VAL[i] << (7 - i));
        }
    }
    return crc;
}

uint16_t crc16ReflectedA001(uint16_t crc, uint8_t byte) noexcept {
    crc ^= byte;
    for (int b = 0; b < 8; ++b) {
        if (crc & 1u) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001u);
        else          crc = static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

} // namespace

Arm9Cpu::Arm9Cpu(NdsBus& bus)
    : m_bus(bus) {
    reset();
}

void Arm9Cpu::reset() {
    m_regs           = CpuRegisters{};
    m_halted         = false;
    m_pcModified     = false;
    m_intrWaitActive = false;
    m_intrWaitMask   = 0;

    m_itcm.fill(0);
    m_dtcm.fill(0);

    m_cp15Ctrl = 0x0005707Du; // High exception vectors (0xFFFF0000), DTCM + ITCM ativos
    m_cp15Dtcm = 0x0300000Au; // Base 0x03000000, tamanho virtual 16 KB
    m_cp15Itcm = 0x00000020u; // Base 0x00000000, tamanho virtual 32 MB
    m_cp15Misc.fill(0);
    m_cp15PuRegions.fill(0);

    m_regs.cpsr  = static_cast<uint32_t>(CpuMode::Supervisor) | CpuRegisters::FLAG_I | CpuRegisters::FLAG_F;
    m_regs.r[15] = 0xFFFF0000u;
    (void)computeCrc16Step;
}

void Arm9Cpu::initDirectBoot(uint32_t entryAddr) {
    reset();

    // Pilhas pós-firmware configuradas pela BIOS do Nintendo DS dentro da DTCM (0x03000000..0x03003FFF)
    m_regs.r[13]         = 0x03002F7Cu; // USR/SYS SP
    m_regs.r8_14_usr[5]  = 0x03002F7Cu;
    m_regs.r13_14_irq[0] = 0x03003F80u; // IRQ SP
    m_regs.r13_14_svc[0] = 0x03003FC0u; // SVC SP

    m_regs.r[12] = entryAddr;
    m_regs.r[14] = entryAddr;
    m_regs.r8_14_usr[6] = entryAddr;

    m_regs.cpsr = static_cast<uint32_t>(CpuMode::System);
    setPcInterwork(entryAddr);
    m_pcModified = false;
}

// =============================================================================
// Roteamento TCM (DTCM 16KB -> ITCM 32KB -> NdsBus)
// Nota: DTCM tem prioridade sobre ITCM em acessos de dados para suportar o
// reposicionamento padrão da NitroSDK (ex: HW_DTCM = 0x00800000 ou 0x027E0000).
// =============================================================================
bool Arm9Cpu::isInDtcmData(uint32_t addr) const noexcept {
    if ((m_cp15Ctrl & (1u << 16)) == 0) return false;
    const uint32_t base  = m_cp15Dtcm & 0xFFFFF000u;
    const uint32_t shift = (m_cp15Dtcm >> 1) & 0x1Fu;
    const uint64_t size  = (shift >= 23u) ? 0x100000000ull : (512ull << shift);
    return (addr >= base) && (static_cast<uint64_t>(addr - base) < size);
}

bool Arm9Cpu::isInItcmData(uint32_t addr) const noexcept {
    if ((m_cp15Ctrl & (1u << 18)) == 0) return false;
    const uint32_t shift = (m_cp15Itcm >> 1) & 0x1Fu;
    const uint64_t size  = (shift >= 23u) ? 0x100000000ull : (512ull << shift);
    return static_cast<uint64_t>(addr) < size;
}

bool Arm9Cpu::isInItcmCode(uint32_t addr) const noexcept {
    if ((m_cp15Ctrl & (1u << 18)) == 0) return false;
    if ((m_cp15Ctrl & (1u << 19)) != 0) return false; // ITCM Load Mode = write-only
    const uint32_t shift = (m_cp15Itcm >> 1) & 0x1Fu;
    const uint64_t size  = (shift >= 23u) ? 0x100000000ull : (512ull << shift);
    return static_cast<uint64_t>(addr) < size;
}

uint8_t Arm9Cpu::dataRead8(uint32_t addr) {
    if (isInDtcmData(addr) && (m_cp15Ctrl & (1u << 17)) == 0) {
        return m_dtcm[addr & 0x3FFFu];
    }
    if (isInItcmData(addr) && (m_cp15Ctrl & (1u << 19)) == 0) {
        return m_itcm[addr & 0x7FFFu];
    }
    return m_bus.arm9Read8(addr);
}

uint16_t Arm9Cpu::dataRead16(uint32_t addr) {
    const uint32_t aligned = addr & ~1u;
    if (isInDtcmData(aligned) && (m_cp15Ctrl & (1u << 17)) == 0) {
        const uint32_t off = aligned & 0x3FFEu;
        return static_cast<uint16_t>(m_dtcm[off] | (static_cast<uint16_t>(m_dtcm[off + 1]) << 8));
    }
    if (isInItcmData(aligned) && (m_cp15Ctrl & (1u << 19)) == 0) {
        const uint32_t off = aligned & 0x7FFEu;
        return static_cast<uint16_t>(m_itcm[off] | (static_cast<uint16_t>(m_itcm[off + 1]) << 8));
    }
    return m_bus.arm9Read16(aligned);
}

uint32_t Arm9Cpu::dataRead32(uint32_t addr) {
    const uint32_t aligned = addr & ~3u;
    if (isInDtcmData(aligned) && (m_cp15Ctrl & (1u << 17)) == 0) {
        const uint32_t off = aligned & 0x3FFCu;
        uint32_t val = 0;
        std::memcpy(&val, &m_dtcm[off], sizeof(uint32_t));
        return val;
    }
    if (isInItcmData(aligned) && (m_cp15Ctrl & (1u << 19)) == 0) {
        const uint32_t off = aligned & 0x7FFCu;
        uint32_t val = 0;
        std::memcpy(&val, &m_itcm[off], sizeof(uint32_t));
        return val;
    }
    return m_bus.arm9Read32(aligned);
}

void Arm9Cpu::dataWrite8(uint32_t addr, uint8_t val) {
    if (isInDtcmData(addr)) {
        m_dtcm[addr & 0x3FFFu] = val;
        return;
    }
    if (isInItcmData(addr)) {
        m_itcm[addr & 0x7FFFu] = val;
        return;
    }
    m_bus.arm9Write8(addr, val);
}

void Arm9Cpu::dataWrite16(uint32_t addr, uint16_t val) {
    const uint32_t aligned = addr & ~1u;
    if (isInDtcmData(aligned)) {
        const uint32_t off = aligned & 0x3FFEu;
        m_dtcm[off]     = static_cast<uint8_t>(val & 0xFFu);
        m_dtcm[off + 1] = static_cast<uint8_t>(val >> 8);
        return;
    }
    if (isInItcmData(aligned)) {
        const uint32_t off = aligned & 0x7FFEu;
        m_itcm[off]     = static_cast<uint8_t>(val & 0xFFu);
        m_itcm[off + 1] = static_cast<uint8_t>(val >> 8);
        return;
    }
    m_bus.arm9Write16(aligned, val);
}

void Arm9Cpu::dataWrite32(uint32_t addr, uint32_t val) {
    const uint32_t aligned = addr & ~3u;
    if (isInDtcmData(aligned)) {
        const uint32_t off = aligned & 0x3FFCu;
        std::memcpy(&m_dtcm[off], &val, sizeof(uint32_t));
        return;
    }
    if (isInItcmData(aligned)) {
        const uint32_t off = aligned & 0x7FFCu;
        std::memcpy(&m_itcm[off], &val, sizeof(uint32_t));
        return;
    }
    m_bus.arm9Write32(aligned, val);
}

uint16_t Arm9Cpu::fetchCode16(uint32_t addr) {
    const uint32_t aligned = addr & ~1u;
    if (isInItcmCode(aligned)) {
        const uint32_t off = aligned & 0x7FFEu;
        return static_cast<uint16_t>(m_itcm[off] | (static_cast<uint16_t>(m_itcm[off + 1]) << 8));
    }
    return m_bus.arm9Read16(aligned);
}

uint32_t Arm9Cpu::fetchCode32(uint32_t addr) {
    const uint32_t aligned = addr & ~3u;
    if (isInItcmCode(aligned)) {
        const uint32_t off = aligned & 0x7FFCu;
        uint32_t val = 0;
        std::memcpy(&val, &m_itcm[off], sizeof(uint32_t));
        return val;
    }
    return m_bus.arm9Read32(aligned);
}

// =============================================================================
// Coprocessador de Sistema CP15 (ARM946E-S)
// =============================================================================
uint32_t Arm9Cpu::readCp15(uint8_t crn, uint8_t crm, uint8_t op2) const noexcept {
    switch (crn) {
        case 0:
            if (op2 == 1) return 0x0F0D2112u; // Cache Type (8KB I-Cache, 4KB D-Cache)
            if (op2 == 2) return 0x00140180u; // TCM Size (16KB DTCM, 32KB ITCM)
            return 0x41059461u;               // Main ID: ARM946E-S r1p1
        case 1:
            return m_cp15Ctrl;
        case 2:
            return m_cp15Misc[op2 & 1u]; // Cachability (Data/Instruction)
        case 3:
            return m_cp15Misc[2];        // Write Bufferability
        case 5:
            return m_cp15Misc[4 + (op2 & 3u)]; // Access Permissions
        case 6:
            return m_cp15PuRegions[crm & 7u];  // Protection Unit Region c6,c0..c7
        case 9:
            if (crm == 1 && op2 == 0) return m_cp15Dtcm;
            if (crm == 1 && op2 == 1) return m_cp15Itcm;
            return m_cp15Misc[8 + (op2 & 1u)]; // Cache Lockdown
        default:
            return 0;
    }
}

void Arm9Cpu::writeCp15(uint8_t crn, uint8_t crm, uint8_t op2, uint32_t val) noexcept {
    switch (crn) {
        case 1:
            // Bits 3..6 fixos em 1 no ARM946E-S; preserva bits configuráveis de TCM/Cache/HighVectors/L4
            m_cp15Ctrl = (val & 0x000FF085u) | 0x00000078u;
            break;
        case 2:
            m_cp15Misc[op2 & 1u] = val;
            break;
        case 3:
            m_cp15Misc[2] = val;
            break;
        case 5:
            m_cp15Misc[4 + (op2 & 3u)] = val;
            break;
        case 6:
            m_cp15PuRegions[crm & 7u] = val;
            break;
        case 7:
            // Wait For Interrupt: MCR p15, 0, Rd, c7, c0, 4 (ou c7, c8, 2)
            if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) {
                m_halted = true;
            }
            break;
        case 9:
            if (crm == 1 && op2 == 0) {
                m_cp15Dtcm = val & 0xFFFFF03Eu;
            } else if (crm == 1 && op2 == 1) {
                m_cp15Itcm = val & 0x0000003Eu; // Base física do ITCM é sempre 0x00000000
            } else {
                m_cp15Misc[8 + (op2 & 1u)] = val;
            }
            break;
        default:
            break;
    }
}

// =============================================================================
// Modos, Interworking ARMv5 e Interrupções
// =============================================================================
void Arm9Cpu::setPcInterwork(uint32_t target) noexcept {
    if ((m_cp15Ctrl & (1u << 15)) == 0) { // Bit L4 == 0 -> ARMv5 interworking ativo
        m_regs.setFlag(CpuRegisters::FLAG_T, (target & 1u) != 0);
    }
    m_regs.r[15] = target & (m_regs.isThumb() ? ~1u : ~3u);
    m_pcModified = true;
}

void Arm9Cpu::switchMode(CpuMode newMode) noexcept {
    const CpuMode oldMode = m_regs.getMode();
    if (oldMode == newMode) return;

    if (oldMode == CpuMode::Fiq) {
        for (int i = 0; i < 7; ++i) m_regs.r8_14_fiq[i] = m_regs.r[8 + i];
        m_regs.spsr_bank[0] = m_regs.spsr;
    } else if (oldMode == CpuMode::User || oldMode == CpuMode::System) {
        for (int i = 0; i < 7; ++i) m_regs.r8_14_usr[i] = m_regs.r[8 + i];
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

    if (newMode == CpuMode::Fiq) {
        for (int i = 0; i < 7; ++i) m_regs.r[8 + i] = m_regs.r8_14_fiq[i];
        m_regs.spsr = m_regs.spsr_bank[0];
    } else if (newMode == CpuMode::User || newMode == CpuMode::System) {
        for (int i = 0; i < 7; ++i) m_regs.r[8 + i] = m_regs.r8_14_usr[i];
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

void Arm9Cpu::pollIntrWait() {
    if (!m_intrWaitActive) return;
    if (m_regs.getMode() == CpuMode::Irq) return;

    const uint32_t flagsAddr = dtcmBase() + 0x3FF8u;
    const uint32_t swFlags   = dataRead32(flagsAddr);
    if ((swFlags & m_intrWaitMask) != 0) {
        dataWrite32(flagsAddr, swFlags & ~m_intrWaitMask);
        m_intrWaitActive = false;
        m_halted         = false;
        return;
    }

    // Caso uma IRQ não relacionada tenha acordado a CPU e retornado, volta a aguardar
    if (!m_bus.anyEnabledIrqFlag9()) {
        m_halted = true;
    }
}

void Arm9Cpu::checkInterrupts() {
    const uint32_t pending = m_bus.ie9() & m_bus.if9();
    if (pending != 0) {
        m_halted = false;
    }

    if ((m_bus.ime9() & 1u) != 0 && pending != 0 && !m_regs.isIrqDisabled()) {
        if (m_intrWaitActive && (pending & m_intrWaitMask) != 0) {
            // Sinaliza também INTR_WAIT_FLAGS em DTCM+0x3FF8 caso o handler customizado não o faça
            const uint32_t flagsAddr = dtcmBase() + 0x3FF8u;
            dataWrite32(flagsAddr, dataRead32(flagsAddr) | (pending & m_intrWaitMask));
            m_intrWaitActive = false;
        }

        const uint32_t savedCpsr = m_regs.cpsr;
        const uint32_t nextPc    = m_regs.r[15] + 4u;
        switchMode(CpuMode::Irq);
        m_regs.spsr  = savedCpsr;
        m_regs.r[14] = nextPc;
        m_regs.cpsr &= ~CpuRegisters::FLAG_T;
        m_regs.cpsr |= CpuRegisters::FLAG_I;
        m_regs.r[15] = exceptionVectorBase() + 0x18u;
    }
}

int32_t Arm9Cpu::step() {
    checkInterrupts();
    pollIntrWait();
    checkInterrupts();

    if (m_halted) {
        return 4;
    }

    int32_t cycles = 1;
    m_pcModified = false;
    if (m_regs.isThumb()) {
        const uint32_t instrAddr = m_regs.r[15] & ~1u;
        const uint16_t opcode    = fetchCode16(instrAddr);
        m_regs.r[15] = instrAddr + 4u;
        cycles = executeThumb(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 2u;
        }
    } else {
        const uint32_t instrAddr = m_regs.r[15] & ~3u;
        const uint32_t opcode    = fetchCode32(instrAddr);
        m_regs.r[15] = instrAddr + 8u;
        cycles = executeArm(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 4u;
        }
    }
    return cycles;
}

bool Arm9Cpu::checkCondition(uint8_t cond) const noexcept {
    const bool n = m_regs.getFlag(CpuRegisters::FLAG_N);
    const bool z = m_regs.getFlag(CpuRegisters::FLAG_Z);
    const bool c = m_regs.getFlag(CpuRegisters::FLAG_C);
    const bool v = m_regs.getFlag(CpuRegisters::FLAG_V);

    switch (cond & 0x0Fu) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && (n == v);
        case 0xD: return z || (n != v);
        case 0xE: return true;
        default:  return true;
    }
}

uint32_t Arm9Cpu::barrelShift(uint32_t val, uint8_t shiftType, uint8_t amount, bool& carryOut, bool immediateShift) const noexcept {
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

void Arm9Cpu::setNzFlags(uint32_t result) noexcept {
    m_regs.setFlag(CpuRegisters::FLAG_N, (result & 0x80000000u) != 0);
    m_regs.setFlag(CpuRegisters::FLAG_Z, result == 0);
}

uint32_t Arm9Cpu::addWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
    const uint32_t res = static_cast<uint32_t>(sum);
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, sum > 0xFFFFFFFFull);
        m_regs.setFlag(CpuRegisters::FLAG_V, ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t Arm9Cpu::adcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
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

uint32_t Arm9Cpu::subWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint32_t res = a - b;
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, a >= b);
        m_regs.setFlag(CpuRegisters::FLAG_V, (((a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t Arm9Cpu::sbcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
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

// =============================================================================
// Rotinas SWI da BIOS ARM9 do Nintendo DS (HLE + Suporte a BIOS Real Externa)
// =============================================================================
void Arm9Cpu::handleSwi(uint8_t comment) {
    if (m_bus.hasCustomBios9()) {
        const uint32_t savedCpsr = m_regs.cpsr;
        const uint32_t retAddr   = m_regs.isThumb() ? (m_regs.r[15] - 2u) : (m_regs.r[15] - 4u);
        switchMode(CpuMode::Supervisor);
        m_regs.spsr  = savedCpsr;
        m_regs.r[14] = retAddr;
        m_regs.cpsr &= ~CpuRegisters::FLAG_T;
        m_regs.cpsr |= CpuRegisters::FLAG_I;
        m_regs.r[15] = exceptionVectorBase() + 0x08u;
        m_pcModified = true;
        return;
    }

    switch (comment) {
        case 0x03: { // WaitByLoop (R0 = loop iterations)
            m_regs.r[0] = 0;
            break;
        }
        case 0x04:   // IntrWait(R0 = discard, R1 = mask32)
        case 0x05: { // VBlankIntrWait -> IntrWait(1, 1)
            const bool     discard   = (comment == 0x05) ? true : (m_regs.r[0] != 0);
            const uint32_t mask      = (comment == 0x05) ? 1u   : m_regs.r[1];
            const uint32_t flagsAddr = dtcmBase() + 0x3FF8u;

            if (discard) {
                dataWrite32(flagsAddr, dataRead32(flagsAddr) & ~mask);
            }
            m_bus.arm9Write32(0x04000208u, 1u); // Garante IME = 1
            m_regs.cpsr &= ~CpuRegisters::FLAG_I;

            const uint32_t curFlags = dataRead32(flagsAddr);
            if (!discard && (curFlags & mask) != 0) {
                dataWrite32(flagsAddr, curFlags & ~mask);
            } else {
                m_intrWaitActive = true;
                m_intrWaitMask   = mask;
                m_halted         = true;
            }
            break;
        }
        case 0x06: // Halt
            m_halted = true;
            break;
        case 0x09: { // Div (NDS SWI 0x09: R0 / R1 -> R0=quot, R1=rem, R3=|quot|)
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
        case 0x0B: { // CpuSet
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t ctrl     = m_regs.r[2];
            const uint32_t count    = ctrl & 0x1FFFFFu;
            const bool     fixedSrc = (ctrl & (1u << 24)) != 0;
            const bool     is32Bit  = (ctrl & (1u << 26)) != 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (is32Bit) {
                    dataWrite32(dst, dataRead32(src));
                    dst += 4;
                    if (!fixedSrc) src += 4;
                } else {
                    dataWrite16(dst, dataRead16(src));
                    dst += 2;
                    if (!fixedSrc) src += 2;
                }
            }
            break;
        }
        case 0x0C: { // CpuFastSet
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t ctrl     = m_regs.r[2];
            const uint32_t rawCount = ctrl & 0x1FFFFFu;
            const uint32_t count    = (rawCount + 7u) & ~7u;
            const bool     fixedSrc = (ctrl & (1u << 24)) != 0;
            const uint32_t fillVal  = fixedSrc ? dataRead32(src) : 0u;
            for (uint32_t i = 0; i < count; ++i) {
                dataWrite32(dst, fixedSrc ? fillVal : dataRead32(src));
                dst += 4;
                if (!fixedSrc) src += 4;
            }
            break;
        }
        case 0x0D: { // Sqrt (NDS SWI 0x0D)
            m_regs.r[0] = static_cast<uint32_t>(std::sqrt(static_cast<double>(m_regs.r[0])));
            break;
        }
        case 0x0E: { // GetCRC16 (R0 = initCrc, R1 = addr, R2 = byteLen)
            uint16_t crc = static_cast<uint16_t>(m_regs.r[0] & 0xFFFFu);
            const uint32_t addr = m_regs.r[1];
            const uint32_t len  = m_regs.r[2];
            for (uint32_t i = 0; i < len; ++i) {
                crc = crc16ReflectedA001(crc, dataRead8(addr + i));
            }
            m_regs.r[0] = crc;
            break;
        }
        case 0x0F: { // IsDebugger (0 = Retail NDS)
            m_regs.r[0] = 0;
            break;
        }
        case 0x10: { // BitUnPack
            uint32_t src  = m_regs.r[0];
            uint32_t dst  = m_regs.r[1];
            uint32_t info = m_regs.r[2];
            const uint16_t srcLen     = dataRead16(info + 0);
            const uint8_t  srcBits    = dataRead8(info + 2);
            const uint8_t  dstBits    = dataRead8(info + 3);
            const uint32_t dataOffset = dataRead32(info + 4);
            const bool     zeroOffset = (dataOffset & (1u << 31)) != 0;
            const uint32_t addOffset  = dataOffset & 0x7FFFFFFFu;

            if (srcBits > 0 && dstBits > 0) {
                const uint32_t mask = (1u << srcBits) - 1u;
                uint32_t outWord   = 0;
                uint32_t outBitPos = 0;
                for (uint16_t i = 0; i < srcLen; ++i) {
                    const uint8_t byte = dataRead8(src++);
                    for (uint8_t b = 0; b < 8; b += srcBits) {
                        uint32_t unit = (byte >> b) & mask;
                        if (unit != 0 || zeroOffset) unit += addOffset;
                        outWord |= (unit << outBitPos);
                        outBitPos += dstBits;
                        if (outBitPos >= 32) {
                            dataWrite32(dst, outWord);
                            dst += 4;
                            outWord   = 0;
                            outBitPos = 0;
                        }
                    }
                }
            }
            break;
        }
        case 0x11:   // LZ77UnCompReadNormalWrite8bit
        case 0x12: { // LZ77UnCompReadByCallbackWrite16bit
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t header = dataRead32(src);
            src += 4;
            const uint32_t remaining = header >> 8;
            if (remaining == 0 || remaining > 0x200000u) break;

            std::vector<uint8_t> decompressed;
            decompressed.reserve(remaining);
            while (decompressed.size() < remaining) {
                const uint8_t flags = dataRead8(src++);
                for (int bit = 7; bit >= 0 && decompressed.size() < remaining; --bit) {
                    if ((flags >> bit) & 1u) {
                        const uint8_t b1 = dataRead8(src++);
                        const uint8_t b2 = dataRead8(src++);
                        const uint32_t count = (b1 >> 4) + 3u;
                        const uint32_t disp  = (static_cast<uint32_t>(b1 & 0x0Fu) << 8) | b2;
                        for (uint32_t k = 0; k < count && decompressed.size() < remaining; ++k) {
                            const std::size_t backIdx = decompressed.size() - (disp + 1u);
                            const uint8_t val = (backIdx < decompressed.size()) ? decompressed[backIdx] : 0;
                            decompressed.push_back(val);
                        }
                    } else {
                        decompressed.push_back(dataRead8(src++));
                    }
                }
            }
            if (comment == 0x12) {
                for (std::size_t i = 0; i + 1 < decompressed.size(); i += 2) {
                    const uint16_t hw = static_cast<uint16_t>(decompressed[i] | (decompressed[i + 1] << 8));
                    dataWrite16(dst + static_cast<uint32_t>(i), hw);
                }
            } else {
                for (std::size_t i = 0; i < decompressed.size(); ++i) {
                    dataWrite8(dst + static_cast<uint32_t>(i), decompressed[i]);
                }
            }
            break;
        }
        case 0x14:   // RLUnCompReadNormalWrite8bit
        case 0x15: { // RLUnCompReadByCallbackWrite16bit
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t header = dataRead32(src);
            src += 4;
            const uint32_t remaining = header >> 8;
            if (remaining == 0 || remaining > 0x200000u) break;

            std::vector<uint8_t> decompressed;
            decompressed.reserve(remaining);
            while (decompressed.size() < remaining) {
                const uint8_t flag = dataRead8(src++);
                if (flag & 0x80u) {
                    const uint32_t len = (flag & 0x7Fu) + 3u;
                    const uint8_t  val = dataRead8(src++);
                    for (uint32_t k = 0; k < len && decompressed.size() < remaining; ++k) {
                        decompressed.push_back(val);
                    }
                } else {
                    const uint32_t len = (flag & 0x7Fu) + 1u;
                    for (uint32_t k = 0; k < len && decompressed.size() < remaining; ++k) {
                        decompressed.push_back(dataRead8(src++));
                    }
                }
            }
            if (comment == 0x15) {
                for (std::size_t i = 0; i + 1 < decompressed.size(); i += 2) {
                    const uint16_t hw = static_cast<uint16_t>(decompressed[i] | (decompressed[i + 1] << 8));
                    dataWrite16(dst + static_cast<uint32_t>(i), hw);
                }
            } else {
                for (std::size_t i = 0; i < decompressed.size(); ++i) {
                    dataWrite8(dst + static_cast<uint32_t>(i), decompressed[i]);
                }
            }
            break;
        }
        case 0x16: { // Diff8bitUnFilterWrite8bit
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t len = dataRead32(src) >> 8;
            src += 4;
            uint8_t acc = 0;
            for (uint32_t i = 0; i < len; ++i) {
                acc = static_cast<uint8_t>(acc + dataRead8(src++));
                dataWrite8(dst++, acc);
            }
            break;
        }
        case 0x18: { // Diff16bitUnFilter
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t len = dataRead32(src) >> 8;
            src += 4;
            uint16_t acc = 0;
            for (uint32_t i = 0; i + 1 < len; i += 2) {
                acc = static_cast<uint16_t>(acc + dataRead16(src));
                src += 2;
                dataWrite16(dst, acc);
                dst += 2;
            }
            break;
        }
        case 0x1F: { // CustomPost
            m_bus.arm9Write32(0x04000300u, m_regs.r[0]);
            break;
        }
        default:
            break;
    }
}

// =============================================================================
// Decodificador ARM de 32 bits (ARMv5TE + DSP + CP15)
// =============================================================================
int32_t Arm9Cpu::executeArm(uint32_t opcode) {
    const uint8_t cond = static_cast<uint8_t>((opcode >> 28) & 0x0Fu);

    // Espaço Incondicional ARMv5TE (cond == 0xF): BLX <imm24>, PLD, Coprocessor2
    if (cond == 0x0Fu) {
        if ((opcode & 0xFE000000u) == 0xFA000000u) {
            // BLX <label>: Salto com link e troca imediata para THUMB
            const uint32_t hBit = (opcode >> 24) & 1u;
            int32_t offset = static_cast<int32_t>(opcode & 0x00FFFFFFu);
            if (offset & 0x00800000) offset |= ~0x00FFFFFF;
            offset = (offset << 2) | static_cast<int32_t>(hBit << 1);

            m_regs.r[14] = m_regs.r[15] - 4u;
            m_regs.setFlag(CpuRegisters::FLAG_T, true);
            m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + offset) & ~1u;
            m_pcModified = true;
            return 3;
        }
        // PLD (Preload Data Cache) e demais instruções 0xF... são dicas/no-op
        return 1;
    }

    if (!checkCondition(cond)) {
        return 1;
    }

    // 1. BX Rm (0x012FFF10) e ARMv5TE BLX Rm (0x012FFF30)
    if ((opcode & 0x0FFFFFD0u) == 0x012FFF10u) {
        const bool     link   = (opcode & (1u << 5)) != 0;
        const uint32_t target = m_regs.r[opcode & 0x0Fu];
        if (link) {
            m_regs.r[14] = m_regs.r[15] - 4u;
        }
        setPcInterwork(target);
        return 3;
    }

    // 2. ARMv5TE CLZ Rd, Rm (Count Leading Zeros): 0001 0110 1111 Rd 1111 0001 Rm
    if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) {
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        const uint32_t rm = opcode & 0x0Fu;
        m_regs.r[rd] = static_cast<uint32_t>(std::countl_zero(m_regs.r[rm]));
        return 1;
    }

    // 3. ARMv5TE DSP Saturating Add/Subtract (QADD / QSUB / QDADD / QDSUB)
    if ((opcode & 0x0F900FF0u) == 0x01000050u) {
        const uint8_t  op = static_cast<uint8_t>((opcode >> 21) & 3u);
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        const uint32_t rm = opcode & 0x0Fu;

        const int32_t rmVal = static_cast<int32_t>(m_regs.r[rm]);
        int32_t       rnVal = static_cast<int32_t>(m_regs.r[rn]);
        bool          sat   = false;

        if (op >= 2u) { // QDADD / QDSUB dobram Rn com saturação antes da soma/subtração
            rnVal = saturate32(2ll * static_cast<int64_t>(rnVal), sat);
        }
        const int64_t raw = (op & 1u)
            ? (static_cast<int64_t>(rmVal) - static_cast<int64_t>(rnVal))
            : (static_cast<int64_t>(rmVal) + static_cast<int64_t>(rnVal));
        const int32_t res = saturate32(raw, sat);
        if (sat) {
            m_regs.setFlag(FLAG_Q, true);
        }
        m_regs.r[rd] = static_cast<uint32_t>(res);
        return 1;
    }

    // 4. ARMv5TE DSP Signed 16-bit Multiplies (SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy)
    if ((opcode & 0x0F900090u) == 0x01000080u) {
        const uint8_t  subOp = static_cast<uint8_t>((opcode >> 21) & 3u);
        const uint32_t rd    = (opcode >> 16) & 0x0Fu;
        const uint32_t rn    = (opcode >> 12) & 0x0Fu;
        const uint32_t rs    = (opcode >> 8) & 0x0Fu;
        const uint32_t rm    = opcode & 0x0Fu;
        const bool     xTop  = (opcode & (1u << 5)) != 0;
        const bool     yTop  = (opcode & (1u << 6)) != 0;

        const int32_t x = xTop
            ? static_cast<int32_t>(static_cast<int16_t>(m_regs.r[rm] >> 16))
            : static_cast<int32_t>(static_cast<int16_t>(m_regs.r[rm] & 0xFFFFu));
        const int32_t y = yTop
            ? static_cast<int32_t>(static_cast<int16_t>(m_regs.r[rs] >> 16))
            : static_cast<int32_t>(static_cast<int16_t>(m_regs.r[rs] & 0xFFFFu));

        switch (subOp) {
            case 0: { // SMLAxy Rd, Rm, Rs, Rn
                const int64_t sum = static_cast<int64_t>(x * y) + static_cast<int64_t>(static_cast<int32_t>(m_regs.r[rn]));
                if (sum < INT32_MIN || sum > INT32_MAX) m_regs.setFlag(FLAG_Q, true);
                m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(sum));
                break;
            }
            case 1: { // SMLAWy (xTop=0) / SMULWy (xTop=1)
                const int64_t prod = (static_cast<int64_t>(static_cast<int32_t>(m_regs.r[rm])) * static_cast<int64_t>(y)) >> 16;
                if (!xTop) {
                    const int64_t sum = prod + static_cast<int64_t>(static_cast<int32_t>(m_regs.r[rn]));
                    if (sum < INT32_MIN || sum > INT32_MAX) m_regs.setFlag(FLAG_Q, true);
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(sum));
                } else {
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(prod));
                }
                break;
            }
            case 2: { // SMLALxy RdLo(rn), RdHi(rd), Rm, Rs
                const uint64_t accBits = (static_cast<uint64_t>(m_regs.r[rd]) << 32) | m_regs.r[rn];
                const int64_t  sum     = static_cast<int64_t>(accBits) + static_cast<int64_t>(x * y);
                m_regs.r[rn] = static_cast<uint32_t>(static_cast<uint64_t>(sum) & 0xFFFFFFFFull);
                m_regs.r[rd] = static_cast<uint32_t>(static_cast<uint64_t>(sum) >> 32);
                break;
            }
            case 3: { // SMULxy Rd, Rm, Rs
                m_regs.r[rd] = static_cast<uint32_t>(x * y);
                break;
            }
        }
        return 2;
    }

    // 5. Software Interrupt (SWI): 1111 xxxx ...
    if ((opcode & 0x0F000000u) == 0x0F000000u) {
        handleSwi(static_cast<uint8_t>((opcode >> 16) & 0xFFu));
        return 3;
    }

    // 6. Coprocessador MRC / MCR (CP15 System Control) & CDP / LDC / STC
    if ((opcode & 0x0F000010u) == 0x0E000010u) {
        const uint8_t  cpNum = static_cast<uint8_t>((opcode >> 8) & 0x0Fu);
        const bool     isMrc = (opcode & (1u << 20)) != 0;
        const uint8_t  crn   = static_cast<uint8_t>((opcode >> 16) & 0x0Fu);
        const uint32_t rd    = (opcode >> 12) & 0x0Fu;
        const uint8_t  op2   = static_cast<uint8_t>((opcode >> 5) & 7u);
        const uint8_t  crm   = static_cast<uint8_t>(opcode & 0x0Fu);

        if (cpNum == 15u) {
            if (isMrc) {
                const uint32_t val = readCp15(crn, crm, op2);
                if (rd == 15u) {
                    m_regs.cpsr = (m_regs.cpsr & 0x0FFFFFFFu) | (val & 0xF0000000u);
                } else {
                    m_regs.r[rd] = val;
                }
            } else {
                writeCp15(crn, crm, op2, m_regs.r[rd]);
            }
        }
        return 2;
    }
    if ((opcode & 0x0E000000u) == 0x0C000000u || (opcode & 0x0F000010u) == 0x0E000000u) {
        return 1; // LDC / STC / CDP no-op
    }

    // 7. Branch / Branch with Link (B / BL): 101L offset24
    if ((opcode & 0x0E000000u) == 0x0A000000u) {
        const bool link = (opcode & (1u << 24)) != 0;
        int32_t offset = static_cast<int32_t>(opcode & 0x00FFFFFFu);
        if (offset & 0x00800000) offset |= ~0x00FFFFFF;
        offset <<= 2;

        if (link) {
            m_regs.r[14] = m_regs.r[15] - 4u;
        }
        m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + offset) & ~3u;
        m_pcModified = true;
        return 3;
    }

    // 8. Block Data Transfer (LDM / STM): 100P USWL Rn reglist
    if ((opcode & 0x0E000000u) == 0x08000000u) {
        const bool preIndex  = (opcode & (1u << 24)) != 0;
        const bool up        = (opcode & (1u << 23)) != 0;
        const bool sBit      = (opcode & (1u << 22)) != 0;
        const bool writeBack = (opcode & (1u << 21)) != 0;
        const bool isLoad    = (opcode & (1u << 20)) != 0;
        const uint32_t rn    = (opcode >> 16) & 0x0Fu;
        const uint16_t list  = static_cast<uint16_t>(opcode & 0xFFFFu);

        const uint32_t count = static_cast<uint32_t>(std::popcount(list));
        const uint32_t base  = m_regs.r[rn];
        uint32_t addr = base;

        if (!up) {
            addr -= count * 4u;
            if (!preIndex) addr += 4u;
        } else if (preIndex) {
            addr += 4u;
        }

        const uint32_t finalBase = up ? (base + count * 4u) : (base - count * 4u);
        const bool userBankTransfer = sBit && !(isLoad && (list & (1u << 15)));
        const CpuMode savedMode = m_regs.getMode();
        if (userBankTransfer) {
            switchMode(CpuMode::User);
        }

        for (uint32_t reg = 0; reg < 16; ++reg) {
            if ((list & (1u << reg)) == 0) continue;
            if (isLoad) {
                const uint32_t val = dataRead32(addr);
                if (reg == 15) {
                    if (sBit) {
                        const uint32_t savedSpsr = m_regs.spsr;
                        switchMode(static_cast<CpuMode>(savedSpsr & 0x1Fu));
                        m_regs.cpsr = savedSpsr;
                        m_regs.r[15] = val & (m_regs.isThumb() ? ~1u : ~3u);
                        m_pcModified = true;
                    } else {
                        // ARMv5TE: LDM {.., PC} suporta interworking ARM <-> THUMB
                        setPcInterwork(val);
                    }
                } else {
                    m_regs.r[reg] = val;
                }
            } else {
                uint32_t val = m_regs.r[reg];
                if (reg == 15) val += 4u;
                dataWrite32(addr, val);
            }
            addr += 4u;
        }

        if (userBankTransfer) {
            switchMode(savedMode);
        } else if (writeBack && !(isLoad && (list & (1u << rn)))) {
            m_regs.r[rn] = finalBase;
        }
        return static_cast<int32_t>(count + 2u);
    }

    // 9. Single Data Transfer (LDR / STR / LDRB / STRB): 01IP UBWL Rn Rd offset12
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

        const uint32_t base       = m_regs.r[rn];
        const uint32_t offsetAddr = up ? (base + offset) : (base - offset);
        const uint32_t addr       = preIndex ? offsetAddr : base;

        if (isLoad) {
            if (isByte) {
                m_regs.r[rd] = dataRead8(addr);
            } else {
                uint32_t val = dataRead32(addr & ~3u);
                if (rd == 15) {
                    // ARMv5TE: LDR PC faz interworking usando bit 0 do valor lido!
                    setPcInterwork(val);
                } else {
                    const uint32_t rot = (addr & 3u) * 8u;
                    if (rot != 0) val = std::rotr(val, rot);
                    m_regs.r[rd] = val;
                }
            }
        } else {
            uint32_t val = m_regs.r[rd];
            if (rd == 15) val += 4u;
            if (isByte) {
                dataWrite8(addr, static_cast<uint8_t>(val & 0xFFu));
            } else {
                dataWrite32(addr & ~3u, val);
            }
        }

        if ((!preIndex || writeBack) && !(isLoad && rd == rn)) {
            m_regs.r[rn] = offsetAddr;
        }
        return isLoad ? 3 : 2;
    }

    // 10. Multiply & Multiply Long
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
        return 2;
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
        return 3;
    }

    // 11. Single Data Swap (SWP / SWPB)
    if ((opcode & 0x0FB00FF0u) == 0x01000090u) {
        const bool isByte = (opcode & (1u << 22)) != 0;
        const uint32_t rn = (opcode >> 16) & 0x0Fu;
        const uint32_t rd = (opcode >> 12) & 0x0Fu;
        const uint32_t rm = opcode & 0x0Fu;
        const uint32_t addr = m_regs.r[rn];
        if (isByte) {
            const uint8_t oldVal = dataRead8(addr);
            dataWrite8(addr, static_cast<uint8_t>(m_regs.r[rm] & 0xFFu));
            m_regs.r[rd] = oldVal;
        } else {
            const uint32_t oldVal = dataRead32(addr & ~3u);
            dataWrite32(addr & ~3u, m_regs.r[rm]);
            m_regs.r[rd] = oldVal;
        }
        return 4;
    }

    // 12. Halfword / Signed / Doubleword Data Transfer (LDRH/STRH/LDRSB/LDRSH + ARMv5TE LDRD/STRD)
    if ((opcode & 0x0E000090u) == 0x00000090u && ((opcode >> 5) & 3u) != 0) {
        const bool preIndex  = (opcode & (1u << 24)) != 0;
        const bool up        = (opcode & (1u << 23)) != 0;
        const bool immOffset = (opcode & (1u << 22)) != 0;
        const bool writeBack = (opcode & (1u << 21)) != 0;
        const bool isLoadBit = (opcode & (1u << 20)) != 0;
        const uint32_t rn    = (opcode >> 16) & 0x0Fu;
        const uint32_t rd    = (opcode >> 12) & 0x0Fu;
        const uint8_t  sh    = static_cast<uint8_t>((opcode >> 5) & 3u);

        const uint32_t offset = immOffset
            ? (((opcode >> 4) & 0xF0u) | (opcode & 0x0Fu))
            : m_regs.r[opcode & 0x0Fu];

        const uint32_t base       = m_regs.r[rn];
        const uint32_t offsetAddr = up ? (base + offset) : (base - offset);
        const uint32_t addr       = preIndex ? offsetAddr : base;
        const bool     isLdrd     = (!isLoadBit && sh == 2u);
        const bool     anyLoad    = isLoadBit || isLdrd;

        if (isLoadBit) {
            switch (sh) {
                case 1:
                    m_regs.r[rd] = dataRead16(addr & ~1u);
                    break;
                case 2:
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dataRead8(addr))));
                    break;
                case 3:
                    m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dataRead16(addr & ~1u))));
                    break;
            }
        } else {
            switch (sh) {
                case 1: // STRH
                    dataWrite16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu));
                    break;
                case 2: { // ARMv5TE LDRD Rd, Rd+1, [addr]
                    const uint32_t rd2 = (rd + 1u) & 0x0Fu;
                    const uint32_t v0  = dataRead32(addr & ~3u);
                    const uint32_t v1  = dataRead32((addr & ~3u) + 4u);
                    m_regs.r[rd]  = v0;
                    m_regs.r[rd2] = v1;
                    break;
                }
                case 3: { // ARMv5TE STRD Rd, Rd+1, [addr]
                    const uint32_t rd2 = (rd + 1u) & 0x0Fu;
                    dataWrite32(addr & ~3u, m_regs.r[rd]);
                    dataWrite32((addr & ~3u) + 4u, m_regs.r[rd2]);
                    break;
                }
            }
        }

        const bool overwroteRn = (isLoadBit && rd == rn) || (isLdrd && (rd == rn || ((rd + 1u) & 0x0Fu) == rn));
        if ((!preIndex || writeBack) && !overwroteRn) {
            m_regs.r[rn] = offsetAddr;
        }
        return anyLoad ? 3 : 2;
    }

    // 13. MRS / MSR (inclui bit 27 Sticky Q Flag no ARMv5TE)
    if ((opcode & 0x0FBF0FFFu) == 0x010F0000u) {
        const bool useSpsr = (opcode & (1u << 22)) != 0;
        const uint32_t rd  = (opcode >> 12) & 0x0Fu;
        m_regs.r[rd] = useSpsr ? m_regs.spsr : m_regs.cpsr;
        return 1;
    }
    if ((opcode & 0x0DB0F000u) == 0x0120F000u) {
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
        if (opcode & (1u << 19)) mask |= 0xF8000000u; // N, Z, C, V, Q
        if (opcode & (1u << 16)) mask |= 0x000000FFu; // I, F, T, Mode

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

    // 14. Data Processing (ALU)
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
            const bool shiftByReg   = (opcode & (1u << 4)) != 0;
            uint32_t rmVal = m_regs.r[rm];
            if (shiftByReg && rm == 15) rmVal += 4u;
            if (shiftByReg && rn == 15) op1   += 4u;
            const uint8_t shiftAmt = shiftByReg
                ? static_cast<uint8_t>(m_regs.r[(opcode >> 8) & 0x0Fu] & 0xFFu)
                : static_cast<uint8_t>((opcode >> 7) & 0x1Fu);
            op2 = barrelShift(rmVal, shiftType, shiftAmt, shifterCarry, !shiftByReg);
        }

        const bool updateAluFlags = setFlags && (rd != 15);
        uint32_t res = 0;
        bool writeResult = true;

        switch (aluOp) {
            case 0x0: res = op1 & op2;  if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
            case 0x1: res = op1 ^ op2;  if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
            case 0x2: res = subWithFlags(op1, op2, updateAluFlags); break;
            case 0x3: res = subWithFlags(op2, op1, updateAluFlags); break;
            case 0x4: res = addWithFlags(op1, op2, updateAluFlags); break;
            case 0x5: res = adcWithFlags(op1, op2, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break;
            case 0x6: res = sbcWithFlags(op1, op2, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break;
            case 0x7: res = sbcWithFlags(op2, op1, m_regs.getFlag(CpuRegisters::FLAG_C), updateAluFlags); break;
            case 0x8: res = op1 & op2;  setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); writeResult = false; break;
            case 0x9: res = op1 ^ op2;  setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); writeResult = false; break;
            case 0xA: subWithFlags(op1, op2, true); writeResult = false; break;
            case 0xB: addWithFlags(op1, op2, true); writeResult = false; break;
            case 0xC: res = op1 | op2;  if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
            case 0xD: res = op2;        if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
            case 0xE: res = op1 & ~op2; if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
            case 0xF: res = ~op2;       if (updateAluFlags) { setNzFlags(res); m_regs.setFlag(CpuRegisters::FLAG_C, shifterCarry); } break;
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

// =============================================================================
// Decodificador THUMB de 16 bits (ARMv5TE: inclui BLX Rs, BLX <imm>, POP {PC} interwork)
// =============================================================================
int32_t Arm9Cpu::executeThumb(uint16_t opcode) {
    // Formato 2: Add / Subtract
    if ((opcode >> 11) == 0b00011) {
        const bool isImm     = (opcode & (1u << 10)) != 0;
        const bool isSub     = (opcode & (1u << 9)) != 0;
        const uint32_t rnImm = (opcode >> 6) & 7u;
        const uint32_t rs    = (opcode >> 3) & 7u;
        const uint32_t rd    = opcode & 7u;
        const uint32_t op2   = isImm ? rnImm : m_regs.r[rnImm];
        m_regs.r[rd] = isSub ? subWithFlags(m_regs.r[rs], op2, true) : addWithFlags(m_regs.r[rs], op2, true);
        return 1;
    }

    // Formato 1: Move shifted register
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

    // Formato 3: Move / Compare / Add / Subtract Immediate
    if ((opcode >> 13) == 0b001) {
        const uint8_t  op  = (opcode >> 11) & 3u;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t imm = opcode & 0xFFu;
        switch (op) {
            case 0: m_regs.r[rd] = imm; setNzFlags(imm); break;
            case 1: subWithFlags(m_regs.r[rd], imm, true); break;
            case 2: m_regs.r[rd] = addWithFlags(m_regs.r[rd], imm, true); break;
            case 3: m_regs.r[rd] = subWithFlags(m_regs.r[rd], imm, true); break;
        }
        return 1;
    }

    // Formato 4: ALU Operations
    if ((opcode >> 10) == 0b010000) {
        const uint8_t  op = (opcode >> 6) & 0x0Fu;
        const uint32_t rs = (opcode >> 3) & 7u;
        const uint32_t rd = opcode & 7u;
        bool carry = m_regs.getFlag(CpuRegisters::FLAG_C);
        switch (op) {
            case 0x0: m_regs.r[rd] &= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
            case 0x1: m_regs.r[rd] ^= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
            case 0x2: m_regs.r[rd] = barrelShift(m_regs.r[rd], 0, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break;
            case 0x3: m_regs.r[rd] = barrelShift(m_regs.r[rd], 1, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break;
            case 0x4: m_regs.r[rd] = barrelShift(m_regs.r[rd], 2, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break;
            case 0x5: m_regs.r[rd] = adcWithFlags(m_regs.r[rd], m_regs.r[rs], carry, true); break;
            case 0x6: m_regs.r[rd] = sbcWithFlags(m_regs.r[rd], m_regs.r[rs], carry, true); break;
            case 0x7: m_regs.r[rd] = barrelShift(m_regs.r[rd], 3, static_cast<uint8_t>(m_regs.r[rs] & 0xFF), carry, false); setNzFlags(m_regs.r[rd]); m_regs.setFlag(CpuRegisters::FLAG_C, carry); break;
            case 0x8: setNzFlags(m_regs.r[rd] & m_regs.r[rs]); break;
            case 0x9: m_regs.r[rd] = subWithFlags(0, m_regs.r[rs], true); break;
            case 0xA: subWithFlags(m_regs.r[rd], m_regs.r[rs], true); break;
            case 0xB: addWithFlags(m_regs.r[rd], m_regs.r[rs], true); break;
            case 0xC: m_regs.r[rd] |= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
            case 0xD: m_regs.r[rd] *= m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
            case 0xE: m_regs.r[rd] &= ~m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
            case 0xF: m_regs.r[rd] = ~m_regs.r[rs]; setNzFlags(m_regs.r[rd]); break;
        }
        return 1;
    }

    // Formato 5: Hi Register Operations / BX / BLX Rs (ARMv5TE)
    if ((opcode >> 10) == 0b010001) {
        const uint8_t  op     = (opcode >> 8) & 3u;
        const uint32_t rd     = (opcode & 7u) | ((opcode >> 4) & 8u);
        const uint32_t rs     = ((opcode >> 3) & 7u) | ((opcode >> 3) & 8u);
        const uint32_t srcVal = m_regs.r[rs];

        switch (op) {
            case 0:
                m_regs.r[rd] += srcVal;
                if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; }
                break;
            case 1:
                subWithFlags(m_regs.r[rd], srcVal, true);
                break;
            case 2:
                m_regs.r[rd] = srcVal;
                if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; }
                break;
            case 3: // BX Rs (H1=0) ou BLX Rs (H1=1 no ARMv5TE)
                if ((opcode & (1u << 7)) != 0) {
                    m_regs.r[14] = (m_regs.r[15] - 2u) | 1u;
                }
                setPcInterwork(srcVal);
                break;
        }
        return 1;
    }

    // Formato 6: PC-relative Load
    if ((opcode >> 11) == 0b01001) {
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t imm  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[15] & ~3u) + imm;
        m_regs.r[rd] = dataRead32(addr);
        return 3;
    }

    // Formato 7 & 8: Load/Store Register Offset
    if ((opcode >> 12) == 0b0101) {
        const uint32_t ro   = (opcode >> 6) & 7u;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + m_regs.r[ro];

        if ((opcode & (1u << 9)) == 0) {
            switch ((opcode >> 10) & 3u) {
                case 0: dataWrite32(addr & ~3u, m_regs.r[rd]); break;
                case 1: dataWrite8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFFu)); break;
                case 2: m_regs.r[rd] = std::rotr(dataRead32(addr & ~3u), (addr & 3u) * 8u); break;
                case 3: m_regs.r[rd] = dataRead8(addr); break;
            }
        } else {
            switch ((opcode >> 10) & 3u) {
                case 0: dataWrite16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu)); break;
                case 1: m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dataRead8(addr)))); break;
                case 2: m_regs.r[rd] = dataRead16(addr & ~1u); break;
                case 3: m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dataRead16(addr & ~1u)))); break;
            }
        }
        return 2;
    }

    // Formato 9: Load/Store Immediate Offset
    if ((opcode >> 13) == 0b011) {
        const bool isByte   = (opcode & (1u << 12)) != 0;
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t off  = (opcode >> 6) & 0x1Fu;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + (isByte ? off : (off * 4u));

        if (isLoad) {
            m_regs.r[rd] = isByte ? dataRead8(addr) : std::rotr(dataRead32(addr & ~3u), (addr & 3u) * 8u);
        } else {
            if (isByte) dataWrite8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFFu));
            else        dataWrite32(addr & ~3u, m_regs.r[rd]);
        }
        return 2;
    }

    // Formato 10: Load/Store Halfword
    if ((opcode >> 12) == 0b1000) {
        const bool isLoad      = (opcode & (1u << 11)) != 0;
        const uint32_t off     = ((opcode >> 6) & 0x1Fu) * 2u;
        const uint32_t rb      = (opcode >> 3) & 7u;
        const uint32_t rd      = opcode & 7u;
        const uint32_t rawAddr = m_regs.r[rb] + off;
        if (isLoad) m_regs.r[rd] = dataRead16(rawAddr & ~1u);
        else        dataWrite16(rawAddr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu));
        return 2;
    }

    // Formato 11: SP-relative Load/Store
    if ((opcode >> 12) == 0b1001) {
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t off  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[13] + off) & ~3u;
        if (isLoad) m_regs.r[rd] = dataRead32(addr);
        else        dataWrite32(addr, m_regs.r[rd]);
        return 2;
    }

    // Formato 12: Load Address
    if ((opcode >> 12) == 0b1010) {
        const bool useSp   = (opcode & (1u << 11)) != 0;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t off = (opcode & 0xFFu) * 4u;
        m_regs.r[rd] = (useSp ? m_regs.r[13] : (m_regs.r[15] & ~3u)) + off;
        return 1;
    }

    // Formato 13: Add Offset to Stack Pointer
    if ((opcode >> 8) == 0b10110000) {
        const uint32_t off = (opcode & 0x7Fu) * 4u;
        if (opcode & 0x80u) m_regs.r[13] -= off;
        else                m_regs.r[13] += off;
        return 1;
    }

    // Formato 14: Push / Pop Registers (ARMv5TE: POP {PC} faz interworking!)
    if ((opcode & 0xF600u) == 0xB400u) {
        const bool isPop   = (opcode & (1u << 11)) != 0;
        const bool pclr    = (opcode & (1u << 8)) != 0;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);

        if (isPop) {
            for (uint32_t i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    m_regs.r[i] = dataRead32(m_regs.r[13]);
                    m_regs.r[13] += 4u;
                }
            }
            if (pclr) {
                const uint32_t target = dataRead32(m_regs.r[13]);
                m_regs.r[13] += 4u;
                setPcInterwork(target);
            }
        } else {
            if (pclr) {
                m_regs.r[13] -= 4u;
                dataWrite32(m_regs.r[13], m_regs.r[14]);
            }
            for (int i = 7; i >= 0; --i) {
                if (list & (1u << i)) {
                    m_regs.r[13] -= 4u;
                    dataWrite32(m_regs.r[13], m_regs.r[i]);
                }
            }
        }
        return 3;
    }

    // Formato 15: Multiple Load/Store
    if ((opcode >> 12) == 0b1100) {
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t rb  = (opcode >> 8) & 7u;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);
        uint32_t addr = m_regs.r[rb];
        for (uint32_t i = 0; i < 8; ++i) {
            if (list & (1u << i)) {
                if (isLoad) m_regs.r[i] = dataRead32(addr);
                else        dataWrite32(addr, m_regs.r[i]);
                addr += 4u;
            }
        }
        if (!(isLoad && (list & (1u << rb)))) {
            m_regs.r[rb] = addr;
        }
        return 3;
    }

    // Formato 16 & 17: Conditional Branch / SWI
    if ((opcode >> 12) == 0b1101) {
        const uint8_t cond = (opcode >> 8) & 0x0Fu;
        if (cond == 0x0Fu) {
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

    // Formato 19 (Sufixo BLX <label> ARMv5TE): 11101 Offset11 (troca THUMB -> ARM!)
    if ((opcode >> 11) == 0b11101) {
        const uint32_t off       = opcode & 0x7FFu;
        const uint32_t nextInstr = (m_regs.r[15] - 2u) | 1u;
        m_regs.setFlag(CpuRegisters::FLAG_T, false);
        m_regs.r[15] = (m_regs.r[14] + (off << 1)) & ~3u;
        m_regs.r[14] = nextInstr;
        m_pcModified = true;
        return 2;
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

void Arm9Cpu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_regs);
    out.writePod(m_halted);
    out.writePod(m_pcModified);
    out.writePod(m_intrWaitActive);
    out.writePod(m_intrWaitMask);
    out.writeBytes(m_itcm);
    out.writeBytes(m_dtcm);
    out.writePod(m_cp15Ctrl);
    out.writePod(m_cp15Dtcm);
    out.writePod(m_cp15Itcm);
    out.writePod(m_cp15Misc);
    out.writePod(m_cp15PuRegions);
}

bool Arm9Cpu::deserializeState(SaveStateReader& in) {
    return in.readPod(m_regs) &&
           in.readPod(m_halted) &&
           in.readPod(m_pcModified) &&
           in.readPod(m_intrWaitActive) &&
           in.readPod(m_intrWaitMask) &&
           in.readBytes(m_itcm) &&
           in.readBytes(m_dtcm) &&
           in.readPod(m_cp15Ctrl) &&
           in.readPod(m_cp15Dtcm) &&
           in.readPod(m_cp15Itcm) &&
           in.readPod(m_cp15Misc) &&
           in.readPod(m_cp15PuRegions);
}

} // namespace gba::core::nds

