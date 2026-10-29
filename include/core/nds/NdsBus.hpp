#pragma once
#include "core/SaveStateStream.hpp"
#include "core/input/InputManager.hpp"
#include "core/nds/NdsCartridge.hpp"
#include "core/nds/NdsGpu.hpp"
#include "core/nds/NdsSpu.hpp"
#include "core/nds/TouchController.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <span>

namespace gba::core::nds {

class Arm9Cpu;
class NdsArm7Cpu;

enum class NdsKey : uint16_t {
    A      = 1 << 0,
    B      = 1 << 1,
    Select = 1 << 2,
    Start  = 1 << 3,
    Right  = 1 << 4,
    Left   = 1 << 5,
    Up     = 1 << 6,
    Down   = 1 << 7,
    R      = 1 << 8,
    L      = 1 << 9,
    X      = 1 << 10, // EXTKEYIN bit 0
    Y      = 1 << 11  // EXTKEYIN bit 1
};

namespace NdsIrq {
    constexpr uint32_t VBlank          = 1u << 0;
    constexpr uint32_t HBlank          = 1u << 1;
    constexpr uint32_t VCount          = 1u << 2;
    constexpr uint32_t Timer0          = 1u << 3;
    constexpr uint32_t Timer1          = 1u << 4;
    constexpr uint32_t Timer2          = 1u << 5;
    constexpr uint32_t Timer3          = 1u << 6;
    constexpr uint32_t RtcSerial       = 1u << 7;
    constexpr uint32_t Dma0            = 1u << 8;
    constexpr uint32_t Dma1            = 1u << 9;
    constexpr uint32_t Dma2            = 1u << 10;
    constexpr uint32_t Dma3            = 1u << 11;
    constexpr uint32_t Keypad          = 1u << 12;
    constexpr uint32_t IpcSync         = 1u << 16;
    constexpr uint32_t IpcSendEmpty    = 1u << 17;
    constexpr uint32_t IpcRecvNotEmpty = 1u << 18;
    constexpr uint32_t CardDataReady   = 1u << 19;
    constexpr uint32_t GxFifo          = 1u << 21; // ARM9
    constexpr uint32_t LidHinge        = 1u << 22; // ARM7
    constexpr uint32_t SpiDataReady    = 1u << 23; // ARM7
}

class NdsBus {
public:
    NdsBus(NdsCartridge& cart, NdsGpu& gpu, NdsSpu& spu, TouchController& touch);

    void attachCpus(Arm9Cpu* arm9, NdsArm7Cpu* arm7) noexcept {
        m_arm9 = arm9;
        m_arm7 = arm7;
    }

    void reset();
    void setupDirectBoot();

    // Carregamento opcional de BIOS real (bios9.bin 4KB, bios7.bin 16KB)
    bool loadBios9(std::span<const uint8_t> data);
    bool loadBios7(std::span<const uint8_t> data);
    [[nodiscard]] bool hasCustomBios9() const noexcept { return m_hasCustomBios9; }
    [[nodiscard]] bool hasCustomBios7() const noexcept { return m_hasCustomBios7; }

    // =========================================================================
    // Barramento ARM9 (66.66 MHz — Nota: ITCM/DTCM são interceptados em Arm9Cpu)
    // =========================================================================
    [[nodiscard]] uint8_t  arm9Read8(uint32_t addr);
    [[nodiscard]] uint16_t arm9Read16(uint32_t addr);
    [[nodiscard]] uint32_t arm9Read32(uint32_t addr);

    void arm9Write8(uint32_t addr, uint8_t val);
    void arm9Write16(uint32_t addr, uint16_t val);
    void arm9Write32(uint32_t addr, uint32_t val);

    // =========================================================================
    // Barramento ARM7 (33.51 MHz)
    // =========================================================================
    [[nodiscard]] uint8_t  arm7Read8(uint32_t addr);
    [[nodiscard]] uint16_t arm7Read16(uint32_t addr);
    [[nodiscard]] uint32_t arm7Read32(uint32_t addr);

    void arm7Write8(uint32_t addr, uint8_t val);
    void arm7Write16(uint32_t addr, uint16_t val);
    void arm7Write32(uint32_t addr, uint32_t val);

    // =========================================================================
    // Entradas (12 Botões NDS: A, B, Select, Start, D-Pad, L, R + X, Y)
    // =========================================================================
    void setNdsKeyState(NdsKey key, bool pressed) noexcept;
    [[nodiscard]] uint16_t keyInputReg() const noexcept { return m_keyInput; }
    [[nodiscard]] uint16_t extKeyInReg() const noexcept;

