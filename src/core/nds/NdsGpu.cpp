#include "core/nds/NdsGpu.hpp"
#include "core/nds/NdsBus.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gba::core::nds {

namespace {

[[nodiscard]] constexpr uint32_t bgr555ToArgb(uint16_t c) noexcept {
    const uint32_t r5 = c & 0x1Fu;
    const uint32_t g5 = (c >> 5) & 0x1Fu;
    const uint32_t b5 = (c >> 10) & 0x1Fu;
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g5 << 3) | (g5 >> 2);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

[[nodiscard]] constexpr uint16_t lycFromDispStat(uint16_t ds) noexcept {
    return static_cast<uint16_t>(((ds >> 8) & 0xFFu) | ((ds & 0x80u) << 1));
}

[[nodiscard]] constexpr uint16_t rd16(const std::array<uint8_t, 0x70>& r, uint32_t off) noexcept {
    return static_cast<uint16_t>(r[off]) | (static_cast<uint16_t>(r[off + 1]) << 8);
}

[[nodiscard]] constexpr uint32_t rd32(const std::array<uint8_t, 0x70>& r, uint32_t off) noexcept {
    return static_cast<uint32_t>(rd16(r, off)) | (static_cast<uint32_t>(rd16(r, off + 2)) << 16);
}

[[nodiscard]] constexpr float fx12ToFloat(int32_t v) noexcept {
    return static_cast<float>(v) / 4096.0f;
}

[[nodiscard]] constexpr int32_t floatToFx12(float f) noexcept {
    return static_cast<int32_t>(f * 4096.0f);
}

} // namespace

NdsGpu::NdsGpu() {
    reset();
}

void NdsGpu::reset() noexcept {
    m_lineCycles  = 0;
    m_vcount      = 0;
    m_inHBlank    = false;
    m_dispStat9   = 0;
    m_dispStat7   = 0;
    m_powCnt1     = 0x820Fu;
    m_vblankCount = 0;

    for (auto& e : m_eng) {
        e = Engine2DState{};
    }
    m_topScreen.fill(0xFF000000u);
    m_bottomScreen.fill(0xFF000000u);

    m_disp3dCnt   = 0;
    m_gxStat      = 0x06000000u;
    m_clearColor  = 0x001F0000u;
    m_clearDepth  = 0x7FFFu;
    m_viewportX1  = 0;
    m_viewportY1  = 0;
    m_viewportX2  = 255;
    m_viewportY2  = 191;

    m_mtxMode     = 0;
    m_projMtx     = Mat4{};
    m_projStack   = Mat4{};
    m_posMtx      = Mat4{};
    m_dirMtx      = Mat4{};
    for (auto& s : m_posStack) s = Mat4{};
    for (auto& s : m_dirStack) s = Mat4{};
    m_posSp       = 0;
    m_texMtx      = Mat4{};

    m_primType        = 0;
    m_polyAttr        = 0x001F00C0u;
    m_pendingPolyAttr = 0x001F00C0u;
    m_texParam        = 0;
    m_plttBase        = 0;
    m_curVtx[0] = m_curVtx[1] = m_curVtx[2] = 0;
    m_curTexS = m_curTexT = 0.0f;
    m_curCol[0] = m_curCol[1] = m_curCol[2] = 255;

    for (auto& lv : m_lightVec) lv = {0.0f, 0.0f, -1.0f};
    for (auto& lc : m_lightCol) lc = {255, 255, 255};
    m_matDiff[0] = m_matDiff[1] = m_matDiff[2] = 255;
    m_matAmb[0]  = m_matAmb[1]  = m_matAmb[2]  = 64;
    m_matSpec[0] = m_matSpec[1] = m_matSpec[2] = 0;
    m_matEmi[0]  = m_matEmi[1]  = m_matEmi[2]  = 0;

    m_primVerts.clear();
    m_stripStep = 0;
    m_submittedPolys.clear();
    m_renderPolys.clear();
    m_swapPending = false;
    m_swapFlags   = 0;

    m_packedCmds   = 0;
    m_currentCmd   = 0;
    m_paramsNeeded = 0;
    m_paramCount   = 0;
    m_gxCmdQueue.clear();

    m_color3D.fill(0x00000000u);
    m_valid3D.fill(false);
    m_depth3D.fill(1.0f);
}

void NdsGpu::writeDispStat9(uint16_t val) noexcept {
    m_dispStat9 = static_cast<uint16_t>((m_dispStat9 & 0x0007u) | (val & 0xFFB8u));
}

void NdsGpu::writeDispStat7(uint16_t val) noexcept {
    m_dispStat7 = static_cast<uint16_t>((m_dispStat7 & 0x0007u) | (val & 0xFFB8u));
}

void NdsGpu::stepArm7Cycles(int32_t arm7Cycles) {
    m_lineCycles += arm7Cycles;

    if (!m_inHBlank && m_lineCycles >= ARM7_DOTS_HBLANK) {
        m_inHBlank = true;
        m_dispStat9 |= 0x0002u;
        m_dispStat7 |= 0x0002u;

        if (m_vcount < VISIBLE_LINES) {
            if (!m_skipRender) {
                renderScanline(m_vcount);
            }
            if (m_bus) m_bus->triggerDmaHBlank();

            // Avança pontos de referência internos dos BGs afins (BG2, BG3)
            for (int e = 0; e < 2; ++e) {
                const int16_t dmx2 = static_cast<int16_t>(rd16(m_eng[e].regs, 0x22));
                const int16_t dmy2 = static_cast<int16_t>(rd16(m_eng[e].regs, 0x26));
                const int16_t dmx3 = static_cast<int16_t>(rd16(m_eng[e].regs, 0x32));
                const int16_t dmy3 = static_cast<int16_t>(rd16(m_eng[e].regs, 0x36));
                m_eng[e].bgRefX[0] += dmx2;
                m_eng[e].bgRefY[0] += dmy2;
                m_eng[e].bgRefX[1] += dmx3;
                m_eng[e].bgRefY[1] += dmy3;
            }
        }

        if (m_bus) {
            if (m_dispStat9 & 0x0010u) m_bus->requestIrq9(NdsIrq::HBlank);
            if (m_dispStat7 & 0x0010u) m_bus->requestIrq7(NdsIrq::HBlank);
        }
    }

    if (m_lineCycles >= ARM7_CYCLES_LINE) {
        m_lineCycles -= ARM7_CYCLES_LINE;
        m_inHBlank = false;
        m_dispStat9 &= ~0x0002u;
        m_dispStat7 &= ~0x0002u;

        m_vcount = static_cast<uint16_t>((m_vcount + 1) % TOTAL_LINES);

        // VCount Match (9 bits no NDS)
        if (m_vcount == lycFromDispStat(m_dispStat9)) {
            m_dispStat9 |= 0x0004u;
            if (m_bus && (m_dispStat9 & 0x0020u)) m_bus->requestIrq9(NdsIrq::VCount);
        } else {
            m_dispStat9 &= ~0x0004u;
        }

        if (m_vcount == lycFromDispStat(m_dispStat7)) {
            m_dispStat7 |= 0x0004u;
            if (m_bus && (m_dispStat7 & 0x0020u)) m_bus->requestIrq7(NdsIrq::VCount);
        } else {
            m_dispStat7 &= ~0x0004u;
        }

        if (m_vcount == VISIBLE_LINES) {
            // Início do VBlank (Scanline 192)
            m_dispStat9 |= 0x0001u;
            m_dispStat7 |= 0x0001u;
            ++m_vblankCount;

            // Recarrega pontos de referência afins BG2X/Y e BG3X/Y
            for (int e = 0; e < 2; ++e) {
                auto sext28 = [](uint32_t v) -> int32_t {
                    v &= 0x0FFFFFFFu;
                    return (v & 0x08000000u) ? static_cast<int32_t>(v | 0xF0000000u) : static_cast<int32_t>(v);
                };
                m_eng[e].bgRefX[0] = sext28(rd32(m_eng[e].regs, 0x28));
                m_eng[e].bgRefY[0] = sext28(rd32(m_eng[e].regs, 0x2C));
                m_eng[e].bgRefX[1] = sext28(rd32(m_eng[e].regs, 0x38));
                m_eng[e].bgRefY[1] = sext28(rd32(m_eng[e].regs, 0x3C));
            }

            // Conclui SWAP_BUFFERS pendente da Engine 3D e rasteriza o próximo quadro 3D
            if (m_swapPending) {
                m_renderPolys = std::move(m_submittedPolys);
                m_submittedPolys.clear();
                m_swapPending = false;
                if (!m_skipRender) {
                    rasterize3DFrame();
                }
            }

            if (m_bus) {
                m_bus->triggerDmaVBlank();
                if (m_dispStat9 & 0x0008u) m_bus->requestIrq9(NdsIrq::VBlank);
                if (m_dispStat7 & 0x0008u) m_bus->requestIrq7(NdsIrq::VBlank);
            }
        } else if (m_vcount == TOTAL_LINES - 1) {
            m_dispStat9 &= ~0x0001u;
            m_dispStat7 &= ~0x0001u;
        }
    }
}

