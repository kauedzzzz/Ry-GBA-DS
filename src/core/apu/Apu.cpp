#include "core/apu/Apu.hpp"
#include "core/memory/MemoryBus.hpp"
#include <algorithm>

namespace gba::core {

Apu::Apu() {
    reset();
}

void Apu::reset() {
    m_sampleCycleCounter   = 0;
    m_frameSequencerCycles = 0;
    m_frameSequencerStep   = 0;
    m_sq1   = SquareChannel{};
    m_sq2   = SquareChannel{};
    m_wave  = WaveChannel{};
    m_noise = NoiseChannel{};
    // Inicializa Wave RAM com uma onda triangular suave padrão
    for (int i = 0; i < 16; ++i) {
        const uint8_t hi = static_cast<uint8_t>(i & 0xF);
        const uint8_t lo = static_cast<uint8_t>((15 - i) & 0xF);
        m_wave.waveRam[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    m_fifoA.reset();
    m_fifoB.reset();
    m_ringBuffer.clear();
    m_regs.fill(0);
    m_soundCntL = 0xFF77;
    m_soundCntH = 0x0002;
    m_soundCntX = 0x0080;
    m_soundBias = 0x0200;
    m_regs[0x88 - 0x60] = 0x00;
    m_regs[0x89 - 0x60] = 0x02;
}

void Apu::onTimerOverflow(uint8_t timerIndex) {
    if ((m_soundCntX & 0x80) == 0) return;

    const uint8_t timerSelectA = (m_soundCntH >> 10) & 1u;
    if (timerSelectA == timerIndex) {
        m_fifoA.popSample();
        if (m_fifoA.size() <= 16 && m_bus) {
            m_bus->triggerDmaFifo(0);
        }
    }

    const uint8_t timerSelectB = (m_soundCntH >> 14) & 1u;
    if (timerSelectB == timerIndex) {
        m_fifoB.popSample();
        if (m_fifoB.size() <= 16 && m_bus) {
            m_bus->triggerDmaFifo(1);
        }
    }
}

void Apu::step(int32_t cpuCycles) {
    stepPsgChannels(cpuCycles);

    m_sampleCycleCounter += cpuCycles;
    while (m_sampleCycleCounter >= m_cyclesPerSample) {
        m_sampleCycleCounter -= m_cyclesPerSample;
        generateAndMixSample();
    }
}

void Apu::stepPsgChannels(int32_t cycles) {
    if ((m_soundCntX & 0x80) == 0) return;

    // Frame Sequencer a 512 Hz (16.777.216 / 512 = 32768 ciclos)
    m_frameSequencerCycles += cycles;
    while (m_frameSequencerCycles >= 32768) {
        m_frameSequencerCycles -= 32768;
        stepFrameSequencer();
    }

    // Square 1: período em ciclos de 16.78MHz = (2048 - freq) * 16
    if (m_sq1.enabled) {
        m_sq1.timer -= cycles;
        while (m_sq1.timer <= 0) {
            const int32_t period = std::max(16, (2048 - static_cast<int32_t>(m_sq1.frequency)) * 16);
            m_sq1.timer += period;
            m_sq1.dutyStep = (m_sq1.dutyStep + 1) & 7;
        }
    }

    // Square 2
    if (m_sq2.enabled) {
        m_sq2.timer -= cycles;
        while (m_sq2.timer <= 0) {
            const int32_t period = std::max(16, (2048 - static_cast<int32_t>(m_sq2.frequency)) * 16);
            m_sq2.timer += period;
            m_sq2.dutyStep = (m_sq2.dutyStep + 1) & 7;
        }
    }

    // Wave Channel: período = (2048 - freq) * 8
    if (m_wave.enabled && m_wave.dacEnabled) {
        m_wave.timer -= cycles;
        while (m_wave.timer <= 0) {
            const int32_t period = std::max(8, (2048 - static_cast<int32_t>(m_wave.frequency)) * 8);
            m_wave.timer += period;
            m_wave.position = (m_wave.position + 1) & 31;
        }
    }

    // Noise Channel (LFSR)
    if (m_noise.enabled) {
        m_noise.timer -= cycles;
        while (m_noise.timer <= 0) {
            static constexpr int32_t DIVISORS[8] = {32, 64, 128, 192, 256, 320, 384, 448};
            const int32_t period = DIVISORS[m_noise.divisorCode & 7] << (m_noise.clockShift & 0xF);
            m_noise.timer += std::max(32, period);

            const uint16_t xorBit = (m_noise.lfsr & 1u) ^ ((m_noise.lfsr >> 1u) & 1u);
            m_noise.lfsr = static_cast<uint16_t>((m_noise.lfsr >> 1u) | (xorBit << 14u));
            if (m_noise.width7Bit) {
                m_noise.lfsr = static_cast<uint16_t>((m_noise.lfsr & ~(1u << 6u)) | (xorBit << 6u));
            }
        }
    }
}

void Apu::stepFrameSequencer() {
    // 256 Hz: Length Counter (passos 0, 2, 4, 6)
    if ((m_frameSequencerStep & 1) == 0) {
        if (m_sq1.lengthEnabled && m_sq1.lengthCounter > 0 && --m_sq1.lengthCounter == 0) m_sq1.enabled = false;
        if (m_sq2.lengthEnabled && m_sq2.lengthCounter > 0 && --m_sq2.lengthCounter == 0) m_sq2.enabled = false;
        if (m_wave.lengthEnabled && m_wave.lengthCounter > 0 && --m_wave.lengthCounter == 0) m_wave.enabled = false;
        if (m_noise.lengthEnabled && m_noise.lengthCounter > 0 && --m_noise.lengthCounter == 0) m_noise.enabled = false;
    }

    // 128 Hz: Sweep do Canal 1 (passos 2 e 6)
    if (m_frameSequencerStep == 2 || m_frameSequencerStep == 6) {
        if (m_sq1.enabled && m_sq1.sweepPeriod > 0 && m_sq1.sweepShift > 0) {
            if (--m_sq1.sweepTimer == 0) {
                m_sq1.sweepTimer = m_sq1.sweepPeriod;
                const uint16_t delta = m_sq1.frequency >> m_sq1.sweepShift;
                const int32_t nextFreq = m_sq1.sweepNegate
                    ? static_cast<int32_t>(m_sq1.frequency) - delta
                    : static_cast<int32_t>(m_sq1.frequency) + delta;
                if (nextFreq > 2047) {
                    m_sq1.enabled = false;
                } else if (nextFreq >= 0) {
                    m_sq1.frequency = static_cast<uint16_t>(nextFreq);
                }
            }
        }
    }

    // 64 Hz: Envelope de Volume (passo 7)
    if (m_frameSequencerStep == 7) {
        auto updateEnv = [](auto& ch) {
            if (ch.enabled && ch.envelopePeriod > 0) {
                if (--ch.envelopeTimer == 0) {
                    ch.envelopeTimer = ch.envelopePeriod;
                    if (ch.envelopeIncrease && ch.volume < 15) ++ch.volume;
                    else if (!ch.envelopeIncrease && ch.volume > 0) --ch.volume;
                }
            }
        };
        updateEnv(m_sq1);
        updateEnv(m_sq2);
        updateEnv(m_noise);
    }

    m_frameSequencerStep = (m_frameSequencerStep + 1) & 7;
}

void Apu::generateAndMixSample() {
    if (m_muted.load(std::memory_order_relaxed) || (m_soundCntX & 0x0080) == 0) {
        m_ringBuffer.pushStereoSample(0, 0);
        return;
    }

    int32_t leftMix  = 0;
    int32_t rightMix = 0;

    const int32_t psgSamples[4] = {
        m_sq1.sample(),
        m_sq2.sample(),
        m_wave.sample(),
        m_noise.sample()
    };

    int32_t psgLeft = 0, psgRight = 0;
    for (int ch = 0; ch < 4; ++ch) {
        if (m_soundCntL & (1 << (12 + ch))) psgLeft  += psgSamples[ch];
        if (m_soundCntL & (1 << (8 + ch)))  psgRight += psgSamples[ch];
    }

    const int32_t volLeft  = ((m_soundCntL >> 4) & 0x7) + 1;
    const int32_t volRight = (m_soundCntL & 0x7) + 1;
    psgLeft  = (psgLeft * volLeft) >> 3;
    psgRight = (psgRight * volRight) >> 3;

    const uint8_t psgRatio = m_soundCntH & 0x3;
    const int32_t psgShift = (psgRatio == 0) ? 2 : (psgRatio == 1 ? 1 : 0);
    leftMix  += psgLeft  >> psgShift;
    rightMix += psgRight >> psgShift;

    const int32_t scaleA = (m_soundCntH & (1 << 2)) ? 4 : 2;
    const int32_t scaleB = (m_soundCntH & (1 << 3)) ? 4 : 2;
    const int32_t dmaA   = static_cast<int32_t>(m_fifoA.currentSample()) * scaleA;
    const int32_t dmaB   = static_cast<int32_t>(m_fifoB.currentSample()) * scaleB;

    if (m_soundCntH & (1 << 9))  leftMix  += dmaA;
    if (m_soundCntH & (1 << 8))  rightMix += dmaA;
    if (m_soundCntH & (1 << 13)) leftMix  += dmaB;
    if (m_soundCntH & (1 << 12)) rightMix += dmaB;

    const float masterVol = m_masterVolume.load(std::memory_order_relaxed);
    const int32_t finalL = std::clamp(static_cast<int32_t>(leftMix * 28.0f * masterVol), -32768, 32767);
    const int32_t finalR = std::clamp(static_cast<int32_t>(rightMix * 28.0f * masterVol), -32768, 32767);

    m_ringBuffer.pushStereoSample(static_cast<int16_t>(finalL), static_cast<int16_t>(finalR));
}

int16_t Apu::SquareChannel::sample() const noexcept {
    if (!enabled || volume == 0) return 0;
    static constexpr uint8_t DUTY_TABLE[4][8] = {
        {0, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 0}
    };
    const int16_t amp = static_cast<int16_t>(volume * 10);
    return DUTY_TABLE[duty & 3][dutyStep & 7] ? amp : static_cast<int16_t>(-amp);
}

int16_t Apu::WaveChannel::sample() const noexcept {
    if (!enabled || !dacEnabled || volumeCode == 0) return 0;
    const uint8_t byte   = waveRam[(position >> 1) & 0x0F];
    const uint8_t nibble = (position & 1) ? (byte & 0x0F) : (byte >> 4);
    const int32_t centered = (static_cast<int32_t>(nibble) - 8) * 16;
    switch (volumeCode & 3) {
        case 1: return static_cast<int16_t>(centered);        // 100%
        case 2: return static_cast<int16_t>(centered >> 1);   // 50%
        case 3: return static_cast<int16_t>(centered >> 2);   // 25%
        default: return static_cast<int16_t>((centered * 3) >> 2); // 75%
    }
}

int16_t Apu::NoiseChannel::sample() const noexcept {
    if (!enabled || volume == 0) return 0;
    const int16_t amp = static_cast<int16_t>(volume * 8);
    return (lfsr & 1u) ? static_cast<int16_t>(-amp) : amp;
}

uint8_t Apu::readRegister(uint32_t offset) const noexcept {
    if (offset >= 0x60 && offset < 0xB0) {
        if (offset == 0x84) {
            uint8_t status = static_cast<uint8_t>(m_soundCntX & 0x80);
            if (m_sq1.enabled)   status |= (1u << 0);
            if (m_sq2.enabled)   status |= (1u << 1);
            if (m_wave.enabled)  status |= (1u << 2);
            if (m_noise.enabled) status |= (1u << 3);
            return status;
        }
        if (offset >= 0x90 && offset < 0xA0) {
            return m_wave.waveRam[offset - 0x90];
        }
        return m_regs[offset - 0x60];
    }
    return 0;
}

void Apu::writeRegister(uint32_t offset, uint8_t value) noexcept {
    if (offset < 0x60 || offset >= 0xB0) return;
    m_regs[offset - 0x60] = value;

    auto reg16 = [this](uint32_t base) -> uint16_t {
        return static_cast<uint16_t>(m_regs[base - 0x60] | (m_regs[base - 0x60 + 1] << 8));
    };

    switch (offset) {
        // Canal 1 (Square 1 + Sweep)
        case 0x60:
            m_sq1.sweepShift  = value & 0x07;
            m_sq1.sweepNegate = (value & 0x08) != 0;
            m_sq1.sweepPeriod = (value >> 4) & 0x07;
            m_sq1.sweepTimer  = m_sq1.sweepPeriod ? m_sq1.sweepPeriod : 8;
            break;
        case 0x62:
            m_sq1.lengthCounter = 64 - (value & 0x3F);
            m_sq1.duty          = (value >> 6) & 0x03;
            break;
        case 0x63:
            m_sq1.envelopePeriod   = value & 0x07;
            m_sq1.envelopeIncrease = (value & 0x08) != 0;
            m_sq1.initialVolume    = (value >> 4) & 0x0F;
            m_sq1.volume           = m_sq1.initialVolume;
            break;
        case 0x64:
        case 0x65: {
            const uint16_t cntX = reg16(0x64);
            m_sq1.frequency     = cntX & 0x07FF;
            m_sq1.lengthEnabled = (cntX & (1u << 14)) != 0;
            if (offset == 0x65 && (value & 0x80)) {
                m_sq1.enabled       = true;
                m_sq1.volume        = m_sq1.initialVolume;
                m_sq1.envelopeTimer = m_sq1.envelopePeriod ? m_sq1.envelopePeriod : 8;
                m_sq1.sweepTimer    = m_sq1.sweepPeriod ? m_sq1.sweepPeriod : 8;
                if (m_sq1.lengthCounter == 0) m_sq1.lengthCounter = 64;
                m_sq1.timer = (2048 - m_sq1.frequency) * 16;
            }
            break;
        }

        // Canal 2 (Square 2)
        case 0x68:
            m_sq2.lengthCounter = 64 - (value & 0x3F);
            m_sq2.duty          = (value >> 6) & 0x03;
            break;
        case 0x69:
            m_sq2.envelopePeriod   = value & 0x07;
            m_sq2.envelopeIncrease = (value & 0x08) != 0;
            m_sq2.initialVolume    = (value >> 4) & 0x0F;
            m_sq2.volume           = m_sq2.initialVolume;
            break;
        case 0x6C:
        case 0x6D: {
            const uint16_t cntH = reg16(0x6C);
            m_sq2.frequency     = cntH & 0x07FF;
            m_sq2.lengthEnabled = (cntH & (1u << 14)) != 0;
            if (offset == 0x6D && (value & 0x80)) {
                m_sq2.enabled       = true;
                m_sq2.volume        = m_sq2.initialVolume;
                m_sq2.envelopeTimer = m_sq2.envelopePeriod ? m_sq2.envelopePeriod : 8;
                if (m_sq2.lengthCounter == 0) m_sq2.lengthCounter = 64;
                m_sq2.timer = (2048 - m_sq2.frequency) * 16;
            }
            break;
        }

        // Canal 3 (Wave RAM)
        case 0x70:
            m_wave.dacEnabled = (value & 0x80) != 0;
            if (!m_wave.dacEnabled) m_wave.enabled = false;
            break;
        case 0x72:
            m_wave.lengthCounter = 256 - value;
            break;
        case 0x73:
            m_wave.volumeCode = (value >> 5) & 0x07;
            break;
        case 0x74:
        case 0x75: {
            const uint16_t cntX = reg16(0x74);
            m_wave.frequency     = cntX & 0x07FF;
            m_wave.lengthEnabled = (cntX & (1u << 14)) != 0;
            if (offset == 0x75 && (value & 0x80) && m_wave.dacEnabled) {
                m_wave.enabled  = true;
                m_wave.position = 0;
                if (m_wave.lengthCounter == 0) m_wave.lengthCounter = 256;
                m_wave.timer = (2048 - m_wave.frequency) * 8;
            }
            break;
        }

        // Canal 4 (LFSR Noise)
        case 0x78:
            m_noise.lengthCounter = 64 - (value & 0x3F);
            break;
        case 0x79:
            m_noise.envelopePeriod   = value & 0x07;
            m_noise.envelopeIncrease = (value & 0x08) != 0;
            m_noise.initialVolume    = (value >> 4) & 0x0F;
            m_noise.volume           = m_noise.initialVolume;
            break;
        case 0x7C:
            m_noise.divisorCode = value & 0x07;
            m_noise.width7Bit   = (value & 0x08) != 0;
            m_noise.clockShift  = (value >> 4) & 0x0F;
            break;
        case 0x7D:
            m_noise.lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80) {
                m_noise.enabled       = true;
                m_noise.volume        = m_noise.initialVolume;
                m_noise.envelopeTimer = m_noise.envelopePeriod ? m_noise.envelopePeriod : 8;
                m_noise.lfsr          = 0x7FFF;
                if (m_noise.lengthCounter == 0) m_noise.lengthCounter = 64;
                m_noise.timer = 64;
            }
            break;

        // Mixagem e Controle Geral
        case 0x80:
        case 0x81:
            m_soundCntL = reg16(0x80);
            break;
        case 0x82:
        case 0x83:
            m_soundCntH = reg16(0x82);
            if (offset == 0x83) {
                if (value & 0x08) m_fifoA.reset(); // Bit 11: Reset FIFO A
                if (value & 0x80) m_fifoB.reset(); // Bit 15: Reset FIFO B
            }
            break;
        case 0x84:
            m_soundCntX = static_cast<uint16_t>(value & 0x80);
            break;
        case 0x88:
        case 0x89:
            m_soundBias = reg16(0x88);
            break;

        default:
            if (offset >= 0x90 && offset < 0xA0) {
                m_wave.waveRam[offset - 0x90] = value;
            } else if (offset >= 0xA0 && offset < 0xA4) {
                m_fifoA.push8(static_cast<int8_t>(value));
            } else if (offset >= 0xA4 && offset < 0xA8) {
                m_fifoB.push8(static_cast<int8_t>(value));
            }
            break;
    }
}

void Apu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_sampleCycleCounter);
    out.writePod(m_frameSequencerCycles);
    out.writePod(m_frameSequencerStep);
    out.writePod(m_sq1);
    out.writePod(m_sq2);
    out.writePod(m_wave);
    out.writePod(m_noise);
    out.writePod(m_fifoA);
    out.writePod(m_fifoB);
    out.writePod(m_regs);
    out.writePod(m_soundCntL);
    out.writePod(m_soundCntH);
    out.writePod(m_soundCntX);
    out.writePod(m_soundBias);
}

bool Apu::deserializeState(SaveStateReader& in) {
    m_ringBuffer.clear();
    return in.readPod(m_sampleCycleCounter) &&
           in.readPod(m_frameSequencerCycles) &&
           in.readPod(m_frameSequencerStep) &&
           in.readPod(m_sq1) &&
           in.readPod(m_sq2) &&
           in.readPod(m_wave) &&
           in.readPod(m_noise) &&
           in.readPod(m_fifoA) &&
           in.readPod(m_fifoB) &&
           in.readPod(m_regs) &&
           in.readPod(m_soundCntL) &&
           in.readPod(m_soundCntH) &&
           in.readPod(m_soundCntX) &&
           in.readPod(m_soundBias);
}

} // namespace gba::core

