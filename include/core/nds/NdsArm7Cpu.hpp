#pragma once
#include "core/SaveStateStream.hpp"
#include "core/cpu/Arm7Tdmi.hpp"
#include <cstdint>

namespace gba::core::nds {

class NdsBus;

// =============================================================================
// Co-Processador ARM7TDMI do Nintendo DS @ 33.51 MHz (Áudio, Touch SPI, IPC)
// =============================================================================
class NdsArm7Cpu {
public:
    explicit NdsArm7Cpu(NdsBus& bus);

    void reset();
    void initDirectBoot(uint32_t entryAddr);

    int32_t step();

    void halt() noexcept { m_halted = true; }
    void wakeFromHalt() noexcept { m_halted = false; }
    [[nodiscard]] bool isHalted() const noexcept { return m_halted; }

    [[nodiscard]] CpuRegisters& registers() noexcept { return m_regs; }
    [[nodiscard]] const CpuRegisters& registers() const noexcept { return m_regs; }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    NdsBus&      m_bus;
    CpuRegisters m_regs{};
    bool         m_halted{false};
    bool         m_pcModified{false};
    bool         m_intrWaitActive{false};
    uint32_t     m_intrWaitMask{0};

    void switchMode(CpuMode newMode) noexcept;
    [[nodiscard]] bool checkCondition(uint8_t cond) const noexcept;
    void checkInterrupts();
    void pollIntrWait();

    int32_t executeArm(uint32_t opcode);
    int32_t executeThumb(uint16_t opcode);
    void handleSwi(uint8_t comment);

    uint32_t barrelShift(uint32_t val, uint8_t shiftType, uint8_t amount, bool& carryOut, bool immediateShift) const noexcept;
    void setNzFlags(uint32_t result) noexcept;
    uint32_t addWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept;
    uint32_t adcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept;
    uint32_t subWithFlags(uint32_t a, uint32_t b, bool updateFlags) noexcept;
    uint32_t sbcWithFlags(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) noexcept;
};

} // namespace gba::core::nds

