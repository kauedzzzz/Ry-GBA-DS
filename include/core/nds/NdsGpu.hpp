#pragma once
#include "core/SaveStateStream.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gba::core::nds {

class NdsBus;

class NdsGpu {
public:
    static constexpr int SCREEN_WIDTH       = 256;
    static constexpr int SCREEN_HEIGHT      = 192;
    static constexpr int VISIBLE_LINES      = 192;
    static constexpr int TOTAL_LINES        = 263;
    static constexpr int ARM7_DOTS_HBLANK   = 1606; // ~256 dots visíveis * 6 + margem
    static constexpr int ARM7_CYCLES_LINE   = 2130; // 560.190 ciclos ARM7 / quadro

    NdsGpu();

    void attachBus(NdsBus* bus) noexcept { m_bus = bus; }
    void reset() noexcept;

    void stepArm7Cycles(int32_t arm7Cycles);

    [[nodiscard]] std::span<const uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> topFramebuffer() const noexcept {
        return m_topScreen;
    }
    [[nodiscard]] std::span<const uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> bottomFramebuffer() const noexcept {
        return m_bottomScreen;
    }

    [[nodiscard]] uint16_t vcount() const noexcept { return m_vcount; }
    [[nodiscard]] uint16_t dispStat9() const noexcept { return m_dispStat9; }
    [[nodiscard]] uint16_t dispStat7() const noexcept { return m_dispStat7; }
    void writeDispStat9(uint16_t val) noexcept;
    void writeDispStat7(uint16_t val) noexcept;

    [[nodiscard]] uint16_t powCnt1() const noexcept { return m_powCnt1; }
    void writePowCnt1(uint16_t val) noexcept { m_powCnt1 = val; }

