#pragma once
#include "core/ICoreEmulator.hpp"
#include "core/apu/Apu.hpp"
#include "core/cpu/Arm7Tdmi.hpp"
#include "core/input/InputManager.hpp"
#include "core/memory/MemoryBus.hpp"
#include "core/ppu/Ppu.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gba::core {

class GbaCore final : public ICoreEmulator {
public:
    static constexpr int32_t CYCLES_PER_FRAME = 280'896; // 228 scanlines * 1232 ciclos

    GbaCore();

    bool loadBios(std::span<const uint8_t> biosData) override;
    bool loadRom(std::span<const uint8_t> romData, SaveType saveType = SaveType::AutoDetect) override;
    bool loadRomFromFile(const std::filesystem::path& romPath, SaveType saveType = SaveType::AutoDetect) override;
    void loadBuiltInDemoRom();

    void reset() override;
    void runFrame() override;

    [[nodiscard]] bool hasRomLoaded() const noexcept override { return m_bus.hasRom(); }
    [[nodiscard]] std::string romTitle() const noexcept override { return m_bus.getRomTitle(); }
    [[nodiscard]] const std::filesystem::path& romPath() const noexcept override { return m_loadedRomPath; }

    [[nodiscard]] std::span<const uint32_t, 240 * 160> framebuffer() const noexcept override {
        return m_ppu.getFramebuffer();
    }

    void setTraceEnabled(bool enabled) noexcept {
        m_cpu.setTraceEnabled(enabled);
        m_ppu.setTraceEnabled(enabled);
        m_bus.setTraceEnabled(enabled);
    }

    // =========================================================================
    // CONTROLE EXTERNO OBRIGATÓRIO DE ÁUDIO (Thread-Safe / Lock-Free)
    // =========================================================================
    void setVolume(float volume) noexcept override { m_apu.setVolume(volume); }
    [[nodiscard]] float getVolume() const noexcept override { return m_apu.getVolume(); }

    void setMuted(bool state) noexcept override { m_apu.setMuted(state); }
    bool toggleMute() noexcept override { return m_apu.toggleMute(); }
    [[nodiscard]] bool isMuted() const noexcept override { return m_apu.isMuted(); }
    void setAudioSampleRate(uint32_t sampleRateHz) noexcept override { m_apu.setSampleRate(sampleRateHz); }

    std::size_t pullAudioSamples(std::span<int16_t> outBuffer) noexcept override {
        return m_apu.pullAudioSamples(outBuffer);
    }

    // =========================================================================
    // ENTRADA VIRTUAL DESACOPLADA (ICoreEmulator)
    // =========================================================================
    void setKeyState(GbaKey key, bool pressed) noexcept override {
        m_input.setVirtualKeyState(key, pressed);
    }

    // =========================================================================
    // SAVE STATES (Slots 1 a 9 — Estilo melonDS)
    // =========================================================================
    bool saveState(std::vector<uint8_t>& outData) const override;
    bool loadState(std::span<const uint8_t> data) override;
    bool saveStateSlot(int slot) const override;
    bool loadStateSlot(int slot) override;
    [[nodiscard]] bool hasStateSlot(int slot) const override;
    [[nodiscard]] std::filesystem::path stateSlotPath(int slot) const;

    // =========================================================================
    // SUBSISTEMAS E ACESSORES DIRETOS (Testes e Diagnóstico)
    // =========================================================================
    [[nodiscard]] InputManager& input() noexcept { return m_input; }
    [[nodiscard]] const InputManager& input() const noexcept { return m_input; }
    [[nodiscard]] Ppu& ppu() noexcept { return m_ppu; }
    [[nodiscard]] const Ppu& ppu() const noexcept { return m_ppu; }
    [[nodiscard]] Apu& apu() noexcept { return m_apu; }
    [[nodiscard]] MemoryBus& bus() noexcept { return m_bus; }
    [[nodiscard]] Arm7Tdmi& cpu() noexcept { return m_cpu; }

    [[nodiscard]] uint64_t frameCount() const noexcept { return m_frameCount; }

private:
    Ppu          m_ppu;
    Apu          m_apu;
    InputManager m_input;
    MemoryBus    m_bus;
    Arm7Tdmi     m_cpu;
    uint64_t     m_frameCount{0};
    std::filesystem::path m_loadedRomPath;
    std::filesystem::path m_saveFilePath;
};

} // namespace gba::core
