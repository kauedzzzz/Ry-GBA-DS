#include "core/nds/NdsCartridge.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace gba::core::nds {

namespace {

[[nodiscard]] uint16_t readLe16(std::span<const uint8_t> buf, std::size_t off) noexcept {
    if (off + 2 > buf.size()) return 0;
    return static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
}

[[nodiscard]] uint32_t readLe32(std::span<const uint8_t> buf, std::size_t off) noexcept {
    if (off + 4 > buf.size()) return 0;
    return static_cast<uint32_t>(buf[off]) |
           (static_cast<uint32_t>(buf[off + 1]) << 8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

[[nodiscard]] uint32_t bgr555ToArgb8888(uint16_t c) noexcept {
    const uint32_t r5 = c & 0x1Fu;
    const uint32_t g5 = (c >> 5) & 0x1Fu;
    const uint32_t b5 = (c >> 10) & 0x1Fu;
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g5 << 3) | (g5 >> 2);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

std::string decodeUtf16LeBannerTitle(std::span<const uint8_t> bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const uint16_t ch = static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i + 1]) << 8);
        if (ch == 0) break;
        if (ch == '\n' || ch == '\r') {
            if (!out.empty() && out.back() != ' ') out += " - ";
            continue;
        }
        if (ch < 0x80) {
            if (ch >= 32) out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0u | (ch >> 6)));
            out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xE0u | (ch >> 12)));
            out.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        }
    }
    return out;
}

bool containsAsciiToken(std::span<const uint8_t> hay, std::string_view needle) noexcept {
    if (needle.empty() || hay.size() < needle.size()) return false;
    const auto it = std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }
    );
    return it != hay.end();
}

} // namespace

bool NdsCartridge::looksLikeNdsRom(std::span<const uint8_t> data) noexcept {
    if (data.size() < 0x200) return false;
    const uint32_t arm9RomOff = readLe32(data, 0x020);
    const uint32_t arm9Entry  = readLe32(data, 0x024);
    const uint32_t arm9Ram    = readLe32(data, 0x028);
    const uint32_t arm9Size   = readLe32(data, 0x02C);
    const uint32_t arm7RomOff = readLe32(data, 0x030);
    const uint32_t arm7Entry  = readLe32(data, 0x034);
    const uint32_t arm7Ram    = readLe32(data, 0x038);
    const uint32_t arm7Size   = readLe32(data, 0x03C);

    const bool arm9Valid = (arm9Entry >= 0x01FF8000u && arm9Entry < 0x02FFFFFFu) &&
                           (arm9Ram >= 0x01FF8000u && arm9Ram < 0x02FFFFFFu) &&
                           (arm9RomOff >= 0x200u && arm9RomOff < data.size()) &&
                           (arm9Size > 0 && arm9Size <= 0x00800000u);

    const bool arm7Valid = ((arm7Entry >= 0x02000000u && arm7Entry < 0x02FFFFFFu) ||
                            (arm7Entry >= 0x037F8000u && arm7Entry < 0x03810000u)) &&
                           ((arm7Ram >= 0x02000000u && arm7Ram < 0x02FFFFFFu) ||
                            (arm7Ram >= 0x037F8000u && arm7Ram < 0x03810000u)) &&
                           (arm7RomOff >= 0x200u && arm7RomOff < data.size()) &&
                           (arm7Size > 0 && arm7Size <= 0x00400000u);

    return arm9Valid && arm7Valid;
}

