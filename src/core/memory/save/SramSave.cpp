#include "core/memory/save/SramSave.hpp"
#include <fstream>

namespace gba::core {

SramSave::SramSave() {
    m_data.fill(0xFF);
}

uint8_t SramSave::readByte(uint32_t address) {
    return m_data[address & (SRAM_SIZE - 1)];
}

void SramSave::writeByte(uint32_t address, uint8_t value) {
    const std::size_t idx = address & (SRAM_SIZE - 1);
    if (m_data[idx] != value) {
        m_data[idx] = value;
        m_dirty = true;
    }
}

bool SramSave::loadFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    m_dirty = false;
    return true;
}

bool SramSave::saveToFile(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(m_data.data()), static_cast<std::streamsize>(m_data.size()));
    return out.good();
}

void SramSave::serializeState(SaveStateWriter& out) const {
    out.writeBytes(m_data.data(), m_data.size());
}

bool SramSave::deserializeState(SaveStateReader& in) {
    m_dirty = true;
    return in.readBytes(m_data.data(), m_data.size());
}

} // namespace gba::core

