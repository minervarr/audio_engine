#include "usb_audio.h"
#include "libusb/libusb/libusb.h"
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <cerrno>
#include <sched.h>
#include <pthread.h>

#define LOG_TAG "UsbAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

UsbAudioDriver::UsbAudioDriver() = default;

UsbAudioDriver::~UsbAudioDriver() {
    close();
    delete ringBuffer;
}

bool UsbAudioDriver::open(int fd) {
    if (opened) close();

    LOGI("Opening USB device fd=%d", fd);

    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    int rc = libusb_init(&ctx);
    if (rc < 0) {
        LOGE("libusb_init failed: %s", libusb_error_name(rc));
        return false;
    }

    rc = libusb_wrap_sys_device(ctx, (intptr_t)fd, &handle);
    if (rc < 0) {
        LOGE("libusb_wrap_sys_device failed: %s", libusb_error_name(rc));
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    usbSpeed = libusb_get_device_speed(libusb_get_device(handle));
    LOGI("USB device speed: %s",
        usbSpeed == LIBUSB_SPEED_LOW ? "Low" :
        usbSpeed == LIBUSB_SPEED_FULL ? "Full" :
        usbSpeed == LIBUSB_SPEED_HIGH ? "High" :
        usbSpeed == LIBUSB_SPEED_SUPER ? "Super" : "Unknown");

    rc = libusb_set_auto_detach_kernel_driver(handle, 1);
    if (rc < 0) {
        LOGI("auto_detach_kernel_driver not supported: %s (non-fatal)", libusb_error_name(rc));
    }

    opened = true;
    LOGI("USB device opened via fd %d", fd);
    return true;
}

bool UsbAudioDriver::parseDescriptors() {
    if (!handle) return false;

    formats.clear();
    uacVersion = 1;
    clockSourceId = -1;

    libusb_device* dev = libusb_get_device(handle);
    struct libusb_config_descriptor* config = nullptr;

    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc < 0) {
        LOGE("get_active_config_descriptor failed: %s", libusb_error_name(rc));
        return false;
    }

    // Pass 1: Find Audio Control interface, detect UAC version & clock source
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; a++) {
            const struct libusb_interface_descriptor& alt = iface.altsetting[a];
            // Audio Control interface: class=1, subclass=1
            if (alt.bInterfaceClass != 1 || alt.bInterfaceSubClass != 1) continue;

            acInterfaceNum = alt.bInterfaceNumber;
            const uint8_t* extra = alt.extra;
            int extraLen = alt.extra_length;
            int pos = 0;

            while (pos + 4 < extraLen) {
                int descLen = extra[pos];
                int descType = extra[pos + 1];
                if (descLen < 3) break;

                if (descType == USB_DT_CS_INTERFACE) {
                    int subType = extra[pos + 2];
                    if (subType == UAC_HEADER && descLen >= 5) {
                        int bcdADC = extra[pos + 3] | (extra[pos + 4] << 8);
                        if (bcdADC >= UAC_VERSION_2) {
                            uacVersion = 2;
                        }
                        LOGI("UAC version: %d (bcdADC=0x%04x) on interface %d",
                             uacVersion, bcdADC, acInterfaceNum);
                    }
                    // UAC2 Clock Source descriptor
                    if (uacVersion == 2 && subType == UAC_CLOCK_SOURCE && descLen >= 5) {
                        clockSourceId = extra[pos + 3];
                        LOGI("UAC2 Clock Source ID: %d", clockSourceId);
                    }
                }
                pos += descLen;
            }
        }
    }

    // Pass 2: Find Audio Streaming interfaces
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface& iface = config->interface[i];

        for (int a = 0; a < iface.num_altsetting; a++) {
            const struct libusb_interface_descriptor& alt = iface.altsetting[a];

            if (alt.bInterfaceClass != 1 || alt.bInterfaceSubClass != 2) continue;
            if (alt.bNumEndpoints == 0) continue;

            // Find isochronous OUT endpoint and optional feedback IN endpoint
            int epAddr = -1;
            int feedbackEp = -1;
            int rawMaxPacket = 0;

            for (int e = 0; e < alt.bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) continue;

                if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                    epAddr = ep.bEndpointAddress;
                    rawMaxPacket = ep.wMaxPacketSize;
                } else {
                    // Isochronous IN = feedback endpoint
                    feedbackEp = ep.bEndpointAddress;
                    LOGI("  Feedback EP 0x%02x on iface %d alt %d",
                         feedbackEp, alt.bInterfaceNumber, alt.bAlternateSetting);
                }
            }
            if (epAddr < 0) continue;

            // Parse class-specific AS descriptors for format info
            int channels = 2;
            int bitDepth = 16;
            std::vector<int> rates;

            const uint8_t* extra = alt.extra;
            int extraLen = alt.extra_length;
            int pos = 0;

            while (pos + 2 < extraLen) {
                int descLen = extra[pos];
                int descType = extra[pos + 1];
                if (descLen < 2) break;

                if (descType == USB_DT_CS_INTERFACE && pos + 3 < extraLen) {
                    int subType = extra[pos + 2];

                    if (uacVersion >= 2) {
                        // UAC2 AS_GENERAL: bNrChannels at offset 10
                        // Layout: [0]=bLength [1]=bDescriptorType [2]=bDescriptorSubtype
                        //         [3]=bTerminalLink [4]=bmControls [5]=bFormatType
                        //         [6..9]=bmFormats [10]=bNrChannels
                        if (subType == UAC_AS_GENERAL && descLen >= 16) {
                            channels = extra[pos + 10];
                        }
                        // UAC2 FORMAT_TYPE: bBitResolution at offset 5
                        if (subType == UAC_FORMAT_TYPE && descLen >= 6) {
                            bitDepth = extra[pos + 5];
                        }
                    } else {
                        // UAC1 FORMAT_TYPE descriptor
                        if (subType == UAC_FORMAT_TYPE && descLen >= 8) {
                            if (extra[pos + 3] == UAC_FORMAT_TYPE_I) {
                                channels = extra[pos + 4];
                                bitDepth = extra[pos + 6];
                                int numRates = extra[pos + 7];
                                if (numRates == 0 && descLen >= 14) {
                                    int minRate = extra[pos + 8] | (extra[pos + 9] << 8) | (extra[pos + 10] << 16);
                                    int maxRate = extra[pos + 11] | (extra[pos + 12] << 8) | (extra[pos + 13] << 16);
                                    int commonRates[] = {44100, 48000, 88200, 96000,
                                                         176400, 192000, 352800, 384000};
                                    for (int r : commonRates) {
                                        if (r >= minRate && r <= maxRate) rates.push_back(r);
                                    }
                                } else {
                                    for (int r = 0; r < numRates && pos + 8 + r * 3 + 2 < extraLen; r++) {
                                        int off = pos + 8 + r * 3;
                                        int rate = extra[off] | (extra[off + 1] << 8) | (extra[off + 2] << 16);
                                        rates.push_back(rate);
                                    }
                                }
                            }
                        }
                    }
                }
                pos += descLen;
            }

            if (rates.empty()) {
                rates = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
            }

            for (int rate : rates) {
                UsbAudioFormat fmt{};
                fmt.interfaceNum = alt.bInterfaceNumber;
                fmt.altSetting = alt.bAlternateSetting;
                fmt.endpointAddr = epAddr;
                fmt.maxPacketSize = rawMaxPacket;
                fmt.sampleRate = rate;
                fmt.channels = channels;
                fmt.bitDepth = bitDepth;
                fmt.feedbackEpAddr = feedbackEp;
                formats.push_back(fmt);
            }
        }
    }

    libusb_free_config_descriptor(config);

    LOGI("Parsed %zu format(s), UAC%d", formats.size(), uacVersion);
    for (auto& f : formats) {
        LOGI("  iface=%d alt=%d ep=0x%02x rate=%d ch=%d bits=%d maxpkt=0x%04x fb=0x%02x",
             f.interfaceNum, f.altSetting, f.endpointAddr,
             f.sampleRate, f.channels, f.bitDepth, f.maxPacketSize, f.feedbackEpAddr);
    }

    return !formats.empty();
}