void NdsGpu::renderScanline(uint16_t line) {
    if ((m_powCnt1 & 1u) == 0) {
        std::fill_n(&m_topScreen[line * SCREEN_WIDTH], SCREEN_WIDTH, 0xFF000000u);
        std::fill_n(&m_bottomScreen[line * SCREEN_WIDTH], SCREEN_WIDTH, 0xFF000000u);
        return;
    }

    std::array<uint32_t, SCREEN_WIDTH> lineA{};
    std::array<uint32_t, SCREEN_WIDTH> lineB{};

    if (m_powCnt1 & (1u << 1)) {
        renderEngine2DLine(0, line, lineA.data());
    } else {
        lineA.fill(0xFF000000u);
    }

    if (m_powCnt1 & (1u << 9)) {
        renderEngine2DLine(1, line, lineB.data());
    } else {
        lineB.fill(0xFF000000u);
    }

    applyMasterBrightness(0, lineA.data());
    applyMasterBrightness(1, lineB.data());

    const bool engAOnTop = (m_powCnt1 & (1u << 15)) != 0;
    uint32_t* dstTop    = &m_topScreen[line * SCREEN_WIDTH];
    uint32_t* dstBottom = &m_bottomScreen[line * SCREEN_WIDTH];

    if (engAOnTop) {
        std::memcpy(dstTop, lineA.data(), SCREEN_WIDTH * sizeof(uint32_t));
        std::memcpy(dstBottom, lineB.data(), SCREEN_WIDTH * sizeof(uint32_t));
    } else {
        std::memcpy(dstTop, lineB.data(), SCREEN_WIDTH * sizeof(uint32_t));
        std::memcpy(dstBottom, lineA.data(), SCREEN_WIDTH * sizeof(uint32_t));
    }
}

void NdsGpu::renderEngine2DLine(int engine, uint16_t line, uint32_t* outLine) {
    if (!m_bus) {
        std::fill_n(outLine, SCREEN_WIDTH, 0xFF000000u);
        return;
    }

    const uint32_t dispCnt = rd32(m_eng[engine].regs, 0x00);
    if (dispCnt & (1u << 7)) { // Forced Blank
        std::fill_n(outLine, SCREEN_WIDTH, 0xFFFFFFFFu);
        return;
    }

    const uint32_t dispMode = (engine == 0) ? ((dispCnt >> 16) & 0x3u) : ((dispCnt >> 16) & 0x1u);
    if (dispMode == 0) {
        std::fill_n(outLine, SCREEN_WIDTH, 0xFFFFFFFFu);
        return;
    }

    if (dispMode == 2 && engine == 0) {
        // VRAM Display direto (256x192 15-bit BGR555 do Banco A, B, C ou D)
        const int vramBank = static_cast<int>((dispCnt >> 18) & 0x3u);
        const uint32_t rowByteOff = static_cast<uint32_t>(line) * SCREEN_WIDTH * 2u;
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            outLine[x] = bgr555ToArgb(m_bus->readLcdcBank16(vramBank, rowByteOff + static_cast<uint32_t>(x * 2)));
        }
        return;
    }

    // Cor de backdrop (cor 0 da paleta 2D do engine)
    const uint32_t backdrop = bgr555ToArgb(m_bus->readPalette16(engine, 0));
    std::array<uint8_t, SCREEN_WIDTH> linePrio{};
    std::fill_n(outLine, SCREEN_WIDTH, backdrop);
    linePrio.fill(4);

    const uint8_t bgMode = dispCnt & 0x7u;

    // Renderiza BGs de prioridade 3 até 0 (para que prioridades menores sobrescrevam ou a gente teste prio < linePrio[x])
    for (int bg = 3; bg >= 0; --bg) {
        if ((dispCnt & (1u << (8 + bg))) == 0) continue;
        const uint16_t bgCnt = rd16(m_eng[engine].regs, 0x08u + static_cast<uint32_t>(bg * 2));
        const uint8_t prio   = bgCnt & 0x3u;

        // BG0 3D na Engine A
        if (engine == 0 && bg == 0 && ((dispCnt & (1u << 3)) != 0 || bgMode == 6)) {
            const std::size_t rowOff = static_cast<std::size_t>(line) * SCREEN_WIDTH;
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                if (m_valid3D[rowOff + x] && prio <= linePrio[x]) {
                    outLine[x]  = m_color3D[rowOff + x];
                    linePrio[x] = prio;
                }
            }
            continue;
        }

        // Determina se o BG é Text ou Affine / Extended Affine conforme BG Mode do NDS
        bool isAffine = false;
        if (bg == 2) {
            isAffine = (bgMode == 2 || bgMode == 4 || bgMode == 5);
        } else if (bg == 3) {
            isAffine = (bgMode >= 1 && bgMode <= 5);
        }

        if (!isAffine) {
            renderTextBgLine(engine, bg, line, dispCnt, bgCnt, outLine, linePrio.data());
        } else {
            renderAffineOrExtBgLine(engine, bg, line, dispCnt, bgCnt, outLine, linePrio.data());
        }
    }

    // Sprites (OAM)
    if (dispCnt & (1u << 12)) {
        renderSpritesLine(engine, line, dispCnt, outLine, linePrio.data());
    }
}

void NdsGpu::renderTextBgLine(
    int engine, int bg, uint16_t line, uint32_t dispCnt, uint16_t bgCnt,
    uint32_t* lineOut, uint8_t* linePrio
) {
    const uint8_t prio     = bgCnt & 0x3u;
    const bool is8bpp      = (bgCnt & (1u << 7)) != 0;
    const bool useExtPal   = is8bpp && ((dispCnt & (1u << 30)) != 0);
    const uint32_t charStep   = (engine == 0) ? (((dispCnt >> 24) & 0x7u) * 0x10000u) : 0u;
    const uint32_t screenStep = (engine == 0) ? (((dispCnt >> 27) & 0x7u) * 0x10000u) : 0u;

    const uint32_t charBase   = charStep + (((bgCnt >> 2) & 0xFu) * 0x4000u);
    const uint32_t screenBase = screenStep + (((bgCnt >> 8) & 0x1Fu) * 0x0800u);
    const uint8_t sizeMode    = (bgCnt >> 14) & 0x3u;
    const uint32_t mapW       = (sizeMode & 1u) ? 512u : 256u;
    const uint32_t mapH       = (sizeMode & 2u) ? 512u : 256u;

    const uint16_t hofs = rd16(m_eng[engine].regs, 0x10u + static_cast<uint32_t>(bg * 4)) & 0x1FFu;
    const uint16_t vofs = rd16(m_eng[engine].regs, 0x12u + static_cast<uint32_t>(bg * 4)) & 0x1FFu;
    const uint32_t srcY = (static_cast<uint32_t>(line) + vofs) & (mapH - 1u);
    const uint32_t tileY = srcY >> 3;
    const uint32_t inTileY = srcY & 7u;

    int extSlot = bg;
    if ((bg == 0 || bg == 1) && (bgCnt & (1u << 13))) {
        extSlot = bg + 2;
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        if (prio > linePrio[x]) continue;

        const uint32_t srcX  = (static_cast<uint32_t>(x) + hofs) & (mapW - 1u);
        const uint32_t tileX = srcX >> 3;

        uint32_t screenBlock = 0;
        if (mapW == 512u && tileX >= 32u) screenBlock += 1u;
        if (mapH == 512u && tileY >= 32u) screenBlock += (mapW == 512u) ? 2u : 1u;

        const uint32_t entryOff = screenBase + screenBlock * 0x800u +
                                  (((tileY & 31u) * 32u + (tileX & 31u)) * 2u);
        const uint16_t entry    = m_bus->readBgVram16(engine, entryOff);
        const uint32_t tileNum  = entry & 0x3FFu;
        const bool hFlip        = (entry & (1u << 10)) != 0;
        const bool vFlip        = (entry & (1u << 11)) != 0;
        const uint32_t palBank  = (entry >> 12) & 0xFu;

        const uint32_t px = hFlip ? (7u - (srcX & 7u)) : (srcX & 7u);
        const uint32_t py = vFlip ? (7u - inTileY) : inTileY;

        if (!is8bpp) {
            const uint8_t packed = m_bus->readBgVram8(engine, charBase + tileNum * 32u + py * 4u + (px >> 1));
            const uint8_t colIdx = (px & 1u) ? ((packed >> 4) & 0xFu) : (packed & 0xFu);
            if (colIdx == 0) continue;
            const uint16_t bgr = m_bus->readPalette16(engine, (palBank * 16u + colIdx) * 2u);
            lineOut[x]  = bgr555ToArgb(bgr);
            linePrio[x] = prio;
        } else {
            const uint8_t colIdx = m_bus->readBgVram8(engine, charBase + tileNum * 64u + py * 8u + px);
            if (colIdx == 0) continue;
            const uint16_t bgr = useExtPal
                ? m_bus->readExtBgPalette16(engine, extSlot, palBank * 256u + colIdx)
                : m_bus->readPalette16(engine, static_cast<uint32_t>(colIdx) * 2u);
            lineOut[x]  = bgr555ToArgb(bgr);
            linePrio[x] = prio;
        }
    }
}