bool NdsCartridge::load(std::span<const uint8_t> romData, const std::filesystem::path& romPath) {
    if (!looksLikeNdsRom(romData)) {
        return false;
    }

    m_rom.assign(romData.begin(), romData.end());
    parseHeaderAndBanner();

    m_backupType = detectBackupType();
    initBackupBuffer(m_backupType);

    m_savePath.clear();
    if (!romPath.empty()) {
        m_savePath = romPath;
        m_savePath.replace_extension(".sav");
        loadSaveFile(m_savePath);
    }

    reset();

    std::cout << "[NDS Cartridge] Loaded: \"" << m_header.gameTitle << "\""
              << " | Code=" << m_header.gameCode
              << " | Maker=" << m_header.makerCode
              << " | Unit=" << (m_header.isDsiEnhanced() ? "DSi/NDS" : "NDS")
              << " | Size=" << (m_rom.size() / 1024) << " KB"
              << " | ARM9=0x" << std::hex << m_header.arm9EntryAddress
              << " (" << std::dec << (m_header.arm9Size / 1024) << " KB)"
              << " | ARM7=0x" << std::hex << m_header.arm7EntryAddress << std::dec
              << " | Save=" << ndsBackupTypeToString(m_backupType);
    if (!m_header.bannerTitleUtf8.empty()) {
        std::cout << " | Banner=\"" << m_header.bannerTitleUtf8 << "\"";
    }
    std::cout << "\n";
    return true;
}

void NdsCartridge::reset() {
    m_auxSpiCnt   = 0;
    m_auxSpiData  = 0;
    m_romCtrl     = 0;
    m_cardCmd.fill(0);
    m_transferBuf.clear();
    m_transferPos = 0;

    m_spiHoldActive        = false;
    m_spiCmd               = 0;
    m_spiAddr              = 0;
    m_spiAddrBytesReceived = 0;
    m_spiStatusReg         = 0;
    m_spiStep              = 0;
}

void NdsCartridge::parseHeaderAndBanner() {
    m_header = NdsHeaderInfo{};
    m_header.actualFileSize = m_rom.size();

    auto readFixedAscii = [&](std::size_t off, std::size_t len) {
        std::string s;
        for (std::size_t i = 0; i < len && off + i < m_rom.size(); ++i) {
            const char c = static_cast<char>(m_rom[off + i]);
            if (c == '\0') break;
            if (c >= 32 && c <= 126) s.push_back(c);
        }
        return s;
    };

    m_header.gameTitle      = readFixedAscii(0x000, 12);
    m_header.gameCode       = readFixedAscii(0x00C, 4);
    m_header.makerCode      = readFixedAscii(0x010, 2);
    m_header.unitCode       = m_rom[0x012];
    m_header.deviceCapacity = m_rom[0x014];
    m_header.romVersion     = m_rom[0x01E];
    m_header.autostartFlags = m_rom[0x01F];

    m_header.arm9RomOffset    = readLe32(m_rom, 0x020);
    m_header.arm9EntryAddress = readLe32(m_rom, 0x024);
    m_header.arm9RamAddress   = readLe32(m_rom, 0x028);
    m_header.arm9Size         = readLe32(m_rom, 0x02C);

    m_header.arm7RomOffset    = readLe32(m_rom, 0x030);
    m_header.arm7EntryAddress = readLe32(m_rom, 0x034);
    m_header.arm7RamAddress   = readLe32(m_rom, 0x038);
    m_header.arm7Size         = readLe32(m_rom, 0x03C);

    m_header.fntOffset        = readLe32(m_rom, 0x040);
    m_header.fntSize          = readLe32(m_rom, 0x044);
    m_header.fatOffset        = readLe32(m_rom, 0x048);
    m_header.fatSize          = readLe32(m_rom, 0x04C);

    m_header.iconBannerOffset = readLe32(m_rom, 0x068);
    m_header.totalUsedRomSize = readLe32(m_rom, 0x080);
    m_header.romHeaderSize    = readLe32(m_rom, 0x084);
    m_header.headerCrc16      = readLe16(m_rom, 0x15E);

    if (m_header.gameTitle.empty()) {
        m_header.gameTitle = "NDS ROM";
    }

    // Extração do Banner 32x32 16 cores (4x4 tiles de 8x8 em 4bpp) + Título UTF-16LE
    const uint32_t bOff = m_header.iconBannerOffset;
    if (bOff >= 0x200u && static_cast<std::size_t>(bOff) + 0x840u <= m_rom.size()) {
        std::array<uint32_t, 16> palette{};
        palette[0] = 0x00000000u; // Cor 0 é transparente no banner do NDS
        for (std::size_t c = 1; c < 16; ++c) {
            palette[c] = bgr555ToArgb8888(readLe16(m_rom, bOff + 0x220u + c * 2));
        }

        for (int ty = 0; ty < 4; ++ty) {
            for (int tx = 0; tx < 4; ++tx) {
                const std::size_t tileBase = bOff + 0x020u + static_cast<std::size_t>((ty * 4 + tx) * 32);
                for (int py = 0; py < 8; ++py) {
                    for (int px = 0; px < 8; px += 2) {
                        const uint8_t packed = m_rom[tileBase + static_cast<std::size_t>(py * 4 + (px / 2))];
                        const uint8_t c0 = packed & 0x0Fu;
                        const uint8_t c1 = (packed >> 4) & 0x0Fu;
                        const int dstY = ty * 8 + py;
                        const int dstX = tx * 8 + px;
                        m_header.bannerIconArgb[static_cast<std::size_t>(dstY * 32 + dstX)]     = palette[c0];
                        m_header.bannerIconArgb[static_cast<std::size_t>(dstY * 32 + dstX + 1)] = palette[c1];
                    }
                }
            }
        }
        m_header.hasBannerIcon = true;

        // Título em Inglês (offset 0x340, 256 bytes UTF-16LE); fallback para slot 0 (0x240)
        m_header.bannerTitleUtf8 = decodeUtf16LeBannerTitle(
            std::span<const uint8_t>(m_rom.data() + bOff + 0x340u, 256)
        );
        if (m_header.bannerTitleUtf8.empty()) {
            m_header.bannerTitleUtf8 = decodeUtf16LeBannerTitle(
                std::span<const uint8_t>(m_rom.data() + bOff + 0x240u, 256)
            );
        }
    }
}

