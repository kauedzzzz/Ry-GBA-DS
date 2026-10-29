#include "core/HybridCore.hpp"
#include "core/nds/NdsCartridge.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace gba::core {

HybridCore::HybridCore() = default;

bool HybridCore::hasNdsExtension(const std::filesystem::path& p) noexcept {
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".nds" || ext == ".dsi" || ext == ".srl";
}

void HybridCore::configureNdsBootFiles(bool directBootHle,
                                       const std::filesystem::path& bios9Path,
                                       const std::filesystem::path& bios7Path,
                                       const std::filesystem::path& firmwarePath) {
    m_nds.configureBootFiles(directBootHle, bios9Path, bios7Path, firmwarePath);
}

bool HybridCore::loadBios(std::span<const uint8_t> biosData) {
    if (biosData.size() == 4096u) {
        return m_nds.loadBios(biosData);
    }
    // 16 KB pode ser BIOS GBA ou BIOS7 NDS; carrega em ambos se aplicável
    bool ok = m_gba.loadBios(biosData);
    if (biosData.size() == 16384u) {
        ok = m_nds.loadBios(biosData) || ok;
    }
    return ok;
}

bool HybridCore::loadRom(std::span<const uint8_t> romData, SaveType saveType) {
    if (nds::NdsCartridge::looksLikeNdsRom(romData)) {
        if (m_nds.loadRom(romData, saveType)) {
            m_activeType = ConsoleType::NDS;
            return true;
        }
    }
    if (m_gba.loadRom(romData, saveType)) {
        m_activeType = ConsoleType::GBA;
        return true;
    }
    return false;
}

bool HybridCore::loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType) {
    if (hasNdsExtension(romPath)) {
        if (m_nds.loadRomFromFile(romPath, saveType)) {
            m_activeType = ConsoleType::NDS;
            return true;
        }
    }

    // Caso a extensão seja genérica (.bin), inspeciona o cabeçalho binário
    if (!hasNdsExtension(romPath)) {
        std::ifstream f(romPath, std::ios::binary | std::ios::ate);
        if (f) {
            const auto sz = f.tellg();
            if (sz >= 0x200) {
                std::vector<uint8_t> probe(static_cast<std::size_t>(std::min<std::streamoff>(sz, 0x4000)));
                f.seekg(0, std::ios::beg);
                f.read(reinterpret_cast<char*>(probe.data()), static_cast<std::streamsize>(probe.size()));
                std::string ext = romPath.extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".gba" && ext != ".agb" && nds::NdsCartridge::looksLikeNdsRom(probe)) {
                    if (m_nds.loadRomFromFile(romPath, saveType)) {
                        m_activeType = ConsoleType::NDS;
                        return true;
                    }
                }
            }
        }
    }

    if (m_gba.loadRomFromFile(romPath, saveType)) {
        m_activeType = ConsoleType::GBA;
        return true;
    }
    return false;
}

void HybridCore::loadBuiltInGbaDemo() {
    m_gba.loadBuiltInDemoRom();
    m_activeType = ConsoleType::GBA;
}

void HybridCore::loadBuiltInNdsDemo() {
    m_nds.loadBuiltInDemoRom();
    m_activeType = ConsoleType::NDS;
}

void HybridCore::reset() {
    activeCore().reset();
}

void HybridCore::runFrame() {
    activeCore().runFrame();
}

void HybridCore::setVolume(float volume) noexcept {
    m_gba.setVolume(volume);
    m_nds.setVolume(volume);
}

void HybridCore::setMuted(bool state) noexcept {
    m_gba.setMuted(state);
    m_nds.setMuted(state);
}

bool HybridCore::toggleMute() noexcept {
    const bool newState = !activeCore().isMuted();
    setMuted(newState);
    return newState;
}

void HybridCore::setAudioSampleRate(uint32_t sampleRateHz) noexcept {
    m_gba.setAudioSampleRate(sampleRateHz);
    m_nds.setAudioSampleRate(sampleRateHz);
}

} // namespace gba::core