std::vector<int> UsbAudioDriver::getSupportedRates() {
    std::vector<int> rates;
    for (auto& f : formats) {
        if (std::find(rates.begin(), rates.end(), f.sampleRate) == rates.end()) {
            rates.push_back(f.sampleRate);
        }
    }
    std::sort(rates.begin(), rates.end());
    return rates;
}

bool UsbAudioDriver::setInterfaceAltSetting(int interface_num, int alt_setting) {
    int rc = libusb_set_interface_alt_setting(handle, interface_num, alt_setting);
    if (rc < 0) {
        LOGE("set_interface_alt_setting(%d, %d) failed: %s",
             interface_num, alt_setting, libusb_error_name(rc));
        return false;
    }
    return true;
}

bool UsbAudioDriver::setSampleRate(int endpoint, int rate) {
    // UAC1: 3-byte SET_CUR to endpoint
    uint8_t data[3];
    data[0] = rate & 0xFF;
    data[1] = (rate >> 8) & 0xFF;
    data[2] = (rate >> 16) & 0xFF;

    int rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT,
        UAC_SET_CUR,
        0x0100, // SAMPLING_FREQ_CONTROL
        endpoint,
        data, 3, 1000);

    if (rc < 0) {
        LOGE("UAC1 setSampleRate(%d) failed: %s", rate, libusb_error_name(rc));
        return true; // non-fatal, some devices auto-detect
    }

    LOGI("UAC1: set sample rate %d Hz on EP 0x%02x", rate, endpoint);
    return true;
}