NdsBackupType NdsCartridge::detectBackupType() const noexcept {
    // 1. Tabela conhecida por GameCode (Pokémon Diamond/Pearl/Platinum/HG/SS/BW, Mario, Zelda, etc.)
    const std::string& code = m_header.gameCode;
    if (code.size() >= 3) {
        const std::string prefix3 = code.substr(0, 3);
        // Pokémon Gen 4 & Gen 5 usam Flash 512KB
        if (prefix3 == "ADA" || prefix3 == "APA" || prefix3 == "CPU" ||
            prefix3 == "IPK" || prefix3 == "IPG" || prefix3 == "IRB" ||
            prefix3 == "IRA" || prefix3 == "IRE" || prefix3 == "IRD") {
            return NdsBackupType::Flash512K;
        }
        // Zelda Phantom Hourglass / Spirit Tracks (Flash 512K / 1M)
        if (prefix3 == "AZE" || prefix3 == "BKI") {
            return NdsBackupType::Flash512K;
        }
        // New Super Mario Bros / Mario Kart DS (EEPROM 64Kb / 512Kb)
        if (prefix3 == "A2D") return NdsBackupType::Eeprom8K;
        if (prefix3 == "AMC" || prefix3 == "ASM") return NdsBackupType::Eeprom64K;
    }

    // 2. Varredura de strings do SDK Nintendo Nitro dentro do binário ARM9/ARM7
    const std::size_t scanLimit = std::min<std::size_t>(m_rom.size(), 4 * 1024 * 1024);
    const std::span<const uint8_t> window(m_rom.data(), scanLimit);

    if (containsAsciiToken(window, "FLASH4M_V") || containsAsciiToken(window, "FLASH512_V")) {
        return NdsBackupType::Flash512K;
    }
    if (containsAsciiToken(window, "FLASH8M_V")) {
        return NdsBackupType::Flash1M;
    }
    if (containsAsciiToken(window, "FLASH2M_V")) {
        return NdsBackupType::Flash256K;
    }
    if (containsAsciiToken(window, "FRAM_V")) {
        return NdsBackupType::Fram32K;
    }
    if (containsAsciiToken(window, "EEPROM_V12") || containsAsciiToken(window, "EEPROM512_V")) {
        return NdsBackupType::Eeprom64K;
    }
    if (containsAsciiToken(window, "EEPROM64_V")) {
        return NdsBackupType::Eeprom8K;
    }
    if (containsAsciiToken(window, "EEPROM4_V")) {
        return NdsBackupType::Eeprom512B;
    }

    // Padrão universal seguro compatível com homebrew e maioria dos jogos comerciais
    return (m_rom.size() >= 32 * 1024 * 1024) ? NdsBackupType::Flash512K : NdsBackupType::Eeprom64K;
}

