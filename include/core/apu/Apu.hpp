#pragma once
#include "core/SaveStateStream.hpp"
#include "core/apu/AudioRingBuffer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace gba::core {

class MemoryBus;

class DirectSoundFifo {
public:
    void reset() noexcept {
        m_head = m_tail = m_count = 0;
        m_currentSample = 0;
        m_data.fill(0);
    }

    void push8(int8_t byte) noexcept {
        if (m_count < 32) {
            m_data[m_tail] = byte;
            m_tail = (m_tail + 1) & 31;
            ++m_count;
        }
    }

    void push32(uint32_t word) noexcept {
        for (int i = 0; i < 4; ++i) {
            push8(static_cast<int8_t>((word >> (i * 8)) & 0xFF));
        }
    }

    int8_t popSample() noexcept {
        if (m_count > 0) {
            m_currentSample = m_data[m_head];
            m_head = (m_head + 1) & 31;
            --m_count;
        }
        return m_currentSample;
    }

    [[nodiscard]] uint8_t size() const noexcept { return m_count; }
    [[nodiscard]] int8_t currentSample() const noexcept { return m_currentSample; }

private:
    std::array<int8_t, 32> m_data{};
    uint8_t m_head{0};
    uint8_t m_tail{0};
    uint8_t m_count{0};
    int8_t  m_currentSample{0};
};

class Apu {
public:
    static constexpr uint32_t CPU_CLOCK_HZ      = 16'777'216;
    static constexpr uint32_t SAMPLE_RATE_HZ    = 48'000;
    static constexpr uint32_t CYCLES_PER_SAMPLE = CPU_CLOCK_HZ / SAMPLE_RATE_HZ; // 349 ciclos

    Apu();
    void attachBus(MemoryBus* bus) noexcept { m_bus = bus; }
    void reset();

    void step(int32_t cpuCycles);
    void onTimerOverflow(uint8_t timerIndex);

    void writeFifoA(uint32_t word) noexcept { m_fifoA.push32(word); }
    void writeFifoB(uint32_t word) noexcept { m_fifoB.push32(word); }
    void writeFifoA8(int8_t byte) noexcept { m_fifoA.push8(byte); }
    void writeFifoB8(int8_t byte) noexcept { m_fifoB.push8(byte); }

    // =========================================================================
    // CONTROLE EXTERNO OBRIGATÓRIO (Thread-Safe e Lock-Free)
    // =========================================================================
    void setVolume(float volume) noexcept {
        const float clamped = std::clamp(volume, 0.0f, 1.0f);
        m_masterVolume.store(clamped, std::memory_order_relaxed);
    }

    [[nodiscard]] float getVolume() const noexcept {
        return m_masterVolume.load(std::memory_order_relaxed);
    }

    void setMuted(bool state) noexcept {
        m_muted.store(state, std::memory_order_relaxed);
    }

    bool toggleMute() noexcept {
        bool expected = m_muted.load(std::memory_order_relaxed);
        while (!m_muted.compare_exchange_weak(expected, !expected, std::memory_order_relaxed)) {}
        return !expected;
    }

    [[nodiscard]] bool isMuted() const noexcept {
        return m_muted.load(std::memory_order_relaxed);
    }

    std::size_t pullAudioSamples(std::span<int16_t> outputBuffer) noexcept {
        return m_ringBuffer.popSamples(outputBuffer);
    }

    [[nodiscard]] std::size_t bufferedSamples() const noexcept {
        return m_ringBuffer.availableSamples();
    }

    [[nodiscard]] uint8_t readRegister(uint32_t offset) const noexcept;
    void writeRegister(uint32_t offset, uint8_t value) noexcept;

    void setSampleRate(uint32_t sampleRateHz) noexcept {
        const uint32_t clamped = std::clamp<uint32_t>(sampleRateHz, 16000u, 96000u);
        m_sampleRateHz    = clamped;
        m_cyclesPerSample = std::max<int32_t>(64, static_cast<int32_t>(CPU_CLOCK_HZ / clamped));
    }
    [[nodiscard]] uint32_t getSampleRate() const noexcept { return m_sampleRateHz; }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    MemoryBus* m_bus{nullptr};
    uint32_t   m_sampleRateHz{SAMPLE_RATE_HZ};
    int32_t    m_cyclesPerSample{static_cast<int32_t>(CYCLES_PER_SAMPLE)};
    int32_t    m_sampleCycleCounter{0};
    int32_t    m_frameSequencerCycles{0};
    uint8_t    m_frameSequencerStep{0};

    struct SquareChannel {
        bool     enabled{false};
        bool     lengthEnabled{false};
        uint8_t  duty{2};
        uint8_t  dutyStep{0};
        uint8_t  volume{0};
        uint8_t  initialVolume{0};
        bool     envelopeIncrease{false};
        uint8_t  envelopePeriod{0};
        uint8_t  envelopeTimer{0};
        uint8_t  sweepPeriod{0};
        uint8_t  sweepShift{0};
        bool     sweepNegate{false};
        uint8_t  sweepTimer{0};
        uint16_t frequency{0};
        int32_t  lengthCounter{0};
        int32_t  timer{0};

        [[nodiscard]] int16_t sample() const noexcept;
    } m_sq1, m_sq2;

    struct WaveChannel {
        bool     enabled{false};
        bool     dacEnabled{false};
        bool     lengthEnabled{false};
        uint8_t  volumeCode{0};
        uint16_t frequency{0};
        uint8_t  position{0};
        int32_t  lengthCounter{0};
        int32_t  timer{0};
        std::array<uint8_t, 16> waveRam{};

        [[nodiscard]] int16_t sample() const noexcept;
    } m_wave;

    struct NoiseChannel {
        bool     enabled{false};
        bool     lengthEnabled{false};
        uint8_t  volume{0};
        uint8_t  initialVolume{0};
        bool     envelopeIncrease{false};
        uint8_t  envelopePeriod{0};
        uint8_t  envelopeTimer{0};
        uint8_t  divisorCode{0};
        uint8_t  clockShift{0};
        bool     width7Bit{false};
        uint16_t lfsr{0x7FFF};
        int32_t  lengthCounter{0};
        int32_t  timer{0};

        [[nodiscard]] int16_t sample() const noexcept;
    } m_noise;

    DirectSoundFifo m_fifoA;
    DirectSoundFifo m_fifoB;

    std::array<uint8_t, 0x50> m_regs{};
    uint16_t m_soundCntL{0};
    uint16_t m_soundCntH{0};
    uint16_t m_soundCntX{0};
    uint16_t m_soundBias{0x0200};

    std::atomic<float> m_masterVolume{0.80f};
    std::atomic<bool>  m_muted{false};

    AudioRingBuffer<16384> m_ringBuffer;

    void stepPsgChannels(int32_t cycles);
    void stepFrameSequencer();
    void generateAndMixSample();
};

} // namespace gba::core

