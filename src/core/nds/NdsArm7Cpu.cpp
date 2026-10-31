#include "core/nds/NdsArm7Cpu.hpp"
#include "core/nds/NdsBus.hpp"
#include <bit>
#include <climits>
#include <cmath>
#include <vector>

namespace gba::core::nds {

namespace {

uint16_t crc16ReflectedA001(uint16_t crc, uint8_t byte) noexcept {
    crc ^= byte;
    for (int b = 0; b < 8; ++b) {
        if (crc & 1u) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001u);
        else          crc = static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

} // namespace

NdsArm7Cpu::NdsArm7Cpu(NdsBus& bus)
    : m_bus(bus) {
    reset();
}

void NdsArm7Cpu::reset() {
    m_regs           = CpuRegisters{};
    m_halted         = false;
    m_pcModified     = false;
    m_intrWaitActive = false;
    m_intrWaitMask   = 0;

    m_regs.cpsr  = static_cast<uint32_t>(CpuMode::Supervisor) | CpuRegisters::FLAG_I | CpuRegisters::FLAG_F;
    m_regs.r[15] = 0x00000000u;
}

void NdsArm7Cpu::initDirectBoot(uint32_t entryAddr) {
    m_regs           = CpuRegisters{};
    m_halted         = false;
    m_pcModified     = false;
    m_intrWaitActive = false;
    m_intrWaitMask   = 0;

    // Pilhas pós-firmware configuradas pela BIOS ARM7 dentro da ARM7 WRAM (0x03800000..0x0380FFFF)
    m_regs.r[13]         = 0x0380FD80u; // USR/SYS SP
    m_regs.r8_14_usr[5]  = 0x0380FD80u;
    m_regs.r13_14_irq[0] = 0x0380FF80u; // IRQ SP
    m_regs.r13_14_svc[0] = 0x0380FFC0u; // SVC SP

    m_regs.r[12]        = entryAddr;
    m_regs.r[14]        = entryAddr;
    m_regs.r8_14_usr[6] = entryAddr;

    m_regs.cpsr = 0x1Fu; // System Mode (0x1F)
    const bool isThumb = (entryAddr & 1u) != 0;
    m_regs.setFlag(CpuRegisters::FLAG_T, isThumb);
    m_regs.r[15] = entryAddr & (isThumb ? ~1u : ~3u);
    m_pcModified = false;
}

void NdsArm7Cpu::switchMode(CpuMode newMode) noexcept {
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

void NdsArm7Cpu::pollIntrWait() {
    if (!m_intrWaitActive) return;
    if (m_regs.getMode() == CpuMode::Irq) return;

    constexpr uint32_t FLAGS_ADDR = 0x0380FFF8u;
    const uint32_t swFlags = m_bus.arm7Read32(FLAGS_ADDR);
    if ((swFlags & m_intrWaitMask) != 0) {
        m_bus.arm7Write32(FLAGS_ADDR, swFlags & ~m_intrWaitMask);
        m_intrWaitActive = false;
        m_halted         = false;
        return;
    }

    if (!m_bus.anyEnabledIrqFlag7()) {
        m_halted = true;
    }
}

void NdsArm7Cpu::checkInterrupts() {
    const uint32_t pending = m_bus.ie7() & m_bus.if7();
    if (pending != 0) {
        m_halted = false;
    }

    if ((m_bus.ime7() & 1u) != 0 && pending != 0 && !m_regs.isIrqDisabled()) {
        if (m_intrWaitActive && (pending & m_intrWaitMask) != 0) {
            constexpr uint32_t FLAGS_ADDR = 0x0380FFF8u;
            m_bus.arm7Write32(FLAGS_ADDR, m_bus.arm7Read32(FLAGS_ADDR) | (pending & m_intrWaitMask));
            m_intrWaitActive = false;
        }

        const uint32_t savedCpsr = m_regs.cpsr;
        const uint32_t nextPc    = m_regs.r[15] + 4u;
        switchMode(CpuMode::Irq);
        m_regs.spsr  = savedCpsr;
        m_regs.r[14] = nextPc;
        m_regs.cpsr &= ~CpuRegisters::FLAG_T;
        m_regs.cpsr |= CpuRegisters::FLAG_I;
        m_regs.r[15] = 0x00000018u;
    }
}

int32_t NdsArm7Cpu::step() {
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
        const uint16_t opcode    = m_bus.arm7Read16(instrAddr);
        m_regs.r[15] = instrAddr + 4u;
        cycles = executeThumb(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 2u;
        }
    } else {
        const uint32_t instrAddr = m_regs.r[15] & ~3u;
        const uint32_t opcode    = m_bus.arm7Read32(instrAddr);
        m_regs.r[15] = instrAddr + 8u;
        cycles = executeArm(opcode);
        if (!m_pcModified) {
            m_regs.r[15] = instrAddr + 4u;
        }
    }
    return cycles;
}

bool NdsArm7Cpu::checkCondition(uint8_t cond) const noexcept {
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
        default:  return false;
    }
}

uint32_t NdsArm7Cpu::barrelShift(uint32_t val, uint8_t shiftType, uint8_t amount, bool& carryOut, bool immediateShift) const noexcept {
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

void NdsArm7Cpu::setNzFlags(uint32_t result) noexcept {
    m_regs.setFlag(CpuRegisters::FLAG_N, (result & 0x80000000u) != 0);
    m_regs.setFlag(CpuRegisters::FLAG_Z, result == 0);
}

uint32_t NdsArm7Cpu::addWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
    const uint32_t res = static_cast<uint32_t>(sum);
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, sum > 0xFFFFFFFFull);
        m_regs.setFlag(CpuRegisters::FLAG_V, ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t NdsArm7Cpu::adcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
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

uint32_t NdsArm7Cpu::subWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept {
    const uint32_t res = a - b;
    if (updateFlags) {
        setNzFlags(res);
        m_regs.setFlag(CpuRegisters::FLAG_C, a >= b);
        m_regs.setFlag(CpuRegisters::FLAG_V, (((a ^ b) & (a ^ res)) & 0x80000000u) != 0);
    }
    return res;
}

uint32_t NdsArm7Cpu::sbcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept {
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
// Rotinas SWI da BIOS ARM7 do Nintendo DS (HLE + Suporte a BIOS7 Externa)
// =============================================================================
void NdsArm7Cpu::handleSwi(uint8_t comment) {
    if (m_bus.hasCustomBios7()) {
        const uint32_t savedCpsr = m_regs.cpsr;
        const uint32_t retAddr   = m_regs.isThumb() ? (m_regs.r[15] - 2u) : (m_regs.r[15] - 4u);
        switchMode(CpuMode::Supervisor);
        m_regs.spsr  = savedCpsr;
        m_regs.r[14] = retAddr;
        m_regs.cpsr &= ~CpuRegisters::FLAG_T;
        m_regs.cpsr |= CpuRegisters::FLAG_I;
        m_regs.r[15] = 0x00000008u;
        m_pcModified = true;
        return;
    }

    switch (comment) {
        case 0x03: // WaitByLoop
            m_regs.r[0] = 0;
            break;
        case 0x04:   // IntrWait
        case 0x05: { // VBlankIntrWait
            const bool     discard = (comment == 0x05) ? true : (m_regs.r[0] != 0);
            const uint32_t mask    = (comment == 0x05) ? 1u   : m_regs.r[1];
            constexpr uint32_t FLAGS_ADDR = 0x0380FFF8u;

            if (discard) {
                m_bus.arm7Write32(FLAGS_ADDR, m_bus.arm7Read32(FLAGS_ADDR) & ~mask);
            }
            m_bus.arm7Write32(0x04000208u, 1u);
            m_regs.cpsr &= ~CpuRegisters::FLAG_I;

            const uint32_t curFlags = m_bus.arm7Read32(FLAGS_ADDR);
            if (!discard && (curFlags & mask) != 0) {
                m_bus.arm7Write32(FLAGS_ADDR, curFlags & ~mask);
            } else {
                m_intrWaitActive = true;
                m_intrWaitMask   = mask;
                m_halted         = true;
            }
            break;
        }
        case 0x06: // Halt
        case 0x07: // Sleep
            m_halted = true;
            break;
        case 0x08: // SoundBias (R0 != 0 -> 0x0200, R0 == 0 -> 0x0000)
            m_bus.arm7Write16(0x04000504u, (m_regs.r[0] != 0) ? 0x0200u : 0x0000u);
            break;
        case 0x09: { // Div
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
                    m_bus.arm7Write32(dst, m_bus.arm7Read32(src));
                    dst += 4;
                    if (!fixedSrc) src += 4;
                } else {
                    m_bus.arm7Write16(dst, m_bus.arm7Read16(src));
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
            const uint32_t fillVal  = fixedSrc ? m_bus.arm7Read32(src) : 0u;
            for (uint32_t i = 0; i < count; ++i) {
                m_bus.arm7Write32(dst, fixedSrc ? fillVal : m_bus.arm7Read32(src));
                dst += 4;
                if (!fixedSrc) src += 4;
            }
            break;
        }
        case 0x0D: { // Sqrt
            m_regs.r[0] = static_cast<uint32_t>(std::sqrt(static_cast<double>(m_regs.r[0])));
            break;
        }
        case 0x0E: { // GetCRC16
            uint16_t crc = static_cast<uint16_t>(m_regs.r[0] & 0xFFFFu);
            const uint32_t addr = m_regs.r[1];
            const uint32_t len  = m_regs.r[2];
            for (uint32_t i = 0; i < len; ++i) {
                crc = crc16ReflectedA001(crc, m_bus.arm7Read8(addr + i));
            }
            m_regs.r[0] = crc;
            break;
        }
        case 0x0F: // IsDebugger
            m_regs.r[0] = 0;
            break;
        case 0x10: { // BitUnPack
            uint32_t src  = m_regs.r[0];
            uint32_t dst  = m_regs.r[1];
            uint32_t info = m_regs.r[2];
            const uint16_t srcLen     = m_bus.arm7Read16(info + 0);
            const uint8_t  srcBits    = m_bus.arm7Read8(info + 2);
            const uint8_t  dstBits    = m_bus.arm7Read8(info + 3);
            const uint32_t dataOffset = m_bus.arm7Read32(info + 4);
            const bool     zeroOffset = (dataOffset & (1u << 31)) != 0;
            const uint32_t addOffset  = dataOffset & 0x7FFFFFFFu;

            if (srcBits > 0 && dstBits > 0) {
                const uint32_t mask = (1u << srcBits) - 1u;
                uint32_t outWord   = 0;
                uint32_t outBitPos = 0;
                for (uint16_t i = 0; i < srcLen; ++i) {
                    const uint8_t byte = m_bus.arm7Read8(src++);
                    for (uint8_t b = 0; b < 8; b += srcBits) {
                        uint32_t unit = (byte >> b) & mask;
                        if (unit != 0 || zeroOffset) unit += addOffset;
                        outWord |= (unit << outBitPos);
                        outBitPos += dstBits;
                        if (outBitPos >= 32) {
                            m_bus.arm7Write32(dst, outWord);
                            dst += 4;
                            outWord   = 0;
                            outBitPos = 0;
                        }
                    }
                }
            }
            break;
        }
        case 0x11: { // LZ77UnCompWram
            uint32_t src = m_regs.r[0];
            uint32_t dst = m_regs.r[1];
            const uint32_t header = m_bus.arm7Read32(src);
            src += 4;
            const uint32_t remaining = header >> 8;
            if (remaining == 0 || remaining > 0x100000u) break;

            std::vector<uint8_t> decompressed;
            decompressed.reserve(remaining);
            while (decompressed.size() < remaining) {
                const uint8_t flags = m_bus.arm7Read8(src++);
                for (int bit = 7; bit >= 0 && decompressed.size() < remaining; --bit) {
                    if ((flags >> bit) & 1u) {
                        const uint8_t b1 = m_bus.arm7Read8(src++);
                        const uint8_t b2 = m_bus.arm7Read8(src++);
                        const uint32_t count = (b1 >> 4) + 3u;
                        const uint32_t disp  = (static_cast<uint32_t>(b1 & 0x0Fu) << 8) | b2;
                        for (uint32_t k = 0; k < count && decompressed.size() < remaining; ++k) {
                            const std::size_t backIdx = decompressed.size() - (disp + 1u);
                            const uint8_t val = (backIdx < decompressed.size()) ? decompressed[backIdx] : 0;
                            decompressed.push_back(val);
                        }
                    } else {
                        decompressed.push_back(m_bus.arm7Read8(src++));
                    }
                }
            }
            for (std::size_t i = 0; i < decompressed.size(); ++i) {
                m_bus.arm7Write8(dst + static_cast<uint32_t>(i), decompressed[i]);
            }
            break;
        }
        case 0x1A: { // GetSineTable (0..63 -> 16-bit sine)
            const uint32_t idx = m_regs.r[0] & 63u;
            const double   rad = (static_cast<double>(idx) * 3.14159265358979323846) / 128.0;
            m_regs.r[0] = static_cast<uint32_t>(std::sin(rad) * 32767.0) & 0xFFFFu;
            break;
        }
        case 0x1B: { // GetPitchTable (0..767 -> semitone pitch adjustment)
            const uint32_t idx = m_regs.r[0] % 768u;
            const double   f   = std::pow(2.0, static_cast<double>(idx) / 768.0) - 1.0;
            m_regs.r[0] = static_cast<uint32_t>(f * 65536.0) & 0xFFFFu;
            break;
        }
        case 0x1C: { // GetVolumeTable (0..723 -> dB attenuation 0..127)
            const uint32_t idx = m_regs.r[0];
            if (idx >= 723u) m_regs.r[0] = 0;
            else             m_regs.r[0] = static_cast<uint32_t>(127.0 * std::pow(10.0, -static_cast<double>(723u - idx) / 384.0));
            break;
        }
        default:
            break;
    }
}

int32_t NdsArm7Cpu::executeArm(uint32_t opcode) {
    const uint8_t cond = static_cast<uint8_t>((opcode >> 28) & 0x0Fu);
    if (!checkCondition(cond)) {
        return 1;
    }

    // 1. BX Rn
    if ((opcode & 0x0FFFFFF0u) == 0x012FFF10u) {
        const uint32_t dst = m_regs.r[opcode & 0x0Fu];
        m_regs.setFlag(CpuRegisters::FLAG_T, (dst & 1u) != 0);
        m_regs.r[15] = dst & (m_regs.isThumb() ? ~1u : ~3u);
        m_pcModified = true;
        return 3;
    }

    // 2. SWI
    if ((opcode & 0x0F000000u) == 0x0F000000u) {
        handleSwi(static_cast<uint8_t>((opcode >> 16) & 0xFFu));
        return 3;
    }

    // 3. B / BL
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

    // 4. LDM / STM (com suporte a transferência User-bank ^)
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
                const uint32_t val = m_bus.arm7Read32(addr);
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
                m_bus.arm7Write32(addr, val);
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

    // 5. LDR / STR / LDRB / STRB
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
                m_regs.r[rd] = m_bus.arm7Read8(addr);
            } else {
                uint32_t val = m_bus.arm7Read32(addr & ~3u);
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
                m_bus.arm7Write8(addr, static_cast<uint8_t>(val & 0xFFu));
            } else {
                m_bus.arm7Write32(addr & ~3u, val);
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

    // 7. SWP / SWPB
    if ((opcode & 0x0FB00FF0u) == 0x01000090u) {
        const bool isByte   = (opcode & (1u << 22)) != 0;
        const uint32_t rn   = (opcode >> 16) & 0x0Fu;
        const uint32_t rd   = (opcode >> 12) & 0x0Fu;
        const uint32_t rm   = opcode & 0x0Fu;
        const uint32_t addr = m_regs.r[rn];
        if (isByte) {
            const uint8_t oldVal = m_bus.arm7Read8(addr);
            m_bus.arm7Write8(addr, static_cast<uint8_t>(m_regs.r[rm] & 0xFFu));
            m_regs.r[rd] = oldVal;
        } else {
            const uint32_t oldVal = m_bus.arm7Read32(addr & ~3u);
            m_bus.arm7Write32(addr & ~3u, m_regs.r[rm]);
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

        const uint32_t base       = m_regs.r[rn];
        const uint32_t offsetAddr = up ? (base + offset) : (base - offset);
        const uint32_t addr       = preIndex ? offsetAddr : base;

        if (isLoad) {
            switch (sh) {
                case 1: m_regs.r[rd] = std::rotr(static_cast<uint32_t>(m_bus.arm7Read16(addr & ~1u)), (addr & 1u) * 8u); break;
                case 2: m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.arm7Read8(addr)))); break;
                case 3:
                    if (addr & 1u) m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.arm7Read8(addr))));
                    else           m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(m_bus.arm7Read16(addr))));
                    break;
            }
        } else if (sh == 1) {
            m_bus.arm7Write16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu));
        }

        if ((!preIndex || writeBack) && !(isLoad && rd == rn)) {
            m_regs.r[rn] = offsetAddr;
        }
        return isLoad ? 3 : 2;
    }

    // 9. MRS / MSR
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
        if (opcode & (1u << 19)) mask |= 0xFF000000u;
        if (opcode & (1u << 16)) mask |= 0x000000FFu;

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