void NdsCartridge::initBackupBuffer(NdsBackupType type) {
    std::size_t bytes = 64 * 1024;
    switch (type) {
        case NdsBackupType::None:       bytes = 0; break;
        case NdsBackupType::Eeprom512B: bytes = 512; break;
        case NdsBackupType::Eeprom8K:   bytes = 8 * 1024; break;
        case NdsBackupType::Eeprom64K:  bytes = 64 * 1024; break;
        case NdsBackupType::Flash256K:  bytes = 256 * 1024; break;
        case NdsBackupType::Flash512K:  bytes = 512 * 1024; break;
        case NdsBackupType::Flash1M:    bytes = 1024 * 1024; break;
        case NdsBackupType::Flash8M:    bytes = 8 * 1024 * 1024; break;
        case NdsBackupType::Fram32K:    bytes = 32 * 1024; break;
    }
    m_saveRam.assign(bytes, 0xFFu);
    m_saveDirty = false;
}

uint8_t NdsCartridge::backupAddrWidth() const noexcept {
    switch (m_backupType) {
        case NdsBackupType::Eeprom512B: return 1;
        case NdsBackupType::Eeprom8K:
        case NdsBackupType::Eeprom64K:
        case NdsBackupType::Fram32K:    return 2;
        case NdsBackupType::Flash256K:
        case NdsBackupType::Flash512K:
        case NdsBackupType::Flash1M:
        case NdsBackupType::Flash8M:    return 3;
        default:                        return 2;
    }
}

bool NdsCartridge::loadSaveFile(const std::filesystem::path& savPath) {
    m_savePath = savPath;
    std::error_code ec;
    if (!std::filesystem::exists(savPath, ec)) {
        // Cria arquivo .sav inicial na mesma pasta da ROM
        if (!m_saveRam.empty()) {
            flushSaveFile();
            m_saveDirty = false;
        }
        return false;
    }

    const auto diskSize = std::filesystem::file_size(savPath, ec);
    if (!ec && diskSize > 0) {
        // Ajusta tipo de backup caso um .sav existente tenha tamanho específico conhecido
        if (diskSize == 512)              initBackupBuffer(m_backupType = NdsBackupType::Eeprom512B);
        else if (diskSize == 8 * 1024)    initBackupBuffer(m_backupType = NdsBackupType::Eeprom8K);
        else if (diskSize == 32 * 1024)   initBackupBuffer(m_backupType = NdsBackupType::Fram32K);
        else if (diskSize == 64 * 1024)   initBackupBuffer(m_backupType = NdsBackupType::Eeprom64K);
        else if (diskSize == 256 * 1024)  initBackupBuffer(m_backupType = NdsBackupType::Flash256K);
        else if (diskSize == 512 * 1024)  initBackupBuffer(m_backupType = NdsBackupType::Flash512K);
        else if (diskSize == 1024 * 1024) initBackupBuffer(m_backupType = NdsBackupType::Flash1M);
    }

    std::ifstream in(savPath, std::ios::binary);
    if (!in || m_saveRam.empty()) return false;
    in.read(reinterpret_cast<char*>(m_saveRam.data()), static_cast<std::streamsize>(m_saveRam.size()));
    m_saveDirty = false;
    return true;
}

