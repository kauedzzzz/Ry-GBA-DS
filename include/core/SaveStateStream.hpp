#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace gba::core {

class SaveStateWriter {
public:
    void reserve(std::size_t bytes) { m_buf.reserve(bytes); }

    void writeBytes(const void* data, std::size_t size) {
        if (size == 0) return;
        const auto* ptr = static_cast<const uint8_t*>(data);
        m_buf.insert(m_buf.end(), ptr, ptr + size);
    }

    void writeBytes(std::span<const uint8_t> bytes) {
        writeBytes(bytes.data(), bytes.size());
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void writePod(const T& value) {
        writeBytes(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
        requires std::is_trivially_copyable_v<T>
    void writeArray(const std::array<T, N>& arr) {
        writeBytes(arr.data(), N * sizeof(T));
    }

    [[nodiscard]] const std::vector<uint8_t>& data() const noexcept { return m_buf; }
    [[nodiscard]] std::vector<uint8_t> takeData() noexcept { return std::move(m_buf); }

private:
    std::vector<uint8_t> m_buf;
};

class SaveStateReader {
public:
    explicit SaveStateReader(std::span<const uint8_t> data) noexcept
        : m_data(data) {}

    bool readBytes(void* dst, std::size_t size) noexcept {
        if (m_offset + size > m_data.size()) {
            m_ok = false;
            return false;
        }
        if (size > 0) {
            std::memcpy(dst, m_data.data() + m_offset, size);
            m_offset += size;
        }
        return true;
    }

    bool readBytes(std::span<uint8_t> dst) noexcept {
        return readBytes(dst.data(), dst.size());
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool readPod(T& outValue) noexcept {
        return readBytes(&outValue, sizeof(T));
    }

    template <typename T, std::size_t N>
        requires std::is_trivially_copyable_v<T>
    bool readArray(std::array<T, N>& outArr) noexcept {
        return readBytes(outArr.data(), N * sizeof(T));
    }

    [[nodiscard]] bool good() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return m_offset <= m_data.size() ? (m_data.size() - m_offset) : 0;
    }

private:
    std::span<const uint8_t> m_data;
    std::size_t              m_offset{0};
    bool                     m_ok{true};
};

} // namespace gba::core