bool UsbAudioDriver::setSampleRateUAC2(int clockId, int rate) {
    // UAC2: 4-byte SET_CUR to clock source entity on AC interface
    uint8_t data[4];
    data[0] = rate & 0xFF;
    data[1] = (rate >> 8) & 0xFF;
    data[2] = (rate >> 16) & 0xFF;
    data[3] = (rate >> 24) & 0xFF;

    // wValue = CS << 8 | CN, CS = SAM_FREQ_CONTROL (0x01), CN = 0
    // wIndex = clock source ID << 8 | interface number
    uint16_t wIndex = (uint16_t)((clockId << 8) | acInterfaceNum);

    int rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        UAC_SET_CUR,
        (UAC2_CS_CONTROL_SAM_FREQ << 8),
        wIndex,
        data, 4, 1000);

    if (rc < 0) {
        LOGE("UAC2 setSampleRate(%d) clockId=%d failed: %s",
             rate, clockId, libusb_error_name(rc));
        return false;
    }

    LOGI("UAC2: set sample rate %d Hz on clock source %d", rate, clockId);

    // Verify clock is valid
    uint8_t validBuf[1] = {0};
    rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        UAC_GET_CUR,
        (UAC2_CS_CONTROL_CLOCK_VALID << 8),
        wIndex,
        validBuf, 1, 1000);

    if (rc >= 1) {
        if (validBuf[0]) {
            LOGI("UAC2: clock valid after rate change");
        } else {
            LOGE("UAC2: clock reports INVALID after rate change");
        }
    } else {
        LOGI("UAC2: clock valid query not supported (rc=%d), continuing", rc);
    }

    return true;
}

bool UsbAudioDriver::configure(int sampleRate, int channels, int bitDepth) {
    if (!opened) {
        LOGE("configure() called but not opened");
        return false;
    }

    // Stop-before-reconfigure guard
    if (streaming.load()) {
        LOGI("configure() called while streaming -- stopping first");
        stop();
    }

    LOGI("configure requested: rate=%d ch=%d bits=%d", sampleRate, channels, bitDepth);

    // Find best matching format
    UsbAudioFormat* best = nullptr;
    for (auto& f : formats) {
        if (f.sampleRate == sampleRate && f.channels == channels && f.bitDepth == bitDepth) {
            best = &f;
            break;
        }
    }
    // Relax: match rate, prefer highest bit depth
    if (!best) {
        for (auto& f : formats) {
            if (f.sampleRate == sampleRate) {
                if (!best || f.bitDepth > best->bitDepth) {
                    best = &f;
                }
            }
        }
    }
    if (!best) {
        LOGE("No matching format for rate=%d ch=%d bits=%d", sampleRate, channels, bitDepth);
        return false;
    }

    activeFormat = *best;
    configuredRate = sampleRate;
    configuredChannels = best->channels;
    configuredBitDepth = best->bitDepth;
    configured = true;

    LOGI("Configured: rate=%d ch=%d bits=%d iface=%d alt=%d ep=0x%02x UAC%d",
         configuredRate, configuredChannels, configuredBitDepth,
         activeFormat.interfaceNum, activeFormat.altSetting,
         activeFormat.endpointAddr, uacVersion);
    return true;
}

// --- Transfer callbacks ---

void UsbAudioDriver::transferCallback(struct libusb_transfer* transfer) {
    auto* driver = static_cast<UsbAudioDriver*>(transfer->user_data);
    driver->handleTransferComplete(transfer);
}

