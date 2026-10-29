#pragma once
#include "core/SaveStateStream.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gba::core {

class MemoryBus;

enum class CpuMode : uint32_t {
    User       = 0b10000, // USR
    Fiq        = 0b10001, // FIQ
    Irq        = 0b10010, // IRQ
    Supervisor = 0b10011, // SVC
    Abort      = 0b10111, // ABT
    Undefined  = 0b11011, // UND
    System     = 0b11111  // SYS
};

enum class InterruptFlag : uint16_t {
    VBlank  = 1 << 0,
    HBlank  = 1 << 1,
    VCount  = 1 << 2,
    Timer0  = 1 << 3,
    Timer1  = 1 << 4,
    Timer2  = 1 << 5,
    Timer3  = 1 << 6,
    Serial  = 1 << 7,
    Dma0    = 1 << 8,
    Dma1    = 1 << 9,
    Dma2    = 1 << 10,
    Dma3    = 1 << 11,
    Keypad  = 1 << 12,
    GamePak = 1 << 13
};

struct CpuRegisters {
    static constexpr uint32_t FLAG_N = 1u << 31;
    static constexpr uint32_t FLAG_Z = 1u << 30;
    static constexpr uint32_t FLAG_C = 1u << 29;
    static constexpr uint32_t FLAG_V = 1u << 28;
    static constexpr uint32_t FLAG_I = 1u << 7;
    static constexpr uint32_t FLAG_F = 1u << 6;
    static constexpr uint32_t FLAG_T = 1u << 5;

    std::array<uint32_t, 16> r{};
    uint32_t cpsr{0x0000001F};
    uint32_t spsr{0};

    std::array<uint32_t, 7> r8_14_fiq{};
    std::array<uint32_t, 7> r8_14_usr{};
    std::array<uint32_t, 2> r13_14_irq{};
    std::array<uint32_t, 2> r13_14_svc{};
    std::array<uint32_t, 2> r13_14_abt{};
    std::array<uint32_t, 2> r13_14_und{};
    std::array<uint32_t, 5> spsr_bank{};

    [[nodiscard]] bool isThumb() const noexcept { return (cpsr & FLAG_T) != 0; }
    [[nodiscard]] bool isIrqDisabled() const noexcept { return (cpsr & FLAG_I) != 0; }
    [[nodiscard]] CpuMode getMode() const noexcept { return static_cast<CpuMode>(cpsr & 0x1Fu); }

    void setFlag(uint32_t flag, bool state) noexcept {
        if (state) cpsr |= flag;
        else       cpsr &= ~flag;
    }

    [[nodiscard]] bool getFlag(uint32_t flag) const noexcept {
        return (cpsr & flag) != 0;
    }
};

class Arm7Tdmi {
public:
    explicit Arm7Tdmi(MemoryBus& bus);

    void reset(bool skipBios = true);
    int32_t step();
    void flushPipeline();
    void requestInterrupt(InterruptFlag flag);
    void halt() noexcept { m_halted = true; }
    [[nodiscard]] bool isHalted() const noexcept { return m_halted; }

    [[nodiscard]] CpuRegisters& registers() noexcept { return m_regs; }
    [[nodiscard]] const CpuRegisters& registers() const noexcept { return m_regs; }

    void setTraceEnabled(bool enabled) noexcept { m_traceEnabled = enabled; }
    [[nodiscard]] bool isTraceEnabled() const noexcept { return m_traceEnabled; }

    [[nodiscard]] const std::array<uint32_t, 16>& pcHistory() const noexcept { return m_pcHistory; }
    [[nodiscard]] const std::array<uint64_t, 256>& swiCallCounts() const noexcept { return m_swiCounts; }
    [[nodiscard]] uint32_t stuckLoopDetectionCount() const noexcept { return m_stuckLoopTotalReports; }

    [[nodiscard]] static std::string disassembleArm(uint32_t addr, uint32_t opcode);
    [[nodiscard]] static std::string disassembleThumb(uint32_t addr, uint16_t opcode);

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    MemoryBus&   m_bus;
    CpuRegisters m_regs{};
    bool         m_halted{false};
    bool         m_pcModified{false};
    bool         m_traceEnabled{false};

    // Histórico circular dos últimos 16 valores de PC e detector de Stuck Loop (> 50.000 ciclos)
    std::array<uint32_t, 16>  m_pcHistory{};
    std::size_t               m_pcHistoryHead{0};
    std::size_t               m_pcHistoryCount{0};
    std::array<uint32_t, 4>   m_stuckLoopPcs{};
    std::size_t               m_stuckLoopPcCount{0};
    uint64_t                  m_stuckLoopCycles{0};
    bool                      m_stuckLoopReportedCurrent{false};
    uint32_t                  m_stuckLoopTotalReports{0};
    uint64_t                  m_totalCycles{0};
    std::vector<std::pair<uint32_t, uint32_t>> m_reportedStuckRanges;
    std::array<uint64_t, 256> m_swiCounts{};

    void recordPcAndDetectStuckLoop(uint32_t instrAddr, int32_t cycles);
    void printStuckLoopDiagnostic(uint32_t minPc, uint32_t maxPc);

    void switchMode(CpuMode newMode);
    [[nodiscard]] bool checkCondition(uint8_t cond) const noexcept;
    void checkInterrupts();

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

} // namespace gba::core
