#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>

struct libusb_device_handle;
struct libusb_context;
struct libusb_transfer;

// USB Audio Class constants
#define UAC_VERSION_1           0x0100
#define UAC_VERSION_2           0x0200

// Descriptor types
#define USB_DT_CS_INTERFACE     0x24
#define USB_DT_CS_ENDPOINT      0x25

// Audio Class-Specific AC Interface Descriptor Subtypes
#define UAC_HEADER              0x01
#define UAC_INPUT_TERMINAL      0x02
#define UAC_OUTPUT_TERMINAL     0x03
#define UAC_MIXER_UNIT          0x04
#define UAC_SELECTOR_UNIT       0x05
#define UAC_FEATURE_UNIT        0x06
#define UAC_CLOCK_SOURCE        0x0a // UAC2

// Audio Class-Specific AS Interface Descriptor Subtypes
#define UAC_AS_GENERAL          0x01
#define UAC_FORMAT_TYPE         0x02

// Audio Class-Specific Endpoint Descriptor Subtypes
#define UAC_EP_GENERAL          0x01

// Audio Class-Specific Request Codes
#define UAC_SET_CUR             0x01
#define UAC_GET_CUR             0x81
#define UAC2_CS_CONTROL_SAM_FREQ 0x01
#define UAC2_CS_CONTROL_CLOCK_VALID 0x02

// Format type codes
#define UAC_FORMAT_TYPE_I       0x01

// Ring buffer for lock-free audio data passing
class RingBuffer {
public:
    RingBuffer(size_t capacity) : capacity(capacity), buffer(new uint8_t[capacity]) {
        readPos.store(0);
        writePos.store(0);
    }
    ~RingBuffer() { delete[] buffer; }

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

struct UsbAudioFormat {
    int interfaceNum;
    int altSetting;
    int endpointAddr;
    int maxPacketSize;
    int sampleRate;
    int channels;
    int bitDepth;
    int feedbackEpAddr = -1;
};

class UsbAudioDriver {
public:
    UsbAudioDriver();
    ~UsbAudioDriver();

    bool open(int fd);
    bool parseDescriptors();
    std::vector<int> getSupportedRates();
    bool configure(int sampleRate, int channels, int bitDepth);
    bool start();
    int write(const uint8_t* data, int length);
    int writeFloat32(const float* data, int numSamples);
    int writeInt16(const int16_t* data, int numSamples);
    void flush();
    void stop();
    void close();

    int getConfiguredRate() const { return configuredRate; }
    int getConfiguredChannels() const { return configuredChannels; }
    int getConfiguredBitDepth() const { return configuredBitDepth; }
    int getUacVersion() const { return uacVersion; }
    std::string getDeviceInfo() const;

private:
    bool setInterfaceAltSetting(int interface_num, int alt_setting);
    bool setSampleRate(int endpoint, int rate);
    bool setSampleRateUAC2(int clockId, int rate);
    void submitTransfer(int index);
    static void transferCallback(struct libusb_transfer* transfer);
    void handleTransferComplete(struct libusb_transfer* transfer);

    static void feedbackCallback(struct libusb_transfer* transfer);
    void handleFeedbackComplete(struct libusb_transfer* transfer);
    void submitFeedbackTransfer();

    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;

    std::vector<UsbAudioFormat> formats;
    UsbAudioFormat activeFormat{};

    int configuredRate = 0;
    int configuredChannels = 0;
    int configuredBitDepth = 0;
    int uacVersion = 1;
    int clockSourceId = -1;
    int acInterfaceNum = 0;
    int usbSpeed = 0;

    // Isochronous transfer ring
    static const int NUM_TRANSFERS = 8;
    static const int PACKETS_PER_TRANSFER = 8;
    struct libusb_transfer* transfers[NUM_TRANSFERS] = {};
    uint8_t* transferBuffers[NUM_TRANSFERS] = {};
    int transferBufSize = 0;
    std::atomic<int> activeTransfers{0};

    // Feedback transfer
    struct libusb_transfer* feedbackTransfer = nullptr;
    uint8_t feedbackBuffer[4] = {};
    std::atomic<uint32_t> currentFeedback{0};
    double feedbackAccumulator = 0.0;

    // Write buffer
    RingBuffer* ringBuffer = nullptr;

    std::atomic<bool> streaming{false};
    std::thread eventThread;
    bool opened = false;
    bool configured = false;
    bool interfaceClaimed = false;
    bool acInterfaceClaimed = false;
};

#endif // USB_AUDIO_H
