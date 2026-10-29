#include "core/ppu/Ppu.hpp"
#include "core/cpu/Arm7Tdmi.hpp"
#include "core/memory/MemoryBus.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>

namespace gba::core {

Ppu::Ppu() {
    reset();
}

void Ppu::reset() {
    m_scanlineCycles = 0;
    m_inHBlank       = false;
    m_vcount         = 0;
    m_dispcnt        = 0x0080;
    m_dispstat       = 0;
    m_vblankIrqCount = 0;
    m_bgCnt.fill(0);
    m_bgHOfs.fill(0);
    m_bgVOfs.fill(0);
    m_bgPa   = {0x0100, 0x0100};
    m_bgPb   = {0, 0};
    m_bgPc   = {0, 0};
    m_bgPd   = {0x0100, 0x0100};
    m_bgRefX = {0, 0};
    m_bgRefY = {0, 0};
    m_ioRegs.fill(0);
    m_pram.fill(0);
    m_vram.fill(0);
    m_oam.fill(0);
    m_backBuffer.fill(0xFF000000u);
    m_frontBuffer.fill(0xFF000000u);
}

uint8_t Ppu::readVram(uint32_t offset) const noexcept {
    uint32_t addr = offset & 0x1FFFF;
    if (addr >= 0x18000) {
        addr -= 0x8000; // Mirror de 32KB superior para 0x10000..0x17FFF
    }
    return m_vram[addr];
}

void Ppu::writeVram(uint32_t offset, uint8_t val) noexcept {
    uint32_t addr = offset & 0x1FFFF;
    if (addr >= 0x18000) {
        addr -= 0x8000;
    }
    m_vram[addr] = val;
}

uint8_t Ppu::readRegister(uint32_t offset) const noexcept {
    if (offset >= 0x60) return 0;
    switch (offset) {
        case 0x00: return static_cast<uint8_t>(m_dispcnt & 0xFF);
        case 0x01: return static_cast<uint8_t>((m_dispcnt >> 8) & 0xFF);
        case 0x04: return static_cast<uint8_t>(m_dispstat & 0xFF);
        case 0x05: return static_cast<uint8_t>((m_dispstat >> 8) & 0xFF);
        case 0x06: return static_cast<uint8_t>(m_vcount & 0xFF);
        case 0x07: return static_cast<uint8_t>((m_vcount >> 8) & 0xFF);
        default:   return m_ioRegs[offset];
    }
}

void Ppu::writeRegister(uint32_t offset, uint8_t val) noexcept {
    if (offset >= 0x60) return;
    m_ioRegs[offset] = val;

    auto reg16 = [this](uint32_t base) -> uint16_t {
        return static_cast<uint16_t>(m_ioRegs[base] | (m_ioRegs[base + 1] << 8));
    };

    switch (offset) {
        case 0x00:
        case 0x01:
            m_dispcnt = reg16(0x00);
            break;
        case 0x04:
        case 0x05:
            // Bits 0..2 de DISPSTAT são somente leitura (flags VBlank/HBlank/VCount)
            m_dispstat = static_cast<uint16_t>((m_dispstat & 0x0007) | (reg16(0x04) & ~0x0007));
            break;
        case 0x08: case 0x09: m_bgCnt[0] = reg16(0x08); break;
        case 0x0A: case 0x0B: m_bgCnt[1] = reg16(0x0A); break;
        case 0x0C: case 0x0D: m_bgCnt[2] = reg16(0x0C); break;
        case 0x0E: case 0x0F: m_bgCnt[3] = reg16(0x0E); break;

        case 0x10: case 0x11: m_bgHOfs[0] = reg16(0x10) & 0x1FF; break;
        case 0x12: case 0x13: m_bgVOfs[0] = reg16(0x12) & 0x1FF; break;
        case 0x14: case 0x15: m_bgHOfs[1] = reg16(0x14) & 0x1FF; break;
        case 0x16: case 0x17: m_bgVOfs[1] = reg16(0x16) & 0x1FF; break;
        case 0x18: case 0x19: m_bgHOfs[2] = reg16(0x18) & 0x1FF; break;
        case 0x1A: case 0x1B: m_bgVOfs[2] = reg16(0x1A) & 0x1FF; break;
        case 0x1C: case 0x1D: m_bgHOfs[3] = reg16(0x1C) & 0x1FF; break;
        case 0x1E: case 0x1F: m_bgVOfs[3] = reg16(0x1E) & 0x1FF; break;

        case 0x20: case 0x21: m_bgPa[0] = static_cast<int16_t>(reg16(0x20)); break;
        case 0x22: case 0x23: m_bgPb[0] = static_cast<int16_t>(reg16(0x22)); break;
        case 0x24: case 0x25: m_bgPc[0] = static_cast<int16_t>(reg16(0x24)); break;
        case 0x26: case 0x27: m_bgPd[0] = static_cast<int16_t>(reg16(0x26)); break;

        case 0x30: case 0x31: m_bgPa[1] = static_cast<int16_t>(reg16(0x30)); break;
        case 0x32: case 0x33: m_bgPb[1] = static_cast<int16_t>(reg16(0x32)); break;
        case 0x34: case 0x35: m_bgPc[1] = static_cast<int16_t>(reg16(0x34)); break;
        case 0x36: case 0x37: m_bgPd[1] = static_cast<int16_t>(reg16(0x36)); break;
        default: break;
    }
}

void Ppu::step(int32_t cycles) {
    m_scanlineCycles += cycles;

    // Transição HDraw -> HBlank (após 960 ciclos)
    if (!m_inHBlank && m_scanlineCycles >= static_cast<int32_t>(HDRAW_CYCLES)) {
        m_inHBlank = true;
        m_dispstat |= (1u << 1); // Set HBlank flag

        if (m_vcount < SCREEN_HEIGHT) {
            renderScanline();
            if (m_bus) m_bus->triggerDmaHBlank();
        }

        if ((m_dispstat & (1u << 4)) && m_cpu) {
            m_cpu->requestInterrupt(InterruptFlag::HBlank);
        }
    }

    // Fim da Scanline (após 1232 ciclos)
    if (m_scanlineCycles >= static_cast<int32_t>(LINE_CYCLES)) {
        m_scanlineCycles -= static_cast<int32_t>(LINE_CYCLES);
        m_inHBlank = false;
        m_dispstat &= ~(1u << 1); // Clear HBlank flag

        m_vcount = (m_vcount + 1) % TOTAL_LINES;

        // Verifica VCount Match
        const uint8_t lyc = static_cast<uint8_t>((m_dispstat >> 8) & 0xFF);
        if (m_vcount == lyc) {
            m_dispstat |= (1u << 2);
            if ((m_dispstat & (1u << 5)) && m_cpu) {
                m_cpu->requestInterrupt(InterruptFlag::VCount);
            }
        } else {
            m_dispstat &= ~(1u << 2);
        }

        // Início do VBlank (Linha 160)
        if (m_vcount == SCREEN_HEIGHT) {
            m_dispstat |= (1u << 0); // Set VBlank flag
            m_frontBuffer = m_backBuffer;
            if (m_bus) m_bus->triggerDmaVBlank();
            if ((m_dispstat & (1u << 3)) && m_cpu) {
                ++m_vblankIrqCount;
                m_cpu->requestInterrupt(InterruptFlag::VBlank);
                if (m_traceEnabled && (m_vblankIrqCount <= 10 || m_vblankIrqCount % 50 == 0)) {
                    std::cout << "[TRACE PPU] VBlank IRQ #" << std::dec << m_vblankIrqCount
                              << " disparada na Scanline 160 | DISPSTAT=0x" << std::hex << std::setw(4) << std::setfill('0') << m_dispstat
                              << " | IE=0x" << std::setw(4) << (m_bus ? m_bus->getIe() : 0)
                              << " | IF=0x" << std::setw(4) << (m_bus ? m_bus->getIf() : 0)
                              << " | IME=" << std::dec << (m_bus ? m_bus->getIme() : 0) << "\n";
                }
            } else if (m_traceEnabled && m_vblankIrqCount == 0 && m_bus && (m_bus->getIe() & 1u)) {
                static uint32_t missedVBlankLogs = 0;
                if (++missedVBlankLogs <= 5) {
                    std::cout << "[TRACE PPU] Scanline 160 (VBlank) atingida, mas bit VBlank IRQ de DISPSTAT (0x"
                              << std::hex << std::setw(4) << std::setfill('0') << m_dispstat
                              << ") esta DESABILITADO (IE=0x" << std::setw(4) << m_bus->getIe() << std::dec << ")\n";
                }
            }
        } else if (m_vcount == TOTAL_LINES - 1) {
            // Linha 227: Flag VBlank é limpa 1 linha antes do reinício
            m_dispstat &= ~(1u << 0);
        }
    }
}

void Ppu::renderScanline() {
    // Forced Blank (Bit 7 de DISPCNT): tela branca (0x7FFF)
    if (m_dispcnt & (1u << 7)) {
        const uint32_t white = rgb555ToArgb8888(0x7FFF);
        std::fill_n(m_backBuffer.begin() + m_vcount * SCREEN_WIDTH, SCREEN_WIDTH, white);
        return;
    }

    for (auto& bg : m_bgLine) {
        bg.fill(LayerPixel{});
    }
    m_objLine.fill(LayerPixel{});

    const uint8_t mode = m_dispcnt & 0x07;

    // Renderiza OAM Sprites se bit 12 de DISPCNT estiver ativo
    if (m_dispcnt & (1u << 12)) {
        renderSpritesScanline();
    }

    switch (mode) {
        case 0:
            for (uint8_t bg = 0; bg < 4; ++bg) {
                if (m_dispcnt & (1u << (8 + bg))) renderTextBackground(bg);
            }
            composeScanline(false);
            break;
        case 1:
            if (m_dispcnt & (1u << 8))  renderTextBackground(0);
            if (m_dispcnt & (1u << 9))  renderTextBackground(1);
            if (m_dispcnt & (1u << 10)) renderAffineBackground(2);
            composeScanline(false);
            break;
        case 2:
            if (m_dispcnt & (1u << 10)) renderAffineBackground(2);
            if (m_dispcnt & (1u << 11)) renderAffineBackground(3);
            composeScanline(false);
            break;
        case 3:
            renderMode3Scanline();
            composeScanline(true);
            break;
        case 4:
            renderMode4Scanline();
            composeScanline(true);
            break;
        case 5:
            renderMode5Scanline();
            composeScanline(true);
            break;
        default:
            composeScanline(false);
            break;
    }
}

void Ppu::renderTextBackground(uint8_t bgIdx) {
    const uint16_t cnt          = m_bgCnt[bgIdx];
    const uint8_t  priority     = cnt & 0x03;
    const uint32_t charBase     = ((cnt >> 2) & 0x03) * 0x4000u;
    const bool     is8Bpp       = (cnt & (1u << 7)) != 0;
    const uint32_t screenBase   = ((cnt >> 8) & 0x1F) * 0x800u;
    const uint8_t  screenSize   = (cnt >> 14) & 0x03;

    const uint32_t widthMask  = (screenSize & 1) ? 511u : 255u;
    const uint32_t heightMask = (screenSize & 2) ? 511u : 255u;

    const uint32_t y = (m_vcount + m_bgVOfs[bgIdx]) & heightMask;
    const uint32_t tileY = y / 8;
    const uint32_t inTileY = y % 8;

    for (uint32_t screenX = 0; screenX < SCREEN_WIDTH; ++screenX) {
        const uint32_t x = (screenX + m_bgHOfs[bgIdx]) & widthMask;
        const uint32_t tileX = x / 8;
        const uint32_t inTileX = x % 8;

        uint32_t mapBlock = 0;
        if (x >= 256) mapBlock += 1;
        if (y >= 256) mapBlock += (screenSize == 3) ? 2 : 1;

        const uint32_t mapEntryAddr = screenBase + mapBlock * 0x800u + ((tileY & 31u) * 32u + (tileX & 31u)) * 2u;
        const uint16_t entry = readVram16(mapEntryAddr);

        const uint16_t tileNum = entry & 0x03FF;
        const bool     hFlip   = (entry & (1u << 10)) != 0;
        const bool     vFlip   = (entry & (1u << 11)) != 0;
        const uint8_t  palBank = (entry >> 12) & 0x0F;

        const uint32_t px = hFlip ? (7u - inTileX) : inTileX;
        const uint32_t py = vFlip ? (7u - inTileY) : inTileY;

        uint8_t colorIdx = 0;
        if (is8Bpp) {
            const uint32_t pixelAddr = charBase + tileNum * 64u + py * 8u + px;
            colorIdx = readVram(pixelAddr);
            if (colorIdx != 0) {
                m_bgLine[bgIdx][screenX] = {readPram16(colorIdx), priority, true};
            }
        } else {
            const uint32_t byteAddr = charBase + tileNum * 32u + py * 4u + (px / 2u);
            const uint8_t  byte     = readVram(byteAddr);
            colorIdx = (px & 1u) ? (byte >> 4) : (byte & 0x0F);
            if (colorIdx != 0) {
                m_bgLine[bgIdx][screenX] = {readPram16(palBank * 16u + colorIdx), priority, true};
            }
        }
    }
}

void Ppu::renderAffineBackground(uint8_t bgIdx) {
    const uint8_t  affIdx     = (bgIdx == 3) ? 1 : 0;
    const uint16_t cnt        = m_bgCnt[bgIdx];
    const uint8_t  priority   = cnt & 0x03;
    const uint32_t charBase   = ((cnt >> 2) & 0x03) * 0x4000u;
    const uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const bool     wraparound = (cnt & (1u << 13)) != 0;
    const uint32_t sizePixels = 128u << ((cnt >> 14) & 0x03);
    const uint32_t tilesPerRow = sizePixels / 8u;

    for (uint32_t screenX = 0; screenX < SCREEN_WIDTH; ++screenX) {
        int32_t texX = (m_bgRefX[affIdx] + static_cast<int32_t>(screenX) * m_bgPa[affIdx] + static_cast<int32_t>(m_vcount) * m_bgPb[affIdx]) >> 8;
        int32_t texY = (m_bgRefY[affIdx] + static_cast<int32_t>(screenX) * m_bgPc[affIdx] + static_cast<int32_t>(m_vcount) * m_bgPd[affIdx]) >> 8;

        if (texX < 0 || texY < 0 || texX >= static_cast<int32_t>(sizePixels) || texY >= static_cast<int32_t>(sizePixels)) {
            if (!wraparound) continue;
            texX &= (sizePixels - 1);
            texY &= (sizePixels - 1);
        }

        const uint32_t tileX   = static_cast<uint32_t>(texX) / 8u;
        const uint32_t tileY   = static_cast<uint32_t>(texY) / 8u;
        const uint8_t  tileNum = readVram(screenBase + tileY * tilesPerRow + tileX);
        const uint8_t  colorIdx = readVram(charBase + tileNum * 64u + (texY & 7) * 8u + (texX & 7));

        if (colorIdx != 0) {
            m_bgLine[bgIdx][screenX] = {readPram16(colorIdx), priority, true};
        }
    }
}

void Ppu::renderMode3Scanline() {
    if ((m_dispcnt & (1u << 10)) == 0) return; // BG2 precisa estar habilitado
    const uint8_t priority = m_bgCnt[2] & 0x03;
    const uint32_t lineBase = m_vcount * SCREEN_WIDTH * 2u;
    for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
        const uint16_t color555 = readVram16(lineBase + x * 2u);
        m_bgLine[2][x] = {color555, priority, true};
    }
}

