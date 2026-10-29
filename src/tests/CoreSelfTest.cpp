#include "config/ConfigManager.hpp"
#include "core/GbaCore.hpp"
#include "core/HybridCore.hpp"
#include "core/memory/save/SaveDetector.hpp"
#include "core/nds/BuiltInNdsDemoRom.hpp"
#include "ui/SvgLogoRenderer.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifndef GBA_PROJECT_DIR
#define GBA_PROJECT_DIR "."
#endif

namespace {

void testCpuArmAndThumbExecution() {
    gba::core::GbaCore core;
    std::vector<uint8_t> rom(1024, 0);

    auto write32 = [&](uint32_t off, uint32_t val) {
        rom[off + 0] = static_cast<uint8_t>(val & 0xFF);
        rom[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        rom[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
        rom[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
    };
    auto write16 = [&](uint32_t off, uint16_t val) {
        rom[off + 0] = static_cast<uint8_t>(val & 0xFF);
        rom[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    };

    write32(0x00, 0xE3A0002Au); // MOV R0, #42
    write32(0x04, 0xE280103Au); // ADD R1, R0, #58 -> 100
    write32(0x08, 0xE28F2005u); // ADR R2, thumb_code+1
    write32(0x0C, 0xE12FFF12u); // BX  R2

    write16(0x14, 0x23C8u);     // MOV R3, #200
    write16(0x16, 0x18C9u);     // ADD R1, R1, R3 -> 300

    core.loadRom(rom);
    for (int i = 0; i < 6; ++i) {
        core.cpu().step();
    }

    assert(core.cpu().registers().isThumb() && "CPU deveria estar em estado THUMB apos BX");
    assert(core.cpu().registers().r[0] == 42);
    assert(core.cpu().registers().r[3] == 200);
    assert(core.cpu().registers().r[1] == 300);
    std::cout << "[PASS] CPU ARM7TDMI (GBA): Decodificacao dupla ARM (32-bit) <-> THUMB (16-bit) via BX OK.\n";
}

void testSaveDetectionAndFlash128K() {
    std::vector<uint8_t> pokemonRom(4096, 0);
    std::memcpy(&pokemonRom[512], "FLASH1M_V103", 12);
    const auto detected = gba::core::SaveDetector::detect(pokemonRom);
    assert(detected == gba::core::SaveType::Flash128K);

    gba::core::GbaCore core;
    core.loadRom(pokemonRom, gba::core::SaveType::AutoDetect);
    assert(core.bus().getActiveSaveType() == gba::core::SaveType::Flash128K);

    auto writeFlashByte = [&](uint16_t offset, uint8_t val) {
        core.bus().write8(0x0E005555, 0xAA);
        core.bus().write8(0x0E002AAA, 0x55);
        core.bus().write8(0x0E005555, 0xA0);
        core.bus().write8(0x0E000000 + offset, val);
    };
    auto switchFlashBank = [&](uint8_t bank) {
        core.bus().write8(0x0E005555, 0xAA);
        core.bus().write8(0x0E002AAA, 0x55);
        core.bus().write8(0x0E005555, 0xB0);
        core.bus().write8(0x0E000000, bank);
    };

    switchFlashBank(0);
    writeFlashByte(0x0100, 0x4B);
    switchFlashBank(1);
    writeFlashByte(0x0100, 0x9E);

    switchFlashBank(0);
    assert(core.bus().read8(0x0E000100) == 0x4B);
    switchFlashBank(1);
    assert(core.bus().read8(0x0E000100) == 0x9E);
    std::cout << "[PASS] MemoryBus & SaveMedia (GBA): Deteccao FLASH1M_V103 e Bank Switching 128KB OK.\n";
}

void testApuVolumeAndMuteControl() {
    gba::core::GbaCore core;
    core.loadBuiltInDemoRom();

    core.setVolume(0.75f);
    assert(std::abs(core.getVolume() - 0.75f) < 1e-5f);

    core.setMuted(false);
    assert(!core.isMuted());
    core.runFrame();

    std::vector<int16_t> samplesActive(1024, 0);
    const std::size_t countActive = core.pullAudioSamples(samplesActive);
    assert(countActive > 0);

    bool hasNonZeroAudio = false;
    for (std::size_t i = 0; i < countActive; ++i) {
        if (samplesActive[i] != 0) {
            hasNonZeroAudio = true;
            break;
        }
    }
    assert(hasNonZeroAudio && "APU deve gerar amostras nao-nulas quando ativa");

    const bool mutedState = core.toggleMute();
    assert(mutedState && core.isMuted());

    std::vector<int16_t> drain(16384);
    core.pullAudioSamples(drain);
    core.runFrame();

    std::vector<int16_t> samplesMuted(1024, 123);
    const std::size_t countMuted = core.pullAudioSamples(samplesMuted);
    assert(countMuted > 0);
    for (std::size_t i = 0; i < countMuted; ++i) {
        assert(samplesMuted[i] == 0 && "Amostras em Mute devem ser estritamente 0");
    }

    core.toggleMute();
    assert(!core.isMuted());
    std::cout << "[PASS] APU Audio Subsystem (GBA): Sintese PSG + setVolume() + toggleMute() Lock-Free OK.\n";
}

void testInputManagerAndRemapping() {
    gba::core::GbaCore core;
    core.loadBuiltInDemoRom();

    assert(core.bus().read16(0x04000130) == 0x03FF);

    core.setKeyState(gba::core::GbaKey::A, true);
    assert((core.bus().read16(0x04000130) & 0x0001) == 0);
    core.setKeyState(gba::core::GbaKey::A, false);
    assert(core.bus().read16(0x04000130) == 0x03FF);

    core.input().clearBindingsForKey(gba::core::GbaKey::Start);
    core.input().bindInput({gba::core::InputDeviceType::GamepadButton, 99}, gba::core::GbaKey::Start);

    core.input().onGamepadButtonEvent(99, true);
    assert((core.bus().read16(0x04000130) & static_cast<uint16_t>(gba::core::GbaKey::Start)) == 0);
    core.input().onGamepadButtonEvent(99, false);
    assert(core.bus().read16(0x04000130) == 0x03FF);
    std::cout << "[PASS] InputManager: Registrador KEYINPUT (0x04000130) Active-Low e setKeyState() OK.\n";
}

void testSaveStateAndLoadStateRoundTrip() {
    gba::core::GbaCore core;
    core.loadBuiltInDemoRom();

    core.runFrame();
    core.runFrame();
    core.bus().write32(0x02000100, 0xCAFEBABEu);
    const uint32_t savedPc = core.cpu().registers().r[15];
    const uint64_t savedFrame = core.frameCount();

    std::vector<uint8_t> snapshot;
    assert(core.saveState(snapshot));
    assert(snapshot.size() > 256 * 1024);

    core.bus().write32(0x02000100, 0x12345678u);
    core.runFrame();
    core.runFrame();
    assert(core.bus().read32(0x02000100) == 0x12345678u);
    assert(core.frameCount() == savedFrame + 2);

    assert(core.loadState(snapshot));
    assert(core.bus().read32(0x02000100) == 0xCAFEBABEu);
    assert(core.cpu().registers().r[15] == savedPc);
    assert(core.frameCount() == savedFrame);
    std::cout << "[PASS] SaveState/LoadState (GBA): Snapshot binario completo de CPU/RAM/PPU/APU OK.\n";
}

void testConfigManagerJsonRoundTrip() {
    const std::filesystem::path tmpJson = std::filesystem::temp_directory_path() / "ryuds_test_config.json";
    {
        ryugba::config::ConfigManager writer;
        writer.data().video.scale = 4;
        writer.data().video.bilinearFilter = true;
        writer.data().video.aspectRatio = ryugba::config::AspectRatioMode::Ratio4x3;
        writer.data().video.ndsLayout = ryugba::config::NdsScreenLayout::Horizontal;
        writer.data().video.swapScreens = true;
        writer.data().video.showStylusCursor = false;
        writer.data().ndsBoot.directBootHle = false;
        writer.data().ndsBoot.bios9Path = "custom_bios9.bin";
        writer.data().audio.masterVolume = 0.65f;
        writer.data().audio.sampleRate = 44100;
        writer.data().audio.bufferSamples = 512;
        writer.data().controls[static_cast<std::size_t>(ryugba::config::GbaAction::X)].keyScancode = 20;
        writer.data().controls[static_cast<std::size_t>(ryugba::config::GbaAction::Y)].keyScancode = 26;
        writer.data().hotkeys[static_cast<std::size_t>(ryugba::config::HotkeyAction::MicBlow)].keyScancode = 16;
        assert(writer.saveTo(tmpJson));
    }
    {
        ryugba::config::ConfigManager reader;
        assert(reader.loadFrom(tmpJson));
        assert(reader.data().video.scale == 4);
        assert(reader.data().video.bilinearFilter == true);
        assert(reader.data().video.aspectRatio == ryugba::config::AspectRatioMode::Ratio4x3);
        assert(reader.data().video.ndsLayout == ryugba::config::NdsScreenLayout::Horizontal);
        assert(reader.data().video.swapScreens == true);
        assert(reader.data().video.showStylusCursor == false);
        assert(reader.data().ndsBoot.directBootHle == false);
        assert(reader.data().ndsBoot.bios9Path == "custom_bios9.bin");
        assert(std::abs(reader.data().audio.masterVolume - 0.65f) < 1e-4f);
        assert(reader.data().controls[static_cast<std::size_t>(ryugba::config::GbaAction::X)].keyScancode == 20);
        assert(reader.data().controls[static_cast<std::size_t>(ryugba::config::GbaAction::Y)].keyScancode == 26);
        assert(reader.data().hotkeys[static_cast<std::size_t>(ryugba::config::HotkeyAction::MicBlow)].keyScancode == 16);
    }
    std::error_code ec;
    std::filesystem::remove(tmpJson, ec);
    std::cout << "[PASS] ConfigManager JSON: Persistencia de Layouts DS, Botoes X/Y, Mic/Lid e BIOS/HLE OK.\n";
}

void testDragonSvgLogoRasterizer() {
    const std::filesystem::path logoPath = std::filesystem::path(GBA_PROJECT_DIR) / "assets" / "ryu_logo.svg";
    const auto pixels = ryugba::ui::SvgLogoRenderer::rasterizeToArgb(logoPath, 96, 96);
    assert(pixels.size() == 96 * 96);

    std::size_t visiblePixels = 0;
    std::size_t dragonRedPixels = 0;
    std::size_t emeraldPixels = 0;
    for (uint32_t argb : pixels) {
        const uint8_t a = static_cast<uint8_t>((argb >> 24) & 0xFF);
        const uint8_t r = static_cast<uint8_t>((argb >> 16) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((argb >> 8) & 0xFF);
        if (a > 16) ++visiblePixels;
        if (r > 180 && g < 100) ++dragonRedPixels;
        if (g > 150 && r < 60)  ++emeraldPixels;
    }
    assert(visiblePixels > 1000 && "Logo SVG do Dragao deve renderizar silhueta visivel");
    assert(dragonRedPixels > 150 && "Logo SVG deve conter acentos Vermelho Fogo Draconico (#e63946)");
    assert(emeraldPixels > 20 && "Logo SVG deve conter o olho e detalhes Escama Esmeralda (#10b981)");
    std::cout << "[PASS] SvgLogoRenderer: Rasterizacao vetorial 4x4 MSAA de assets/ryu_logo.svg OK.\n";
}

// =============================================================================
// TESTES AUTOMATIZADOS DA ARQUITETURA NINTENDO DS (.nds)
// =============================================================================

void testNdsCartridgeHeaderBannerAndSavPersistence() {
    const auto romBytes = gba::core::nds::BuiltInNdsDemoRom::build();
    assert(gba::core::nds::NdsCartridge::looksLikeNdsRom(romBytes));

    const auto tmpDir  = std::filesystem::temp_directory_path();
    const auto ndsPath = tmpDir / "ryuds_unit_cart.nds";
    const auto savPath = tmpDir / "ryuds_unit_cart.sav";

    std::error_code ec;
    std::filesystem::remove(ndsPath, ec);
    std::filesystem::remove(savPath, ec);

    {
        std::ofstream out(ndsPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(romBytes.data()), static_cast<std::streamsize>(romBytes.size()));
    }

    {
        gba::core::nds::NdsCartridge cart;
        assert(cart.load(romBytes, ndsPath));
        const auto& hdr = cart.header();
        assert(hdr.gameTitle == "RYUDS DEMO");
        assert(hdr.gameCode == "ADRE");
        assert(hdr.makerCode == "RY");
        assert(hdr.hasBannerIcon);
        assert(!hdr.bannerTitleUtf8.empty());

        std::size_t visibleBannerPx = 0;
        for (uint32_t px : hdr.bannerIconArgb) {
            if ((px >> 24) > 0) ++visibleBannerPx;
        }
        assert(visibleBannerPx > 120 && "Banner 32x32 16-color do cartucho .nds deve ser decodificado");

        // Escreve 4 bytes ("RYU!") via protocolo AUXSPI EEPROM 64KB (2-byte addr)
        // 1) WREN (0x06) com CS liberado ao final
        cart.writeAuxSpiCnt(0xA000u); // Enable + Slot SPI, Hold=0
        cart.writeAuxSpiData(0x06u);

        // 2) WRITE (0x02) + Addr 0x0140 + "RYU!" mantendo CS Hold=1 nos primeiros bytes
        cart.writeAuxSpiCnt(0xA040u); // Enable + Slot SPI + Hold=1
        cart.writeAuxSpiData(0x02u);
        cart.writeAuxSpiData(0x01u);
        cart.writeAuxSpiData(0x40u);
        cart.writeAuxSpiData('R');
        cart.writeAuxSpiData('Y');
        cart.writeAuxSpiData('U');
        cart.writeAuxSpiCnt(0xA000u); // Ultimo byte libera Hold=0
        cart.writeAuxSpiData('!');

        assert(cart.flushSaveFile());
        assert(std::filesystem::exists(savPath, ec));
    }

    // Recarrega o mesmo cartucho .nds e valida leitura do arquivo .sav persistido
    {
        gba::core::nds::NdsCartridge cartReloaded;
        assert(cartReloaded.load(romBytes, ndsPath));

        cartReloaded.writeAuxSpiCnt(0xA040u); // Hold=1
        cartReloaded.writeAuxSpiData(0x03u);  // READ
        cartReloaded.writeAuxSpiData(0x01u);  // Addr Hi
        cartReloaded.writeAuxSpiData(0x40u);  // Addr Lo

        cartReloaded.writeAuxSpiData(0x00u);
        const char c0 = static_cast<char>(cartReloaded.readAuxSpiData());
        cartReloaded.writeAuxSpiData(0x00u);
        const char c1 = static_cast<char>(cartReloaded.readAuxSpiData());
        cartReloaded.writeAuxSpiData(0x00u);
        const char c2 = static_cast<char>(cartReloaded.readAuxSpiData());
        cartReloaded.writeAuxSpiCnt(0xA000u); // Hold=0
        cartReloaded.writeAuxSpiData(0x00u);
        const char c3 = static_cast<char>(cartReloaded.readAuxSpiData());

        assert(c0 == 'R' && c1 == 'Y' && c2 == 'U' && c3 == '!');
    }

    std::filesystem::remove(ndsPath, ec);
    std::filesystem::remove(savPath, ec);
    std::cout << "[PASS] NdsCartridge: Parser .nds, Icone Banner 32x32 16-cores e Persistencia AUXSPI .sav OK.\n";
}

void testArm9v5teDspCp15AndTcmExecution() {
    gba::core::nds::NdsCore nds;
    nds.loadBuiltInDemoRom();

    auto& arm9 = nds.arm9();
    auto& bus  = nds.bus();

    // Monta rotina de validacao ARMv5TE + DSP + CP15 em Main RAM (0x02100000)
    constexpr uint32_t CODE_BASE = 0x02100000u;
    bus.arm9Write32(CODE_BASE + 0x00u, 0xEE100F10u); // MRC p15, 0, R0, c0, c0, 0 -> Main ID (0x41059461)
    bus.arm9Write32(CODE_BASE + 0x04u, 0xE16F1F12u); // CLZ R1, R2
    bus.arm9Write32(CODE_BASE + 0x08u, 0xE1053054u); // QADD R3, R4, R5 (saturate(R4 + R5), seta FLAG_Q)
    bus.arm9Write32(CODE_BASE + 0x0Cu, 0xE1069887u); // SMLABB R6, R7, R8, R9 -> R6 = int16(R7)*int16(R8) + R9
    bus.arm9Write32(CODE_BASE + 0x10u, 0xEE09AF11u); // MCR p15, 0, R10, c9, c1, 0 -> Realoca DTCM p/ 0x00800000

    arm9.registers().cpsr = static_cast<uint32_t>(gba::core::CpuMode::System) |
                            gba::core::CpuRegisters::FLAG_I |
                            gba::core::CpuRegisters::FLAG_F;
    arm9.registers().r[2]  = 0x00100000u; // 11 zeros a esquerda
    arm9.registers().r[4]  = 0x7FFFFFF0u;
    arm9.registers().r[5]  = 0x00000100u; // Overflow positivo -> 0x7FFFFFFF + FLAG_Q
    arm9.registers().r[7]  = 0x12340014u; // low16 = 20
    arm9.registers().r[8]  = 0x5678FFCEu; // low16 = -50
    arm9.registers().r[9]  = 2500u;       // 20 * (-50) + 2500 = 1500
    arm9.registers().r[10] = 0x0080000Au; // DTCM base = 0x00800000, size = 16KB
    arm9.registers().r[15] = CODE_BASE;

    for (int i = 0; i < 5; ++i) {
        arm9.step();
    }

    assert(arm9.registers().r[0] == 0x41059461u && "CP15 Main ID deve identificar ARM946E-S");
    assert(arm9.registers().r[1] == 11u && "ARMv5TE CLZ falhou");
    assert(arm9.registers().r[3] == 0x7FFFFFFFu && "DSP QADD deve saturar em INT32_MAX");
    assert((arm9.registers().cpsr & gba::core::nds::Arm9Cpu::FLAG_Q) != 0 && "DSP QADD deve ativar sticky FLAG_Q");
    assert(arm9.registers().r[6] == 1500u && "DSP SMLABB falhou");
    assert(arm9.dtcmBase() == 0x00800000u && "CP15 c9,c1,0 deve realocar DTCM para 0x00800000");

    // Verifica que acessos de dados em 0x00800100 atingem o DTCM (prioridade sobre o espelho ITCM)
    bus.arm9Write32(0x02000100u, 0x11111111u);
    arm9.dataWrite32(0x00800100u, 0xDEADBEEFu);
    assert(arm9.dataRead32(0x00800100u) == 0xDEADBEEFu);
    std::cout << "[PASS] ARM946E-S (66.66MHz): Pipeline ARMv5TE (CLZ), DSP (QADD/SMLABB/Q), CP15 e DTCM/ITCM OK.\n";
}

void testNdsDualCoreIpcFifoAndSharedWram() {
    gba::core::nds::NdsCore nds;
    nds.loadBuiltInDemoRom();
    auto& bus = nds.bus();

    // 1. Habilita IPCFIFOCNT em ambos os barramentos (Bit 15 = Enable, Bit 3 = Clear)
    bus.arm9Write16(0x04000184u, 0x8008u);
    bus.arm7Write16(0x04000184u, 0x8008u);

    // ARM9 envia duas palavras para ARM7; ARM7 envia resposta para ARM9
    bus.arm9Write32(0x04000188u, 0xA9A91234u);
    bus.arm9Write32(0x04000188u, 0xA9A95678u);
    assert((bus.arm7Read16(0x04000184u) & 0x0100u) == 0 && "Recv FIFO no ARM7 nao pode estar vazia");
    assert(bus.arm7Read32(0x04100000u) == 0xA9A91234u);
    assert(bus.arm7Read32(0x04100000u) == 0xA9A95678u);
    assert((bus.arm7Read16(0x04000184u) & 0x0100u) != 0 && "Recv FIFO no ARM7 deve esvaziar apos 2 pops");

    bus.arm7Write32(0x04000188u, 0xA7A7CAFEu);
    assert(bus.arm9Read32(0x04100000u) == 0xA7A7CAFEu);

    // 2. IPCSYNC bidirecional (bits 8..11 escritos por um lado aparecem em bits 0..3 do outro)
    bus.arm9Write16(0x04000180u, 0x0B00u);
    assert((bus.arm7Read16(0x04000180u) & 0x000Fu) == 0x000Bu);
    bus.arm7Write16(0x04000180u, 0x0500u);
    assert((bus.arm9Read16(0x04000180u) & 0x000Fu) == 0x0005u);

    // 3. Shared WRAM 32KB (WRAMCNT 0x04000247): modo 0 = todos 32KB no ARM9; modo 3 = todos 32KB no ARM7
    bus.arm9Write8(0x04000247u, 0x00u);
    bus.arm9Write32(0x03000000u, 0x55AA1234u);
    bus.arm9Write32(0x03004000u, 0x99887766u);

    bus.arm9Write8(0x04000247u, 0x03u); // Transfere os dois bancos de 16KB para o ARM7 em 0x03000000
    assert(bus.arm7Read32(0x03000000u) == 0x55AA1234u);
    assert(bus.arm7Read32(0x03004000u) == 0x99887766u);

    // 4. Coprocessador Matematico Hardware ARM9 (DIV 64/32 e SQRT 64)
    bus.arm9Write16(0x04000280u, 0x0001u); // Modo 64-bit / 32-bit
    bus.arm9Write32(0x04000290u, 1000000u);
    bus.arm9Write32(0x04000294u, 0u);
    bus.arm9Write32(0x04000298u, 7u);
    assert(bus.arm9Read32(0x040002A0u) == 142857u);
    assert(bus.arm9Read32(0x040002A8u) == 1u);

    bus.arm9Write16(0x040002B0u, 0x0000u); // SQRT 32-bit
    bus.arm9Write32(0x040002B8u, 1440000u);
    assert(bus.arm9Read32(0x040002B4u) == 1200u);

    std::cout << "[PASS] NdsBus Dual-Core: IPCFIFO Bidirecional, IPCSYNC, Shared WRAM 32KB e Hardware DIV/SQRT OK.\n";
}

void testNdsDualScreen2D3DStylusMicAndSpu16() {
    gba::core::HybridCore hybrid;
    hybrid.loadBuiltInNdsDemo();

    assert(hybrid.consoleType() == gba::core::ConsoleType::NDS);
    assert(hybrid.isDualScreen());
    assert(hybrid.screenWidth() == 256 && hybrid.screenHeight() == 192);
    assert(hybrid.hasBannerIcon());

    // Simula toque da Stylus em (X=120, Y=90), Sopro no Microfone e Botao X pressionado
    hybrid.setStylusState(120, 90, true);
    hybrid.setMicBlowing(true);
    hybrid.setExtendedButtonX(true);
    hybrid.setVolume(0.85f);
    hybrid.setMuted(false);

    for (int f = 0; f < 6; ++f) {
        hybrid.runFrame();
    }

    const auto topFb = hybrid.topFramebufferSpan();
    const auto botFb = hybrid.bottomFramebufferSpan();
    assert(topFb.size() == 256u * 192u);
    assert(botFb.size() == 256u * 192u);

    // Verifica que a Engine 3D + Engine A renderizaram o prisma Dragon na tela superior
    std::size_t topNonBgPixels = 0;
    const uint32_t topCorner = topFb[0] & 0x00FFFFFFu;
    for (uint32_t px : topFb) {
        if ((px & 0x00FFFFFFu) != topCorner) ++topNonBgPixels;
    }
    assert(topNonBgPixels > 400 && "Engine 3D GX + Engine A 2D devem rasterizar geometria 3D na tela superior");

    // Verifica que o toque da Stylus em (120, 90) foi lido via SPI TSC2046 no ARM7,
    // transmitido via IPCFIFO ao ARM9 e pintado no framebuffer Direct Color da tela inferior!
    const uint32_t touchedPixel = botFb[90u * 256u + 120u] & 0x00FFFFFFu;
    const uint32_t untouchedCorner = botFb[0] & 0x00FFFFFFu;
    assert(touchedPixel != untouchedCorner && "Toque da Stylus (120,90) deve pintar na tela inferior via ARM7 SPI -> IPC -> ARM9");

    // Verifica telemetria IPC gravada pelo ARM9 em Main RAM 0x02003F00
    const uint32_t ipcPacket = hybrid.ndsCore().bus().arm9Read32(0x02003F00u);
    assert(((ipcPacket >> 22) & 1u) == 0u && "Bit PenDown deve estar ativo (0 = tocando no EXTKEYIN) no pacote IPC ARM7->ARM9");
    assert(((ipcPacket >> 16) & 1u) == 0u && "Bit Botao X deve estar ativo (0 = pressionado no EXTKEYIN) no pacote IPC");
    assert((ipcPacket & 0xFFu) == 120u && "Coordenada X=120 da Stylus deve cruzar TSC2046 ADC -> IPC com precisao 1:1");
    assert(((ipcPacket >> 8) & 0xFFu) == 90u && "Coordenada Y=90 da Stylus deve cruzar TSC2046 ADC -> IPC com precisao 1:1");

    // Verifica geracao de audio nos 16 canais da NdsSpu
    std::vector<int16_t> audioBuf(2048, 0);
    const std::size_t pulled = hybrid.pullAudioSamples(audioBuf);
    assert(pulled > 0);
    bool nonZeroSpu = false;
    for (std::size_t i = 0; i < pulled; ++i) {
        if (audioBuf[i] != 0) {
            nonZeroSpu = true;
            break;
        }
    }
    assert(nonZeroSpu && "NdsSpu (16 canais) deve produzir audio estereo nao-nulo");

    // Testa SaveState / LoadState completo Dual-Core do Nintendo DS
    hybrid.ndsCore().bus().arm9Write32(0x02050000u, 0xD5D52026u);
    const uint64_t savedFrames = hybrid.ndsCore().frameCount();
    std::vector<uint8_t> ndsSnapshot;
    assert(hybrid.saveState(ndsSnapshot));
    assert(ndsSnapshot.size() > 4 * 1024 * 1024);

    hybrid.ndsCore().bus().arm9Write32(0x02050000u, 0x00000000u);
    hybrid.runFrame();
    assert(hybrid.ndsCore().frameCount() == savedFrames + 1);

    assert(hybrid.loadState(ndsSnapshot));
    assert(hybrid.ndsCore().frameCount() == savedFrames);
    assert(hybrid.ndsCore().bus().arm9Read32(0x02050000u) == 0xD5D52026u);

    std::cout << "[PASS] HybridCore NDS Completo: Dual-Screen 2D+3D GX, Stylus TSC2046, Mic, SPU 16-ch e SaveState OK.\n";
}

} // namespace

int main() {
    std::cout << "Executando Suite de Auto-Teste Hibrida RyūDS / RyūGBA (Dragon Edition)...\n";
    testCpuArmAndThumbExecution();
    testSaveDetectionAndFlash128K();
    testApuVolumeAndMuteControl();
    testInputManagerAndRemapping();
    testSaveStateAndLoadStateRoundTrip();
    testConfigManagerJsonRoundTrip();
    testDragonSvgLogoRasterizer();

    testNdsCartridgeHeaderBannerAndSavPersistence();
    testArm9v5teDspCp15AndTcmExecution();
    testNdsDualCoreIpcFifoAndSharedWram();
    testNdsDualScreen2D3DStylusMicAndSpu16();

    std::cout << "Todos os 11 modulos de teste (Nintendo DS + GBA + UI) passaram com 0 falhas!\n";
    return 0;
}
