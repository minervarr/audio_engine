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
#define UAC2_RANGE              0x02
#define UAC2_CS_CONTROL_SAM_FREQ 0x01
#define UAC2_CS_CONTROL_CLOCK_VALID 0x02

// Feature Unit control selectors (used in wValue high byte for GET/SET CUR)
#define UAC_FU_MUTE_CONTROL     0x01
#define UAC_FU_VOLUME_CONTROL   0x02

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
    int endpointAddr;       // iso data EP (OUT when isCapture==false, IN when isCapture==true)
    int maxPacketSize;
    int sampleRate;
    int channels;
    int bitDepth;       // bBitResolution: number of significant bits per sample
    int subslotSize;    // bSubslotSize: bytes on the wire per sample (>= bitDepth/8)
    uint32_t bmFormats = 0;  // UAC2 AS_GENERAL bmFormats bitmap (bit 31 = RAW_DATA / DSD)
    bool isDsd = false;      // True when this alt-setting is the DSD variant
    bool isCapture = false;  // True when this alt-setting feeds data IN from the device (ADC/mic)
    int feedbackEpAddr = -1; // Iso IN feedback EP for playback alt-settings; -1 for capture
    int clockSourceId = -1;  // Resolved via AS_GENERAL.bTerminalLink -> Terminal.bCSourceID
};

class UsbAudioDriver {
public:
    UsbAudioDriver();
    ~UsbAudioDriver();

    bool open(int fd);
    bool parseDescriptors();
    std::vector<int> getSupportedRates();
    bool configure(int sampleRate, int channels, int bitDepth, bool preferDsd = false);
    bool start();
    int write(const uint8_t* data, int length);
    int writeFloat32(const float* data, int numSamples);
    int writeInt16(const int16_t* data, int numSamples);
    int writeInt24Packed(const uint8_t* data, int numBytes);
    int writeInt32(const int32_t* data, int numSamples);
    void flush();
    void stop();
    void close();

    int getConfiguredRate() const { return configuredRate; }
    int getConfiguredChannels() const { return configuredChannels; }
    int getConfiguredBitDepth() const { return configuredBitDepth; }
    int getConfiguredSubslotSize() const { return configuredSubslotSize; }
    int getUacVersion() const { return uacVersion; }
    std::string getDeviceInfo() const;

    // Volume control. Hardware path drives the UAC Feature Unit's volume/mute
    // controls; software path multiplies samples in writeFloat32/writeInt16.
    bool hasHardwareVolume() const { return volumeFuUnitId >= 0; }
    bool hasHardwareMute() const { return hasFuMute; }
    int getVolumeMinDbQ8() const { return volumeMinDbQ8; }
    int getVolumeMaxDbQ8() const { return volumeMaxDbQ8; }
    int getVolumeResDbQ8() const { return volumeResDbQ8; }
    bool setHwVolumeDbQ8(int valueDbQ8);
    bool setHwMute(bool muted);
    void setSoftwareGain(float gain) { softwareGain.store(gain, std::memory_order_relaxed); }

    // --- Capture (ADC -> host) ---
    bool configureCapture(int sampleRate, int channels, int bitDepth);
    bool startCapture();
    int  readCapture(uint8_t* out, int maxBytes);
    void stopCapture();
    int  getConfiguredCaptureRate() const        { return capRate; }
    int  getConfiguredCaptureChannels() const    { return capChannels; }
    int  getConfiguredCaptureBitDepth() const    { return capBitDepth; }
    int  getConfiguredCaptureSubslotSize() const { return capSubslotSize; }
    bool isCapturing() const                     { return capStreaming.load(); }
    bool hasCaptureFormats() const;

private:
    bool setInterfaceAltSetting(int interface_num, int alt_setting);
    bool setSampleRate(int endpoint, int rate);
    bool setSampleRateUAC2(int clockId, int rate);
    std::vector<int> queryUac2SampleRates(int clockId);
    bool queryHwVolumeRange();
    void submitTransfer(int index);
    static void transferCallback(struct libusb_transfer* transfer);
    void handleTransferComplete(struct libusb_transfer* transfer);

    static void feedbackCallback(struct libusb_transfer* transfer);
    void handleFeedbackComplete(struct libusb_transfer* transfer);
    void submitFeedbackTransfer();

    static void captureCallback(struct libusb_transfer* transfer);
    void handleCaptureComplete(struct libusb_transfer* transfer);
    void submitCaptureTransfer(int index);

    void ensureEventThread();
    void releaseEventThread();

    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;

    std::vector<UsbAudioFormat> formats;
    UsbAudioFormat activeFormat{};

    int configuredRate = 0;
    int configuredChannels = 0;
    int configuredBitDepth = 0;
    int configuredSubslotSize = 0;
    int uacVersion = 1;
    int clockSourceId = -1;
    int acInterfaceNum = 0;
    int usbSpeed = 0;

    // Volume / Feature Unit state. -1 unitId == no hardware volume detected.
    // Q8.8 signed dB on the UAC2 wire (UAC1 SLR/LR widely used as Q8.8 too).
    int volumeFuUnitId = -1;
    bool hasFuMute = false;
    int16_t volumeMinDbQ8 = -8000;  // -32 dB default until queried
    int16_t volumeMaxDbQ8 = 0;
    int16_t volumeResDbQ8 = 256;    // 1 dB step default
    std::atomic<float> softwareGain{1.0f};

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
    std::atomic<int> eventThreadUsers{0};
    bool opened = false;
    bool configured = false;
    bool interfaceClaimed = false;
    bool acInterfaceClaimed = false;

    // --- Capture-side mirror of OUT pipeline ---
    UsbAudioFormat capActiveFormat{};
    int capRate = 0;
    int capChannels = 0;
    int capBitDepth = 0;
    int capSubslotSize = 0;
    int capEffectiveMaxPkt = 0;

    static const int CAP_NUM_TRANSFERS = 8;
    static const int CAP_PACKETS_PER_TRANSFER = 8;
    struct libusb_transfer* capTransfers[CAP_NUM_TRANSFERS] = {};
    uint8_t* capTransferBuffers[CAP_NUM_TRANSFERS] = {};
    int capTransferBufSize = 0;
    std::atomic<int> capActiveTransfers{0};

    RingBuffer* capRingBuffer = nullptr;

    std::atomic<bool> capStreaming{false};
    bool capInterfaceClaimed = false;
    bool capConfigured = false;
};

#endif // USB_AUDIO_H
