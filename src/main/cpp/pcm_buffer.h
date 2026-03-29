#ifndef PCM_BUFFER_H
#define PCM_BUFFER_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <atomic>

class NativePcmBuffer {
public:
    NativePcmBuffer(size_t capacity)
        : capacity(capacity), buffer(new uint8_t[capacity]),
          readPos(0), writePos(0), available(0),
          endSignaled(false), flushed(false) {}

    ~NativePcmBuffer() { delete[] buffer; }

    // Blocks until all data is written or flushed. Returns true on success, false on flush.
    bool write(const uint8_t* data, int offset, int length) {
        int written = 0;
        while (written < length) {
            std::unique_lock<std::mutex> lock(mtx);
            writerCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return available < capacity || flushed.load(std::memory_order_relaxed);
            });
            if (flushed.load(std::memory_order_relaxed)) {
                flushed.store(false, std::memory_order_relaxed);
                return false;
            }
            size_t space = capacity - available;
            size_t toWrite = std::min((size_t)(length - written), space);
            if (toWrite == 0) continue;

            // Two-part memcpy for ring wrap
            size_t firstPart = std::min(toWrite, capacity - writePos);
            memcpy(buffer + writePos, data + offset + written, firstPart);
            if (toWrite > firstPart) {
                memcpy(buffer, data + offset + written + firstPart, toWrite - firstPart);
            }
            writePos = (writePos + toWrite) % capacity;
            available += toWrite;
            written += toWrite;

            readerCv.notify_one();
        }
        return true;
    }

    // Blocks until data available. Returns bytes read, -1 for end-of-stream, -2 for flush.
    int read(uint8_t* dest, int offset, int maxLength) {
        std::unique_lock<std::mutex> lock(mtx);
        readerCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return available > 0
                || endSignaled.load(std::memory_order_relaxed)
                || flushed.load(std::memory_order_relaxed);
        });
        if (flushed.load(std::memory_order_relaxed)) {
            flushed.store(false, std::memory_order_relaxed);
            return -2;
        }
        if (available == 0 && endSignaled.load(std::memory_order_relaxed)) {
            return -1;
        }
        if (available == 0) {
            return 0; // spurious wakeup, no data yet
        }
        size_t toRead = std::min((size_t)maxLength, available);

        // Two-part memcpy for ring wrap
        size_t firstPart = std::min(toRead, capacity - readPos);
        memcpy(dest + offset, buffer + readPos, firstPart);
        if (toRead > firstPart) {
            memcpy(dest + offset + firstPart, buffer, toRead - firstPart);
        }
        readPos = (readPos + toRead) % capacity;
        available -= toRead;

        writerCv.notify_one();
        return (int)toRead;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mtx);
        readPos = 0;
        writePos = 0;
        available = 0;
        endSignaled.store(false, std::memory_order_relaxed);
        flushed.store(true, std::memory_order_relaxed);
        readerCv.notify_all();
        writerCv.notify_all();
    }

    void signalEnd() {
        std::lock_guard<std::mutex> lock(mtx);
        endSignaled.store(true, std::memory_order_relaxed);
        readerCv.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        readPos = 0;
        writePos = 0;
        available = 0;
        endSignaled.store(false, std::memory_order_relaxed);
        flushed.store(false, std::memory_order_relaxed);
        readerCv.notify_all();
        writerCv.notify_all();
    }

private:
    const size_t capacity;
    uint8_t* const buffer;
    size_t readPos;
    size_t writePos;
    size_t available;
    std::atomic<bool> endSignaled;
    std::atomic<bool> flushed;
    std::mutex mtx;
    std::condition_variable readerCv;
    std::condition_variable writerCv;
};

#endif // PCM_BUFFER_H