    // =========================================================================
    // Interrupções, Timers e Gatilhos DMA
    // =========================================================================
    void requestIrq9(uint32_t mask) noexcept;
    void requestIrq7(uint32_t mask) noexcept;
    [[nodiscard]] bool hasPendingIrq9() const noexcept { return (m_ime9 & 1u) != 0 && (m_ie9 & m_if9) != 0; }
    [[nodiscard]] bool hasPendingIrq7() const noexcept { return (m_ime7 & 1u) != 0 && (m_ie7 & m_if7) != 0; }
    [[nodiscard]] bool anyEnabledIrqFlag9() const noexcept { return (m_ie9 & m_if9) != 0; }
    [[nodiscard]] bool anyEnabledIrqFlag7() const noexcept { return (m_ie7 & m_if7) != 0; }

    [[nodiscard]] uint32_t ie9() const noexcept { return m_ie9; }
    [[nodiscard]] uint32_t if9() const noexcept { return m_if9; }
    [[nodiscard]] uint32_t ime9() const noexcept { return m_ime9; }
    [[nodiscard]] uint32_t ie7() const noexcept { return m_ie7; }
    [[nodiscard]] uint32_t if7() const noexcept { return m_if7; }
    [[nodiscard]] uint32_t ime7() const noexcept { return m_ime7; }

    void stepTimersArm7Cycles(int32_t arm7Cycles);
    void triggerDmaVBlank();
    void triggerDmaHBlank();
    void triggerDmaGxFifo();
    void triggerDmaCardData(bool forArm9);

