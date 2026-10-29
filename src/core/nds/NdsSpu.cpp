#include "core/nds/NdsSpu.hpp"
#include "core/nds/NdsBus.hpp"
#include <algorithm>

namespace gba::core::nds {

namespace {

constexpr std::array<int32_t, 89> ADPCM_STEP_TABLE = {{
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
}};

constexpr std::array<int32_t, 8> ADPCM_INDEX_TABLE = {{
    -1, -1, -1, -1, 2, 4, 6, 8
}};

} // namespace

NdsSpu::NdsSpu() {
    reset();
}

void NdsSpu::reset() noexcept {
    m_outSampleAccum = 0;
    for (auto& ch : m_channels) {
        ch = Channel{};
    }
    m_soundCnt  = 0;
    m_soundBias = 0x0200;
    m_captureRegs.fill(0);
    m_ringBuffer.clear();
}

void NdsSpu::startChannel(std::size_t chIdx) noexcept {
    auto& ch = m_channels[chIdx];
    ch.active          = true;
    ch.timerCounter    = static_cast<int32_t>(0x10000u - ch.tmr) * 2; // em ciclos ARM7
    ch.bytePos         = 0;
    ch.currentSample   = 0;
    ch.psgDutyPos      = 0;
    ch.noiseLfsr       = 0x7FFFu;
    ch.adpcmHighNibble = false;

    const uint8_t format = (ch.cnt >> 29) & 0x3u;
    if (format == 2 && m_bus) {
        // Cabeçalho IMA-ADPCM de 32 bits no início de SAD: bits 0..15 = valor inicial, 16..22 = step index
        const uint32_t hdr = m_bus->arm7Read32(ch.sad);
        ch.adpcmValue     = static_cast<int16_t>(hdr & 0xFFFFu);
        ch.adpcmIndex     = std::clamp<int32_t>(static_cast<int32_t>((hdr >> 16) & 0x7Fu), 0, 88);
        ch.adpcmLoopValue = ch.adpcmValue;
        ch.adpcmLoopIndex = ch.adpcmIndex;
        ch.currentSample  = static_cast<int16_t>(ch.adpcmValue);
        ch.bytePos        = 4;
    }
}

void NdsSpu::advanceChannelSample(std::size_t chIdx) noexcept {
    auto& ch = m_channels[chIdx];
    const uint8_t format     = (ch.cnt >> 29) & 0x3u;
    const uint8_t repeatMode = (ch.cnt >> 27) & 0x3u;
    const uint32_t loopStart = static_cast<uint32_t>(ch.pnt) * 4u;
    const uint32_t totalEnd  = loopStart + (ch.len & 0x3FFFFFu) * 4u;

    if (format == 3) {
        // PSG Square (canais 8..13) ou White Noise LFSR (canais 14..15)
        if (chIdx >= 8 && chIdx <= 13) {
            const uint8_t duty = (ch.cnt >> 24) & 0x7u;
            ch.currentSample = (ch.psgDutyPos <= duty) ? int16_t{0x7FFF} : int16_t{-0x7FFF};
            ch.psgDutyPos    = (ch.psgDutyPos + 1) & 0x7u;
        } else if (chIdx >= 14) {
            const bool carry = (ch.noiseLfsr & 1u) != 0;
            ch.noiseLfsr >>= 1;
            if (carry) {
                ch.noiseLfsr ^= 0x6000u;
                ch.currentSample = -0x7FFF;
            } else {
                ch.currentSample = 0x7FFF;
            }
        } else {
            ch.currentSample = 0;
        }
        return;
    }

    if (!m_bus || totalEnd == 0) {
        ch.active = false;
        ch.cnt &= ~(1u << 31);
        return;
    }

    auto handleEndReached = [&]() -> bool {
        if (repeatMode == 1) { // Loop infinito
            ch.bytePos = loopStart;
            if (format == 2) {
                ch.adpcmValue      = ch.adpcmLoopValue;
                ch.adpcmIndex      = ch.adpcmLoopIndex;
                ch.adpcmHighNibble = false;
                if (ch.bytePos < 4) ch.bytePos = 4;
            }
            return true;
        }
        // One-shot terminado
        ch.active = false;
        ch.cnt &= ~(1u << 31);
        if ((ch.cnt & (1u << 15)) == 0) {
            ch.currentSample = 0;
        }
        return false;
    };

    if (ch.bytePos >= totalEnd) {
        if (!handleEndReached()) return;
    }

    if (format == 0) { // PCM8
        const int8_t s8 = static_cast<int8_t>(m_bus->arm7Read8(ch.sad + ch.bytePos));
        ch.currentSample = static_cast<int16_t>(static_cast<int32_t>(s8) << 8);
        ch.bytePos += 1;
    } else if (format == 1) { // PCM16
        ch.currentSample = static_cast<int16_t>(m_bus->arm7Read16(ch.sad + ch.bytePos));
        ch.bytePos += 2;
    } else if (format == 2) { // IMA-ADPCM 4-bit
        if (ch.bytePos == loopStart && !ch.adpcmHighNibble) {
            ch.adpcmLoopValue = ch.adpcmValue;
            ch.adpcmLoopIndex = ch.adpcmIndex;
        }
        const uint8_t raw = m_bus->arm7Read8(ch.sad + ch.bytePos);
        const uint8_t nibble = ch.adpcmHighNibble ? ((raw >> 4) & 0x0Fu) : (raw & 0x0Fu);
        if (ch.adpcmHighNibble) {
            ch.adpcmHighNibble = false;
            ch.bytePos += 1;
        } else {
            ch.adpcmHighNibble = true;
        }

        const int32_t step = ADPCM_STEP_TABLE[static_cast<std::size_t>(ch.adpcmIndex)];
        int32_t diff = step >> 3;
        if (nibble & 1u) diff += step >> 2;
        if (nibble & 2u) diff += step >> 1;
        if (nibble & 4u) diff += step;

        if (nibble & 8u) {
            ch.adpcmValue = std::max<int32_t>(-32767, ch.adpcmValue - diff);
        } else {
            ch.adpcmValue = std::min<int32_t>(32767, ch.adpcmValue + diff);
        }
        ch.adpcmIndex    = std::clamp<int32_t>(ch.adpcmIndex + ADPCM_INDEX_TABLE[nibble & 7u], 0, 88);
        ch.currentSample = static_cast<int16_t>(ch.adpcmValue);
    }
}

void NdsSpu::stepArm7Cycles(int32_t arm7Cycles) noexcept {
    const bool masterEn = (m_soundCnt & (1u << 15)) != 0;

    if (masterEn) {
        for (std::size_t i = 0; i < 16; ++i) {
            auto& ch = m_channels[i];
            if (!ch.active) continue;

            const int32_t period = std::max<int32_t>(4, static_cast<int32_t>(0x10000u - ch.tmr) * 2);
            ch.timerCounter -= arm7Cycles;
            int guard = 0;
            while (ch.timerCounter <= 0 && ch.active && guard < 32) {
                ch.timerCounter += period;
                advanceChannelSample(i);
                ++guard;
            }
            if (ch.timerCounter <= 0) {
                ch.timerCounter = period;
            }
        }
    }

    m_outSampleAccum += arm7Cycles;
    while (m_outSampleAccum >= m_arm7CyclesPerOutSample) {
        m_outSampleAccum -= m_arm7CyclesPerOutSample;
        mixAndPushStereoSample();
    }
}

void NdsSpu::mixAndPushStereoSample() noexcept {
    if (isMuted() || (m_soundCnt & (1u << 15)) == 0) {
        m_ringBuffer.pushStereoSample(0, 0);
        return;
    }

    int64_t mixL = 0;
    int64_t mixR = 0;

    for (std::size_t i = 0; i < 16; ++i) {
        const auto& ch = m_channels[i];
        if (!ch.active) continue;

        const int32_t vol = static_cast<int32_t>(ch.cnt & 0x7Fu);
        if (vol == 0) continue;

        const uint8_t divCode = (ch.cnt >> 8) & 0x3u;
        const int32_t divShift = (divCode == 3) ? 4 : divCode; // 0, 1, 2 ou 4 (/16)
        const int32_t pan = static_cast<int32_t>((ch.cnt >> 16) & 0x7Fu); // 0=Esq .. 127=Dir

        const int64_t scaled = (static_cast<int64_t>(ch.currentSample) * vol) >> (7 + divShift);
        mixL += (scaled * (128 - pan)) >> 7;
        mixR += (scaled * pan) >> 7;
    }

    const int32_t masterVol = static_cast<int32_t>(m_soundCnt & 0x7Fu);
    mixL = (mixL * masterVol) >> 7;
    mixR = (mixR * masterVol) >> 7;

    const float userGain = getVolume();
    const int32_t finalL = std::clamp<int32_t>(static_cast<int32_t>(static_cast<float>(mixL) * userGain), -32768, 32767);
    const int32_t finalR = std::clamp<int32_t>(static_cast<int32_t>(static_cast<float>(mixR) * userGain), -32768, 32767);

    m_ringBuffer.pushStereoSample(static_cast<int16_t>(finalL), static_cast<int16_t>(finalR));
}

uint8_t NdsSpu::read8(uint32_t offset) const noexcept {
    if (offset < 0x100u) {
        const uint32_t w = read32(offset & ~3u);
        return static_cast<uint8_t>((w >> ((offset & 3u) * 8)) & 0xFFu);
    }
    if (offset >= 0x100u && offset < 0x104u) {
        const uint16_t v = m_soundCnt;
        return static_cast<uint8_t>((offset == 0x100u) ? (v & 0xFFu) : ((v >> 8) & 0xFFu));
    }
    if (offset >= 0x104u && offset < 0x108u) {
        const uint16_t v = m_soundBias;
        return static_cast<uint8_t>((offset == 0x104u) ? (v & 0xFFu) : ((v >> 8) & 0xFFu));
    }
    if (offset >= 0x108u && offset < 0x120u) {
        return m_captureRegs[offset - 0x108u];
    }
    return 0;
}

uint16_t NdsSpu::read16(uint32_t offset) const noexcept {
    return static_cast<uint16_t>(read8(offset)) |
           (static_cast<uint16_t>(read8(offset + 1)) << 8);
}

uint32_t NdsSpu::read32(uint32_t offset) const noexcept {
    if (offset < 0x100u) {
        const std::size_t chIdx = (offset >> 4) & 0xFu;
        const uint32_t reg      = offset & 0xCu;
        const auto& ch          = m_channels[chIdx];
        switch (reg) {
            case 0x0: return (ch.cnt & 0x7FFF7F7Fu) | (ch.active ? (1u << 31) : 0u);
            default:  return 0; // SAD/TMR/PNT/LEN são write-only no hardware NDS
        }
    }
    return static_cast<uint32_t>(read16(offset)) |
           (static_cast<uint32_t>(read16(offset + 2)) << 16);
}

void NdsSpu::write8(uint32_t offset, uint8_t val) noexcept {
    if (offset < 0x100u) {
        const std::size_t chIdx = (offset >> 4) & 0xFu;
        const uint32_t sub      = offset & 0xFu;
        auto& ch                = m_channels[chIdx];
        if (sub < 4) {
            const uint32_t shift = sub * 8;
            const bool wasEn = ch.active;
            ch.cnt = (ch.cnt & ~(0xFFu << shift)) | (static_cast<uint32_t>(val) << shift);
            if (sub == 3) {
                const bool nowEn = (val & 0x80u) != 0;
                if (!wasEn && nowEn) startChannel(chIdx);
                else if (!nowEn)     ch.active = false;
            }
        } else if (sub < 8) {
            const uint32_t shift = (sub - 4) * 8;
            ch.sad = ((ch.sad & ~(0xFFu << shift)) | (static_cast<uint32_t>(val) << shift)) & 0x07FFFFFCu;
        } else if (sub < 10) {
            const uint32_t shift = (sub - 8) * 8;
            ch.tmr = static_cast<uint16_t>((ch.tmr & ~(0xFFu << shift)) | (static_cast<uint16_t>(val) << shift));
        } else if (sub < 12) {
            const uint32_t shift = (sub - 10) * 8;
            ch.pnt = static_cast<uint16_t>((ch.pnt & ~(0xFFu << shift)) | (static_cast<uint16_t>(val) << shift));
        } else {
            const uint32_t shift = (sub - 12) * 8;
            ch.len = ((ch.len & ~(0xFFu << shift)) | (static_cast<uint32_t>(val) << shift)) & 0x003FFFFFu;
        }
        return;
    }

    if (offset == 0x100u)      m_soundCnt  = static_cast<uint16_t>((m_soundCnt & 0xFF00u) | val);
    else if (offset == 0x101u) m_soundCnt  = static_cast<uint16_t>((m_soundCnt & 0x00FFu) | (static_cast<uint16_t>(val) << 8));
    else if (offset == 0x104u) m_soundBias = static_cast<uint16_t>((m_soundBias & 0xFF00u) | val);
    else if (offset == 0x105u) m_soundBias = static_cast<uint16_t>((m_soundBias & 0x00FFu) | (static_cast<uint16_t>(val & 0x03u) << 8));
    else if (offset >= 0x108u && offset < 0x120u) {
        m_captureRegs[offset - 0x108u] = val;
    }
}

void NdsSpu::write16(uint32_t offset, uint16_t val) noexcept {
    write8(offset, static_cast<uint8_t>(val & 0xFFu));
    write8(offset + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsSpu::write32(uint32_t offset, uint32_t val) noexcept {
    if (offset < 0x100u && (offset & 0xFu) == 0u) {
        const std::size_t chIdx = (offset >> 4) & 0xFu;
        auto& ch                = m_channels[chIdx];
        const bool wasEn        = ch.active;
        const bool nowEn        = (val & (1u << 31)) != 0;
        ch.cnt = val;
        if (!wasEn && nowEn) startChannel(chIdx);
        else if (!nowEn)     ch.active = false;
        return;
    }
    write16(offset, static_cast<uint16_t>(val & 0xFFFFu));
    write16(offset + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

void NdsSpu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_outSampleAccum);
    out.writeArray(m_channels);
    out.writePod(m_soundCnt);
    out.writePod(m_soundBias);
    out.writeArray(m_captureRegs);
}

bool NdsSpu::deserializeState(SaveStateReader& in) {
    if (!in.readPod(m_outSampleAccum) || !in.readArray(m_channels) ||
        !in.readPod(m_soundCnt) || !in.readPod(m_soundBias) || !in.readArray(m_captureRegs)) {
        return false;
    }
    m_ringBuffer.clear();
    return true;
}

} // namespace gba::core::nds

