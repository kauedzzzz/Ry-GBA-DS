#include "core/HybridCore.hpp"
#include "frontend/EmulatorApp.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#ifndef GBA_PROJECT_DIR
#define GBA_PROJECT_DIR "."
#endif

namespace fs = std::filesystem;

namespace {

fs::path findBestAvailableRom(const std::string& explicitArg) {
    std::error_code ec;
    if (!explicitArg.empty() && fs::exists(explicitArg, ec)) {
        return fs::path(explicitArg);
    }

    const fs::path projDir(GBA_PROJECT_DIR);
    const fs::path searchDirs[] = {
        projDir.parent_path() / "roms",
        projDir / "roms",
        fs::path("roms")
    };

    // Prioriza ROMs .nds se presentes, seguido por .gba comerciais
    fs::path fallbackGba;
    for (const auto& dir : searchDirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".nds" || ext == ".dsi" || ext == ".srl") {
                return entry.path();
            }
            if (fallbackGba.empty() &&
                (ext == ".gba" || ext == ".agb" || ext == ".bin") &&
                entry.path().filename() != "GBA_Core_Lab_Demo.gba") {
                fallbackGba = entry.path();
            }
        }
    }
    return fallbackGba;
}

int runHeadlessDiagnostic(
    gba::core::HybridCore& core,
    int headlessFrames,
    const std::string& romArg,
    bool forceNdsDemo
) {
    core.setTraceEnabled(true);

    bool loaded = false;
    if (forceNdsDemo) {
        core.loadBuiltInNdsDemo();
        loaded = core.hasRomLoaded();
    } else {
        const fs::path targetRom = findBestAvailableRom(romArg);
        if (!targetRom.empty()) {
            loaded = core.loadRomFromFile(targetRom);
        }
        if (!loaded) {
            core.loadBuiltInGbaDemo();
            loaded = core.hasRomLoaded();
        }
    }

    int errorCount = 0;
    if (!core.hasRomLoaded()) {
        std::cerr << "[ERRO] Nenhuma ROM carregada.\n";
        ++errorCount;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < headlessFrames; ++f) {
        core.runFrame();
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0
    ).count();

    if (core.consoleType() == gba::core::ConsoleType::NDS) {
        auto& nds = core.ndsCore();
        if (nds.frameCount() != static_cast<uint64_t>(headlessFrames)) {
            std::cerr << "[ERRO] Quadros NDS executados (" << nds.frameCount()
                      << ") diferente do solicitado (" << headlessFrames << ").\n";
            ++errorCount;
        }

        const auto topFb = core.topFramebufferSpan();
        const auto botFb = core.bottomFramebufferSpan();
        std::size_t topNonBlack = 0;
        std::size_t botNonBlack = 0;
        for (uint32_t px : topFb) {
            if ((px & 0x00FFFFFFu) != 0x000000u) ++topNonBlack;
        }
        for (uint32_t px : botFb) {
            if ((px & 0x00FFFFFFu) != 0x000000u) ++botNonBlack;
        }

        const auto& hdr = nds.cartridge().header();
        std::cout << "================ RELATORIO HEADLESS (RyūDS — Nintendo DS) ================\n"
                  << "  ROM          : " << core.romTitle() << " [" << hdr.gameCode << " / Maker " << hdr.makerCode << "]\n"
                  << "  Banner 32x32 : " << (core.hasBannerIcon() ? "Extraido (16 cores 4bpp)" : "Ausente") << "\n"
                  << "  Save (.sav)  : " << gba::core::nds::ndsBackupTypeToString(nds.cartridge().backupType()) << "\n"
                  << "  Quadros      : " << nds.frameCount() << " em " << elapsedMs << " ms\n"
                  << "  ARM946E-S PC : 0x" << std::hex << std::setw(8) << std::setfill('0') << nds.arm9().registers().r[15]
                  << " (" << (nds.arm9().registers().isThumb() ? "THUMB" : "ARM") << ")"
                  << " | DTCM=0x" << std::setw(8) << nds.arm9().dtcmBase()
                  << " | HALT=" << (nds.arm9().isHalted() ? "SIM" : "NAO") << "\n"
                  << "  ARM7TDMI  PC : 0x" << std::setw(8) << nds.arm7().registers().r[15]
                  << " (" << (nds.arm7().registers().isThumb() ? "THUMB" : "ARM") << ")"
                  << " | HALT=" << (nds.arm7().isHalted() ? "SIM" : "NAO") << "\n"
                  << "  IRQ ARM9     : IE9=0x" << std::setw(8) << nds.bus().ie9()
                  << " | IF9=0x" << std::setw(8) << nds.bus().if9()
                  << " | IME9=" << std::dec << nds.bus().ime9() << "\n"
                  << "  IRQ ARM7     : IE7=0x" << std::hex << std::setw(8) << nds.bus().ie7()
                  << " | IF7=0x" << std::setw(8) << nds.bus().if7()
                  << " | IME7=" << std::dec << nds.bus().ime7() << "\n"
                  << "  Tela Superior: " << topNonBlack << "/" << topFb.size() << " pixels ativos (256x192 2D+3D)\n"
                  << "  Tela Inferior: " << botNonBlack << "/" << botFb.size() << " pixels ativos (256x192 Touch)\n"
                  << "  Erros        : " << errorCount << "\n"
                  << (errorCount == 0 ? "[OK] Execucao headless concluida com sucesso.\n"
                                      : "[FALHA] Execucao headless com erros.\n");
        return errorCount == 0 ? 0 : 1;
    }

    auto& gba = core.gbaCore();
    if (gba.frameCount() != static_cast<uint64_t>(headlessFrames)) {
        std::cerr << "[ERRO] Quadros executados (" << gba.frameCount()
                  << ") diferente do solicitado (" << headlessFrames << ").\n";
        ++errorCount;
    }

    const auto fb = gba.framebuffer();
    std::size_t nonBlack = 0;
    std::size_t nonWhite = 0;
    for (uint32_t px : fb) {
        const uint32_t rgb = px & 0x00FFFFFFu;
        if (rgb != 0x000000u) ++nonBlack;
        if (rgb != 0xFFFFFFu && rgb != 0xF8F8F8u) ++nonWhite;
    }

    const auto& swiCounts = gba.cpu().swiCallCounts();
    std::cout << "================ RELATORIO HEADLESS (RyūGBA) ================\n"
              << "  ROM          : " << gba.romTitle() << "\n"
              << "  Save Type    : " << gba::core::saveTypeToString(gba.bus().getActiveSaveType()) << "\n"
              << "  Quadros      : " << gba.frameCount() << " em " << elapsedMs << " ms\n"
              << "  CPU PC       : 0x" << std::hex << std::setw(8) << std::setfill('0') << gba.cpu().registers().r[15]
              << " (" << (gba.cpu().registers().isThumb() ? "THUMB" : "ARM") << ")"
              << " | HALT=" << (gba.cpu().isHalted() ? "SIM" : "NAO") << "\n"
              << "  DISPCNT      : 0x" << std::setw(4) << gba.ppu().getDispCnt()
              << " | DISPSTAT=0x" << std::setw(4) << gba.ppu().getDispStat()
              << " | IE=0x" << std::setw(4) << gba.bus().getIe()
              << " | IF=0x" << std::setw(4) << gba.bus().getIf()
              << " | IME=" << std::dec << gba.bus().getIme() << "\n"
              << "  SWI Chamadas : SWI 0x02(Halt)=" << swiCounts[0x02]
              << " | SWI 0x04(IntrWait)=" << swiCounts[0x04]
              << " | SWI 0x05(VBlankIntrWait)=" << swiCounts[0x05]
              << " | SWI 0x0B(CpuSet)=" << swiCounts[0x0B]
              << " | SWI 0x11/12(LZ77)=" << (swiCounts[0x11] + swiCounts[0x12]) << "\n"
              << "  PPU VBlank   : " << gba.ppu().getVBlankIrqCount() << " IRQs VBlank disparadas pela PPU\n"
              << "  Save 0x0E00..: Leituras=" << gba.bus().getSaveReadCount()
              << " | Escritas=" << gba.bus().getSaveWriteCount() << "\n"
              << "  Stuck Loops  : " << gba.cpu().stuckLoopDetectionCount() << " ocorrencia(s) > 50.000 ciclos\n"
              << "  Framebuffer  : " << nonBlack << "/" << fb.size() << " pixels nao-pretos ("
              << nonWhite << " nao-brancos)\n"
              << "  Erros        : " << errorCount << "\n"
              << (errorCount == 0 ? "[OK] Execucao headless concluida com sucesso.\n"
                                  : "[FALHA] Execucao headless com erros.\n");
    return errorCount == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    int headlessFrames = 0;
    bool forceNdsDemo  = false;
    std::string initialRomArg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless-frames" && i + 1 < argc) {
            headlessFrames = std::stoi(argv[++i]);
        } else if (arg == "--nds-demo") {
            forceNdsDemo = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "RyūDS / RyūGBA (Dragon Edition) — Nintendo DS & GBA Emulator\n"
                      << "Usage: " << argv[0] << " [rom_path.nds|rom_path.gba] [--nds-demo] [--headless-frames N]\n";
            return 0;
        } else {
            initialRomArg = arg;
        }
    }

    gba::core::HybridCore core;

    if (headlessFrames > 0) {
        return runHeadlessDiagnostic(core, headlessFrames, initialRomArg, forceNdsDemo);
    }

    if (forceNdsDemo) {
        core.loadBuiltInNdsDemo();
    }

    ryugba::frontend::EmulatorApp app(core);
    if (!app.init(initialRomArg.empty() ? fs::path{} : fs::path(initialRomArg))) {
        return 1;
    }
    return app.run();
}
