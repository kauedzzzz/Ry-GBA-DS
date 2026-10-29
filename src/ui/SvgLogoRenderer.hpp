#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ryugba::ui {

class SvgLogoRenderer {
public:
    // Rasteriza um arquivo SVG vetorial (com supersampling 4x4 MSAA) para pixels ARGB8888
    [[nodiscard]] static std::vector<uint32_t> rasterizeToArgb(
        const std::filesystem::path& svgPath,
        int outWidth,
        int outHeight
    );

    // Cria SDL_Surface* para SDL_SetWindowIcon (ícone na barra de título da janela)
    [[nodiscard]] static SDL_Surface* createWindowIconSurface(
        const std::filesystem::path& svgPath,
        int size = 64
    );

    // Cria SDL_Texture* anti-aliased para a Tela Inicial (Idle) e o diálogo "About RyūGBA"
    [[nodiscard]] static SDL_Texture* createLogoTexture(
        SDL_Renderer* renderer,
        const std::filesystem::path& svgPath,
        int size
    );
};

} // namespace ryugba::ui