void Ppu::renderMode4Scanline() {
    if ((m_dispcnt & (1u << 10)) == 0) return;
    const uint8_t  priority = m_bgCnt[2] & 0x03;
    const uint32_t pageBase = (m_dispcnt & (1u << 4)) ? 0xA000u : 0x0000u;
    const uint32_t lineBase = pageBase + m_vcount * SCREEN_WIDTH;
    for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
        const uint8_t colorIdx = readVram(lineBase + x);
        if (colorIdx != 0) {
            m_bgLine[2][x] = {readPram16(colorIdx), priority, true};
        }
    }
}

void Ppu::renderMode5Scanline() {
    if ((m_dispcnt & (1u << 10)) == 0 || m_vcount >= 128) return;
    const uint8_t  priority = m_bgCnt[2] & 0x03;
    const uint32_t pageBase = (m_dispcnt & (1u << 4)) ? 0xA000u : 0x0000u;
    const uint32_t lineBase = pageBase + m_vcount * 160u * 2u;
    for (uint32_t x = 0; x < 160; ++x) {
        const uint16_t color555 = readVram16(lineBase + x * 2u);
        m_bgLine[2][x] = {color555, priority, true};
    }
}

void Ppu::renderSpritesScanline() {
    static constexpr uint8_t OBJ_WIDTH[4][4] = {
        {8, 16, 32, 64},  // Square
        {16, 32, 32, 64}, // Horizontal
        {8, 8, 16, 32},   // Vertical
        {8, 8, 8, 8}
    };
    static constexpr uint8_t OBJ_HEIGHT[4][4] = {
        {8, 16, 32, 64},  // Square
        {8, 8, 16, 32},   // Horizontal
        {16, 32, 32, 64}, // Vertical
        {8, 8, 8, 8}
    };

    const bool mapping1D = (m_dispcnt & (1u << 6)) != 0;

    // Avalia os 128 Sprites da OAM (do índice 127 ao 0 para que índices menores tenham precedência)
    for (int i = 127; i >= 0; --i) {
        const uint32_t baseAddr = static_cast<uint32_t>(i) * 8u;
        const uint16_t attr0    = readOam16(baseAddr + 0);
        const uint16_t attr1    = readOam16(baseAddr + 2);
        const uint16_t attr2    = readOam16(baseAddr + 4);

        const uint8_t objMode = (attr0 >> 8) & 0x03;
        if (objMode == 2) continue; // Sprite desabilitado (Hidden)

        const uint8_t shape  = (attr0 >> 14) & 0x03;
        const uint8_t size   = (attr1 >> 14) & 0x03;
        const uint8_t width  = OBJ_WIDTH[shape][size];
        const uint8_t height = OBJ_HEIGHT[shape][size];

        int32_t sprY = attr0 & 0xFF;
        if (sprY >= 160) sprY -= 256;

        if (m_vcount < sprY || m_vcount >= sprY + height) continue;

        int32_t sprX = attr1 & 0x1FF;
        if (sprX >= 240) sprX -= 512;

        const bool     is8Bpp   = (attr0 & (1u << 13)) != 0;
        const bool     hFlip    = (objMode == 0) && ((attr1 & (1u << 12)) != 0);
        const bool     vFlip    = (objMode == 0) && ((attr1 & (1u << 13)) != 0);
        const uint16_t baseTile = attr2 & 0x03FF;
        const uint8_t  priority = (attr2 >> 10) & 0x03;
        const uint8_t  palBank  = (attr2 >> 12) & 0x0F;

        const uint32_t localY = vFlip ? (height - 1u - (m_vcount - sprY)) : (m_vcount - sprY);
        const uint32_t tileRow = localY / 8u;
        const uint32_t inTileY = localY % 8u;

        for (uint8_t sx = 0; sx < width; ++sx) {
            const int32_t screenX = sprX + sx;
            if (screenX < 0 || screenX >= static_cast<int32_t>(SCREEN_WIDTH)) continue;

            const uint32_t localX  = hFlip ? (width - 1u - sx) : sx;
            const uint32_t tileCol = localX / 8u;
            const uint32_t inTileX = localX % 8u;

            uint32_t tileIndex = 0;
            if (mapping1D) {
                const uint32_t tilesPerRow = width / 8u;
                tileIndex = baseTile + (tileRow * tilesPerRow + tileCol) * (is8Bpp ? 2u : 1u);
            } else {
                tileIndex = (baseTile & ~(is8Bpp ? 1u : 0u)) + tileRow * 32u + tileCol * (is8Bpp ? 2u : 1u);
            }

            uint8_t colorIdx = 0;
            if (is8Bpp) {
                const uint32_t addr = 0x10000u + (tileIndex & 0x3FEu) * 32u + inTileY * 8u + inTileX;
                colorIdx = readVram(addr);
                if (colorIdx != 0 && priority <= m_objLine[screenX].priority) {
                    m_objLine[screenX] = {readPram16(256u + colorIdx), priority, true};
                }
            } else {
                const uint32_t addr = 0x10000u + (tileIndex & 0x3FFu) * 32u + inTileY * 4u + (inTileX / 2u);
                const uint8_t  byte = readVram(addr);
                colorIdx = (inTileX & 1u) ? (byte >> 4) : (byte & 0x0F);
                if (colorIdx != 0 && priority <= m_objLine[screenX].priority) {
                    m_objLine[screenX] = {readPram16(256u + palBank * 16u + colorIdx), priority, true};
                }
            }
        }
    }
}