void UsbAudioDriver::handleTransferComplete(struct libusb_transfer* transfer) {
    if (!streaming.load()) {
        activeTransfers--;
        return;
    }

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
        transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
        LOGE("Transfer status=%d (%s)", transfer->status,
             transfer->status == LIBUSB_TRANSFER_ERROR ? "ERROR" :
             transfer->status == LIBUSB_TRANSFER_STALL ? "STALL" :
             transfer->status == LIBUSB_TRANSFER_NO_DEVICE ? "NO_DEVICE" :
             transfer->status == LIBUSB_TRANSFER_OVERFLOW ? "OVERFLOW" :
             transfer->status == LIBUSB_TRANSFER_CANCELLED ? "CANCELLED" : "UNKNOWN");
        int remaining = --activeTransfers;
        if (remaining <= 0) {
            LOGE("All isochronous transfers failed -- stopping streaming");
            streaming.store(false);
        }
        return;
    }

    // Find which transfer index this is
    int idx = -1;
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (transfers[i] == transfer) { idx = i; break; }
    }
    if (idx < 0) {
        int remaining = --activeTransfers;
        if (remaining <= 0) {
            LOGE("All transfers lost -- stopping streaming");
            streaming.store(false);
        }
        return;
    }

    submitTransfer(idx);
}

void UsbAudioDriver::submitTransfer(int index) {
    if (!streaming.load()) return;

    libusb_transfer* xfr = transfers[index];
    int bytesPerFrame = (configuredBitDepth / 8) * configuredChannels;
    uint8_t* buf = transferBuffers[index];

    // Determine frames per packet, using feedback if available
    bool isHighSpeed = (usbSpeed >= LIBUSB_SPEED_HIGH);
    double nominalFpp = isHighSpeed ?
        (configuredRate / 8000.0) : (configuredRate / 1000.0);

    uint32_t fb = currentFeedback.load(std::memory_order_relaxed);
    double fpp;
    if (fb > 0) {
        // Feedback is in 16.16 format (UAC1 10.14 was converted to 16.16 on receipt)
        fpp = fb / 65536.0;
    } else {
        fpp = nominalFpp;
    }

    // Parse effective max packet size from raw wMaxPacketSize
    int basePktSize = activeFormat.maxPacketSize & 0x7FF;
    int mult = ((activeFormat.maxPacketSize >> 11) & 0x03) + 1;
    int effectiveMaxPkt = basePktSize * mult;

    int totalFilled = 0;

    // Fill each packet individually with feedback-adjusted frame count
    for (int p = 0; p < PACKETS_PER_TRANSFER; p++) {
        feedbackAccumulator += fpp;
        int frames = (int)feedbackAccumulator;
        feedbackAccumulator -= frames;

        int packetBytes = frames * bytesPerFrame;
        if (packetBytes > effectiveMaxPkt) {
            packetBytes = effectiveMaxPkt;
        }

        int offset = totalFilled;
        int got = 0;
        if (ringBuffer) {
            got = (int)ringBuffer->read(buf + offset, packetBytes);
        }
        // Zero-fill remainder (silence)
        if (got < packetBytes) {
            memset(buf + offset + got, 0, packetBytes - got);
        }

        xfr->iso_packet_desc[p].length = packetBytes;
        totalFilled += packetBytes;
    }

    xfr->length = totalFilled;

    int rc = libusb_submit_transfer(xfr);
    if (rc < 0) {
        LOGE("resubmit_transfer failed: %s", libusb_error_name(rc));
        int remaining = --activeTransfers;
        if (remaining <= 0) {
            LOGE("All transfers lost on resubmit -- stopping streaming");
            streaming.store(false);
        }
    }
}

// --- Feedback endpoint ---

void UsbAudioDriver::feedbackCallback(struct libusb_transfer* transfer) {
    auto* driver = static_cast<UsbAudioDriver*>(transfer->user_data);
    driver->handleFeedbackComplete(transfer);
}

