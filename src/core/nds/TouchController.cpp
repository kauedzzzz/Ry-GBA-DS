#include "core/nds/TouchController.hpp"
#include <algorithm>
#include <cstring>

namespace gba::core::nds {

namespace {

[[nodiscard]] uint16_t calcNdsCrc16(const uint8_t* data, std::size_t length) noexcept {
    static constexpr uint16_t VALS[8] = {
        0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001
    };
    uint32_t crc = 0xFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ (static_cast<uint32_t>(VALS[b]) << (7 - b));
            } else {
                crc >>= 1;
            }
        }
    }
    return static_cast<uint16_t>(crc & 0xFFFFu);
}

void putLe16(uint8_t* dst, std::size_t off, uint16_t v) noexcept {
    dst[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    dst[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

} // namespace

TouchController::TouchController() {
    buildHleFirmware();
    reset();
}

void TouchController::reset() noexcept {
    m_touchX     = 128;
    m_touchY     = 96;
    m_touching   = false;
    m_micBlowing = false;
    m_lidClosed  = false;

    m_spiCnt          = 0;
    m_spiData         = 0;
    m_spiChipSelected = false;
    m_spiByteIndex    = 0;

    m_pmRegSelect   = 0;
    m_pmControl     = 0x0Du;
    m_pmMicAmp      = 0x01u;
    m_pmMicGain     = 0x02u;
    m_fwCmd         = 0;
    m_fwAddr        = 0;
    m_fwStatus      = 0;
    m_tscOutputShift = 0;

    if (!m_hasCustomFirmware) {
        buildHleFirmware();
    }
}

void TouchController::setStylusState(int x, int y, bool touching) noexcept {
    m_touching = touching;
    if (touching) {
        m_touchX = std::clamp(x, 0, 255);
        m_touchY = std::clamp(y, 0, 191);
    }
}

void TouchController::loadOptionalFirmware(const uint8_t* data, std::size_t size) {
    if (!data || size < 128 * 1024) return;
    std::memset(m_firmware, 0xFF, sizeof(m_firmware));
    std::memcpy(m_firmware, data, std::min<std::size_t>(size, sizeof(m_firmware)));
    m_hasCustomFirmware = true;
}

void TouchController::populateDefaultFirmwareUserSettings(uint8_t* dst) const noexcept {
    std::memset(dst, 0, 0x80);
    putLe16(dst, 0x00, 5); // Versão 5 (NDS padrão)
    dst[0x02] = 2;         // Cor favorita (Vermelho Dracônico)
    dst[0x03] = 10;        // Mês aniversário
    dst[0x04] = 29;        // Dia aniversário

    // Nickname UTF-16LE: "RyuDS" (0x06..0x19, 10 chars)
    const char nick[] = "RyuDS";
    for (std::size_t i = 0; i < 5; ++i) {
        putLe16(dst, 0x06 + i * 2, static_cast<uint16_t>(nick[i]));
    }
    putLe16(dst, 0x1A, 5); // Tamanho do nickname

    // Calibração linear exata 1:1 (ADC = pixel * 16):
    // Ponto 1: Pixel (32, 32)   -> ADC (0x0200, 0x0200)
    // Ponto 2: Pixel (224, 160) -> ADC (0x0E00, 0x0A00)
    putLe16(dst, 0x58, 0x0200u); // adc.x1
    putLe16(dst, 0x5A, 0x0200u); // adc.y1
    dst[0x5C] = 32;              // scr.x1
    dst[0x5D] = 32;              // scr.y1
    putLe16(dst, 0x5E, 0x0E00u); // adc.x2
    putLe16(dst, 0x60, 0x0A00u); // adc.y2
    dst[0x62] = 224;             // scr.x2
    dst[0x63] = 160;             // scr.y2

    // Language = English (1) + Flags válidas
    putLe16(dst, 0x64, 0x0001u);
    dst[0x66] = 0;

    // CRC16 dos primeiros 0x70 bytes gravado em 0x72
    const uint16_t crc = calcNdsCrc16(dst, 0x70);
    putLe16(dst, 0x72, crc);
}

void TouchController::buildHleFirmware() noexcept {
    std::memset(m_firmware, 0xFF, sizeof(m_firmware));
    // Assinatura MAC e ponteiro User Settings (0x3FE00 / 8 = 0x7FC0) no cabeçalho do Firmware
    putLe16(m_firmware, 0x20, 0x7FC0u);
    m_firmware[0x36] = 0x00;
    m_firmware[0x37] = 0x09;
    m_firmware[0x38] = 0xBF;
    m_firmware[0x39] = 0x12;
    m_firmware[0x3A] = 0x34;
    m_firmware[0x3B] = 0x56;

    // Slots 0 e 1 de User Settings em 0x3FE00 e 0x3FF00
    populateDefaultFirmwareUserSettings(&m_firmware[0x3FE00]);
    populateDefaultFirmwareUserSettings(&m_firmware[0x3FF00]);
}

uint16_t TouchController::sampleMicWhiteNoise12Bit() noexcept {
    if (!m_micBlowing) {
        return 0x0800u; // Ponto médio DC silencioso (12 bits = 2048)
    }
    // LFSR Galois 32 bits simulando rajada realista de sopro no microfone
    const uint32_t lsb = m_micLfsr & 1u;
    m_micLfsr >>= 1;
    if (lsb) m_micLfsr ^= 0xB4000001u;
    const int32_t noise = static_cast<int32_t>(m_micLfsr & 0x7FFu) - 1024;
    return static_cast<uint16_t>(std::clamp(2048 + noise, 64, 4032));
}

void TouchController::writeSpiCnt(uint16_t val) noexcept {
    const bool    enable    = (val & (1u << 15)) != 0;
    const uint8_t oldDevice = (m_spiCnt >> 8) & 0x3u;
    const uint8_t newDevice = (val >> 8) & 0x3u;
    if (!enable || oldDevice != newDevice) {
        m_spiChipSelected = false;
        m_spiByteIndex    = 0;
    }
    m_spiCnt = val & 0xCF03u; // Busy (bit 7) sempre 0 (transferência instantânea)
}

void TouchController::writeSpiData(uint8_t val) noexcept {
    if ((m_spiCnt & (1u << 15)) == 0) {
        m_spiData = 0;
        return;
    }

    const uint8_t device = (m_spiCnt >> 8) & 0x3u;
    switch (device) {
        case 0:  m_spiData = transferPowerMgr(val); break;
        case 1:  m_spiData = transferFirmware(val); break;
        case 2:  m_spiData = transferTsc2046(val);  break;
        default: m_spiData = 0x00u;                 break;
    }

    const bool hold = (m_spiCnt & (1u << 11)) != 0;
    if (hold) {
        m_spiChipSelected = true;
        ++m_spiByteIndex;
    } else {
        m_spiChipSelected = false;
        m_spiByteIndex    = 0;
    }
}

uint8_t TouchController::transferPowerMgr(uint8_t tx) noexcept {
    if (m_spiByteIndex == 0) {
        m_pmRegSelect = tx;
        return 0x00u;
    }
    const bool isRead = (m_pmRegSelect & 0x80u) != 0;
    const uint8_t reg = m_pmRegSelect & 0x07u;
    if (isRead) {
        switch (reg) {
            case 0: return m_pmControl;
            case 1: return 0x00u; // Bateria cheia (bit 0 = 0 -> OK)
            case 2: return m_pmMicAmp;
            case 3: return m_pmMicGain;
            case 4: return 0x43u; // Backlight level
            default: return 0x00u;
        }
    } else {
        switch (reg) {
            case 0: m_pmControl = tx; break;
            case 2: m_pmMicAmp  = tx & 0x01u; break;
            case 3: m_pmMicGain = tx & 0x03u; break;
            default: break;
        }
        return 0x00u;
    }
}

uint8_t TouchController::transferFirmware(uint8_t tx) noexcept {
    if (m_spiByteIndex == 0) {
        m_fwCmd  = tx;
        m_fwAddr = 0;
        if (m_fwCmd == 0x06) m_fwStatus |= 0x02u;
        if (m_fwCmd == 0x04) m_fwStatus &= ~0x02u;
        return 0x00u;
    }

    switch (m_fwCmd) {
        case 0x05: // RDSR
            return m_fwStatus;
        case 0x03: // READ
            if (m_spiByteIndex <= 3) {
                m_fwAddr = (m_fwAddr << 8) | tx;
                return 0x00u;
            } else {
                const uint8_t out = m_firmware[m_fwAddr & 0x3FFFFu];
                ++m_fwAddr;
                return out;
            }
        case 0x9F: // JEDEC ID
            if (m_spiByteIndex == 1) return 0x20u;
            if (m_spiByteIndex == 2) return 0x40u;
            if (m_spiByteIndex == 3) return 0x12u;
            return 0x00u;
        default:
            return 0xFFu;
    }
}

uint8_t TouchController::transferTsc2046(uint8_t tx) noexcept {
    const uint8_t outByte = static_cast<uint8_t>((m_tscOutputShift >> 8) & 0xFFu);
    m_tscOutputShift <<= 8;

    // Se o bit 7 (Start Bit do TSC2046) estiver setado, inicia nova conversão ADC 12 bits
    if ((tx & 0x80u) != 0) {
        const uint8_t channel = (tx >> 4) & 0x07u;
        uint16_t adc12 = 0x000u;

        switch (channel) {
            case 1: // Touchscreen Y-Position (0..191 -> 0..3056)
                adc12 = m_touching ? static_cast<uint16_t>(m_touchY * 16) : 0x0FFFu;
                break;
            case 3: // Touchscreen Z1 (Pressão)
                adc12 = m_touching ? 0x0400u : 0x0000u;
                break;
            case 4: // Touchscreen Z2
                adc12 = m_touching ? 0x0800u : 0x0FFFu;
                break;
            case 5: // Touchscreen X-Position (0..255 -> 0..4080)
                adc12 = m_touching ? static_cast<uint16_t>(m_touchX * 16) : 0x0000u;
                break;
            case 6: // AUX / Microfone
                adc12 = sampleMicWhiteNoise12Bit();
                break;
            case 0: // Temp 0
                adc12 = 0x0640u;
                break;
            case 7: // Temp 1
                adc12 = 0x0780u;
                break;
            default:
                adc12 = 0x0800u;
                break;
        }

        // Alinhamento serial TSC2046: próximos 16 clocks entregam (adc12 << 3) & 0x7FF8
        m_tscOutputShift = static_cast<uint16_t>((adc12 & 0x0FFFu) << 3);
    }

    return outByte;
}

void TouchController::serializeState(SaveStateWriter& out) const {
    out.writePod(m_touchX);
    out.writePod(m_touchY);
    out.writePod(m_touching);
    out.writePod(m_micBlowing);
    out.writePod(m_lidClosed);
    out.writePod(m_spiCnt);
    out.writePod(m_spiData);
    out.writePod(m_spiChipSelected);
    out.writePod(m_spiByteIndex);
    out.writePod(m_pmRegSelect);
    out.writePod(m_pmControl);
    out.writePod(m_pmMicAmp);
    out.writePod(m_pmMicGain);
    out.writePod(m_fwCmd);
    out.writePod(m_fwAddr);
    out.writePod(m_fwStatus);
    out.writePod(m_tscOutputShift);
    out.writePod(m_micLfsr);
}

bool TouchController::deserializeState(SaveStateReader& in) {
    return in.readPod(m_touchX) && in.readPod(m_touchY) && in.readPod(m_touching) &&
           in.readPod(m_micBlowing) && in.readPod(m_lidClosed) && in.readPod(m_spiCnt) &&
           in.readPod(m_spiData) && in.readPod(m_spiChipSelected) && in.readPod(m_spiByteIndex) &&
           in.readPod(m_pmRegSelect) && in.readPod(m_pmControl) && in.readPod(m_pmMicAmp) &&
           in.readPod(m_pmMicGain) && in.readPod(m_fwCmd) && in.readPod(m_fwAddr) &&
           in.readPod(m_fwStatus) && in.readPod(m_tscOutputShift) && in.readPod(m_micLfsr);
}

} // namespace gba::core::nds
