#ifndef AE_CORE_RING_BUFFER_H
#define AE_CORE_RING_BUFFER_H

// Lock-free single-producer / single-consumer byte ring. Safe to touch from a
// real-time callback (no locks, no allocation, no syscalls) — which is why the
// USB isochronous transfer callbacks and the JACK process callback both use it
// instead of the blocking mutex+CV NativePcmBuffer.
//
// Extracted from the USB driver so every backend that needs an RT-safe ring
// (USB, JACK, ...) shares one implementation.

#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>

namespace ae {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity(capacity), buffer(new uint8_t[capacity]) {
        readPos.store(0);
        writePos.store(0);
    }
    ~RingBuffer() { delete[] buffer; }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    size_t write(const uint8_t* data, size_t len) {
        size_t r = readPos.load(std::memory_order_acquire);
        size_t w = writePos.load(std::memory_order_relaxed);
        size_t available = (r + capacity - w - 1) % capacity;
        size_t toWrite = std::min(len, available);

        size_t firstPart = std::min(toWrite, capacity - w);
        memcpy(buffer + w, data, firstPart);
        if (toWrite > firstPart) {
            memcpy(buffer, data + firstPart, toWrite - firstPart);
        }

        writePos.store((w + toWrite) % capacity, std::memory_order_release);
        return toWrite;
    }

    size_t read(uint8_t* data, size_t len) {
        size_t w = writePos.load(std::memory_order_acquire);
        size_t r = readPos.load(std::memory_order_relaxed);
        size_t available = (w + capacity - r) % capacity;
        size_t toRead = std::min(len, available);

        size_t firstPart = std::min(toRead, capacity - r);
        memcpy(data, buffer + r, firstPart);
        if (toRead > firstPart) {
            memcpy(data + firstPart, buffer, toRead - firstPart);
        }

        readPos.store((r + toRead) % capacity, std::memory_order_release);
        return toRead;
    }

    size_t getAvailable() const {
        size_t w = writePos.load(std::memory_order_acquire);
        size_t r = readPos.load(std::memory_order_acquire);
        return (w + capacity - r) % capacity;
    }

    // Returns a conservative lower bound on free space (reader may free more at any time).
    size_t getFreeSpace() const {
        size_t r = readPos.load(std::memory_order_acquire);
        size_t w = writePos.load(std::memory_order_relaxed);
        return (r + capacity - w - 1) % capacity;
    }

    void clear() {
        readPos.store(0);
        writePos.store(0);
    }

    uint8_t* getBuffer() const { return buffer; }
    size_t getCapacity() const { return capacity; }

private:
    const size_t capacity;
    uint8_t* const buffer;
    std::atomic<size_t> readPos;
    std::atomic<size_t> writePos;
};

} // namespace ae

#endif // AE_CORE_RING_BUFFER_H
