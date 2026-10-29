cmake_minimum_required(VERSION 3.18)
project(RyuDS VERSION 2.0.0 LANGUAGES CXX)

# Padrão C++20 obrigatório (std::span, std::rotr, std::popcount, std::countl_zero, std::atomic)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Exporta build/compile_commands.json para IntelliSense / clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SDL2: prioriza pkg-config (-lSDL2) para evitar caminhos .so com versão específica
# obsoleta em SDL2Targets.cmake, mantendo o target padrão SDL2::SDL2.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2_PC QUIET IMPORTED_TARGET GLOBAL sdl2)
endif()

if(TARGET PkgConfig::SDL2_PC)
    add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2_PC)
else()
    find_package(SDL2 REQUIRED)
endif()

# =============================================================================
# 1. NÚCLEO HÍBRIDO NDS + GBA DESACOPLADO (Zero dependências de UI / SDL2)
# =============================================================================
set(CORE_SOURCES
    src/core/HybridCore.cpp
    src/core/GbaCore.cpp
    src/core/cpu/Arm7Tdmi.cpp
    src/core/memory/MemoryBus.cpp
    src/core/memory/save/SaveDetector.cpp
    src/core/memory/save/SramSave.cpp
    src/core/memory/save/FlashSave.cpp
    src/core/memory/save/EepromSave.cpp
    src/core/ppu/Ppu.cpp
    src/core/apu/Apu.cpp
    src/core/input/InputManager.cpp
    src/core/BuiltInDemoRom.cpp
    # Subsistema Dual-Core Nintendo DS (.nds / .dsi / .srl)
    src/core/nds/NdsCartridge.cpp
    src/core/nds/TouchController.cpp
    src/core/nds/NdsSpu.cpp
    src/core/nds/NdsGpu.cpp
    src/core/nds/NdsBus.cpp
    src/core/nds/Arm9Cpu.cpp
    src/core/nds/NdsArm7Cpu.cpp
    src/core/nds/NdsCore.cpp
    src/core/nds/BuiltInNdsDemoRom.cpp
)

add_library(gba_core STATIC ${CORE_SOURCES})
target_include_directories(gba_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(gba_core PRIVATE -Wall -Wextra)

# =============================================================================
# 2. FRONTEND MINIMALISTA ESTILO melonDS — RyūDS / RyūGBA (Dragon Edition)
# =============================================================================
set(FRONTEND_SOURCES
    src/config/ConfigManager.cpp
    src/input/InputMapper.cpp
    src/ui/SvgLogoRenderer.cpp
    src/ui/MenuBar.cpp
    src/ui/Modals.cpp
    src/frontend/EmulatorApp.cpp
    src/frontend/main.cpp
)

add_executable(gba_emulator ${FRONTEND_SOURCES})
target_include_directories(gba_emulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(gba_emulator PRIVATE gba_core SDL2::SDL2)
target_compile_options(gba_emulator PRIVATE -Wall -Wextra)
target_compile_definitions(gba_emulator PRIVATE GBA_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Gera os aliases executáveis build/ryugba e build/ryuds
add_custom_command(TARGET gba_emulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:gba_emulator>
            $<TARGET_FILE_DIR:gba_emulator>/ryugba
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:gba_emulator>
            $<TARGET_FILE_DIR:gba_emulator>/ryuds
    COMMENT "Criando binarios alias ryugba e ryuds"
)

# =============================================================================
# 3. SUÍTE DE AUTO-TESTE (NDS Dual-Core + GBA + SaveStates + Config + SVG Logo)
# =============================================================================
add_executable(gba_tests
    src/tests/CoreSelfTest.cpp
    src/config/ConfigManager.cpp
    src/ui/SvgLogoRenderer.cpp
)
target_include_directories(gba_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(gba_tests PRIVATE gba_core SDL2::SDL2)
target_compile_options(gba_tests PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(gba_tests PRIVATE GBA_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