int32_t NdsArm7Cpu::executeThumb(uint16_t opcode) {
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

    if ((opcode >> 10) == 0b010001) {
        const uint8_t  op     = (opcode >> 8) & 3u;
        const uint32_t rd     = (opcode & 7u) | ((opcode >> 4) & 8u);
        const uint32_t rs     = ((opcode >> 3) & 7u) | ((opcode >> 3) & 8u);
        const uint32_t srcVal = m_regs.r[rs];

        switch (op) {
            case 0: m_regs.r[rd] += srcVal; if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; } break;
            case 1: subWithFlags(m_regs.r[rd], srcVal, true); break;
            case 2: m_regs.r[rd] = srcVal;  if (rd == 15) { m_regs.r[15] &= ~1u; m_pcModified = true; } break;
            case 3:
                m_regs.setFlag(CpuRegisters::FLAG_T, (srcVal & 1u) != 0);
                m_regs.r[15] = srcVal & (m_regs.isThumb() ? ~1u : ~3u);
                m_pcModified = true;
                break;
        }
        return 1;
    }

    if ((opcode >> 11) == 0b01001) {
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t imm  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[15] & ~3u) + imm;
        m_regs.r[rd] = m_bus.arm7Read32(addr);
        return 3;
    }

    if ((opcode >> 12) == 0b0101) {
        const uint32_t ro   = (opcode >> 6) & 7u;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + m_regs.r[ro];

        if ((opcode & (1u << 9)) == 0) {
            switch ((opcode >> 10) & 3u) {
                case 0: m_bus.arm7Write32(addr & ~3u, m_regs.r[rd]); break;
                case 1: m_bus.arm7Write8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFFu)); break;
                case 2: m_regs.r[rd] = std::rotr(m_bus.arm7Read32(addr & ~3u), (addr & 3u) * 8u); break;
                case 3: m_regs.r[rd] = m_bus.arm7Read8(addr); break;
            }
        } else {
            switch ((opcode >> 10) & 3u) {
                case 0: m_bus.arm7Write16(addr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu)); break;
                case 1: m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.arm7Read8(addr)))); break;
                case 2: m_regs.r[rd] = std::rotr(static_cast<uint32_t>(m_bus.arm7Read16(addr & ~1u)), (addr & 1u) * 8u); break;
                case 3:
                    if (addr & 1u) m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_bus.arm7Read8(addr))));
                    else           m_regs.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(m_bus.arm7Read16(addr))));
                    break;
            }
        }
        return 2;
    }

    if ((opcode >> 13) == 0b011) {
        const bool isByte   = (opcode & (1u << 12)) != 0;
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t off  = (opcode >> 6) & 0x1Fu;
        const uint32_t rb   = (opcode >> 3) & 7u;
        const uint32_t rd   = opcode & 7u;
        const uint32_t addr = m_regs.r[rb] + (isByte ? off : (off * 4u));

        if (isLoad) {
            m_regs.r[rd] = isByte ? m_bus.arm7Read8(addr) : std::rotr(m_bus.arm7Read32(addr & ~3u), (addr & 3u) * 8u);
        } else {
            if (isByte) m_bus.arm7Write8(addr, static_cast<uint8_t>(m_regs.r[rd] & 0xFFu));
            else        m_bus.arm7Write32(addr & ~3u, m_regs.r[rd]);
        }
        return 2;
    }

    if ((opcode >> 12) == 0b1000) {
        const bool isLoad      = (opcode & (1u << 11)) != 0;
        const uint32_t off     = ((opcode >> 6) & 0x1Fu) * 2u;
        const uint32_t rb      = (opcode >> 3) & 7u;
        const uint32_t rd      = opcode & 7u;
        const uint32_t rawAddr = m_regs.r[rb] + off;
        if (isLoad) m_regs.r[rd] = std::rotr(static_cast<uint32_t>(m_bus.arm7Read16(rawAddr & ~1u)), (rawAddr & 1u) * 8u);
        else        m_bus.arm7Write16(rawAddr & ~1u, static_cast<uint16_t>(m_regs.r[rd] & 0xFFFFu));
        return 2;
    }

    if ((opcode >> 12) == 0b1001) {
        const bool isLoad   = (opcode & (1u << 11)) != 0;
        const uint32_t rd   = (opcode >> 8) & 7u;
        const uint32_t off  = (opcode & 0xFFu) * 4u;
        const uint32_t addr = (m_regs.r[13] + off) & ~3u;
        if (isLoad) m_regs.r[rd] = m_bus.arm7Read32(addr);
        else        m_bus.arm7Write32(addr, m_regs.r[rd]);
        return 2;
    }

    if ((opcode >> 12) == 0b1010) {
        const bool useSp   = (opcode & (1u << 11)) != 0;
        const uint32_t rd  = (opcode >> 8) & 7u;
        const uint32_t off = (opcode & 0xFFu) * 4u;
        m_regs.r[rd] = (useSp ? m_regs.r[13] : (m_regs.r[15] & ~3u)) + off;
        return 1;
    }

    if ((opcode >> 8) == 0b10110000) {
        const uint32_t off = (opcode & 0x7Fu) * 4u;
        if (opcode & 0x80u) m_regs.r[13] -= off;
        else                m_regs.r[13] += off;
        return 1;
    }

    if ((opcode & 0xF600u) == 0xB400u) {
        const bool isPop   = (opcode & (1u << 11)) != 0;
        const bool pclr    = (opcode & (1u << 8)) != 0;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);

        if (isPop) {
            for (uint32_t i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    m_regs.r[i] = m_bus.arm7Read32(m_regs.r[13]);
                    m_regs.r[13] += 4u;
                }
            }
            if (pclr) {
                m_regs.r[15] = m_bus.arm7Read32(m_regs.r[13]) & ~1u;
                m_regs.r[13] += 4u;
                m_pcModified = true;
            }
        } else {
            if (pclr) {
                m_regs.r[13] -= 4u;
                m_bus.arm7Write32(m_regs.r[13], m_regs.r[14]);
            }
            for (int i = 7; i >= 0; --i) {
                if (list & (1u << i)) {
                    m_regs.r[13] -= 4u;
                    m_bus.arm7Write32(m_regs.r[13], m_regs.r[i]);
                }
            }
        }
        return 3;
    }

    if ((opcode >> 12) == 0b1100) {
        const bool isLoad  = (opcode & (1u << 11)) != 0;
        const uint32_t rb  = (opcode >> 8) & 7u;
        const uint8_t list = static_cast<uint8_t>(opcode & 0xFFu);
        uint32_t addr = m_regs.r[rb];
        for (uint32_t i = 0; i < 8; ++i) {
            if (list & (1u << i)) {
                if (isLoad) m_regs.r[i] = m_bus.arm7Read32(addr);
                else        m_bus.arm7Write32(addr, m_regs.r[i]);
                addr += 4u;
            }
        }
        if (!(isLoad && (list & (1u << rb)))) {
            m_regs.r[rb] = addr;
        }
        return 3;
    }

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

    if ((opcode >> 11) == 0b11100) {
        int32_t off = opcode & 0x7FFu;
        if (off & 0x400) off |= ~0x7FF;
        off <<= 1;
        m_regs.r[15] = static_cast<uint32_t>(static_cast<int32_t>(m_regs.r[15]) + off) & ~1u;
        m_pcModified = true;
        return 3;
    }

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

void NdsArm7Cpu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_regs);
    out.writePod(m_halted);
    out.writePod(m_pcModified);
    out.writePod(m_intrWaitActive);
    out.writePod(m_intrWaitMask);
}

bool NdsArm7Cpu::deserializeState(SaveStateReader& in) {
    return in.readPod(m_regs) &&
           in.readPod(m_halted) &&
           in.readPod(m_pcModified) &&
           in.readPod(m_intrWaitActive) &&
           in.readPod(m_intrWaitMask);
}

} // namespace gba::core::nds