void UsbAudioDriver::handleFeedbackComplete(struct libusb_transfer* transfer) {
    if (!streaming.load()) return;

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int actualLen = transfer->iso_packet_desc[0].actual_length;
        if (actualLen >= 3) {
            uint32_t fb;
            if (actualLen >= 4 || uacVersion == 2) {
                // UAC2: 16.16 fixed point, already in desired format
                fb = feedbackBuffer[0] | ((uint32_t)feedbackBuffer[1] << 8) |
                     ((uint32_t)feedbackBuffer[2] << 16);
                if (actualLen >= 4) {
                    fb |= ((uint32_t)feedbackBuffer[3] << 24);
                }
            } else {
                // UAC1: 10.14 fixed point -- convert to 16.16 by shifting left 2
                fb = feedbackBuffer[0] | ((uint32_t)feedbackBuffer[1] << 8) |
                     ((uint32_t)feedbackBuffer[2] << 16);
                fb <<= 2;
            }

            uint32_t prev = currentFeedback.load(std::memory_order_relaxed);
            currentFeedback.store(fb, std::memory_order_relaxed);

            if (prev == 0) {
                double rate = fb / 65536.0;
                LOGI("First feedback value: %.4f frames/packet (raw=0x%08x)", rate, fb);
            }
        }
    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        LOGE("Feedback: device disconnected");
        return; // don't resubmit
    }

    // Resubmit
    submitFeedbackTransfer();
}

void UsbAudioDriver::submitFeedbackTransfer() {
    if (!feedbackTransfer || !streaming.load()) return;

    int rc = libusb_submit_transfer(feedbackTransfer);
    if (rc < 0 && rc != LIBUSB_ERROR_NO_DEVICE) {
        LOGE("Feedback submit failed: %s", libusb_error_name(rc));
    }
}

// --- Start / Stop ---

