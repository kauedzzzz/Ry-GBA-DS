#include "ui/SvgLogoRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace ryugba::ui {

namespace {

// Fallback idêntico a assets/ryu_logo.svg caso o binário seja executado fora da pasta raiz
constexpr std::string_view EMBEDDED_RYU_LOGO_SVG = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">
  <polygon points="26,18 174,18 184,28 184,174 172,186 28,186 16,174 16,28" fill="#1a1c23" stroke="#2e3340" stroke-width="3"/>
  <rect x="44" y="18" width="112" height="12" fill="#22252e"/>
  <rect x="56" y="21" width="16" height="5" fill="#121316"/>
  <rect x="92" y="21" width="16" height="5" fill="#121316"/>
  <rect x="128" y="21" width="16" height="5" fill="#121316"/>
  <polygon points="28,36 172,36 172,172 28,172" fill="#121316" stroke="#22252e" stroke-width="2"/>
  <polygon points="32,102 42,56 84,32 162,38 128,54 166,62 128,78 92,76 62,94" fill="#e63946"/>
  <polygon points="32,102 62,94 106,88 122,96 88,104 60,116 54,144 88,162 114,166 76,174 42,160 26,130" fill="#c92a37"/>
  <polygon points="74,92 106,88 98,102 86,96" fill="#f87171"/>
  <polygon points="74,66 98,55 116,65 92,74" fill="#10b981"/>
  <polygon points="93,57 97,57 99,65 97,72 93,72 91,65" fill="#121316"/>
  <rect x="90" y="121" width="72" height="24" fill="#22252e" stroke="#383e4e" stroke-width="2"/>
  <rect x="114" y="97" width="24" height="72" fill="#22252e" stroke="#383e4e" stroke-width="2"/>
  <rect x="115" y="120" width="22" height="26" fill="#22252e"/>
  <polygon points="126,102 120,112 132,112" fill="#10b981"/>
  <polygon points="126,164 120,154 132,154" fill="#10b981"/>
  <polygon points="95,133 105,127 105,139" fill="#10b981"/>
  <polygon points="157,133 147,127 147,139" fill="#10b981"/>
  <circle cx="126" cy="133" r="5" fill="#e63946"/>
</svg>
)SVG";

struct Vec2 {
    float x{0.0f};
    float y{0.0f};
};

struct Rgba {
    uint8_t r{0}, g{0}, b{0}, a{0};
};

enum class ShapeKind {
    Polygon,
    Circle
};

struct SvgShape {
    ShapeKind         kind{ShapeKind::Polygon};
    std::vector<Vec2> pts;
    Vec2              center{};
    float             radius{0.0f};
    Rgba              fill{};
    bool              hasFill{false};
    Rgba              stroke{};
    bool              hasStroke{false};
    float             strokeWidth{1.0f};
    float             minX{0}, minY{0}, maxX{0}, maxY{0};
};

std::string getAttr(std::string_view tag, std::string_view attrName) {
    const std::string key = std::string(attrName) + "=\"";
    std::size_t pos = tag.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    const std::size_t end = tag.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(tag.substr(pos, end - pos));
}

bool parseHexColor(std::string_view hex, Rgba& out) {
    if (hex.empty() || hex == "none") return false;
    if (hex[0] == '#') hex.remove_prefix(1);
    if (hex.size() != 6) return false;
    auto hexByte = [](std::string_view s) -> uint8_t {
        unsigned val = 0;
        for (char c : s) {
            val <<= 4;
            if (c >= '0' && c <= '9') val |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') val |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= static_cast<unsigned>(c - 'A' + 10);
        }
        return static_cast<uint8_t>(val & 0xFFu);
    };
    out.r = hexByte(hex.substr(0, 2));
    out.g = hexByte(hex.substr(2, 2));
    out.b = hexByte(hex.substr(4, 2));
    out.a = 255;
    return true;
}

float parseFloatOr(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    try {
        return std::stof(s);
    } catch (...) {
        return fallback;
    }
}

void computeBounds(SvgShape& sh) {
    const float pad = sh.hasStroke ? (sh.strokeWidth * 0.6f) : 0.0f;
    if (sh.kind == ShapeKind::Circle) {
        sh.minX = sh.center.x - sh.radius - pad;
        sh.maxX = sh.center.x + sh.radius + pad;
        sh.minY = sh.center.y - sh.radius - pad;
        sh.maxY = sh.center.y + sh.radius + pad;
        return;
    }
    if (sh.pts.empty()) return;
    sh.minX = sh.maxX = sh.pts[0].x;
    sh.minY = sh.maxY = sh.pts[0].y;
    for (const auto& p : sh.pts) {
        sh.minX = std::min(sh.minX, p.x);
        sh.maxX = std::max(sh.maxX, p.x);
        sh.minY = std::min(sh.minY, p.y);
        sh.maxY = std::max(sh.maxY, p.y);
    }
    sh.minX -= pad;
    sh.minY -= pad;
    sh.maxX += pad;
    sh.maxY += pad;
}