void NdsGpu::renderAffineOrExtBgLine(
    int engine, int bg, uint16_t /*line*/, uint32_t dispCnt, uint16_t bgCnt,
    uint32_t* lineOut, uint8_t* linePrio
) {
    const int affIdx          = bg - 2; // 0=BG2, 1=BG3
    if (affIdx < 0 || affIdx > 1) return;

    const uint8_t prio        = bgCnt & 0x3u;
    const uint8_t bgMode      = dispCnt & 0x7u;
    const bool wrapAround     = (bgCnt & (1u << 13)) != 0;
    const uint8_t sizeCode    = (bgCnt >> 14) & 0x3u;
    const uint32_t charStep   = (engine == 0) ? (((dispCnt >> 24) & 0x7u) * 0x10000u) : 0u;
    const uint32_t screenStep = (engine == 0) ? (((dispCnt >> 27) & 0x7u) * 0x10000u) : 0u;

    const int16_t pa = static_cast<int16_t>(rd16(m_eng[engine].regs, 0x20u + static_cast<uint32_t>(affIdx * 0x10)));
    const int16_t pc = static_cast<int16_t>(rd16(m_eng[engine].regs, 0x24u + static_cast<uint32_t>(affIdx * 0x10)));
    int32_t curX = m_eng[engine].bgRefX[affIdx];
    int32_t curY = m_eng[engine].bgRefY[affIdx];

    // Determina subtipo do BG: Affine clássico vs Extended Affine (Text16 / Bitmap8 / DirectBitmap16)
    const bool isExtended = (bgMode == 5) || (bgMode == 3 && bg == 3) || (bgMode == 4 && bg == 3);
    const bool isBitmap   = isExtended && ((bgCnt & (1u << 7)) != 0);
    const bool isDirect16 = isBitmap && ((bgCnt & (1u << 2)) != 0);

    int32_t dimW = 128 << sizeCode;
    int32_t dimH = 128 << sizeCode;
    if (isBitmap) {
        switch (sizeCode) {
            case 0: dimW = 128; dimH = 128; break;
            case 1: dimW = 256; dimH = 256; break;
            case 2: dimW = 512; dimH = 256; break;
            case 3: dimW = 512; dimH = 512; break;
        }
    }

    const uint32_t bmpBase    = ((bgCnt >> 8) & 0x1Fu) * 0x4000u;
    const uint32_t charBase   = charStep + (((bgCnt >> 2) & 0xFu) * 0x4000u);
    const uint32_t screenBase = screenStep + (((bgCnt >> 8) & 0x1Fu) * 0x0800u);
    const bool useExtPal      = (dispCnt & (1u << 30)) != 0;

    for (int x = 0; x < SCREEN_WIDTH; ++x, curX += pa, curY += pc) {
        if (prio > linePrio[x]) continue;

        int32_t sx = curX >> 8;
        int32_t sy = curY >> 8;
        if (sx < 0 || sx >= dimW || sy < 0 || sy >= dimH) {
            if (!wrapAround) continue;
            sx &= (dimW - 1);
            sy &= (dimH - 1);
        }

        if (isBitmap) {
            if (isDirect16) {
                const uint32_t off = bmpBase + static_cast<uint32_t>(sy * dimW + sx) * 2u;
                const uint16_t px16 = m_bus->readBgVram16(engine, off);
                if ((px16 & 0x8000u) == 0) continue; // Bit 15 = Alpha no Direct Color Bitmap NDS!
                lineOut[x]  = bgr555ToArgb(px16);
                linePrio[x] = prio;
            } else {
                const uint32_t off = bmpBase + static_cast<uint32_t>(sy * dimW + sx);
                const uint8_t idx = m_bus->readBgVram8(engine, off);
                if (idx == 0) continue;
                lineOut[x]  = bgr555ToArgb(m_bus->readPalette16(engine, static_cast<uint32_t>(idx) * 2u));
                linePrio[x] = prio;
            }
        } else if (isExtended) {
            // Extended Rotscale 16-bit tilemap entries
            const uint32_t tilesPerRow = static_cast<uint32_t>(dimW >> 3);
            const uint32_t entry = m_bus->readBgVram16(
                engine,
                screenBase + ((static_cast<uint32_t>(sy >> 3) * tilesPerRow + static_cast<uint32_t>(sx >> 3)) * 2u)
            );
            const uint32_t tileNum = entry & 0x3FFu;
            const bool hFlip       = (entry & (1u << 10)) != 0;
            const bool vFlip       = (entry & (1u << 11)) != 0;
            const uint32_t palBank = (entry >> 12) & 0xFu;
            const uint32_t px = hFlip ? (7u - (sx & 7)) : (sx & 7);
            const uint32_t py = vFlip ? (7u - (sy & 7)) : (sy & 7);
            const uint8_t colIdx = m_bus->readBgVram8(engine, charBase + tileNum * 64u + py * 8u + px);
            if (colIdx == 0) continue;
            const uint16_t bgr = useExtPal
                ? m_bus->readExtBgPalette16(engine, bg, palBank * 256u + colIdx)
                : m_bus->readPalette16(engine, static_cast<uint32_t>(colIdx) * 2u);
            lineOut[x]  = bgr555ToArgb(bgr);
            linePrio[x] = prio;
        } else {
            // Affine clássico (mapa de 8 bits)
            const uint32_t tilesPerRow = static_cast<uint32_t>(dimW >> 3);
            const uint8_t tileNum = m_bus->readBgVram8(
                engine,
                screenBase + (static_cast<uint32_t>(sy >> 3) * tilesPerRow + static_cast<uint32_t>(sx >> 3))
            );
            const uint8_t colIdx = m_bus->readBgVram8(
                engine,
                charBase + static_cast<uint32_t>(tileNum) * 64u + static_cast<uint32_t>((sy & 7) * 8 + (sx & 7))
            );
            if (colIdx == 0) continue;
            lineOut[x]  = bgr555ToArgb(m_bus->readPalette16(engine, static_cast<uint32_t>(colIdx) * 2u));
            linePrio[x] = prio;
        }
    }
}

void NdsGpu::renderSpritesLine(
    int engine, uint16_t line, uint32_t dispCnt,
    uint32_t* lineOut, uint8_t* linePrio
) {
    static constexpr int OBJ_W[4][4] = {
        {8, 16, 32, 64},
        {16, 32, 32, 64},
        {8, 8, 16, 32},
        {8, 8, 8, 8}
    };
    static constexpr int OBJ_H[4][4] = {
        {8, 16, 32, 64},
        {8, 8, 16, 32},
        {16, 32, 32, 64},
        {8, 8, 8, 8}
    };

    const bool tile1D      = (dispCnt & (1u << 4)) != 0;
    const uint32_t tileStep = 32u << ((dispCnt >> 20) & 0x3u);
    const bool bmp1D       = (dispCnt & (1u << 6)) != 0;
    const uint32_t bmpStep = ((dispCnt >> 22) & 1u) ? 256u : 128u;
    const bool useExtObjPal = (dispCnt & (1u << 31)) != 0;

    for (int i = 127; i >= 0; --i) {
        const uint32_t oamOff = static_cast<uint32_t>(i * 8);
        const uint16_t attr0  = m_bus->readOam16(engine, oamOff + 0);
        const uint16_t attr1  = m_bus->readOam16(engine, oamOff + 2);
        const uint16_t attr2  = m_bus->readOam16(engine, oamOff + 4);

        const uint8_t rotMode = (attr0 >> 8) & 0x3u;
        if (rotMode == 2) continue; // Sprite desabilitado

        const uint8_t objMode = (attr0 >> 10) & 0x3u;
        if (objMode == 2) continue; // OBJ Window

        const uint8_t shape = (attr0 >> 14) & 0x3u;
        const uint8_t size  = (attr1 >> 14) & 0x3u;
        const int sprW = OBJ_W[shape][size];
        const int sprH = OBJ_H[shape][size];
        const int boxW = (rotMode == 3) ? (sprW * 2) : sprW;
        const int boxH = (rotMode == 3) ? (sprH * 2) : sprH;

        int sprY = attr0 & 0xFFu;
        if (sprY >= 192 && (sprY + boxH) > 256) sprY -= 256;
        if (static_cast<int>(line) < sprY || static_cast<int>(line) >= sprY + boxH) continue;

        int sprX = attr1 & 0x1FFu;
        if (sprX >= 256) sprX -= 512;

        const uint8_t prio    = (attr2 >> 10) & 0x3u;
        const uint32_t tileId = attr2 & 0x3FFu;
        const uint32_t palIdx = (attr2 >> 12) & 0xFu;
        const bool is8bpp     = (attr0 & (1u << 13)) != 0;
        const bool isBitmapObj = (objMode == 3);

        const bool isAffine = (rotMode & 1u) != 0;
        int16_t pa = 0x0100, pb = 0, pc = 0, pd = 0x0100;
        if (isAffine) {
            const uint32_t paramGrp = (attr1 >> 9) & 0x1Fu;
            pa = static_cast<int16_t>(m_bus->readOam16(engine, paramGrp * 32u + 0x06u));
            pb = static_cast<int16_t>(m_bus->readOam16(engine, paramGrp * 32u + 0x0Eu));
            pc = static_cast<int16_t>(m_bus->readOam16(engine, paramGrp * 32u + 0x16u));
            pd = static_cast<int16_t>(m_bus->readOam16(engine, paramGrp * 32u + 0x1Eu));
        }

        const int localBoxY = static_cast<int>(line) - sprY;

        for (int bx = 0; bx < boxW; ++bx) {
            const int screenX = sprX + bx;
            if (screenX < 0 || screenX >= SCREEN_WIDTH) continue;
            if (prio > linePrio[screenX]) continue;

            int texX = bx;
            int texY = localBoxY;
            if (isAffine) {
                const int cx = bx - (boxW / 2);
                const int cy = localBoxY - (boxH / 2);
                texX = ((pa * cx + pb * cy) >> 8) + (sprW / 2);
                texY = ((pc * cx + pd * cy) >> 8) + (sprH / 2);
                if (texX < 0 || texX >= sprW || texY < 0 || texY >= sprH) continue;
            } else {
                if (attr1 & (1u << 12)) texX = sprW - 1 - texX; // HFlip
                if (attr1 & (1u << 13)) texY = sprH - 1 - texY; // VFlip
            }

            if (isBitmapObj) {
                if (palIdx == 0) continue; // Alpha = 0 em Bitmap OBJ
                uint32_t vramOff = 0;
                if (bmp1D) {
                    vramOff = tileId * bmpStep + static_cast<uint32_t>(texY * sprW + texX) * 2u;
                } else {
                    const bool wide256 = ((dispCnt >> 5) & 1u) != 0;
                    const uint32_t pitch = wide256 ? 256u : 128u;
                    const uint32_t baseX = wide256 ? ((tileId & 0x1Fu) * 8u) : ((tileId & 0x0Fu) * 8u);
                    const uint32_t baseY = wide256 ? ((tileId >> 5) * 8u)    : ((tileId >> 4) * 8u);
                    vramOff = ((baseY + static_cast<uint32_t>(texY)) * pitch + (baseX + static_cast<uint32_t>(texX))) * 2u;
                }
                const uint16_t px16 = m_bus->readObjVram16(engine, vramOff);
                if ((px16 & 0x8000u) == 0) continue;
                lineOut[screenX]  = bgr555ToArgb(px16);
                linePrio[screenX] = prio;
            } else if (!is8bpp) {
                const uint32_t tx = static_cast<uint32_t>(texX >> 3);
                const uint32_t ty = static_cast<uint32_t>(texY >> 3);
                const uint32_t baseAddr = tile1D
                    ? (tileId * tileStep + (ty * static_cast<uint32_t>(sprW >> 3) + tx) * 32u)
                    : ((tileId + ty * 32u + tx) * 32u);
                const uint8_t packed = m_bus->readObjVram8(
                    engine, baseAddr + static_cast<uint32_t>((texY & 7) * 4 + ((texX & 7) >> 1))
                );
                const uint8_t colIdx = (texX & 1) ? ((packed >> 4) & 0xFu) : (packed & 0xFu);
                if (colIdx == 0) continue;
                const uint16_t bgr = m_bus->readPalette16(engine, 0x200u + (palIdx * 16u + colIdx) * 2u);
                lineOut[screenX]  = bgr555ToArgb(bgr);
                linePrio[screenX] = prio;
            } else {
                const uint32_t tx = static_cast<uint32_t>(texX >> 3);
                const uint32_t ty = static_cast<uint32_t>(texY >> 3);
                const uint32_t baseAddr = tile1D
                    ? (tileId * tileStep + (ty * static_cast<uint32_t>(sprW >> 3) + tx) * 64u)
                    : ((tileId * 32u) + (ty * 1024u) + (tx * 64u));
                const uint8_t colIdx = m_bus->readObjVram8(
                    engine, baseAddr + static_cast<uint32_t>((texY & 7) * 8 + (texX & 7))
                );
                if (colIdx == 0) continue;
                const uint16_t bgr = useExtObjPal
                    ? m_bus->readExtObjPalette16(engine, palIdx * 256u + colIdx)
                    : m_bus->readPalette16(engine, 0x200u + static_cast<uint32_t>(colIdx) * 2u);
                lineOut[screenX]  = bgr555ToArgb(bgr);
                linePrio[screenX] = prio;
            }
        }
    }
}