bool UsbAudioDriver::start() {
    if (!configured || !handle) {
        LOGE("start() called but configured=%d handle=%p", configured, handle);
        return false;
    }

    // Parse effective max packet size
    int basePktSize = activeFormat.maxPacketSize & 0x7FF;
    int mult = ((activeFormat.maxPacketSize >> 11) & 0x03) + 1;
    int effectiveMaxPkt = basePktSize * mult;

    bool isHighSpeed = (usbSpeed >= LIBUSB_SPEED_HIGH);
    int bytesPerFrame = (configuredBitDepth / 8) * configuredChannels;

    // Correct frames-per-packet based on USB speed
    int nominalFpp;
    if (isHighSpeed) {
        nominalFpp = configuredRate / 8000; // 125us microframes
    } else {
        nominalFpp = configuredRate / 1000; // 1ms frames
    }
    int bytesPerPacket = nominalFpp * bytesPerFrame;
    if (bytesPerPacket > effectiveMaxPkt) {
        LOGI("bytesPerPacket %d capped to effectiveMaxPkt %d", bytesPerPacket, effectiveMaxPkt);
        bytesPerPacket = effectiveMaxPkt;
    }

    LOGI("Starting: iface=%d alt=%d ep=0x%02x rate=%d ch=%d bits=%d "
         "maxpkt=0x%04x(eff=%d) speed=%s fpp=%d bpp=%d UAC%d",
         activeFormat.interfaceNum, activeFormat.altSetting, activeFormat.endpointAddr,
         configuredRate, configuredChannels, configuredBitDepth,
         activeFormat.maxPacketSize, effectiveMaxPkt,
         isHighSpeed ? "High" : "Full",
         nominalFpp, bytesPerPacket, uacVersion);

    // Allocate ring buffer: 200ms of output audio
    int ringSize = configuredRate * bytesPerFrame / 5; // 200ms
    if (ringSize < 65536) ringSize = 65536;
    delete ringBuffer;
    ringBuffer = new RingBuffer(ringSize);
    LOGI("Ring buffer allocated: %d bytes (%.0f ms)",
         ringSize, ringSize * 1000.0 / (configuredRate * bytesPerFrame));

    // Lock ring buffer pages to prevent page faults during streaming
    if (mlock(ringBuffer->getBuffer(), ringBuffer->getCapacity()) != 0) {
        LOGI("mlock ring buffer failed: %s (non-fatal)", strerror(errno));
    } else {
        LOGI("mlock ring buffer: %zu bytes locked", ringBuffer->getCapacity());
    }

    // Detach kernel driver from both Audio Control and Streaming interfaces
    // to prevent Android's snd-usb-audio from reclaiming the device
    int rc;
    if (acInterfaceNum >= 0) {
        rc = libusb_detach_kernel_driver(handle, acInterfaceNum);
        LOGI("detach_kernel_driver(AC iface %d): %s", acInterfaceNum,
             rc == 0 ? "OK" : libusb_error_name(rc));
    }
    rc = libusb_detach_kernel_driver(handle, activeFormat.interfaceNum);
    LOGI("detach_kernel_driver(AS iface %d): %s", activeFormat.interfaceNum,
         rc == 0 ? "OK" : libusb_error_name(rc));

    // Claim Audio Control interface to hold off kernel driver
    if (acInterfaceNum >= 0) {
        rc = libusb_claim_interface(handle, acInterfaceNum);
        if (rc < 0) {
            LOGI("claim_interface(AC %d) failed: %s (non-fatal)",
                 acInterfaceNum, libusb_error_name(rc));
        } else {
            acInterfaceClaimed = true;
            LOGI("Claimed AC interface %d", acInterfaceNum);
        }
    }

    // Claim streaming interface
    rc = libusb_claim_interface(handle, activeFormat.interfaceNum);
    if (rc < 0) {
        LOGE("claim_interface(%d) failed: %s", activeFormat.interfaceNum, libusb_error_name(rc));
        stop();
        return false;
    }
    interfaceClaimed = true;

    // Reset to zero-bandwidth, then set active alt setting
    libusb_set_interface_alt_setting(handle, activeFormat.interfaceNum, 0);
    if (!setInterfaceAltSetting(activeFormat.interfaceNum, activeFormat.altSetting)) {
        stop();
        return false;
    }

    // Set sample rate based on UAC version
    // Note: uacVersion is stored as integer (1 or 2), not BCD
    if (uacVersion >= 2 && clockSourceId >= 0) {
        setSampleRateUAC2(clockSourceId, configuredRate);
    } else {
        setSampleRate(activeFormat.endpointAddr, configuredRate);
    }

    // Pre-allocate and pre-fault transfer buffers
    // Use effectiveMaxPkt for buffer sizing to handle feedback-adjusted packets
    int maxBufSize = effectiveMaxPkt * PACKETS_PER_TRANSFER;
    // Ensure buffer is at least big enough for nominal case
    int nominalBufSize = bytesPerPacket * PACKETS_PER_TRANSFER;
    if (maxBufSize < nominalBufSize) maxBufSize = nominalBufSize;

    transferBufSize = maxBufSize;

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        transfers[i] = libusb_alloc_transfer(PACKETS_PER_TRANSFER);
        if (!transfers[i]) {
            LOGE("alloc_transfer failed");
            stop();
            return false;
        }
        transferBuffers[i] = new uint8_t[maxBufSize];
        memset(transferBuffers[i], 0, maxBufSize); // pre-fault pages

        // Lock transfer buffer pages
        if (mlock(transferBuffers[i], maxBufSize) != 0) {
            LOGI("mlock transfer[%d] failed: %s (non-fatal)", i, strerror(errno));
        }

        libusb_fill_iso_transfer(transfers[i], handle,
            activeFormat.endpointAddr,
            transferBuffers[i], nominalBufSize,
            PACKETS_PER_TRANSFER,
            transferCallback, this, 1000);

        libusb_set_iso_packet_lengths(transfers[i], bytesPerPacket);
    }

    // Initialize feedback state
    feedbackAccumulator = 0.0;
    currentFeedback.store(0, std::memory_order_relaxed);

    streaming.store(true);
    activeTransfers.store(NUM_TRANSFERS);

    // Submit all data transfers
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        rc = libusb_submit_transfer(transfers[i]);
        if (rc < 0) {
            LOGE("submit_transfer[%d] failed: %s", i, libusb_error_name(rc));
            activeTransfers--;
        }
    }

    if (activeTransfers.load() <= 0) {
        LOGE("All initial transfers failed to submit");
        streaming.store(false);
        stop();
        return false;
    }

    LOGI("%d/%d initial transfers submitted", activeTransfers.load(), NUM_TRANSFERS);

    // Set up feedback endpoint if present (async mode)
    if (activeFormat.feedbackEpAddr >= 0) {
        int fbPktSize = (uacVersion == 2) ? 4 : 3;
        feedbackTransfer = libusb_alloc_transfer(1); // 1 iso packet
        if (feedbackTransfer) {
            memset(feedbackBuffer, 0, sizeof(feedbackBuffer));
            libusb_fill_iso_transfer(feedbackTransfer, handle,
                activeFormat.feedbackEpAddr,
                feedbackBuffer, fbPktSize,
                1, // 1 iso packet
                feedbackCallback, this, 1000);
            libusb_set_iso_packet_lengths(feedbackTransfer, fbPktSize);

            rc = libusb_submit_transfer(feedbackTransfer);
            if (rc < 0) {
                LOGE("Feedback EP submit failed: %s", libusb_error_name(rc));
                libusb_free_transfer(feedbackTransfer);
                feedbackTransfer = nullptr;
            } else {
                LOGI("Feedback EP 0x%02x active (%d-byte packets)",
                     activeFormat.feedbackEpAddr, fbPktSize);
            }
        }
    }

    // Start event handling thread with elevated priority
    eventThread = std::thread([this]() {
        // THREAD_PRIORITY_URGENT_AUDIO equivalent (-19), no starvation risk
        if (setpriority(PRIO_PROCESS, 0, -19) != 0) {
            LOGI("setpriority(-19) failed: %s, using default", strerror(errno));
        } else {
            LOGI("Event thread: nice=-19 (urgent audio)");
        }

        LOGI("Event thread started");
        while (streaming.load() && activeTransfers.load() > 0) {
            struct timeval tv = {0, 10000}; // 10ms timeout
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
        LOGI("Event thread exited (streaming=%d active=%d)",
             streaming.load() ? 1 : 0, activeTransfers.load());
    });

    LOGI("USB audio streaming started (%d transfers, ring=%d bytes)", NUM_TRANSFERS, ringSize);
    return true;
}