void Ppu::composeScanline(bool /*bitmapMode*/) {
    const uint16_t backdropColor555 = readPram16(0);
    const uint32_t lineOffset = m_vcount * SCREEN_WIDTH;

    for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
        uint16_t bestColor = backdropColor555;
        uint8_t  bestPrio  = 4;

        // Avalia BG3 -> BG0 (menor índice tem prioridade em caso de empate)
        for (int bg = 3; bg >= 0; --bg) {
            const auto& px = m_bgLine[bg][x];
            if (px.opaque && px.priority <= bestPrio) {
                bestColor = px.color555;
                bestPrio  = px.priority;
            }
        }

        // Sprites (OBJ) têm precedência sobre BGs de mesma prioridade
        const auto& objPx = m_objLine[x];
        if (objPx.opaque && objPx.priority <= bestPrio) {
            bestColor = objPx.color555;
        }

        m_backBuffer[lineOffset + x] = rgb555ToArgb8888(bestColor);
    }
}

void Ppu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_scanlineCycles);
    out.writePod(m_inHBlank);
    out.writePod(m_vcount);
    out.writePod(m_dispcnt);
    out.writePod(m_dispstat);
    out.writePod(m_bgCnt);
    out.writePod(m_bgHOfs);
    out.writePod(m_bgVOfs);
    out.writePod(m_bgPa);
    out.writePod(m_bgPb);
    out.writePod(m_bgPc);
    out.writePod(m_bgPd);
    out.writePod(m_bgRefX);
    out.writePod(m_bgRefY);
    out.writePod(m_ioRegs);
    out.writePod(m_pram);
    out.writePod(m_vram);
    out.writePod(m_oam);
    out.writePod(m_backBuffer);
    out.writePod(m_frontBuffer);
}

bool Ppu::deserializeState(SaveStateReader& in) {
    return in.readPod(m_scanlineCycles) &&
           in.readPod(m_inHBlank) &&
           in.readPod(m_vcount) &&
           in.readPod(m_dispcnt) &&
           in.readPod(m_dispstat) &&
           in.readPod(m_bgCnt) &&
           in.readPod(m_bgHOfs) &&
           in.readPod(m_bgVOfs) &&
           in.readPod(m_bgPa) &&
           in.readPod(m_bgPb) &&
           in.readPod(m_bgPc) &&
           in.readPod(m_bgPd) &&
           in.readPod(m_bgRefX) &&
           in.readPod(m_bgRefY) &&
           in.readPod(m_ioRegs) &&
           in.readPod(m_pram) &&
           in.readPod(m_vram) &&
           in.readPod(m_oam) &&
           in.readPod(m_backBuffer) &&
           in.readPod(m_frontBuffer);
}

} // namespace gba::core