void NdsGpu::applyMasterBrightness(int engine, uint32_t* outLine) const noexcept {
    const uint16_t mb = rd16(m_eng[engine].regs, 0x6C);
    const uint8_t mode = (mb >> 14) & 0x3u;
    if (mode != 1 && mode != 2) return;
    const int factor = std::min<int>(16, mb & 0x1Fu);
    if (factor == 0) return;

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        const uint32_t px = outLine[x];
        int r = static_cast<int>((px >> 16) & 0xFFu);
        int g = static_cast<int>((px >> 8) & 0xFFu);
        int b = static_cast<int>(px & 0xFFu);
        if (mode == 1) { // Fade to White
            r += ((255 - r) * factor) >> 4;
            g += ((255 - g) * factor) >> 4;
            b += ((255 - b) * factor) >> 4;
        } else { // Fade to Black
            r -= (r * factor) >> 4;
            g -= (g * factor) >> 4;
            b -= (b * factor) >> 4;
        }
        outLine[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
}

// =============================================================================
// Registradores MMIO 2D (Engine A e Engine B)
// =============================================================================
uint8_t NdsGpu::readEngine2D8(int engine, uint32_t offset) const noexcept {
    if (offset == 0x04) return static_cast<uint8_t>(m_dispStat9 & 0xFFu);
    if (offset == 0x05) return static_cast<uint8_t>((m_dispStat9 >> 8) & 0xFFu);
    if (offset == 0x06) return static_cast<uint8_t>(m_vcount & 0xFFu);
    if (offset == 0x07) return static_cast<uint8_t>((m_vcount >> 8) & 0xFFu);
    if (engine == 0 && offset == 0x60) return static_cast<uint8_t>(m_disp3dCnt & 0xFFu);
    if (engine == 0 && offset == 0x61) return static_cast<uint8_t>((m_disp3dCnt >> 8) & 0xFFu);
    if (offset < 0x70) return m_eng[engine].regs[offset];
    return 0;
}

uint16_t NdsGpu::readEngine2D16(int engine, uint32_t offset) const noexcept {
    return static_cast<uint16_t>(readEngine2D8(engine, offset)) |
           (static_cast<uint16_t>(readEngine2D8(engine, offset + 1)) << 8);
}

uint32_t NdsGpu::readEngine2D32(int engine, uint32_t offset) const noexcept {
    return static_cast<uint32_t>(readEngine2D16(engine, offset)) |
           (static_cast<uint32_t>(readEngine2D16(engine, offset + 2)) << 16);
}

void NdsGpu::writeEngine2D8(int engine, uint32_t offset, uint8_t val) noexcept {
    if (offset >= 0x70) return;
    if (offset == 0x04) {
        writeDispStat9(static_cast<uint16_t>((m_dispStat9 & 0xFF00u) | val));
        return;
    }
    if (offset == 0x05) {
        writeDispStat9(static_cast<uint16_t>((m_dispStat9 & 0x00FFu) | (static_cast<uint16_t>(val) << 8)));
        return;
    }
    if (offset == 0x06 || offset == 0x07) return; // VCOUNT é read-only

    if (engine == 0 && offset == 0x60) {
        m_disp3dCnt = static_cast<uint16_t>((m_disp3dCnt & 0xFF00u) | val);
    } else if (engine == 0 && offset == 0x61) {
        m_disp3dCnt = static_cast<uint16_t>((m_disp3dCnt & 0x00FFu) | (static_cast<uint16_t>(val) << 8));
    }

    m_eng[engine].regs[offset] = val;

    auto sext28 = [](uint32_t v) -> int32_t {
        v &= 0x0FFFFFFFu;
        return (v & 0x08000000u) ? static_cast<int32_t>(v | 0xF0000000u) : static_cast<int32_t>(v);
    };
    if (offset >= 0x28 && offset <= 0x2B) m_eng[engine].bgRefX[0] = sext28(rd32(m_eng[engine].regs, 0x28));
    if (offset >= 0x2C && offset <= 0x2F) m_eng[engine].bgRefY[0] = sext28(rd32(m_eng[engine].regs, 0x2C));
    if (offset >= 0x38 && offset <= 0x3B) m_eng[engine].bgRefX[1] = sext28(rd32(m_eng[engine].regs, 0x38));
    if (offset >= 0x3C && offset <= 0x3F) m_eng[engine].bgRefY[1] = sext28(rd32(m_eng[engine].regs, 0x3C));
}

void NdsGpu::writeEngine2D16(int engine, uint32_t offset, uint16_t val) noexcept {
    writeEngine2D8(engine, offset, static_cast<uint8_t>(val & 0xFFu));
    writeEngine2D8(engine, offset + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsGpu::writeEngine2D32(int engine, uint32_t offset, uint32_t val) noexcept {
    writeEngine2D16(engine, offset, static_cast<uint16_t>(val & 0xFFFFu));
    writeEngine2D16(engine, offset + 2, static_cast<uint16_t>((val >> 16) & 0xFFFFu));
}

// =============================================================================
// Motor 3D (Geometry Engine GXFIFO + Software Rasterizer)
// =============================================================================
uint8_t NdsGpu::gxParamCount(uint8_t cmd) noexcept {
    switch (cmd) {
        case 0x00: return 0;  // NOP
        case 0x10: return 1;  // MTX_MODE
        case 0x11: return 0;  // MTX_PUSH
        case 0x12: return 1;  // MTX_POP
        case 0x13: return 1;  // MTX_STORE
        case 0x14: return 1;  // MTX_RESTORE
        case 0x15: return 0;  // MTX_IDENTITY
        case 0x16: return 16; // MTX_LOAD_4x4
        case 0x17: return 12; // MTX_LOAD_4x3
        case 0x18: return 16; // MTX_MULT_4x4
        case 0x19: return 12; // MTX_MULT_4x3
        case 0x1A: return 9;  // MTX_MULT_3x3
        case 0x1B: return 3;  // MTX_SCALE
        case 0x1C: return 3;  // MTX_TRANS
        case 0x20: return 1;  // COLOR
        case 0x21: return 1;  // NORMAL
        case 0x22: return 1;  // TEXCOORD
        case 0x23: return 2;  // VTX_16
        case 0x24: return 1;  // VTX_10
        case 0x25: return 1;  // VTX_XY
        case 0x26: return 1;  // VTX_XZ
        case 0x27: return 1;  // VTX_YZ
        case 0x28: return 1;  // VTX_DIFF
        case 0x29: return 1;  // POLYGON_ATTR
        case 0x2A: return 1;  // TEXIMAGE_PARAM
        case 0x2B: return 1;  // PLTT_BASE
        case 0x30: return 1;  // DIF_AMB
        case 0x31: return 1;  // SPE_EMI
        case 0x32: return 1;  // LIGHT_VECTOR
        case 0x33: return 1;  // LIGHT_COLOR
        case 0x34: return 32; // SHININESS
        case 0x40: return 1;  // BEGIN_VTXS
        case 0x41: return 0;  // END_VTXS
        case 0x50: return 1;  // SWAP_BUFFERS
        case 0x60: return 1;  // VIEWPORT
        case 0x70: return 3;  // BOX_TEST
        case 0x71: return 2;  // POS_TEST
        case 0x72: return 1;  // VEC_TEST
        default:   return 0;
    }
}

uint8_t NdsGpu::read3D8(uint32_t offset) const noexcept {
    const uint32_t w = read3D32(offset & ~3u);
    return static_cast<uint8_t>((w >> ((offset & 3u) * 8)) & 0xFFu);
}

uint16_t NdsGpu::read3D16(uint32_t offset) const noexcept {
    const uint32_t w = read3D32(offset & ~3u);
    return static_cast<uint16_t>((w >> ((offset & 2u) * 8)) & 0xFFFFu);
}

uint32_t NdsGpu::read3D32(uint32_t offset) const noexcept {
    // offset relativo a 0x04000000
    if (offset == 0x600u) {
        // GXSTAT: bit 25 = FIFO < half full (sempre 1), bit 26 = FIFO empty (sempre 1), bits 30..31 = IRQ mode
        return (m_gxStat & 0xC0000000u) | 0x06000002u |
               (static_cast<uint32_t>(m_posSp & 0x1Fu) << 8);
    }
    if (offset == 0x604u) {
        // RAM_COUNT: Polygons bits 0..15, Vertices bits 16..31
        const uint32_t pCount = std::min<uint32_t>(2048u, static_cast<uint32_t>(m_submittedPolys.size()));
        const uint32_t vCount = std::min<uint32_t>(6144u, pCount * 3u);
        return pCount | (vCount << 16);
    }
    if (offset >= 0x640u && offset < 0x680u) {
        // CLIPMTX_RESULT (posMtx * projMtx, row-major 4x4 em 20.12 fixed-point)
        const std::size_t idx = (offset - 0x640u) >> 2;
        const std::size_t r = idx >> 2;
        const std::size_t c = idx & 3u;
        float val = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            val += m_posMtx.m[r * 4 + k] * m_projMtx.m[k * 4 + c];
        }
        return static_cast<uint32_t>(floatToFx12(val));
    }
    if (offset >= 0x680u && offset < 0x6A4u) {
        // VECMTX_RESULT (3x3 direction matrix)
        const std::size_t idx = (offset - 0x680u) >> 2;
        const std::size_t r = idx / 3;
        const std::size_t c = idx % 3;
        return static_cast<uint32_t>(floatToFx12(m_dirMtx.m[r * 4 + c]));
    }
    return 0;
}

void NdsGpu::write3D8(uint32_t offset, uint8_t val) noexcept {
    if (offset >= 0x350u && offset < 0x354u) {
        const uint32_t s = (offset - 0x350u) * 8;
        m_clearColor = (m_clearColor & ~(0xFFu << s)) | (static_cast<uint32_t>(val) << s);
    } else if (offset == 0x354u) {
        m_clearDepth = static_cast<uint16_t>((m_clearDepth & 0xFF00u) | val);
    } else if (offset == 0x355u) {
        m_clearDepth = static_cast<uint16_t>((m_clearDepth & 0x00FFu) | (static_cast<uint16_t>(val & 0x7Fu) << 8));
    } else if (offset == 0x603u) {
        m_gxStat = (m_gxStat & 0x00FFFFFFu) | (static_cast<uint32_t>(val & 0xC0u) << 24);
        checkGxFifoIrq();
    }
}

void NdsGpu::write3D16(uint32_t offset, uint16_t val) noexcept {
    if (offset == 0x354u) {
        m_clearDepth = val & 0x7FFFu;
        return;
    }
    write3D8(offset, static_cast<uint8_t>(val & 0xFFu));
    write3D8(offset + 1, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void NdsGpu::write3D32(uint32_t offset, uint32_t val) noexcept {
    if (offset == 0x350u) {
        m_clearColor = val;
        return;
    }
    if (offset == 0x354u) {
        m_clearDepth = val & 0x7FFFu;
        return;
    }
    if (offset == 0x600u) {
        if (val & (1u << 15)) {
            m_posSp = 0; // Reset matrix stack error/pointer
        }
        m_gxStat = (m_gxStat & 0x3FFFFFFFu) | (val & 0xC0000000u);
        checkGxFifoIrq();
        return;
    }

    // GXFIFO (0x400..0x43F): escrita de 4 comandos empacotados ou parâmetros subsequentes
    if (offset >= 0x400u && offset < 0x440u) {
        if (m_currentCmd == 0 && m_packedCmds == 0) {
            m_packedCmds = val;
            while (m_packedCmds != 0 && m_paramsNeeded == 0) {
                const uint8_t nextCmd = static_cast<uint8_t>(m_packedCmds & 0xFFu);
                m_packedCmds >>= 8;
                submitGxCommandByte(nextCmd);
            }
        } else {
            submitGxParam(val);
            while (m_packedCmds != 0 && m_paramsNeeded == 0) {
                const uint8_t nextCmd = static_cast<uint8_t>(m_packedCmds & 0xFFu);
                m_packedCmds >>= 8;
                submitGxCommandByte(nextCmd);
            }
        }
        checkGxFifoIrq();
        return;
    }

    // Portas diretas de comando GX (0x440..0x5DF -> cmd = (offset - 0x400) >> 2)
    if (offset >= 0x440u && offset <= 0x5DFu) {
        const uint8_t cmd = static_cast<uint8_t>((offset - 0x400u) >> 2);
        if (m_currentCmd != cmd || m_paramsNeeded == 0) {
            m_packedCmds = 0;
            submitGxCommandByte(cmd);
        }
        if (m_paramsNeeded > 0) {
            submitGxParam(val);
        }
        checkGxFifoIrq();
    }
}

void NdsGpu::submitGxCommandByte(uint8_t cmd) {
    if (cmd == 0) return;
    const uint8_t needed = gxParamCount(cmd);
    if (needed == 0) {
        executeGxCommand(cmd, nullptr);
        m_currentCmd   = 0;
        m_paramsNeeded = 0;
        m_paramCount   = 0;
    } else {
        m_currentCmd   = cmd;
        m_paramsNeeded = needed;
        m_paramCount   = 0;
    }
}

void NdsGpu::submitGxParam(uint32_t param) {
    if (m_paramsNeeded == 0) return;
    if (m_paramCount < m_paramBuf.size()) {
        m_paramBuf[m_paramCount++] = param;
    }
    --m_paramsNeeded;
    if (m_paramsNeeded == 0) {
        executeGxCommand(m_currentCmd, m_paramBuf.data());
        m_currentCmd = 0;
        m_paramCount = 0;
    }
}

void NdsGpu::checkGxFifoIrq() noexcept {
    if (!m_bus) return;
    const uint32_t irqMode = (m_gxStat >> 30) & 0x3u;
    if (irqMode == 1 || irqMode == 2) {
        // Como processamos comandos síncronos, a FIFO está sempre < half-full e vazia
        m_bus->requestIrq9(NdsIrq::GxFifo);
        m_bus->triggerDmaGxFifo();
    }
}

void NdsGpu::executeGxCommand(uint8_t cmd, const uint32_t* p) {
    auto multMat = [](const Mat4& left, const Mat4& right) -> Mat4 {
        // No NDS, os vetores linha [x y z 1] multiplicam M à direita, então M_new = left * M_old
        Mat4 res{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += left.m[r * 4 + k] * right.m[k * 4 + c];
                }
                res.m[r * 4 + c] = sum;
            }
        }
        return res;
    };

    auto targetMatrixApply = [&](const auto& fn) {
        if (m_mtxMode == 0) {
            fn(m_projMtx);
        } else if (m_mtxMode == 1) {
            fn(m_posMtx);
        } else if (m_mtxMode == 2) {
            fn(m_posMtx);
            fn(m_dirMtx);
        } else if (m_mtxMode == 3) {
            fn(m_texMtx);
        }
    };

    switch (cmd) {
        case 0x10: // MTX_MODE
            m_mtxMode = static_cast<uint8_t>(p[0] & 0x3u);
            break;

        case 0x11: // MTX_PUSH
            if (m_mtxMode == 0) {
                m_projStack = m_projMtx;
            } else if (m_mtxMode == 1 || m_mtxMode == 2) {
                m_posStack[m_posSp & 31] = m_posMtx;
                m_dirStack[m_posSp & 31] = m_dirMtx;
                m_posSp = (m_posSp + 1) & 31;
            }
            break;

        case 0x12: { // MTX_POP
            if (m_mtxMode == 0) {
                m_projMtx = m_projStack;
            } else if (m_mtxMode == 1 || m_mtxMode == 2) {
                int offset = static_cast<int>(p[0] & 0x3Fu);
                if (offset & 0x20) offset -= 64;
                m_posSp = (m_posSp - offset) & 31;
                m_posMtx = m_posStack[m_posSp & 31];
                m_dirMtx = m_dirStack[m_posSp & 31];
            }
            break;
        }

        case 0x13: // MTX_STORE
            if (m_mtxMode == 0) {
                m_projStack = m_projMtx;
            } else if (m_mtxMode == 1 || m_mtxMode == 2) {
                const uint32_t idx = p[0] & 31u;
                m_posStack[idx] = m_posMtx;
                m_dirStack[idx] = m_dirMtx;
            }
            break;

        case 0x14: // MTX_RESTORE
            if (m_mtxMode == 0) {
                m_projMtx = m_projStack;
            } else if (m_mtxMode == 1 || m_mtxMode == 2) {
                const uint32_t idx = p[0] & 31u;
                m_posMtx = m_posStack[idx];
                m_dirMtx = m_dirStack[idx];
            }
            break;

        case 0x15: // MTX_IDENTITY
            targetMatrixApply([](Mat4& m) { m = Mat4{}; });
            break;

        case 0x16: { // MTX_LOAD_4x4
            Mat4 loaded{};
            for (int i = 0; i < 16; ++i) loaded.m[i] = fx12ToFloat(static_cast<int32_t>(p[i]));
            targetMatrixApply([&](Mat4& m) { m = loaded; });
            break;
        }

        case 0x17: { // MTX_LOAD_4x3
            Mat4 loaded{};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    loaded.m[r * 4 + c] = fx12ToFloat(static_cast<int32_t>(p[r * 3 + c]));
                }
                loaded.m[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
            }
            targetMatrixApply([&](Mat4& m) { m = loaded; });
            break;
        }

        case 0x18: { // MTX_MULT_4x4
            Mat4 lhs{};
            for (int i = 0; i < 16; ++i) lhs.m[i] = fx12ToFloat(static_cast<int32_t>(p[i]));
            targetMatrixApply([&](Mat4& m) { m = multMat(lhs, m); });
            break;
        }

        case 0x19: { // MTX_MULT_4x3
            Mat4 lhs{};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    lhs.m[r * 4 + c] = fx12ToFloat(static_cast<int32_t>(p[r * 3 + c]));
                }
                lhs.m[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
            }
            targetMatrixApply([&](Mat4& m) { m = multMat(lhs, m); });
            break;
        }

        case 0x1A: { // MTX_MULT_3x3
            Mat4 lhs{};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    lhs.m[r * 4 + c] = fx12ToFloat(static_cast<int32_t>(p[r * 3 + c]));
                }
            }
            targetMatrixApply([&](Mat4& m) { m = multMat(lhs, m); });
            break;
        }

        case 0x1B: { // MTX_SCALE (não altera Direction Matrix no modo 2!)
            Mat4 s{};
            s.m[0]  = fx12ToFloat(static_cast<int32_t>(p[0]));
            s.m[5]  = fx12ToFloat(static_cast<int32_t>(p[1]));
            s.m[10] = fx12ToFloat(static_cast<int32_t>(p[2]));
            if (m_mtxMode == 0)      m_projMtx = multMat(s, m_projMtx);
            else if (m_mtxMode == 3) m_texMtx  = multMat(s, m_texMtx);
            else                     m_posMtx  = multMat(s, m_posMtx);
            break;
        }

        case 0x1C: { // MTX_TRANS
            Mat4 t{};
            t.m[12] = fx12ToFloat(static_cast<int32_t>(p[0]));
            t.m[13] = fx12ToFloat(static_cast<int32_t>(p[1]));
            t.m[14] = fx12ToFloat(static_cast<int32_t>(p[2]));
            targetMatrixApply([&](Mat4& m) { m = multMat(t, m); });
            break;
        }

        case 0x20: { // COLOR (BGR555)
            const uint32_t c = p[0] & 0x7FFFu;
            m_curCol[0] = static_cast<uint8_t>(((c & 0x1Fu) << 3) | ((c & 0x1Fu) >> 2));
            m_curCol[1] = static_cast<uint8_t>((((c >> 5) & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) >> 2));
            m_curCol[2] = static_cast<uint8_t>((((c >> 10) & 0x1Fu) << 3) | (((c >> 10) & 0x1Fu) >> 2));
            break;
        }

        case 0x21: { // NORMAL (3x 10-bit signed 1.9 fixed-point + iluminação direcional)
            auto sext10 = [](uint32_t v) -> int32_t {
                v &= 0x3FFu;
                return (v & 0x200u) ? static_cast<int32_t>(v | ~0x3FFu) : static_cast<int32_t>(v);
            };
            const float nx = static_cast<float>(sext10(p[0])) / 512.0f;
            const float ny = static_cast<float>(sext10(p[0] >> 10)) / 512.0f;
            const float nz = static_cast<float>(sext10(p[0] >> 20)) / 512.0f;

            // Transforma normal pela matriz direcional 3x3
            const float tx = nx * m_dirMtx.m[0] + ny * m_dirMtx.m[4] + nz * m_dirMtx.m[8];
            const float ty = nx * m_dirMtx.m[1] + ny * m_dirMtx.m[5] + nz * m_dirMtx.m[9];
            const float tz = nx * m_dirMtx.m[2] + ny * m_dirMtx.m[6] + nz * m_dirMtx.m[10];

            int rAcc = m_matEmi[0];
            int gAcc = m_matEmi[1];
            int bAcc = m_matEmi[2];

            for (int l = 0; l < 4; ++l) {
                if ((m_polyAttr & (1u << l)) == 0) continue;
                const float dot = std::max(0.0f, -(tx * m_lightVec[l][0] + ty * m_lightVec[l][1] + tz * m_lightVec[l][2]));
                rAcc += static_cast<int>((dot * static_cast<float>(m_matDiff[0]) + static_cast<float>(m_matAmb[0])) *
                                         (static_cast<float>(m_lightCol[l][0]) / 255.0f));
                gAcc += static_cast<int>((dot * static_cast<float>(m_matDiff[1]) + static_cast<float>(m_matAmb[1])) *
                                         (static_cast<float>(m_lightCol[l][1]) / 255.0f));
                bAcc += static_cast<int>((dot * static_cast<float>(m_matDiff[2]) + static_cast<float>(m_matAmb[2])) *
                                         (static_cast<float>(m_lightCol[l][2]) / 255.0f));
            }
            m_curCol[0] = static_cast<uint8_t>(std::clamp(rAcc, 0, 255));
            m_curCol[1] = static_cast<uint8_t>(std::clamp(gAcc, 0, 255));
            m_curCol[2] = static_cast<uint8_t>(std::clamp(bAcc, 0, 255));
            break;
        }

        case 0x22: { // TEXCOORD (S, T signed 12.4 fixed-point)
            const float rawS = static_cast<float>(static_cast<int16_t>(p[0] & 0xFFFFu)) / 16.0f;
            const float rawT = static_cast<float>(static_cast<int16_t>((p[0] >> 16) & 0xFFFFu)) / 16.0f;
            const uint8_t texTransMode = (m_texParam >> 30) & 0x3u;
            if (texTransMode == 1) {
                m_curTexS = rawS * m_texMtx.m[0] + rawT * m_texMtx.m[4] + m_texMtx.m[8] + m_texMtx.m[12];
                m_curTexT = rawS * m_texMtx.m[1] + rawT * m_texMtx.m[5] + m_texMtx.m[9] + m_texMtx.m[13];
            } else {
                m_curTexS = rawS;
                m_curTexT = rawT;
            }
            break;
        }

        case 0x23: { // VTX_16 (2 words: X,Y em p[0], Z em p[1] como signed 4.12)
            m_curVtx[0] = static_cast<int16_t>(p[0] & 0xFFFFu);
            m_curVtx[1] = static_cast<int16_t>((p[0] >> 16) & 0xFFFFu);
            m_curVtx[2] = static_cast<int16_t>(p[1] & 0xFFFFu);
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;
        }

        case 0x24: { // VTX_10 (signed 4.6 -> shift left 6 para 4.12)
            auto s10To16 = [](uint32_t v) -> int16_t {
                v &= 0x3FFu;
                const int32_t s = (v & 0x200u) ? static_cast<int32_t>(v | ~0x3FFu) : static_cast<int32_t>(v);
                return static_cast<int16_t>(s << 6);
            };
            m_curVtx[0] = s10To16(p[0]);
            m_curVtx[1] = s10To16(p[0] >> 10);
            m_curVtx[2] = s10To16(p[0] >> 20);
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;
        }

        case 0x25: // VTX_XY
            m_curVtx[0] = static_cast<int16_t>(p[0] & 0xFFFFu);
            m_curVtx[1] = static_cast<int16_t>((p[0] >> 16) & 0xFFFFu);
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;

        case 0x26: // VTX_XZ
            m_curVtx[0] = static_cast<int16_t>(p[0] & 0xFFFFu);
            m_curVtx[2] = static_cast<int16_t>((p[0] >> 16) & 0xFFFFu);
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;

        case 0x27: // VTX_YZ
            m_curVtx[1] = static_cast<int16_t>(p[0] & 0xFFFFu);
            m_curVtx[2] = static_cast<int16_t>((p[0] >> 16) & 0xFFFFu);
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;

        case 0x28: { // VTX_DIFF (10-bit signed deltas em 0.12)
            auto sext10 = [](uint32_t v) -> int16_t {
                v &= 0x3FFu;
                return static_cast<int16_t>((v & 0x200u) ? (v | ~0x3FFu) : v);
            };
            m_curVtx[0] = static_cast<int16_t>(m_curVtx[0] + sext10(p[0]));
            m_curVtx[1] = static_cast<int16_t>(m_curVtx[1] + sext10(p[0] >> 10));
            m_curVtx[2] = static_cast<int16_t>(m_curVtx[2] + sext10(p[0] >> 20));
            submitVertex(m_curVtx[0], m_curVtx[1], m_curVtx[2]);
            break;
        }

        case 0x29: // POLYGON_ATTR (aplicado no próximo BEGIN_VTXS)
            m_pendingPolyAttr = p[0];
            break;

        case 0x2A: // TEXIMAGE_PARAM
            m_texParam = p[0];
            break;

        case 0x2B: // PLTT_BASE
            m_plttBase = p[0] & 0x1FFFu;
            break;

        case 0x30: { // DIF_AMB
            auto unpack5 = [](uint32_t c, uint8_t* dst) {
                dst[0] = static_cast<uint8_t>(((c & 0x1Fu) << 3) | ((c & 0x1Fu) >> 2));
                dst[1] = static_cast<uint8_t>((((c >> 5) & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) >> 2));
                dst[2] = static_cast<uint8_t>((((c >> 10) & 0x1Fu) << 3) | (((c >> 10) & 0x1Fu) >> 2));
            };
            unpack5(p[0] & 0x7FFFu, m_matDiff);
            unpack5((p[0] >> 16) & 0x7FFFu, m_matAmb);
            if (p[0] & (1u << 15)) {
                m_curCol[0] = m_matDiff[0];
                m_curCol[1] = m_matDiff[1];
                m_curCol[2] = m_matDiff[2];
            }
            break;
        }

        case 0x31: { // SPE_EMI
            auto unpack5 = [](uint32_t c, uint8_t* dst) {
                dst[0] = static_cast<uint8_t>(((c & 0x1Fu) << 3) | ((c & 0x1Fu) >> 2));
                dst[1] = static_cast<uint8_t>((((c >> 5) & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) >> 2));
                dst[2] = static_cast<uint8_t>((((c >> 10) & 0x1Fu) << 3) | (((c >> 10) & 0x1Fu) >> 2));
            };
            unpack5(p[0] & 0x7FFFu, m_matSpec);
            unpack5((p[0] >> 16) & 0x7FFFu, m_matEmi);
            break;
        }

        case 0x32: { // LIGHT_VECTOR
            auto sext10 = [](uint32_t v) -> int32_t {
                v &= 0x3FFu;
                return (v & 0x200u) ? static_cast<int32_t>(v | ~0x3FFu) : static_cast<int32_t>(v);
            };
            const uint32_t l = (p[0] >> 30) & 3u;
            const float lx = static_cast<float>(sext10(p[0])) / 512.0f;
            const float ly = static_cast<float>(sext10(p[0] >> 10)) / 512.0f;
            const float lz = static_cast<float>(sext10(p[0] >> 20)) / 512.0f;
            m_lightVec[l][0] = lx * m_dirMtx.m[0] + ly * m_dirMtx.m[4] + lz * m_dirMtx.m[8];
            m_lightVec[l][1] = lx * m_dirMtx.m[1] + ly * m_dirMtx.m[5] + lz * m_dirMtx.m[9];
            m_lightVec[l][2] = lx * m_dirMtx.m[2] + ly * m_dirMtx.m[6] + lz * m_dirMtx.m[10];
            break;
        }

        case 0x33: { // LIGHT_COLOR
            const uint32_t l = (p[0] >> 30) & 3u;
            const uint32_t c = p[0] & 0x7FFFu;
            m_lightCol[l][0] = static_cast<uint8_t>(((c & 0x1Fu) << 3) | ((c & 0x1Fu) >> 2));
            m_lightCol[l][1] = static_cast<uint8_t>((((c >> 5) & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) >> 2));
            m_lightCol[l][2] = static_cast<uint8_t>((((c >> 10) & 0x1Fu) << 3) | (((c >> 10) & 0x1Fu) >> 2));
            break;
        }

        case 0x40: // BEGIN_VTXS
            m_primType  = static_cast<uint8_t>(p[0] & 0x3u);
            m_polyAttr  = m_pendingPolyAttr;
            m_primVerts.clear();
            m_stripStep = 0;
            break;

        case 0x41: // END_VTXS
            m_primVerts.clear();
            break;

        case 0x50: // SWAP_BUFFERS
            m_swapFlags   = p[0] & 0x3u;
            m_swapPending = true;
            if (m_renderPolys.empty() && !m_submittedPolys.empty()) {
                m_renderPolys = m_submittedPolys;
                rasterize3DFrame();
            }
            break;

        case 0x60: // VIEWPORT (x1, y1, x2, y2)
            m_viewportX1 = static_cast<uint8_t>(p[0] & 0xFFu);
            m_viewportY1 = static_cast<uint8_t>((p[0] >> 8) & 0xFFu);
            m_viewportX2 = static_cast<uint8_t>((p[0] >> 16) & 0xFFu);
            m_viewportY2 = static_cast<uint8_t>((p[0] >> 24) & 0xFFu);
            break;

        default:
            break;
    }
}