int UsbAudioDriver::write(const uint8_t* data, int length) {
    if (!streaming.load() || !ringBuffer) return -1;
    // Align write to frame boundary to prevent sample misalignment in ring buffer
    int bytesPerFrame = (configuredBitDepth / 8) * configuredChannels;
    if (bytesPerFrame > 0) {
        size_t space = ringBuffer->getFreeSpace();
        int aligned = ((int)space / bytesPerFrame) * bytesPerFrame;
        if (aligned <= 0) return 0;
        length = std::min(length, aligned);
    }
    return (int)ringBuffer->write(data, length);
}

int UsbAudioDriver::writeFloat32(const float* data, int numSamples) {
    if (!streaming.load() || !ringBuffer) return -1;

    int bytesPerSample = configuredBitDepth / 8;

    // Convert in chunks to avoid large stack allocation
    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4]; // max 4 bytes per sample (32-bit)
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);

        // Pre-check free space and limit batch to what fits, aligned to sample size.
        // This prevents partial-sample writes that corrupt frame alignment.
        // getFreeSpace() is a lower bound (reader may free more), so the
        // subsequent write is guaranteed to accept the full batch.
        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / bytesPerSample);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        int outBytes = 0;

        for (int i = 0; i < batch; i++) {
            float s = data[totalConsumed + i];
            // Clamp
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;

            switch (configuredBitDepth) {
                case 16: {
                    int16_t v = (int16_t)(s * 32767.0f);
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    break;
                }
                case 24: {
                    int32_t v = (int32_t)(s * 8388607.0f);
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    convBuf[outBytes++] = (v >> 16) & 0xFF;
                    break;
                }
                case 32: {
                    int32_t v = (int32_t)(s * 2147483647.0f);
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    convBuf[outBytes++] = (v >> 16) & 0xFF;
                    convBuf[outBytes++] = (v >> 24) & 0xFF;
                    break;
                }
                default: {
                    int16_t v = (int16_t)(s * 32767.0f);
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    break;
                }
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / bytesPerSample;
        totalConsumed += samplesWritten;

        if (samplesWritten < batch) {
            break; // ring buffer full, return what we consumed
        }
    }

    return totalConsumed;
}

int UsbAudioDriver::writeInt16(const int16_t* data, int numSamples) {
    if (!streaming.load() || !ringBuffer) return -1;

    int bytesPerSample = configuredBitDepth / 8;

    // Convert in chunks to avoid large stack allocation
    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4]; // max 4 bytes per sample (32-bit)
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);

        // Pre-check free space and limit batch to what fits, aligned to sample size.
        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / bytesPerSample);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        int outBytes = 0;

        for (int i = 0; i < batch; i++) {
            int16_t s = data[totalConsumed + i];

            switch (configuredBitDepth) {
                case 16: {
                    // Bit-perfect passthrough
                    convBuf[outBytes++] = s & 0xFF;
                    convBuf[outBytes++] = (s >> 8) & 0xFF;
                    break;
                }
                case 24: {
                    // Shift left 8 to place 16-bit data in upper bits of 24-bit
                    int32_t v = (int32_t)s << 8;
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    convBuf[outBytes++] = (v >> 16) & 0xFF;
                    break;
                }
                case 32: {
                    // Shift left 16 to place 16-bit data in upper bits of 32-bit
                    int32_t v = (int32_t)s << 16;
                    convBuf[outBytes++] = v & 0xFF;
                    convBuf[outBytes++] = (v >> 8) & 0xFF;
                    convBuf[outBytes++] = (v >> 16) & 0xFF;
                    convBuf[outBytes++] = (v >> 24) & 0xFF;
                    break;
                }
                default: {
                    convBuf[outBytes++] = s & 0xFF;
                    convBuf[outBytes++] = (s >> 8) & 0xFF;
                    break;
                }
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / bytesPerSample;
        totalConsumed += samplesWritten;

        if (samplesWritten < batch) {
            break; // ring buffer full
        }
    }

    return totalConsumed;
}

