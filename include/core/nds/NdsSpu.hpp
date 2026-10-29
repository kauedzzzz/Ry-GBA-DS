#pragma once
#include "core/SaveStateStream.hpp"
#include "core/apu/AudioRingBuffer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace gba::core::nds {

class NdsBus;

// =============================================================================
// SPU 16 Canais do ARM7 do Nintendo DS (PCM8, PCM16, IMA-ADPCM, PSG & Ruído)
// =============================================================================
class NdsSpu {
public:
    static constexpr uint32_t ARM7_CLOCK_HZ     = 33'513'982u;
    static constexpr uint32_t SPU_TIMER_BASE_HZ = 16'756'991u; // ARM7_CLOCK_HZ / 2

    NdsSpu();

    void attachBus(NdsBus* bus) noexcept { m_bus = bus; }
    void reset() noexcept;

    void stepArm7Cycles(int32_t arm7Cycles) noexcept;

    // Registradores MMIO do ARM7 (0x04000400 .. 0x0400051F)
    [[nodiscard]] uint8_t  read8(uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t read16(uint32_t offset) const noexcept;
    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept;

    void write8(uint32_t offset, uint8_t val) noexcept;
    void write16(uint32_t offset, uint16_t val) noexcept;
    void write32(uint32_t offset, uint32_t val) noexcept;

    // Controle externo Lock-Free idêntico à APU GBA
    void setVolume(float vol) noexcept {
        m_masterVolume.store(std::clamp(vol, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    [[nodiscard]] float getVolume() const noexcept {
        return m_masterVolume.load(std::memory_order_relaxed);
    }
    void setMuted(bool muted) noexcept {
        m_muted.store(muted, std::memory_order_relaxed);
    }
    bool toggleMute() noexcept {
        bool expected = m_muted.load(std::memory_order_relaxed);
        while (!m_muted.compare_exchange_weak(expected, !expected, std::memory_order_relaxed)) {}
        return !expected;
    }
    [[nodiscard]] bool isMuted() const noexcept {
        return m_muted.load(std::memory_order_relaxed);
    }
    void setSampleRate(uint32_t hz) noexcept {
        m_outputSampleRate = std::clamp<uint32_t>(hz, 16000u, 96000u);
        m_arm7CyclesPerOutSample = std::max<int32_t>(64, static_cast<int32_t>(ARM7_CLOCK_HZ / m_outputSampleRate));
    }
    [[nodiscard]] uint32_t getSampleRate() const noexcept { return m_outputSampleRate; }

    std::size_t pullAudioSamples(std::span<int16_t> outBuffer) noexcept {
        return m_ringBuffer.popSamples(outBuffer);
    }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    struct Channel {
        uint32_t cnt{0};        // SOUNDxCNT (0x00)
        uint32_t sad{0};        // SOUNDxSAD (0x04)
        uint16_t tmr{0};        // SOUNDxTMR (0x08)
        uint16_t pnt{0};        // SOUNDxPNT (0x0A) em palavras de 32 bits
        uint32_t len{0};        // SOUNDxLEN (0x0C) em palavras de 32 bits

        // Estado interno de reprodução
        bool     active{false};
        int32_t  timerCounter{0};
        uint32_t bytePos{0};
        int16_t  currentSample{0};

        // IMA-ADPCM
        int32_t  adpcmValue{0};
        int32_t  adpcmIndex{0};
        int32_t  adpcmLoopValue{0};
        int32_t  adpcmLoopIndex{0};
        bool     adpcmHighNibble{false};

        // PSG / Ruído (canais 8-15)
        uint8_t  psgDutyPos{0};
        uint16_t noiseLfsr{0x7FFFu};
    };

    NdsBus*  m_bus{nullptr};
    uint32_t m_outputSampleRate{48000};
    int32_t  m_arm7CyclesPerOutSample{static_cast<int32_t>(ARM7_CLOCK_HZ / 48000)};
    int32_t  m_outSampleAccum{0};

    std::array<Channel, 16>   m_channels{};
    uint16_t                  m_soundCnt{0};
    uint16_t                  m_soundBias{0x0200};
    std::array<uint8_t, 0x18> m_captureRegs{};

    std::atomic<float>     m_masterVolume{0.80f};
    std::atomic<bool>      m_muted{false};
    AudioRingBuffer<16384> m_ringBuffer{};

    void startChannel(std::size_t chIdx) noexcept;
    void advanceChannelSample(std::size_t chIdx) noexcept;
    void mixAndPushStereoSample() noexcept;
};

} // namespace gba::core::nds