    // =========================================================================
    // Acessores Rápidos de VRAM / Paleta / OAM para a NdsGpu (2D + 3D)
    // =========================================================================
    [[nodiscard]] uint8_t  readBgVram8(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t readBgVram16(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t  readObjVram8(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t readObjVram16(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t readLcdcBank16(int bankIdx, uint32_t byteOffset) const noexcept;

    [[nodiscard]] uint16_t readPalette16(int engine, uint32_t byteOffset) const noexcept;
    [[nodiscard]] uint16_t readExtBgPalette16(int engine, int slot, uint32_t colorIdx) const noexcept;
    [[nodiscard]] uint16_t readExtObjPalette16(int engine, uint32_t colorIdx) const noexcept;
    [[nodiscard]] uint16_t readOam16(int engine, uint32_t byteOffset) const noexcept;

    [[nodiscard]] uint8_t  readTexVram8(uint32_t texOffset) const noexcept;
    [[nodiscard]] uint16_t readTexVram16(uint32_t texOffset) const noexcept;
    [[nodiscard]] uint16_t readTexPalette16(uint32_t byteOffset) const noexcept;

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    struct DmaChannel {
        uint32_t sad{0};
        uint32_t dad{0};
        uint32_t cnt{0}; // 32 bits completos (word count + control)
        uint32_t internalSrc{0};
        uint32_t internalDst{0};
        uint32_t internalCount{0};
    };

    struct TimerChannel {
        uint16_t reload{0};
        uint16_t counter{0};
        uint16_t control{0};
        int32_t  cyclesAccum{0};
    };

    NdsCartridge&    m_cart;
    NdsGpu&          m_gpu;
    NdsSpu&          m_spu;
    TouchController& m_touch;
    Arm9Cpu*         m_arm9{nullptr};
    NdsArm7Cpu*      m_arm7{nullptr};

    // Memórias principais do NDS
    std::vector<uint8_t>            m_mainRam;    // 4 MB (0x02000000)
    std::array<uint8_t, 0x008000>   m_sharedWram{}; // 32 KB Shared WRAM
    std::array<uint8_t, 0x010000>   m_arm7Wram{};   // 64 KB ARM7 WRAM (0x03800000)
    std::array<uint8_t, 0x001000>   m_bios9{};      // 4 KB ARM9 BIOS (0xFFFF0000)
    std::array<uint8_t, 0x004000>   m_bios7{};      // 16 KB ARM7 BIOS (0x00000000)
    bool                            m_hasCustomBios9{false};
    bool                            m_hasCustomBios7{false};

    // 9 Bancos físicos de VRAM (656 KB total)
    std::array<uint8_t, 128 * 1024> m_vramA{};
    std::array<uint8_t, 128 * 1024> m_vramB{};
    std::array<uint8_t, 128 * 1024> m_vramC{};
    std::array<uint8_t, 128 * 1024> m_vramD{};
    std::array<uint8_t, 64 * 1024>  m_vramE{};
    std::array<uint8_t, 16 * 1024>  m_vramF{};
    std::array<uint8_t, 16 * 1024>  m_vramG{};
    std::array<uint8_t, 32 * 1024>  m_vramH{};
    std::array<uint8_t, 16 * 1024>  m_vramI{};
    std::array<uint8_t, 9>          m_vramCnt{}; // A=0, B=1, C=2, D=3, E=4, F=5, G=6, H=7, I=8
    uint8_t                         m_wramCnt{3};

    std::array<uint8_t, 0x000800>   m_palettes{}; // 1KB Engine A + 1KB Engine B
    std::array<uint8_t, 0x000800>   m_oam{};      // 1KB Engine A + 1KB Engine B
    std::array<uint8_t, 0x008000>   m_wifiRam{};  // 32KB WiFi stub (0x04800000 no ARM7)

    // Interrupções e Controle
    uint32_t m_ime9{0}, m_ie9{0}, m_if9{0};
    uint32_t m_ime7{0}, m_ie7{0}, m_if7{0};
    uint16_t m_exMemCnt{0x6000}; // Bit 11 = Cart access rights (0=ARM9, 1=ARM7)
    uint8_t  m_postFlg9{1};
    uint8_t  m_postFlg7{1};
    uint8_t  m_haltCnt7{0};
    uint8_t  m_rtcReg{0};

    // Botões
    uint16_t m_keyInput{0x03FFu}; // A,B,Sel,Start,Dir,L,R (active-low)
    uint16_t m_xyMask{0x0003u};   // bit 0=X, bit 1=Y (active-low)
    uint16_t m_keyCnt9{0};
    uint16_t m_keyCnt7{0};

    // IPC Sync & FIFOs bidirecionais (16 palavras cada)
    uint16_t             m_ipcSync9{0};
    uint16_t             m_ipcSync7{0};
    uint16_t             m_ipcFifoCnt9{0x0101u};
    uint16_t             m_ipcFifoCnt7{0x0101u};
    std::deque<uint32_t> m_fifo9to7;
    std::deque<uint32_t> m_fifo7to9;
    uint32_t             m_lastFifo9to7{0};
    uint32_t             m_lastFifo7to9{0};

    // DMA (4 canais ARM9 + DMAFILL + 4 canais ARM7) e Timers (4 ARM9 + 4 ARM7)
    std::array<DmaChannel, 4>   m_dma9{};
    std::array<uint32_t, 4>     m_dmaFill9{};
    std::array<DmaChannel, 4>   m_dma7{};
    std::array<TimerChannel, 4> m_timers9{};
    std::array<TimerChannel, 4> m_timers7{};

    // Coprocessador Matemático Hardware ARM9 (DIV & SQRT)
    uint16_t m_divCnt{0};
    int64_t  m_divNumer{0};
    int64_t  m_divDenom{0};
    int64_t  m_divResult{0};
    int64_t  m_divRem{0};

    uint16_t m_sqrtCnt{0};
    uint64_t m_sqrtParam{0};
    uint32_t m_sqrtResult{0};

    void installHleBiosVectors();
    void recomputeDiv() noexcept;
    void recomputeSqrt() noexcept;

    uint8_t* resolveVramCpuWritePtr(bool isArm9, uint32_t addr) noexcept;
    const uint8_t* resolveVramCpuReadPtr(bool isArm9, uint32_t addr) const noexcept;

    uint8_t  readIo9_8(uint32_t addr);
    uint16_t readIo9_16(uint32_t addr);
    uint32_t readIo9_32(uint32_t addr);
    void     writeIo9_8(uint32_t addr, uint8_t val);
    void     writeIo9_16(uint32_t addr, uint16_t val);
    void     writeIo9_32(uint32_t addr, uint32_t val);

    uint8_t  readIo7_8(uint32_t addr);
    uint16_t readIo7_16(uint32_t addr);
    uint32_t readIo7_32(uint32_t addr);
    void     writeIo7_8(uint32_t addr, uint8_t val);
    void     writeIo7_16(uint32_t addr, uint16_t val);
    void     writeIo7_32(uint32_t addr, uint32_t val);

    uint16_t buildIpcFifoCnt(bool isArm9) const noexcept;
    void     writeIpcFifoCnt(bool isArm9, uint16_t val) noexcept;
    void     pushIpcFifo(bool fromArm9, uint32_t word) noexcept;
    uint32_t popIpcFifo(bool forArm9) noexcept;

    void onDmaWrite9(std::size_t ch, uint32_t oldCnt, uint32_t newCnt);
    void executeDma9(std::size_t ch);
    void onDmaWrite7(std::size_t ch, uint32_t oldCnt, uint32_t newCnt);
    void executeDma7(std::size_t ch);
};

} // namespace gba::core::nds