void NdsGpu::submitVertex(int16_t vx, int16_t vy, int16_t vz) {
    if (m_submittedPolys.size() >= 2048) return;

    Vertex3D v{};
    v.x = fx12ToFloat(vx);
    v.y = fx12ToFloat(vy);
    v.z = fx12ToFloat(vz);
    v.w = 1.0f;
    v.s = m_curTexS;
    v.t = m_curTexT;
    v.r = m_curCol[0];
    v.g = m_curCol[1];
    v.b = m_curCol[2];

    // View space = [x y z 1] * m_posMtx
    const float ex = v.x * m_posMtx.m[0] + v.y * m_posMtx.m[4] + v.z * m_posMtx.m[8]  + m_posMtx.m[12];
    const float ey = v.x * m_posMtx.m[1] + v.y * m_posMtx.m[5] + v.z * m_posMtx.m[9]  + m_posMtx.m[13];
    const float ez = v.x * m_posMtx.m[2] + v.y * m_posMtx.m[6] + v.z * m_posMtx.m[10] + m_posMtx.m[14];
    const float ew = v.x * m_posMtx.m[3] + v.y * m_posMtx.m[7] + v.z * m_posMtx.m[11] + m_posMtx.m[15];

    // Clip space = [ex ey ez ew] * m_projMtx
    v.cx = ex * m_projMtx.m[0] + ey * m_projMtx.m[4] + ez * m_projMtx.m[8]  + ew * m_projMtx.m[12];
    v.cy = ex * m_projMtx.m[1] + ey * m_projMtx.m[5] + ez * m_projMtx.m[9]  + ew * m_projMtx.m[13];
    v.cz = ex * m_projMtx.m[2] + ey * m_projMtx.m[6] + ez * m_projMtx.m[10] + ew * m_projMtx.m[14];
    v.cw = ex * m_projMtx.m[3] + ey * m_projMtx.m[7] + ez * m_projMtx.m[11] + ew * m_projMtx.m[15];

    m_primVerts.push_back(v);

    auto pushPoly3 = [&](const Vertex3D& a, const Vertex3D& b, const Vertex3D& c) {
        Poly3D poly{};
        poly.count    = 3;
        poly.verts    = {a, b, c, {}};
        poly.polyAttr = m_polyAttr;
        poly.texParam = m_texParam;
        poly.plttBase = m_plttBase;
        m_submittedPolys.push_back(poly);
    };

    auto pushPoly4 = [&](const Vertex3D& a, const Vertex3D& b, const Vertex3D& c, const Vertex3D& d) {
        Poly3D poly{};
        poly.count    = 4;
        poly.verts    = {a, b, c, d};
        poly.polyAttr = m_polyAttr;
        poly.texParam = m_texParam;
        poly.plttBase = m_plttBase;
        m_submittedPolys.push_back(poly);
    };

    switch (m_primType) {
        case 0: // Separate Triangles
            if (m_primVerts.size() == 3) {
                pushPoly3(m_primVerts[0], m_primVerts[1], m_primVerts[2]);
                m_primVerts.clear();
            }
            break;
        case 1: // Separate Quads
            if (m_primVerts.size() == 4) {
                pushPoly4(m_primVerts[0], m_primVerts[1], m_primVerts[2], m_primVerts[3]);
                m_primVerts.clear();
            }
            break;
        case 2: // Triangle Strip
            if (m_primVerts.size() >= 3) {
                const std::size_t n = m_primVerts.size();
                if ((m_stripStep & 1u) == 0) {
                    pushPoly3(m_primVerts[n - 3], m_primVerts[n - 2], m_primVerts[n - 1]);
                } else {
                    pushPoly3(m_primVerts[n - 2], m_primVerts[n - 3], m_primVerts[n - 1]);
                }
                ++m_stripStep;
            }
            break;
        case 3: // Quad Strip (v0, v1, v3, v2 no padrão NDS!)
            if (m_primVerts.size() >= 4 && ((m_primVerts.size() - 4) % 2 == 0)) {
                const std::size_t n = m_primVerts.size();
                pushPoly4(m_primVerts[n - 4], m_primVerts[n - 3], m_primVerts[n - 1], m_primVerts[n - 2]);
            }
            break;
        default:
            break;
    }
}