    // Registradores 2D Engine A (offset 0x00..0x6F) e Engine B (offset 0x00..0x6F)
    [[nodiscard]] uint8_t  readEngine2D8(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t readEngine2D16(int engine, uint32_t offset) const noexcept;
    [[nodiscard]] uint32_t readEngine2D32(int engine, uint32_t offset) const noexcept;

    void writeEngine2D8(int engine, uint32_t offset, uint8_t val) noexcept;
    void writeEngine2D16(int engine, uint32_t offset, uint16_t val) noexcept;
    void writeEngine2D32(int engine, uint32_t offset, uint32_t val) noexcept;

    // Registradores 3D Geometry & Rasterizer (0x04000320..0x040006FF)
    [[nodiscard]] uint8_t  read3D8(uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t read3D16(uint32_t offset) const noexcept;
    [[nodiscard]] uint32_t read3D32(uint32_t offset) const noexcept;

    void write3D8(uint32_t offset, uint8_t val) noexcept;
    void write3D16(uint32_t offset, uint16_t val) noexcept;
    void write3D32(uint32_t offset, uint32_t val) noexcept;

    [[nodiscard]] bool isGxFifoLessThanHalfFull() const noexcept { return m_gxCmdQueue.size() < 128; }
    [[nodiscard]] uint64_t vblankCount() const noexcept { return m_vblankCount; }

    void setSkipRender(bool skip) noexcept { m_skipRender = skip; }
    [[nodiscard]] bool isSkipRender() const noexcept { return m_skipRender; }

    void serializeState(SaveStateWriter& out) const;
    bool deserializeState(SaveStateReader& in);

private:
    struct Engine2DState {
        std::array<uint8_t, 0x70> regs{};
        int32_t bgRefX[2]{0, 0}; // BG2X, BG3X internos (20.8 fixed-point)
        int32_t bgRefY[2]{0, 0}; // BG2Y, BG3Y internos (20.8 fixed-point)
    };

    struct Mat4 {
        std::array<float, 16> m{
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
    };

    struct Vertex3D {
        float    x{0}, y{0}, z{0}, w{1};
        float    cx{0}, cy{0}, cz{0}, cw{1}; // Clip space
        float    s{0}, t{0};                 // Texcoords em texels
        uint8_t  r{255}, g{255}, b{255};
    };

    struct Poly3D {
        uint8_t  count{3}; // 3 ou 4 vértices
        std::array<Vertex3D, 4> verts{};
        uint32_t polyAttr{0x001F0000u};
        uint32_t texParam{0};
        uint32_t plttBase{0};
    };

    struct GxCmdEntry {
        uint8_t  cmd{0};
        uint32_t param{0};
    };

    NdsBus*  m_bus{nullptr};
    int32_t  m_lineCycles{0};
    uint16_t m_vcount{0};
    bool     m_inHBlank{false};
    uint16_t m_dispStat9{0};
    uint16_t m_dispStat7{0};
    uint16_t m_powCnt1{0x820Fu}; // Telas, 2D A/B e 3D ativos; Engine A no topo por padrão
    uint64_t m_vblankCount{0};
    bool     m_skipRender{false};

    std::array<Engine2DState, 2> m_eng{};
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_topScreen{};
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_bottomScreen{};

    // =========================================================================
    // Estado do Motor 3D (Geometry Engine + Software Rasterizer)
    // =========================================================================
    uint16_t m_disp3dCnt{0};
    uint32_t m_gxStat{0x06000000u}; // FIFO empty + < half full
    uint32_t m_clearColor{0x001F0000u};
    uint16_t m_clearDepth{0x7FFFu};
    uint8_t  m_viewportX1{0}, m_viewportY1{0}, m_viewportX2{255}, m_viewportY2{191};

    uint8_t  m_mtxMode{0};
    Mat4     m_projMtx{};
    Mat4     m_projStack{};
    Mat4     m_posMtx{};
    Mat4     m_dirMtx{};
    std::array<Mat4, 32> m_posStack{};
    std::array<Mat4, 32> m_dirStack{};
    int      m_posSp{0};
    Mat4     m_texMtx{};

    // Geometria e Iluminação atual
    uint8_t  m_primType{0};
    uint32_t m_polyAttr{0x001F0000u};
    uint32_t m_pendingPolyAttr{0x001F0000u};
    uint32_t m_texParam{0};
    uint32_t m_plttBase{0};
    int16_t  m_curVtx[3]{0, 0, 0};
    float    m_curTexS{0.0f}, m_curTexT{0.0f};
    uint8_t  m_curCol[3]{255, 255, 255};

    // 4 Luzes direcionais + Material
    std::array<std::array<float, 3>, 4>   m_lightVec{};
    std::array<std::array<uint8_t, 3>, 4> m_lightCol{};
    uint8_t m_matDiff[3]{255, 255, 255};
    uint8_t m_matAmb[3]{64, 64, 64};
    uint8_t m_matSpec[3]{0, 0, 0};
    uint8_t m_matEmi[3]{0, 0, 0};

    std::vector<Vertex3D> m_primVerts;
    uint32_t              m_stripStep{0};
    std::vector<Poly3D>   m_submittedPolys;
    std::vector<Poly3D>   m_renderPolys;
    bool                  m_swapPending{false};
    uint32_t              m_swapFlags{0};

    // Decodificador de comandos GXFIFO
    uint32_t                m_packedCmds{0};
    uint8_t                 m_currentCmd{0};
    uint8_t                 m_paramsNeeded{0};
    std::array<uint32_t, 32> m_paramBuf{};
    uint8_t                 m_paramCount{0};
    std::vector<GxCmdEntry> m_gxCmdQueue;

    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_color3D{};
    std::array<bool,     SCREEN_WIDTH * SCREEN_HEIGHT> m_valid3D{};
    std::array<float,    SCREEN_WIDTH * SCREEN_HEIGHT> m_depth3D{};

    void renderScanline(uint16_t line);
    void renderEngine2DLine(int engine, uint16_t line, uint32_t* outLine);
    void renderTextBgLine(int engine, int bg, uint16_t line, uint32_t dispCnt, uint16_t bgCnt,
                          uint32_t* lineOut, uint8_t* linePrio);
    void renderAffineOrExtBgLine(int engine, int bg, uint16_t line, uint32_t dispCnt, uint16_t bgCnt,
                                 uint32_t* lineOut, uint8_t* linePrio);
    void renderSpritesLine(int engine, uint16_t line, uint32_t dispCnt,
                           uint32_t* lineOut, uint8_t* linePrio);
    void applyMasterBrightness(int engine, uint32_t* outLine) const noexcept;

    // Execução 3D
    [[nodiscard]] static uint8_t gxParamCount(uint8_t cmd) noexcept;
    void submitGxCommandByte(uint8_t cmd);
    void submitGxParam(uint32_t param);
    void executeGxCommand(uint8_t cmd, const uint32_t* params);
    void submitVertex(int16_t vx, int16_t vy, int16_t vz);
    void rasterize3DFrame();
    void rasterizeTriangle(const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                           uint32_t polyAttr, uint32_t texParam, uint32_t plttBase);
    [[nodiscard]] uint32_t sample3DTexture(uint32_t texParam, uint32_t plttBase, float s, float t) const noexcept;
    void checkGxFifoIrq() noexcept;
};

} // namespace gba::core::nds

