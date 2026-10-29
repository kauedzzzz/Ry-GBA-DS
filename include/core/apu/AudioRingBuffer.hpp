#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gba::core {

template <std::size_t CapacityPow2>
class AudioRingBuffer {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "CapacityPow2 deve ser potencia de 2.");

public:
    bool pushStereoSample(int16_t left, int16_t right) noexcept {
        const std::size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        const std::size_t nextWriteIdx = (writeIdx + 2) & MASK;

        if (nextWriteIdx == m_readIndex.load(std::memory_order_acquire)) {
            return false; // Buffer cheio
        }

        m_buffer[writeIdx]     = left;
        m_buffer[writeIdx + 1] = right;
        m_writeIndex.store(nextWriteIdx, std::memory_order_release);
        return true;
    }

    std::size_t popSamples(std::span<int16_t> outBuffer) noexcept {
        std::size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        const std::size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);

        std::size_t count = 0;
        while (count < outBuffer.size() && readIdx != writeIdx) {
            outBuffer[count++] = m_buffer[readIdx];
            readIdx = (readIdx + 1) & MASK;
        }

        m_readIndex.store(readIdx, std::memory_order_release);
        return count;
    }

    [[nodiscard]] std::size_t availableSamples() const noexcept {
        const std::size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        const std::size_t readIdx  = m_readIndex.load(std::memory_order_acquire);
        return (writeIdx - readIdx) & MASK;
    }

    void clear() noexcept {
        m_readIndex.store(0, std::memory_order_release);
        m_writeIndex.store(0, std::memory_order_release);
    }

private:
    static constexpr std::size_t MASK = CapacityPow2 - 1;
    alignas(64) std::atomic<std::size_t> m_writeIndex{0};
    alignas(64) std::atomic<std::size_t> m_readIndex{0};
    std::array<int16_t, CapacityPow2> m_buffer{};
};

} // namespace gba::core