void NdsGpu::rasterize3DFrame() {
    const uint8_t clearAlpha = static_cast<uint8_t>((m_clearColor >> 16) & 0x1Fu);
    const uint32_t clearArgb = bgr555ToArgb(static_cast<uint16_t>(m_clearColor & 0x7FFFu));
    const float clearZ = static_cast<float>(m_clearDepth & 0x7FFFu) / 32767.0f;

    m_color3D.fill(clearArgb);
    m_valid3D.fill(clearAlpha > 0);
    m_depth3D.fill(clearZ);

    for (const auto& poly : m_renderPolys) {
        const uint8_t alpha = static_cast<uint8_t>((poly.polyAttr >> 16) & 0x1Fu);
        if (alpha == 0) continue; // Wireframe / invisível

        rasterizeTriangle(poly.verts[0], poly.verts[1], poly.verts[2],
                          poly.polyAttr, poly.texParam, poly.plttBase);
        if (poly.count == 4) {
            rasterizeTriangle(poly.verts[0], poly.verts[2], poly.verts[3],
                              poly.polyAttr, poly.texParam, poly.plttBase);
        }
    }
}

void NdsGpu::rasterizeTriangle(
    const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
    uint32_t polyAttr, uint32_t texParam, uint32_t plttBase
) {
    // Rejeita triângulos atrás do plano near (cw <= 0.0001f)
    if (v0.cw <= 0.0001f || v1.cw <= 0.0001f || v2.cw <= 0.0001f) {
        return;
    }

    const float vpW  = static_cast<float>(std::max(1, m_viewportX2 - m_viewportX1 + 1));
    const float vpH  = static_cast<float>(std::max(1, m_viewportY2 - m_viewportY1 + 1));
    const float vpX0 = static_cast<float>(m_viewportX1);
    const float vpY0 = static_cast<float>(191 - m_viewportY2);

    auto project = [&](const Vertex3D& v, float& sx, float& sy, float& sz, float& invW) {
        invW = 1.0f / v.cw;
        const float ndcX = v.cx * invW;
        const float ndcY = v.cy * invW;
        const float ndcZ = v.cz * invW;
        sx = vpX0 + (ndcX + 1.0f) * 0.5f * vpW;
        sy = vpY0 + (1.0f - ndcY) * 0.5f * vpH;
        sz = std::clamp((ndcZ + 1.0f) * 0.5f, 0.0f, 1.0f);
    };

    float x0, y0, z0, iw0;
    float x1, y1, z1, iw1;
    float x2, y2, z2, iw2;
    project(v0, x0, y0, z0, iw0);
    project(v1, x1, y1, z1, iw1);
    project(v2, x2, y2, z2, iw2);

    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (std::abs(area) < 0.001f) return;

    const bool drawFront = (polyAttr & (1u << 7)) != 0;
    const bool drawBack  = (polyAttr & (1u << 6)) != 0;
    // Em coordenadas de tela (Y cresce para baixo), CCW tem area < 0 (front face)
    if (area < 0.0f && !drawFront) return;
    if (area > 0.0f && !drawBack)  return;

    const int minX = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
    const int maxX = std::min(SCREEN_WIDTH - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
    const int maxY = std::min(SCREEN_HEIGHT - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));

    const float invArea = 1.0f / area;
    const bool hasTex   = (m_disp3dCnt & 1u) != 0 && (((texParam >> 26) & 0x7u) != 0);
    const uint8_t polyAlpha = static_cast<uint8_t>((polyAttr >> 16) & 0x1Fu);

    for (int py = minY; py <= maxY; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int px = minX; px <= maxX; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;

            const float w0 = ((x1 - fx) * (y2 - fy) - (y1 - fy) * (x2 - fx)) * invArea;
            const float w1 = ((x2 - fx) * (y0 - fy) - (y2 - fy) * (x0 - fx)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f) continue;

            const float z = w0 * z0 + w1 * z1 + w2 * z2;
            const std::size_t idx = static_cast<std::size_t>(py * SCREEN_WIDTH + px);
            if (z > m_depth3D[idx] + 0.00005f) continue;

            const float interpIw = w0 * iw0 + w1 * iw1 + w2 * iw2;
            const float perspW   = (interpIw > 1e-7f) ? (1.0f / interpIw) : 1.0f;

            uint32_t r = static_cast<uint32_t>(std::clamp((w0 * v0.r * iw0 + w1 * v1.r * iw1 + w2 * v2.r * iw2) * perspW, 0.0f, 255.0f));
            uint32_t g = static_cast<uint32_t>(std::clamp((w0 * v0.g * iw0 + w1 * v1.g * iw1 + w2 * v2.g * iw2) * perspW, 0.0f, 255.0f));
            uint32_t b = static_cast<uint32_t>(std::clamp((w0 * v0.b * iw0 + w1 * v1.b * iw1 + w2 * v2.b * iw2) * perspW, 0.0f, 255.0f));
            uint8_t effAlpha = polyAlpha;

            if (hasTex) {
                const float s = (w0 * v0.s * iw0 + w1 * v1.s * iw1 + w2 * v2.s * iw2) * perspW;
                const float t = (w0 * v0.t * iw0 + w1 * v1.t * iw1 + w2 * v2.t * iw2) * perspW;
                const uint32_t texCol = sample3DTexture(texParam, plttBase, s, t);
                const uint8_t texA = static_cast<uint8_t>((texCol >> 24) & 0xFFu);
                if (texA == 0) continue;

                const uint32_t tr = (texCol >> 16) & 0xFFu;
                const uint32_t tg = (texCol >> 8) & 0xFFu;
                const uint32_t tb = texCol & 0xFFu;
                const uint8_t polyMode = (polyAttr >> 4) & 0x3u;
                if (polyMode == 0) { // Modulate
                    r = (r * tr) / 255u;
                    g = (g * tg) / 255u;
                    b = (b * tb) / 255u;
                    effAlpha = static_cast<uint8_t>((static_cast<uint32_t>(effAlpha) * (texA >> 3)) / 31u);
                } else { // Decal / Highlight
                    r = tr;
                    g = tg;
                    b = tb;
                }
            }

            if (effAlpha == 0) continue;

            if (effAlpha < 31 && m_valid3D[idx] && (m_disp3dCnt & (1u << 3))) {
                const uint32_t dst = m_color3D[idx];
                const uint32_t dr = (dst >> 16) & 0xFFu;
                const uint32_t dg = (dst >> 8) & 0xFFu;
                const uint32_t db = dst & 0xFFu;
                r = (r * effAlpha + dr * (31u - effAlpha)) / 31u;
                g = (g * effAlpha + dg * (31u - effAlpha)) / 31u;
                b = (b * effAlpha + db * (31u - effAlpha)) / 31u;
            } else {
                m_depth3D[idx] = z;
            }

            m_color3D[idx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            m_valid3D[idx] = true;
        }
    }
}