bool pointInPolygon(float px, float py, const std::vector<Vec2>& pts) noexcept {
    bool inside = false;
    const std::size_t n = pts.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[i].x, yi = pts[i].y;
        const float xj = pts[j].x, yj = pts[j].y;
        const bool intersect = ((yi > py) != (yj > py)) &&
                               (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12f) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

float distSqToSegment(float px, float py, Vec2 a, Vec2 b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float wx = px - a.x;
    const float wy = py - a.y;
    const float lenSq = vx * vx + vy * vy;
    float t = (lenSq > 1e-8f) ? ((wx * vx + wy * vy) / lenSq) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = px - (a.x + t * vx);
    const float dy = py - (a.y + t * vy);
    return dx * dx + dy * dy;
}

bool pointOnPolygonStroke(float px, float py, const std::vector<Vec2>& pts, float halfWidth) noexcept {
    const float threshSq = halfWidth * halfWidth;
    const std::size_t n = pts.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        if (distSqToSegment(px, py, pts[j], pts[i]) <= threshSq) {
            return true;
        }
    }
    return false;
}

struct ParsedSvgDocument {
    float viewW{200.0f};
    float viewH{200.0f};
    std::vector<SvgShape> shapes;
};

ParsedSvgDocument parseSvg(std::string_view xml) {
    ParsedSvgDocument doc;

    std::size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string_view::npos) {
        if (xml.substr(pos).starts_with("<!--")) {
            const auto closeComment = xml.find("-->", pos + 4);
            if (closeComment == std::string_view::npos) break;
            pos = closeComment + 3;
            continue;
        }
        const std::size_t endTag = xml.find('>', pos);
        if (endTag == std::string_view::npos) break;
        const std::string_view tag = xml.substr(pos + 1, endTag - pos - 1);
        pos = endTag + 1;

        if (tag.starts_with("svg")) {
            const std::string vb = getAttr(tag, "viewBox");
            if (!vb.empty()) {
                std::istringstream iss(vb);
                float minX = 0, minY = 0, w = 200, h = 200;
                if (iss >> minX >> minY >> w >> h && w > 0 && h > 0) {
                    doc.viewW = w;
                    doc.viewH = h;
                }
            }
            continue;
        }

        if (tag.starts_with("polygon")) {
            SvgShape sh;
            sh.kind        = ShapeKind::Polygon;
            sh.hasFill     = parseHexColor(getAttr(tag, "fill"), sh.fill);
            sh.hasStroke   = parseHexColor(getAttr(tag, "stroke"), sh.stroke);
            sh.strokeWidth = parseFloatOr(getAttr(tag, "stroke-width"), 1.0f);

            std::string ptsStr = getAttr(tag, "points");
            for (char& c : ptsStr) {
                if (c == ',') c = ' ';
            }
            std::istringstream iss(ptsStr);
            float x = 0, y = 0;
            while (iss >> x >> y) {
                sh.pts.push_back({x, y});
            }
            if (sh.pts.size() >= 3) {
                computeBounds(sh);
                doc.shapes.push_back(std::move(sh));
            }
        } else if (tag.starts_with("rect")) {
            const float x = parseFloatOr(getAttr(tag, "x"), 0.0f);
            const float y = parseFloatOr(getAttr(tag, "y"), 0.0f);
            const float w = parseFloatOr(getAttr(tag, "width"), 0.0f);
            const float h = parseFloatOr(getAttr(tag, "height"), 0.0f);
            if (w > 0 && h > 0) {
                SvgShape sh;
                sh.kind        = ShapeKind::Polygon;
                sh.hasFill     = parseHexColor(getAttr(tag, "fill"), sh.fill);
                sh.hasStroke   = parseHexColor(getAttr(tag, "stroke"), sh.stroke);
                sh.strokeWidth = parseFloatOr(getAttr(tag, "stroke-width"), 1.0f);
                sh.pts = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
                computeBounds(sh);
                doc.shapes.push_back(std::move(sh));
            }
        } else if (tag.starts_with("circle")) {
            const float cx = parseFloatOr(getAttr(tag, "cx"), 0.0f);
            const float cy = parseFloatOr(getAttr(tag, "cy"), 0.0f);
            const float r  = parseFloatOr(getAttr(tag, "r"), 0.0f);
            if (r > 0) {
                SvgShape sh;
                sh.kind        = ShapeKind::Circle;
                sh.center      = {cx, cy};
                sh.radius      = r;
                sh.hasFill     = parseHexColor(getAttr(tag, "fill"), sh.fill);
                sh.hasStroke   = parseHexColor(getAttr(tag, "stroke"), sh.stroke);
                sh.strokeWidth = parseFloatOr(getAttr(tag, "stroke-width"), 1.0f);
                computeBounds(sh);
                doc.shapes.push_back(std::move(sh));
            }
        }
    }

    return doc;
}

} // namespace