bool NdsCartridge::flushSaveFile() const {
    if (m_savePath.empty() || m_saveRam.empty()) return false;
    std::ofstream out(m_savePath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(m_saveRam.data()), static_cast<std::streamsize>(m_saveRam.size()));
    m_saveDirty = false;
    return out.good();
}

void NdsCartridge::writeAuxSpiCnt(uint16_t val) noexcept {
    const bool enable  = (val & (1u << 15)) != 0;
    const bool spiMode = (val & (1u << 13)) != 0;
    if (!enable || !spiMode) {
        if (m_saveDirty) {
            flushSaveFile();
        }
        m_spiHoldActive = false;
        m_spiStep       = 0;
    }
    m_auxSpiCnt = val & 0xE043u; // Limpa bit 7 (Busy = 0 imediato)
}

void NdsCartridge::writeAuxSpiData(uint8_t val) {
    const bool holdCs = (m_auxSpiCnt & (1u << 6)) != 0;
    m_auxSpiData = transferSpiBackupByte(val, holdCs);
    if (!holdCs) {
        if (m_spiCmd == 0x02 || m_spiCmd == 0x0A) {
            m_spiStatusReg &= ~0x02u; // Limpa WEL ao encerrar Page Write (/CS high)
        }
        if (m_saveDirty) {
            flushSaveFile();
        }
        m_spiHoldActive = false;
        m_spiStep       = 0;
    } else {
        m_spiHoldActive = true;
    }
}

uint8_t NdsCartridge::transferSpiBackupByte(uint8_t txByte, bool /*holdCs*/) {
    if (m_saveRam.empty()) return 0xFFu;

    if (m_spiStep == 0) {
        m_spiCmd               = txByte;
        m_spiAddr              = 0;
        m_spiAddrBytesReceived = 0;
        if (m_backupType == NdsBackupType::Eeprom512B && (m_spiCmd == 0x0B || m_spiCmd == 0x0A)) {
            m_spiAddr = 0x100u; // Bit 8 do endereço embutido no comando 4Kbit EEPROM
        }
        ++m_spiStep;

        if (m_spiCmd == 0x06) { // WREN
            m_spiStatusReg |= 0x02u;
        } else if (m_spiCmd == 0x04) { // WRDI
            m_spiStatusReg &= ~0x02u;
        } else if (m_spiCmd == 0xC7) { // Chip Erase
            if (m_spiStatusReg & 0x02u) {
                std::fill(m_saveRam.begin(), m_saveRam.end(), 0xFFu);
                m_saveDirty = true;
                m_spiStatusReg &= ~0x02u;
            }
        }
        return 0x00u;
    }

    switch (m_spiCmd) {
        case 0x05: // RDSR (Read Status Register)
            return m_spiStatusReg;

        case 0x01: // WRSR
            m_spiStatusReg = (txByte & 0xFCu) | (m_spiStatusReg & 0x03u);
            return 0x00u;

        case 0x9F: { // RDID (JEDEC Flash ID: ST M45PE40 512KB = 0x20 0x40 0x13)
            const uint32_t idx = m_spiStep++;
            if (idx == 1) return 0x20u;
            if (idx == 2) return 0x40u;
            if (idx == 3) return (m_backupType == NdsBackupType::Flash1M) ? 0x14u : 0x13u;
            return 0x00u;
        }

        case 0x03: // READ
        case 0x0B: // FAST READ / EEPROM bit-8 READ
        {
            const uint8_t addrW = backupAddrWidth();
            if (m_spiAddrBytesReceived < addrW) {
                m_spiAddr = (m_spiAddr << 8) | txByte;
                ++m_spiAddrBytesReceived;
                return 0x00u;
            }
            const uint8_t out = m_saveRam[m_spiAddr % m_saveRam.size()];
            ++m_spiAddr;
            return out;
        }

        case 0x02: // WRITE / Page Program
        case 0x0A: // Page Write
        {
            const uint8_t addrW = backupAddrWidth();
            if (m_spiAddrBytesReceived < addrW) {
                m_spiAddr = (m_spiAddr << 8) | txByte;
                ++m_spiAddrBytesReceived;
                return 0x00u;
            }
            if (m_spiStatusReg & 0x02u) {
                m_saveRam[m_spiAddr % m_saveRam.size()] = txByte;
                m_saveDirty = true;
            }
            ++m_spiAddr;
            return 0x00u;
        }

        case 0xD8: // Sector Erase (64KB)
        case 0xDB: // Page Erase (256B)
        {
            if (m_spiAddrBytesReceived < 3) {
                m_spiAddr = (m_spiAddr << 8) | txByte;
                ++m_spiAddrBytesReceived;
                if (m_spiAddrBytesReceived == 3 && (m_spiStatusReg & 0x02u)) {
                    const std::size_t eraseLen = (m_spiCmd == 0xD8) ? 65536u : 256u;
                    const std::size_t base = (m_spiAddr & ~(eraseLen - 1)) % m_saveRam.size();
                    for (std::size_t i = 0; i < eraseLen && (base + i) < m_saveRam.size(); ++i) {
                        m_saveRam[base + i] = 0xFFu;
                    }
                    m_saveDirty = true;
                    m_spiStatusReg &= ~0x02u;
                }
            }
            return 0x00u;
        }

        default:
            return 0xFFu;
    }
}