uint32_t NdsGpu::sample3DTexture(uint32_t texParam, uint32_t plttBase, float s, float t) const noexcept {
    if (!m_bus) return 0xFFFFFFFFu;
    const uint8_t fmt    = (texParam >> 26) & 0x7u;
    const int texW       = 8 << ((texParam >> 20) & 0x7u);
    const int texH       = 8 << ((texParam >> 23) & 0x7u);
    const bool repS      = (texParam & (1u << 16)) != 0;
    const bool repT      = (texParam & (1u << 17)) != 0;
    const bool flipS     = (texParam & (1u << 18)) != 0;
    const bool flipT     = (texParam & (1u << 19)) != 0;
    const bool col0Trans = (texParam & (1u << 29)) != 0;
    const uint32_t baseOff = (texParam & 0xFFFFu) << 3;

    int tx = static_cast<int>(std::floor(s));
    int ty = static_cast<int>(std::floor(t));

    if (repS) {
        const bool odd = (tx & texW) != 0;
        tx &= (texW - 1);
        if (flipS && odd) tx = texW - 1 - tx;
    } else {
        tx = std::clamp(tx, 0, texW - 1);
    }

    if (repT) {
        const bool odd = (ty & texH) != 0;
        ty &= (texH - 1);
        if (flipT && odd) ty = texH - 1 - ty;
    } else {
        ty = std::clamp(ty, 0, texH - 1);
    }

    const uint32_t pixelIndex = static_cast<uint32_t>(ty * texW + tx);

    switch (fmt) {
        case 1: { // A3I5 Translucent (5-bit color index + 3-bit alpha)
            const uint8_t v = m_bus->readTexVram8(baseOff + pixelIndex);
            const uint8_t idx = v & 0x1Fu;
            const uint8_t a3  = (v >> 5) & 0x7u;
            const uint32_t a8 = (a3 * 255u) / 7u;
            const uint32_t rgb = bgr555ToArgb(m_bus->readTexPalette16((plttBase << 4) + idx * 2u)) & 0x00FFFFFFu;
            return (a8 << 24) | rgb;
        }
        case 2: { // 4-Color Palette (2bpp; plttBase << 3!)
            const uint8_t b = m_bus->readTexVram8(baseOff + (pixelIndex >> 2));
            const uint8_t idx = (b >> ((pixelIndex & 3u) * 2u)) & 0x3u;
            if (col0Trans && idx == 0) return 0x00000000u;
            return bgr555ToArgb(m_bus->readTexPalette16((plttBase << 3) + idx * 2u));
        }
        case 3: { // 16-Color Palette (4bpp)
            const uint8_t b = m_bus->readTexVram8(baseOff + (pixelIndex >> 1));
            const uint8_t idx = (pixelIndex & 1u) ? ((b >> 4) & 0xFu) : (b & 0xFu);
            if (col0Trans && idx == 0) return 0x00000000u;
            return bgr555ToArgb(m_bus->readTexPalette16((plttBase << 4) + idx * 2u));
        }
        case 4: { // 256-Color Palette (8bpp)
            const uint8_t idx = m_bus->readTexVram8(baseOff + pixelIndex);
            if (col0Trans && idx == 0) return 0x00000000u;
            return bgr555ToArgb(m_bus->readTexPalette16((plttBase << 4) + static_cast<uint32_t>(idx) * 2u));
        }
        case 6: { // A5I3 Translucent (3-bit color index + 5-bit alpha)
            const uint8_t v = m_bus->readTexVram8(baseOff + pixelIndex);
            const uint8_t idx = v & 0x7u;
            const uint8_t a5  = (v >> 3) & 0x1Fu;
            const uint32_t a8 = (a5 << 3) | (a5 >> 2);
            const uint32_t rgb = bgr555ToArgb(m_bus->readTexPalette16((plttBase << 4) + idx * 2u)) & 0x00FFFFFFu;
            return (a8 << 24) | rgb;
        }
        case 7: { // Direct Color 16-bit BGR555 + Alpha bit 15
            const uint16_t px = m_bus->readTexVram16(baseOff + pixelIndex * 2u);
            if ((px & 0x8000u) == 0) return 0x00000000u;
            return bgr555ToArgb(px);
        }
        default:
            return 0xFFFFFFFFu;
    }
}

