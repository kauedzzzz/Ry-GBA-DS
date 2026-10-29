#pragma once
#include "core/SaveStateStream.hpp"
#include "core/cpu/Arm7Tdmi.hpp"
#include <array>
#include <cstdint>

namespace gba::core::nds {

class NdsBus;

// =============================================================================
// Processador Principal ARM946E-S @ 66.66 MHz (ARMv5TE + DSP + CP15 + ITCM/DTCM)
// =============================================================================
class Arm9Cpu {
public:
    static constexpr uint32_t FLAG_Q = 1u << 27; // Sticky Overflow (DSP ARMv5TE)

    explicit Arm9Cpu(NdsBus& bus);

    void reset();
    void initDirectBoot(uint32_t entryAddr);

    int32_t step();

    void halt() noexcept { m_halted = true; }
    void wakeFromHalt() noexcept { m_halted = false; }
    [[nodiscard]] bool isHalted() const noexcept { return m_halted; }

    [[nodiscard]] CpuRegisters& registers() noexcept { return m_regs; }
    [[nodiscard]] const CpuRegisters& registers() const noexcept { return m_regs; }

    // Acesso público a memória com roteamento TCM (ITCM 32KB / DTCM 16KB / NdsBus)
    [[nodiscard]] uint8_t  dataRead8(uint32_t addr);
    [[nodiscard]] uint16_t dataRead16(uint32_t addr);
    [[nodiscard]] uint32_t dataRead32(uint32_t addr);
    void dataWrite8(uint32_t addr, uint8_t val);
    void dataWrite16(uint32_t addr, uint16_t val);
    void dataWrite32(uint32_t addr, uint32_t val);

    [[nodiscard]] uint16_t fetchCode16(uint32_t addr);
    [[nodiscard]] uint32_t fetchCode32(uint32_t addr);

    [[nodiscard]] uint32_t exceptionVectorBase() const noexcept {
        return (m_cp15Ctrl & (1u << 13)) ? 0xFFFF0000u : 0x00000000u;
    }
    [[nodiscard]] uint32_t dtcmBase() const noexcept { return m_cp15Dtcm & 0xFFFFF000u; }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    NdsBus&      m_bus;
    CpuRegisters m_regs{};
    bool         m_halted{false};
    bool         m_pcModified{false};
    bool         m_intrWaitActive{false};
    uint32_t     m_intrWaitMask{0};

    // Memórias TCM dedicadas de ciclo único do ARM946E-S
    std::array<uint8_t, 32 * 1024> m_itcm{}; // 32 KB Instruction TCM
    std::array<uint8_t, 16 * 1024> m_dtcm{}; // 16 KB Data TCM

    // Registradores CP15 (System Control Coprocessor)
    uint32_t m_cp15Ctrl{0x0005707Du}; // High vectors (0xFFFF0000) + ITCM + DTCM ativos
    uint32_t m_cp15Dtcm{0x0300000Au}; // Base 0x03000000, tamanho virtual 16 KB
    uint32_t m_cp15Itcm{0x00000020u}; // Base 0x00000000, janela virtual 32 MB
    std::array<uint32_t, 16> m_cp15Misc{};      // PU Cachability / Bufferability / Access Perms
    std::array<uint32_t, 8>  m_cp15PuRegions{}; // 8 Protection Unit Regions (c6, c0..c7)

    [[nodiscard]] bool isInItcmData(uint32_t addr) const noexcept;
    [[nodiscard]] bool isInItcmCode(uint32_t addr) const noexcept;
    [[nodiscard]] bool isInDtcmData(uint32_t addr) const noexcept;

    void switchMode(CpuMode newMode) noexcept;
    [[nodiscard]] bool checkCondition(uint8_t cond) const noexcept;
    void checkInterrupts();
    void pollIntrWait();

    int32_t executeArm(uint32_t opcode);
    int32_t executeThumb(uint16_t opcode);
    void handleSwi(uint8_t comment);

    uint32_t readCp15(uint8_t crn, uint8_t crm, uint8_t op2) const noexcept;
    void writeCp15(uint8_t crn, uint8_t crm, uint8_t op2, uint32_t val) noexcept;

    void setPcInterwork(uint32_t target) noexcept;
    uint32_t barrelShift(uint32_t val, uint8_t shiftType, uint8_t amount, bool& carryOut, bool immediateShift) const noexcept;
    void setNzFlags(uint32_t result) noexcept;
    uint32_t addWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept;
    uint32_t adcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept;
    uint32_t subWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept;
    uint32_t sbcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept;
};

} // namespace gba::core::nds