void UsbAudioDriver::flush() {
    if (ringBuffer) {
        ringBuffer->clear();
    }
}

void UsbAudioDriver::stop() {
    streaming.store(false);

    // Wait for event thread to exit before touching any libusb resources.
    // The thread loop checks streaming.load() and will exit after its current
    // libusb_handle_events_timeout_completed returns (at most 10ms).
    if (eventThread.joinable()) {
        eventThread.join();
    }

    if (ctx) {
        // Process pending events to let transfers complete
        for (int retry = 0; retry < 50 && activeTransfers.load() > 0; retry++) {
            struct timeval tv = {0, 10000};
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }

        // Cancel data transfers
        for (int i = 0; i < NUM_TRANSFERS; i++) {
            if (transfers[i]) {
                libusb_cancel_transfer(transfers[i]);
            }
        }

        // Cancel feedback transfer
        if (feedbackTransfer) {
            libusb_cancel_transfer(feedbackTransfer);
        }

        // Process cancellations
        for (int retry = 0; retry < 30 && activeTransfers.load() > 0; retry++) {
            struct timeval tv = {0, 5000};
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
    }

    // Unlock and free data transfers
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (transfers[i]) {
            libusb_free_transfer(transfers[i]);
            transfers[i] = nullptr;
        }
        if (transferBuffers[i]) {
            if (transferBufSize > 0) {
                munlock(transferBuffers[i], transferBufSize);
            }
            delete[] transferBuffers[i];
            transferBuffers[i] = nullptr;
        }
    }
    transferBufSize = 0;

    // Free feedback transfer
    if (feedbackTransfer) {
        libusb_free_transfer(feedbackTransfer);
        feedbackTransfer = nullptr;
    }

    // Reset alt setting and release interfaces
    if (handle && interfaceClaimed) {
        libusb_set_interface_alt_setting(handle, activeFormat.interfaceNum, 0);
        libusb_release_interface(handle, activeFormat.interfaceNum);
        interfaceClaimed = false;
    }
    if (handle && acInterfaceClaimed) {
        libusb_release_interface(handle, acInterfaceNum);
        acInterfaceClaimed = false;
    }

    // Unlock and clear ring buffer
    if (ringBuffer) {
        munlock(ringBuffer->getBuffer(), ringBuffer->getCapacity());
        ringBuffer->clear();
    }

    activeTransfers.store(0);
    currentFeedback.store(0, std::memory_order_relaxed);
    feedbackAccumulator = 0.0;

    LOGI("USB audio streaming stopped");
}

void UsbAudioDriver::close() {
    LOGI("close() called, streaming=%d opened=%d", streaming.load() ? 1 : 0, opened);

    if (streaming.load()) {
        stop();
    }

    // Release AC interface if still held
    if (handle && acInterfaceClaimed) {
        libusb_release_interface(handle, acInterfaceNum);
        acInterfaceClaimed = false;
    }

    if (handle) {
        // Process any remaining events before closing
        if (ctx) {
            struct timeval tv = {0, 50000}; // 50ms
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
        libusb_close(handle);
        handle = nullptr;
    }
    if (ctx) {
        libusb_exit(ctx);
        ctx = nullptr;
    }

    formats.clear();
    configured = false;
    opened = false;
    uacVersion = 1;
    clockSourceId = -1;
    LOGI("USB audio device closed");
}

std::string UsbAudioDriver::getDeviceInfo() const {
    if (!configured) return "Not configured";

    char buf[256];
    snprintf(buf, sizeof(buf), "%dHz/%dbit/%dch UAC%d %s",
             configuredRate, configuredBitDepth, configuredChannels,
             uacVersion,
             usbSpeed >= LIBUSB_SPEED_HIGH ? "High-Speed" :
             usbSpeed == LIBUSB_SPEED_FULL ? "Full-Speed" : "");
    return std::string(buf);
}