void NdsGpu::serializeState(SaveStateWriter& out) const {
    out.writePod(m_lineCycles);
    out.writePod(m_vcount);
    out.writePod(m_inHBlank);
    out.writePod(m_dispStat9);
    out.writePod(m_dispStat7);
    out.writePod(m_powCnt1);
    out.writePod(m_vblankCount);
    out.writeArray(m_eng);
    out.writeArray(m_topScreen);
    out.writeArray(m_bottomScreen);
    out.writePod(m_disp3dCnt);
    out.writePod(m_gxStat);
    out.writePod(m_clearColor);
    out.writePod(m_clearDepth);
    out.writePod(m_mtxMode);
    out.writePod(m_projMtx);
    out.writePod(m_posMtx);
    out.writePod(m_dirMtx);
    out.writeArray(m_posStack);
    out.writeArray(m_dirStack);
    out.writePod(m_posSp);
    out.writeArray(m_color3D);
    out.writeArray(m_valid3D);
}

bool NdsGpu::deserializeState(SaveStateReader& in) {
    m_submittedPolys.clear();
    m_renderPolys.clear();
    m_primVerts.clear();
    return in.readPod(m_lineCycles) && in.readPod(m_vcount) && in.readPod(m_inHBlank) &&
           in.readPod(m_dispStat9) && in.readPod(m_dispStat7) && in.readPod(m_powCnt1) &&
           in.readPod(m_vblankCount) && in.readArray(m_eng) &&
           in.readArray(m_topScreen) && in.readArray(m_bottomScreen) &&
           in.readPod(m_disp3dCnt) && in.readPod(m_gxStat) && in.readPod(m_clearColor) &&
           in.readPod(m_clearDepth) && in.readPod(m_mtxMode) && in.readPod(m_projMtx) &&
           in.readPod(m_posMtx) && in.readPod(m_dirMtx) && in.readArray(m_posStack) &&
           in.readArray(m_dirStack) && in.readPod(m_posSp) &&
           in.readArray(m_color3D) && in.readArray(m_valid3D);
}

} // namespace gba::core::nds