void NdsCartridge::writeRomCtrl(uint32_t val, bool& outDataReady, bool& outTransferDone) {
    outDataReady    = false;
    outTransferDone = false;
    m_romCtrl       = val;

    if ((val & (1u << 31)) == 0) {
        return;
    }

    const uint32_t blockSizeCode = (val >> 24) & 0x7u;
    std::size_t transferBytes = 0;
    if (blockSizeCode == 0)      transferBytes = 0;
    else if (blockSizeCode == 7) transferBytes = 4;
    else                         transferBytes = static_cast<std::size_t>(0x100u) << blockSizeCode;

    m_transferBuf.assign(transferBytes, 0xFFu);
    m_transferPos = 0;

    if (transferBytes == 0) {
        m_romCtrl &= ~((1u << 31) | (1u << 23));
        outTransferDone = true;
        return;
    }

    const uint8_t op = m_cardCmd[0];
    if (op == 0x90 || op == 0xB8) {
        // Chip ID repetido em palavras de 32 bits
        for (std::size_t i = 0; i + 3 < transferBytes; i += 4) {
            m_transferBuf[i + 0] = static_cast<uint8_t>(CHIP_ID & 0xFFu);
            m_transferBuf[i + 1] = static_cast<uint8_t>((CHIP_ID >> 8) & 0xFFu);
            m_transferBuf[i + 2] = static_cast<uint8_t>((CHIP_ID >> 16) & 0xFFu);
            m_transferBuf[i + 3] = static_cast<uint8_t>((CHIP_ID >> 24) & 0xFFu);
        }
    } else if (op == 0x00) {
        // Read Header (0x000..0xFFF)
        for (std::size_t i = 0; i < transferBytes; ++i) {
            const std::size_t off = i & 0xFFFu;
            m_transferBuf[i] = (off < m_rom.size()) ? m_rom[off] : 0x00u;
        }
    } else if (op == 0xB7) {
        // Read Data (big-endian address nos bytes 1..4)
        uint32_t addr = (static_cast<uint32_t>(m_cardCmd[1]) << 24) |
                        (static_cast<uint32_t>(m_cardCmd[2]) << 16) |
                        (static_cast<uint32_t>(m_cardCmd[3]) << 8)  |
                        static_cast<uint32_t>(m_cardCmd[4]);
        if (addr < 0x8000u) {
            addr = 0x8000u + (addr & 0x1FFu);
        }
        const uint32_t pageBase = addr & ~0xFFFu;
        for (std::size_t i = 0; i < transferBytes; ++i) {
            const uint32_t cur = pageBase | ((addr + static_cast<uint32_t>(i)) & 0xFFFu);
            m_transferBuf[i] = (cur < m_rom.size()) ? m_rom[cur] : 0xFFu;
        }
    }

    // Marca Data Word Ready (bit 23) e mantém Busy (bit 31) até todas as palavras serem lidas
    m_romCtrl |= (1u << 23) | (1u << 31);
    outDataReady = true;
}

