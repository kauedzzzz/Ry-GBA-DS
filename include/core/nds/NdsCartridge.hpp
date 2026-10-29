#pragma once
#include "core/SaveStateStream.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gba::core::nds {

enum class NdsBackupType : uint8_t {
    None        = 0,
    Eeprom512B  = 1, // 4 Kbit (1-byte addr + bit 8 in cmd)
    Eeprom8K    = 2, // 64 Kbit (2-byte addr)
    Eeprom64K   = 3, // 512 Kbit (2-byte addr)
    Flash256K   = 4, // 256 KB (3-byte addr)
    Flash512K   = 5, // 512 KB (3-byte addr)
    Flash1M     = 6, // 1 MB (3-byte addr)
    Flash8M     = 7, // 8 MB (3-byte addr)
    Fram32K     = 8  // 32 KB FRAM (2-byte addr)
};

[[nodiscard]] constexpr std::string_view ndsBackupTypeToString(NdsBackupType t) noexcept {
    switch (t) {
        case NdsBackupType::None:       return "NONE";
        case NdsBackupType::Eeprom512B: return "EEPROM 4Kb (512B)";
        case NdsBackupType::Eeprom8K:   return "EEPROM 64Kb (8KB)";
        case NdsBackupType::Eeprom64K:  return "EEPROM 512Kb (64KB)";
        case NdsBackupType::Flash256K:  return "FLASH 256KB";
        case NdsBackupType::Flash512K:  return "FLASH 512KB";
        case NdsBackupType::Flash1M:    return "FLASH 1MB";
        case NdsBackupType::Flash8M:    return "FLASH 8MB";
        case NdsBackupType::Fram32K:    return "FRAM 32KB";
    }
    return "UNKNOWN";
}

struct NdsHeaderInfo {
    std::string gameTitle{};       // 0x000 (12 bytes)
    std::string gameCode{};        // 0x00C (4 bytes)
    std::string makerCode{};       // 0x010 (2 bytes)
    uint8_t     unitCode{0};       // 0x012 (0x00=NDS, 0x02=NDS+DSi, 0x03=DSi)
    uint8_t     deviceCapacity{0}; // 0x014 (128KB << cap)
    uint8_t     romVersion{0};     // 0x01E
    uint8_t     autostartFlags{0}; // 0x01F

    uint32_t    arm9RomOffset{0};    // 0x020
    uint32_t    arm9EntryAddress{0}; // 0x024
    uint32_t    arm9RamAddress{0};   // 0x028
    uint32_t    arm9Size{0};         // 0x02C

    uint32_t    arm7RomOffset{0};    // 0x030
    uint32_t    arm7EntryAddress{0}; // 0x034
    uint32_t    arm7RamAddress{0};   // 0x038
    uint32_t    arm7Size{0};         // 0x03C

    uint32_t    fntOffset{0};        // 0x040
    uint32_t    fntSize{0};          // 0x044
    uint32_t    fatOffset{0};        // 0x048
    uint32_t    fatSize{0};          // 0x04C

    uint32_t    iconBannerOffset{0}; // 0x068
    uint32_t    totalUsedRomSize{0}; // 0x080
    uint32_t    romHeaderSize{0};    // 0x084
    uint16_t    headerCrc16{0};      // 0x15E
    std::size_t actualFileSize{0};

    std::string bannerTitleUtf8{};
    bool        hasBannerIcon{false};
    std::array<uint32_t, 32 * 32> bannerIconArgb{};

    [[nodiscard]] bool isDsiEnhanced() const noexcept { return (unitCode & 0x02u) != 0; }
};

class NdsCartridge {
public:
    static constexpr uint32_t CHIP_ID = 0x00001FC2u;

    NdsCartridge() = default;

    [[nodiscard]] static bool looksLikeNdsRom(std::span<const uint8_t> data) noexcept;

    bool load(std::span<const uint8_t> romData, const std::filesystem::path& romPath = {});
    void reset();

    [[nodiscard]] bool isLoaded() const noexcept { return !m_rom.empty(); }
    [[nodiscard]] const NdsHeaderInfo& header() const noexcept { return m_header; }
    [[nodiscard]] std::span<const uint8_t> rawRom() const noexcept { return m_rom; }
    [[nodiscard]] NdsBackupType backupType() const noexcept { return m_backupType; }

    // Persistência automática do arquivo .sav na mesma pasta da ROM
    bool loadSaveFile(const std::filesystem::path& savPath);
    bool flushSaveFile() const;
    [[nodiscard]] const std::filesystem::path& saveFilePath() const noexcept { return m_savePath; }

    // =========================================================================
    // Interface do Barramento do Cartucho NDS (ROMCTRL / AUXSPICNT / CARDCMD)
    // =========================================================================
    [[nodiscard]] uint16_t readAuxSpiCnt() const noexcept { return m_auxSpiCnt; }
    void writeAuxSpiCnt(uint16_t val) noexcept;

    [[nodiscard]] uint8_t readAuxSpiData() const noexcept { return m_auxSpiData; }
    void writeAuxSpiData(uint8_t val);

    [[nodiscard]] uint32_t readRomCtrl() const noexcept { return m_romCtrl; }
    // Retorna true se a transferência terminou imediatamente ou disparou dados prontos
    void writeRomCtrl(uint32_t val, bool& outDataReady, bool& outTransferDone);

    void writeCardCmd(uint8_t index, uint8_t byte) noexcept {
        if (index < 8) m_cardCmd[index] = byte;
    }
    [[nodiscard]] uint8_t readCardCmd(uint8_t index) const noexcept {
        return index < 8 ? m_cardCmd[index] : 0;
    }

    // Lê 32 bits de CARDDATA (0x04100010); avança a transferência do bloco atual
    [[nodiscard]] uint32_t readCardData(bool& outMoreDataReady, bool& outTransferFinished) noexcept;

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    std::vector<uint8_t>  m_rom;
    NdsHeaderInfo         m_header{};
    NdsBackupType         m_backupType{NdsBackupType::Eeprom64K};
    std::vector<uint8_t>  m_saveRam;
    std::filesystem::path m_savePath;
    mutable bool          m_saveDirty{false};

    // Estado de transferência de ROM (ROMCTRL + buffer de bloco)
    uint16_t               m_auxSpiCnt{0};
    uint8_t                m_auxSpiData{0};
    uint32_t               m_romCtrl{0};
    std::array<uint8_t, 8> m_cardCmd{};
    std::vector<uint8_t>   m_transferBuf;
    std::size_t            m_transferPos{0};

    // Estado da máquina SPI de Backup (.sav EEPROM/Flash/FRAM)
    bool     m_spiHoldActive{false};
    uint8_t  m_spiCmd{0};
    uint32_t m_spiAddr{0};
    uint8_t  m_spiAddrBytesReceived{0};
    uint8_t  m_spiStatusReg{0}; // Bit 0 = WIP, Bit 1 = WEL
    uint32_t m_spiStep{0};

    void parseHeaderAndBanner();
    [[nodiscard]] NdsBackupType detectBackupType() const noexcept;
    void initBackupBuffer(NdsBackupType type);
    [[nodiscard]] uint8_t backupAddrWidth() const noexcept;
    uint8_t transferSpiBackupByte(uint8_t txByte, bool holdCs);
};

} // namespace gba::core::nds