std::vector<uint32_t> SvgLogoRenderer::rasterizeToArgb(
    const std::filesystem::path& svgPath,
    int outWidth,
    int outHeight
) {
    if (outWidth <= 0 || outHeight <= 0) return {};

    std::string xmlContent;
    {
        std::ifstream in(svgPath);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            xmlContent = ss.str();
        }
    }
    if (xmlContent.empty()) {
        xmlContent = std::string(EMBEDDED_RYU_LOGO_SVG);
    }

    const ParsedSvgDocument doc = parseSvg(xmlContent);
    std::vector<uint32_t> pixels(static_cast<std::size_t>(outWidth * outHeight), 0x00000000u);

    const float scaleX = doc.viewW / static_cast<float>(outWidth);
    const float scaleY = doc.viewH / static_cast<float>(outHeight);

    // 4x4 Supersampled Anti-Aliasing (16 amostras por pixel)
    static constexpr int SUB = 4;
    static constexpr float INV_SAMPLES = 1.0f / static_cast<float>(SUB * SUB);

    for (int py = 0; py < outHeight; ++py) {
        for (int px = 0; px < outWidth; ++px) {
            uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;

            for (int sy = 0; sy < SUB; ++sy) {
                const float svgY = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / SUB) * scaleY;
                for (int sx = 0; sx < SUB; ++sx) {
                    const float svgX = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / SUB) * scaleX;

                    Rgba sample{0, 0, 0, 0};
                    for (const auto& sh : doc.shapes) {
                        if (svgX < sh.minX || svgX > sh.maxX || svgY < sh.minY || svgY > sh.maxY) {
                            continue;
                        }
                        if (sh.kind == ShapeKind::Circle) {
                            const float dx = svgX - sh.center.x;
                            const float dy = svgY - sh.center.y;
                            const float dSq = dx * dx + dy * dy;
                            if (sh.hasFill && dSq <= sh.radius * sh.radius) {
                                sample = sh.fill;
                            }
                            if (sh.hasStroke) {
                                const float dist = std::sqrt(dSq);
                                if (std::fabs(dist - sh.radius) <= sh.strokeWidth * 0.5f) {
                                    sample = sh.stroke;
                                }
                            }
                        } else {
                            if (sh.hasFill && pointInPolygon(svgX, svgY, sh.pts)) {
                                sample = sh.fill;
                            }
                            if (sh.hasStroke && pointOnPolygonStroke(svgX, svgY, sh.pts, sh.strokeWidth * 0.5f)) {
                                sample = sh.stroke;
                            }
                        }
                    }

                    sumR += sample.r * sample.a;
                    sumG += sample.g * sample.a;
                    sumB += sample.b * sample.a;
                    sumA += sample.a;
                }
            }

            const auto finalA = static_cast<uint8_t>(std::clamp(static_cast<float>(sumA) * INV_SAMPLES, 0.0f, 255.0f));
            uint8_t finalR = 0, finalG = 0, finalB = 0;
            if (sumA > 0) {
                finalR = static_cast<uint8_t>(std::clamp(static_cast<float>(sumR) / static_cast<float>(sumA), 0.0f, 255.0f));
                finalG = static_cast<uint8_t>(std::clamp(static_cast<float>(sumG) / static_cast<float>(sumA), 0.0f, 255.0f));
                finalB = static_cast<uint8_t>(std::clamp(static_cast<float>(sumB) / static_cast<float>(sumA), 0.0f, 255.0f));
            }

            pixels[static_cast<std::size_t>(py * outWidth + px)] =
                (static_cast<uint32_t>(finalA) << 24) |
                (static_cast<uint32_t>(finalR) << 16) |
                (static_cast<uint32_t>(finalG) << 8)  |
                static_cast<uint32_t>(finalB);
        }
    }

    return pixels;
}

SDL_Surface* SvgLogoRenderer::createWindowIconSurface(const std::filesystem::path& svgPath, int size) {
    const auto pixels = rasterizeToArgb(svgPath, size, size);
    if (pixels.empty()) return nullptr;

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;

    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    for (int y = 0; y < size; ++y) {
        auto* dstRow = reinterpret_cast<uint8_t*>(surf->pixels) + y * surf->pitch;
        std::copy_n(pixels.data() + y * size, size, reinterpret_cast<uint32_t*>(dstRow));
    }
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
    return surf;
}

SDL_Texture* SvgLogoRenderer::createLogoTexture(
    SDL_Renderer* renderer,
    const std::filesystem::path& svgPath,
    int size
) {
    if (!renderer || size <= 0) return nullptr;
    const auto pixels = rasterizeToArgb(svgPath, size, size);
    if (pixels.empty()) return nullptr;

    SDL_Texture* tex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC,
        size,
        size
    );
    if (!tex) return nullptr;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    SDL_UpdateTexture(tex, nullptr, pixels.data(), size * static_cast<int>(sizeof(uint32_t)));
    return tex;
}

} // namespace ryugba::ui