uint32_t NdsCartridge::readCardData(bool& outMoreDataReady, bool& outTransferFinished) noexcept {
    outMoreDataReady    = false;
    outTransferFinished = false;

    if (m_transferPos + 4 > m_transferBuf.size()) {
        m_romCtrl &= ~((1u << 31) | (1u << 23));
        return 0xFFFFFFFFu;
    }

    const uint32_t word = static_cast<uint32_t>(m_transferBuf[m_transferPos + 0]) |
                          (static_cast<uint32_t>(m_transferBuf[m_transferPos + 1]) << 8) |
                          (static_cast<uint32_t>(m_transferBuf[m_transferPos + 2]) << 16) |
                          (static_cast<uint32_t>(m_transferBuf[m_transferPos + 3]) << 24);
    m_transferPos += 4;

    if (m_transferPos >= m_transferBuf.size()) {
        m_romCtrl &= ~((1u << 31) | (1u << 23));
        outTransferFinished = true;
    } else {
        m_romCtrl |= (1u << 23) | (1u << 31);
        outMoreDataReady = true;
    }
    return word;
}

void NdsCartridge::serializeState(SaveStateWriter& out) const {
    out.writePod(m_auxSpiCnt);
    out.writePod(m_auxSpiData);
    out.writePod(m_romCtrl);
    out.writeArray(m_cardCmd);
    out.writePod(static_cast<uint32_t>(m_transferBuf.size()));
    if (!m_transferBuf.empty()) {
        out.writeBytes(m_transferBuf);
    }
    out.writePod(static_cast<uint32_t>(m_transferPos));
    out.writePod(m_spiHoldActive);
    out.writePod(m_spiCmd);
    out.writePod(m_spiAddr);
    out.writePod(m_spiAddrBytesReceived);
    out.writePod(m_spiStatusReg);
    out.writePod(m_spiStep);
    out.writePod(static_cast<uint32_t>(m_saveRam.size()));
    if (!m_saveRam.empty()) {
        out.writeBytes(m_saveRam);
    }
}

bool NdsCartridge::deserializeState(SaveStateReader& in) {
    uint32_t tSize = 0, tPos = 0, savSize = 0;
    if (!in.readPod(m_auxSpiCnt) || !in.readPod(m_auxSpiData) || !in.readPod(m_romCtrl) ||
        !in.readArray(m_cardCmd) || !in.readPod(tSize) || tSize > 65536u) {
        return false;
    }
    m_transferBuf.resize(tSize);
    if (tSize > 0 && !in.readBytes(m_transferBuf)) return false;
    if (!in.readPod(tPos)) return false;
    m_transferPos = tPos;

    if (!in.readPod(m_spiHoldActive) || !in.readPod(m_spiCmd) || !in.readPod(m_spiAddr) ||
        !in.readPod(m_spiAddrBytesReceived) || !in.readPod(m_spiStatusReg) ||
        !in.readPod(m_spiStep) || !in.readPod(savSize) || savSize > 16 * 1024 * 1024u) {
        return false;
    }
    m_saveRam.resize(savSize);
    if (savSize > 0 && !in.readBytes(m_saveRam)) return false;
    return true;
}

} // namespace gba::core::nds
