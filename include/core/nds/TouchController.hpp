#pragma once
#include "core/SaveStateStream.hpp"
#include <cstdint>

namespace gba::core::nds {

// =============================================================================
// Controlador SPI do ARM7: Touchscreen TSC2046, Microfone, Power Manager e Firmware
// =============================================================================
class TouchController {
public:
    TouchController();

    void reset() noexcept;

    // Atualiza estado da caneta Stylus (x: 0..255, y: 0..191)
    void setStylusState(int x, int y, bool touching) noexcept;
    [[nodiscard]] bool isTouching() const noexcept { return m_touching; }
    [[nodiscard]] int stylusX() const noexcept { return m_touchX; }
    [[nodiscard]] int stylusY() const noexcept { return m_touchY; }

    // Simulação de sopro no microfone (gerador LFSR de ruído branco no ADC do mic)
    void setMicBlowing(bool active) noexcept { m_micBlowing = active; }
    [[nodiscard]] bool isMicBlowing() const noexcept { return m_micBlowing; }

    // Simulação de abrir/fechar a tampa (Sleep Mode / Hinge)
    void setLidClosed(bool closed) noexcept { m_lidClosed = closed; }
    [[nodiscard]] bool isLidClosed() const noexcept { return m_lidClosed; }

    // Carrega firmware externo opcional (256 KB); caso ausente, usa Firmware HLE embutido
    void loadOptionalFirmware(const uint8_t* data, std::size_t size);
    void populateDefaultFirmwareUserSettings(uint8_t* dest128Bytes) const noexcept;

    // Registradores SPI do ARM7 (0x040001C0 = SPICNT, 0x040001C2 = SPIDATA)
    [[nodiscard]] uint16_t readSpiCnt() const noexcept { return m_spiCnt; }
    void writeSpiCnt(uint16_t val) noexcept;

    [[nodiscard]] uint8_t readSpiData() const noexcept { return m_spiData; }
    void writeSpiData(uint8_t val) noexcept;

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    int  m_touchX{128};
    int  m_touchY{96};
    bool m_touching{false};
    bool m_micBlowing{false};
    bool m_lidClosed{false};

    uint16_t m_spiCnt{0};
    uint8_t  m_spiData{0};
    bool     m_spiChipSelected{false};
    uint32_t m_spiByteIndex{0};

    // Dispositivo 0: Power Manager
    uint8_t  m_pmRegSelect{0};
    uint8_t  m_pmControl{0x0D}; // Backlights superior e inferior acesos
    uint8_t  m_pmMicAmp{0x01};
    uint8_t  m_pmMicGain{0x02};

    // Dispositivo 1: Firmware NVRAM (256 KB)
    uint8_t  m_fwCmd{0};
    uint32_t m_fwAddr{0};
    uint8_t  m_fwStatus{0};
    bool     m_hasCustomFirmware{false};
    uint8_t  m_firmware[256 * 1024]{};

    // Dispositivo 2: Touchscreen / Microfone ADC (TSC2046)
    uint16_t m_tscOutputShift{0};
    uint32_t m_micLfsr{0xACE1BEEFu};

    void buildHleFirmware() noexcept;
    [[nodiscard]] uint16_t sampleMicWhiteNoise12Bit() noexcept;
    uint8_t transferPowerMgr(uint8_t tx) noexcept;
    uint8_t transferFirmware(uint8_t tx) noexcept;
    uint8_t transferTsc2046(uint8_t tx) noexcept;
};

} // namespace gba::core::nds

