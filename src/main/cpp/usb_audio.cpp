#include "usb_audio.h"
#include "libusb/libusb/libusb.h"
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>
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

    // Pass 1: Find Audio Control interface, detect UAC version, enumerate clock
    // sources, and build a terminal -> clock-source ID map. Devices that are both
    // a DAC and an ADC have two clock sources (one per direction) -- without the
    // terminal-link chain we can't tell which clock to query for which alt-setting.
    // We also collect Feature Units here so we can pick the playback-chain FU
    // for hardware volume control.
    std::vector<int> clockSourceIds;
    std::unordered_map<int, int> terminalToClock;
    // Terminal IDs whose wTerminalType == 0x0101 (USB streaming) coming IN
    // to the audio function -- i.e., the playback-path entry points.
    std::unordered_map<int, bool> isUsbStreamingIT;
    // Terminal IDs that represent physical audio entering the function
    // (Microphone 0x0201..0x0205, External 0x0601..0x0603, etc.) -- capture sources.
    std::unordered_map<int, bool> isCaptureSourceIT;
    struct FuCandidate { int unitId; int sourceId; bool hasVolume; bool hasMute; };
    std::vector<FuCandidate> featureUnits;
    volumeFuUnitId = -1;
    hasFuMute = false;
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
                        int csId = extra[pos + 3];
                        if (clockSourceId < 0) clockSourceId = csId;  // first wins for legacy paths
                        if (std::find(clockSourceIds.begin(), clockSourceIds.end(), csId)
                                == clockSourceIds.end()) {
                            clockSourceIds.push_back(csId);
                        }
                        LOGI("UAC2 Clock Source ID: %d", csId);
                    }
                    // UAC2 Input Terminal (data flowing INTO audio function): bCSourceID at offset 7
                    if (uacVersion == 2 && subType == UAC_INPUT_TERMINAL && descLen >= 8) {
                        int termId = extra[pos + 3];
                        int termType = extra[pos + 4] | (extra[pos + 5] << 8);
                        int csId = extra[pos + 7];
                        terminalToClock[termId] = csId;
                        // 0x0101 = USB streaming -> playback entry point.
                        // Anything outside the 0x01xx (USB function) category is a
                        // physical capture source: Microphone (0x02xx), External
                        // (0x06xx), etc. Track those so we can identify capture
                        // alt-settings in pass 2.
                        if (termType == 0x0101) {
                            isUsbStreamingIT[termId] = true;
                        } else if ((termType & 0xFF00) != 0x0100) {
                            isCaptureSourceIT[termId] = true;
                        }
                        LOGI("UAC2 Input Terminal %d (type 0x%04x) -> Clock %d", termId, termType, csId);
                    }
                    // UAC2 Output Terminal (data flowing OUT of audio function): bCSourceID at offset 8
                    if (uacVersion == 2 && subType == UAC_OUTPUT_TERMINAL && descLen >= 9) {
                        int termId = extra[pos + 3];
                        int csId = extra[pos + 8];
                        terminalToClock[termId] = csId;
                        LOGI("UAC2 Output Terminal %d -> Clock %d", termId, csId);
                    }
                    // UAC2 Feature Unit: layout [3]=bUnitID [4]=bSourceID
                    // [5..]=bmaControls[N+1] (4 bytes per channel, master first).
                    // Master controls live at offset 5..8 little-endian.
                    if (uacVersion == 2 && subType == UAC_FEATURE_UNIT && descLen >= 11) {
                        FuCandidate fu{};
                        fu.unitId = extra[pos + 3];
                        fu.sourceId = extra[pos + 4];
                        uint32_t masterCtrls = (uint32_t)extra[pos + 5]
                                             | ((uint32_t)extra[pos + 6] << 8)
                                             | ((uint32_t)extra[pos + 7] << 16)
                                             | ((uint32_t)extra[pos + 8] << 24);
                        // bits[1:0]=mute, bits[3:2]=volume. !=0 means present.
                        fu.hasMute = ((masterCtrls >> 0) & 0x3) != 0;
                        fu.hasVolume = ((masterCtrls >> 2) & 0x3) != 0;
                        featureUnits.push_back(fu);
                        LOGI("UAC2 Feature Unit %d <- src %d ctrls=0x%08x vol=%d mute=%d",
                             fu.unitId, fu.sourceId, masterCtrls, fu.hasVolume, fu.hasMute);
                    }
                    // UAC1 Feature Unit: layout [3]=bUnitID [4]=bSourceID
                    // [5]=bControlSize [6..]=bmaControls[N+1] (bControlSize bytes each).
                    // Master is the first entry. bit 0=mute, bit 1=volume.
                    if (uacVersion < 2 && subType == UAC_FEATURE_UNIT && descLen >= 7) {
                        FuCandidate fu{};
                        fu.unitId = extra[pos + 3];
                        fu.sourceId = extra[pos + 4];
                        int ctrlSize = extra[pos + 5];
                        if (ctrlSize >= 1 && descLen >= 6 + ctrlSize) {
                            uint8_t masterCtrls = extra[pos + 6];
                            fu.hasMute = (masterCtrls & 0x01) != 0;
                            fu.hasVolume = (masterCtrls & 0x02) != 0;
                        }
                        featureUnits.push_back(fu);
                        LOGI("UAC1 Feature Unit %d <- src %d vol=%d mute=%d",
                             fu.unitId, fu.sourceId, fu.hasVolume, fu.hasMute);
                    }
                }
                pos += descLen;
            }
        }
    }

    // Pass 1.5: UAC2 only -- query every clock source we found. Each can have
    // a different rate range (e.g., DAC clock 44.1k..384k, ADC clock 44.1k..96k).
    // Cache results per clock so pass 2 can pick the right list per alt-setting.
    std::unordered_map<int, std::vector<int>> clockRates;
    if (uacVersion >= 2) {
        for (int csId : clockSourceIds) {
            clockRates[csId] = queryUac2SampleRates(csId);
        }
    }

    // Pick the best Feature Unit for hardware volume. Prefer FUs whose source
    // chain is rooted at a USB-streaming Input Terminal (the playback path).
    // Fall back to any FU with volume on master. DAC+ADC devices have an FU
    // per direction; this prefers the playback one.
    for (auto& fu : featureUnits) {
        if (!fu.hasVolume) continue;
        if (isUsbStreamingIT.count(fu.sourceId)) {
            volumeFuUnitId = fu.unitId;
            hasFuMute = fu.hasMute;
            break;
        }
    }
    if (volumeFuUnitId < 0) {
        // No FU directly fed by a USB-streaming IT. Could be chained through
        // a Selector/Mixer unit -- take the first FU with volume on master.
        for (auto& fu : featureUnits) {
            if (fu.hasVolume) {
                volumeFuUnitId = fu.unitId;
                hasFuMute = fu.hasMute;
                break;
            }
        }
    }
    if (volumeFuUnitId >= 0) {
        LOGI("Selected Feature Unit %d for hardware volume (mute=%d)",
             volumeFuUnitId, hasFuMute);
        queryHwVolumeRange();
    } else {
        LOGI("No usable Feature Unit with FU_VOLUME found -- software gain only");
    }

    // Pass 2: Find Audio Streaming interfaces
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface& iface = config->interface[i];

        for (int a = 0; a < iface.num_altsetting; a++) {
            const struct libusb_interface_descriptor& alt = iface.altsetting[a];

            if (alt.bInterfaceClass != 1 || alt.bInterfaceSubClass != 2) continue;
            if (alt.bNumEndpoints == 0) continue;

            // Walk endpoints. We classify direction here but don't decide which
            // is data vs. feedback yet -- that depends on whether the AS feeds
            // a playback (USB-streaming) IT or a capture-source IT, which we
            // learn from AS_GENERAL.bTerminalLink parsed below.
            int outEp = -1, inEp = -1;
            int rawMaxPacketOut = 0, rawMaxPacketIn = 0;

            for (int e = 0; e < alt.bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) continue;

                if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                    outEp = ep.bEndpointAddress;
                    rawMaxPacketOut = ep.wMaxPacketSize;
                } else {
                    inEp = ep.bEndpointAddress;
                    rawMaxPacketIn = ep.wMaxPacketSize;
                }
            }
            if (outEp < 0 && inEp < 0) continue;

            // Parse class-specific AS descriptors for format info
            int channels = 2;
            int bitDepth = 16;
            int subslotSize = 0;  // 0 = not parsed, will default to bitDepth/8 later
            uint32_t bmFormats = 0;  // UAC2 AS_GENERAL bmFormats (bit 31 = RAW_DATA / DSD)
            int bTerminalLink = 0;   // UAC2 AS_GENERAL.bTerminalLink -> terminalToClock lookup
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
                            bTerminalLink = extra[pos + 3];
                            channels = extra[pos + 10];
                            bmFormats = (uint32_t)extra[pos + 6]
                                      | ((uint32_t)extra[pos + 7] << 8)
                                      | ((uint32_t)extra[pos + 8] << 16)
                                      | ((uint32_t)extra[pos + 9] << 24);
                        }
                        // UAC2 FORMAT_TYPE: bSubslotSize at offset 4, bBitResolution at offset 5
                        if (subType == UAC_FORMAT_TYPE && descLen >= 6) {
                            subslotSize = extra[pos + 4];
                            bitDepth = extra[pos + 5];
                        }
                    } else {
                        // UAC1 FORMAT_TYPE descriptor
                        // Layout: [3]=bFormatType [4]=bNrChannels [5]=bSubFrameSize
                        //         [6]=bBitResolution [7]=bSamFreqType ...
                        if (subType == UAC_FORMAT_TYPE && descLen >= 8) {
                            if (extra[pos + 3] == UAC_FORMAT_TYPE_I) {
                                channels = extra[pos + 4];
                                subslotSize = extra[pos + 5];
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

            // Resolve which clock source serves this alt-setting.
            // AS_GENERAL.bTerminalLink -> terminalToClock map. Fall back to the
            // first clock found (legacy single-clock devices).
            int resolvedClockId = -1;
            if (uacVersion >= 2 && bTerminalLink > 0) {
                auto it = terminalToClock.find(bTerminalLink);
                if (it != terminalToClock.end()) resolvedClockId = it->second;
            }
            if (resolvedClockId < 0) resolvedClockId = clockSourceId;

            // Direction: capture iff bTerminalLink points to a physical source IT
            // (Microphone/Line-in/etc.). On capture alt-settings the IN EP carries
            // data and there is no async feedback EP (capture is source-clocked).
            // Without a positive capture hint we keep legacy behavior: iso OUT is
            // data, iso IN is feedback.
            bool isCaptureAs = false;
            if (uacVersion >= 2 && bTerminalLink > 0
                    && isCaptureSourceIT.count(bTerminalLink)) {
                isCaptureAs = true;
            }

            int dataEp, rawMaxPacket, fbEp;
            if (isCaptureAs) {
                if (inEp < 0) continue;            // capture AS without IN EP is malformed
                dataEp = inEp;
                rawMaxPacket = rawMaxPacketIn;
                fbEp = -1;
            } else {
                if (outEp < 0) continue;           // playback AS without OUT EP -- skip
                dataEp = outEp;
                rawMaxPacket = rawMaxPacketOut;
                fbEp = inEp;
                if (fbEp >= 0) {
                    LOGI("  Feedback EP 0x%02x on iface %d alt %d",
                         fbEp, alt.bInterfaceNumber, alt.bAlternateSetting);
                }
            }

            if (rates.empty()) {
                // Prefer rates for this alt's own clock source. On DAC+ADC devices
                // this keeps the ADC's lower ceiling from leaking into playback.
                if (resolvedClockId >= 0) {
                    auto it = clockRates.find(resolvedClockId);
                    if (it != clockRates.end() && !it->second.empty()) {
                        rates = it->second;
                    }
                }
                if (rates.empty()) {
                    // Conservative fallback. High rates (705.6k/768k) ONLY enter
                    // via a successful UAC2 GET RANGE -- otherwise we'd be
                    // claiming capabilities the DAC may not have, and start()
                    // could ship isochronous data at an un-acked rate (noise).
                    rates = {44100, 48000, 88200, 96000, 176400, 192000,
                             352800, 384000};
                }
            }

            // Fall back if subslot wasn't parsed (older/malformed descriptors)
            int effectiveSubslot = (subslotSize > 0) ? subslotSize : ((bitDepth + 7) / 8);

            for (int rate : rates) {
                UsbAudioFormat fmt{};
                fmt.interfaceNum = alt.bInterfaceNumber;
                fmt.altSetting = alt.bAlternateSetting;
                fmt.endpointAddr = dataEp;
                fmt.maxPacketSize = rawMaxPacket;
                fmt.sampleRate = rate;
                fmt.channels = channels;
                fmt.bitDepth = bitDepth;
                fmt.subslotSize = effectiveSubslot;
                fmt.bmFormats = bmFormats;
                fmt.isDsd = (bmFormats & 0x80000000u) != 0;
                fmt.isCapture = isCaptureAs;
                fmt.feedbackEpAddr = fbEp;
                fmt.clockSourceId = resolvedClockId;
                formats.push_back(fmt);
            }
        }
    }

    libusb_free_config_descriptor(config);

    // Duplicate-alt heuristic: some DACs expose two alt-settings with identical
    // (rate, channels, bitDepth) but don't set the RAW_DATA bit in bmFormats.
    // When that happens, assume the higher-numbered alt is the DSD one. Only
    // applies within a single direction -- capture alts never produce DSD.
    for (auto& f : formats) {
        if (f.isDsd || f.isCapture) continue;
        bool dsdAlreadyInGroup = false;
        UsbAudioFormat* higher = nullptr;
        for (auto& g : formats) {
            if (&g == &f) continue;
            if (g.isCapture) continue;
            if (g.sampleRate != f.sampleRate) continue;
            if (g.channels != f.channels) continue;
            if (g.bitDepth != f.bitDepth) continue;
            if (g.isDsd) { dsdAlreadyInGroup = true; break; }
            if (g.altSetting > f.altSetting && (!higher || g.altSetting > higher->altSetting)) {
                higher = &g;
            }
        }
        if (dsdAlreadyInGroup || !higher) continue;
        if (!higher->isDsd) {
            higher->isDsd = true;
            LOGI("heuristic: marked alt=%d (rate=%d ch=%d bits=%d) as DSD candidate",
                 higher->altSetting, higher->sampleRate, higher->channels, higher->bitDepth);
        }
    }

    LOGI("Parsed %zu format(s), UAC%d", formats.size(), uacVersion);
    for (auto& f : formats) {
        LOGI("  iface=%d alt=%d ep=0x%02x rate=%d ch=%d bits=%d subslot=%d bmFormats=0x%08x dsd=%d cap=%d clk=%d maxpkt=0x%04x fb=0x%02x",
             f.interfaceNum, f.altSetting, f.endpointAddr,
             f.sampleRate, f.channels, f.bitDepth, f.subslotSize,
             f.bmFormats, f.isDsd ? 1 : 0, f.isCapture ? 1 : 0, f.clockSourceId,
             f.maxPacketSize, f.feedbackEpAddr);
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

bool UsbAudioDriver::queryHwVolumeRange() {
    if (!handle || volumeFuUnitId < 0) return false;

    // UAC2 GET RANGE on FU_VOLUME_CONTROL, master channel.
    // Response: wNumSubRanges (2 bytes) + N * (wMIN, wMAX, wRES) each 2 bytes Q8.8 dB.
    const uint8_t bmRequest = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS
                            | LIBUSB_RECIPIENT_INTERFACE;
    const uint8_t bRequest = UAC2_RANGE;
    const uint16_t wValue = (UAC_FU_VOLUME_CONTROL << 8) | 0;  // master channel
    const uint16_t wIndex = (uint16_t)((volumeFuUnitId << 8) | acInterfaceNum);

    bool tempClaim = false;
    if (acInterfaceNum >= 0 && !acInterfaceClaimed) {
        if (libusb_claim_interface(handle, acInterfaceNum) == 0) tempClaim = true;
    }

    if (uacVersion >= 2) {
        uint8_t header[2] = {0, 0};
        int rc = libusb_control_transfer(handle, bmRequest, bRequest, wValue, wIndex,
                                         header, sizeof(header), 1000);
        if (rc < 2) {
            LOGI("FU volume GET_RANGE header read failed (rc=%d), using defaults", rc);
            if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
            return false;
        }
        uint16_t n = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
        if (n == 0) {
            if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
            return false;
        }
        int totalLen = 2 + (int)n * 6;
        std::vector<uint8_t> buf(totalLen, 0);
        rc = libusb_control_transfer(handle, bmRequest, bRequest, wValue, wIndex,
                                     buf.data(), (uint16_t)totalLen, 1000);
        if (rc < totalLen) {
            LOGI("FU volume GET_RANGE full read short (rc=%d expected=%d)", rc, totalLen);
            if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
            return false;
        }
        // Take the widest range across all sub-ranges (most DACs report one).
        int16_t minDb = 32767, maxDb = -32768;
        uint16_t res = 256;
        for (int i = 0; i < n; i++) {
            int off = 2 + i * 6;
            int16_t mn = (int16_t)(buf[off]     | ((uint16_t)buf[off + 1] << 8));
            int16_t mx = (int16_t)(buf[off + 2] | ((uint16_t)buf[off + 3] << 8));
            uint16_t rs = (uint16_t)(buf[off + 4] | ((uint16_t)buf[off + 5] << 8));
            if (mn < minDb) minDb = mn;
            if (mx > maxDb) maxDb = mx;
            if (rs > 0) res = rs;
        }
        volumeMinDbQ8 = minDb;
        volumeMaxDbQ8 = maxDb;
        volumeResDbQ8 = (int16_t)res;
        LOGI("FU volume range: min=%.2f dB max=%.2f dB res=%.2f dB",
             volumeMinDbQ8 / 256.0f, volumeMaxDbQ8 / 256.0f, volumeResDbQ8 / 256.0f);
    }
    // UAC1: would require three separate GET_MIN/MAX/RES queries. Keep defaults
    // for UAC1 (-32 dB .. 0 dB, 1 dB step); good enough for SET_CUR to work.

    if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
    return true;
}

bool UsbAudioDriver::setHwVolumeDbQ8(int valueDbQ8) {
    if (!handle || volumeFuUnitId < 0) return false;
    // Clamp to declared range.
    if (valueDbQ8 < volumeMinDbQ8) valueDbQ8 = volumeMinDbQ8;
    if (valueDbQ8 > volumeMaxDbQ8) valueDbQ8 = volumeMaxDbQ8;

    int16_t v = (int16_t)valueDbQ8;
    uint8_t data[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };

    const uint16_t wValue = (UAC_FU_VOLUME_CONTROL << 8) | 0;
    const uint16_t wIndex = (uint16_t)((volumeFuUnitId << 8) | acInterfaceNum);

    int rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        UAC_SET_CUR, wValue, wIndex, data, sizeof(data), 1000);
    if (rc < 0) {
        LOGE("setHwVolumeDbQ8(%d) failed: %s", valueDbQ8, libusb_error_name(rc));
        return false;
    }
    return true;
}

bool UsbAudioDriver::setHwMute(bool muted) {
    if (!handle || volumeFuUnitId < 0 || !hasFuMute) return false;
    uint8_t data[1] = { (uint8_t)(muted ? 1 : 0) };

    const uint16_t wValue = (UAC_FU_MUTE_CONTROL << 8) | 0;
    const uint16_t wIndex = (uint16_t)((volumeFuUnitId << 8) | acInterfaceNum);

    int rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        UAC_SET_CUR, wValue, wIndex, data, sizeof(data), 1000);
    if (rc < 0) {
        LOGE("setHwMute(%d) failed: %s", muted, libusb_error_name(rc));
        return false;
    }
    return true;
}

std::vector<int> UsbAudioDriver::queryUac2SampleRates(int clockId) {
    // UAC2 GET RANGE on CS_SAM_FREQ_CONTROL of the clock source unit.
    // Response: wNumSubRanges (2 bytes) followed by N * (dMIN, dMAX, dRES) of 4
    // bytes each (12 bytes per sub-range). All values little-endian.
    std::vector<int> result;
    if (!handle) return result;

    const uint8_t bmRequest = LIBUSB_ENDPOINT_IN
                            | LIBUSB_REQUEST_TYPE_CLASS
                            | LIBUSB_RECIPIENT_INTERFACE;
    const uint8_t bRequest = 0x02;  // RANGE
    const uint16_t wValue = (UAC2_CS_CONTROL_SAM_FREQ << 8);
    const uint16_t wIndex = (uint16_t)((clockId << 8) | acInterfaceNum);

    // Some Android USB stacks require the AC interface to be claimed before
    // accepting class-specific control transfers to it. Claim here, release
    // after; start() will re-claim later.
    bool tempClaim = false;
    if (acInterfaceNum >= 0 && !acInterfaceClaimed) {
        if (libusb_claim_interface(handle, acInterfaceNum) == 0) {
            tempClaim = true;
        }
    }

    // First read: 2-byte header to learn wNumSubRanges.
    uint8_t header[2] = {0, 0};
    int rc = libusb_control_transfer(handle, bmRequest, bRequest, wValue, wIndex,
                                     header, sizeof(header), 1000);
    if (rc < 2) {
        LOGI("UAC2 GET RANGE header read failed (rc=%d), falling back to hardcoded rates",
             rc);
        if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
        return result;
    }
    uint16_t numSubRanges = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
    if (numSubRanges == 0) {
        LOGI("UAC2 GET RANGE: zero sub-ranges reported");
        if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
        return result;
    }

    int totalLen = 2 + (int)numSubRanges * 12;
    std::vector<uint8_t> buf(totalLen, 0);
    rc = libusb_control_transfer(handle, bmRequest, bRequest, wValue, wIndex,
                                 buf.data(), (uint16_t)totalLen, 1000);
    if (rc < totalLen) {
        LOGI("UAC2 GET RANGE full read short (rc=%d expected=%d)", rc, totalLen);
        if (tempClaim) libusb_release_interface(handle, acInterfaceNum);
        return result;
    }

    LOGI("UAC2 Clock %d: %d sub-range(s)", clockId, (int)numSubRanges);
    const int commonRates[] = {44100, 48000, 88200, 96000, 176400, 192000,
                               352800, 384000, 705600, 768000};
    for (int i = 0; i < numSubRanges; i++) {
        int off = 2 + i * 12;
        uint32_t dMin = (uint32_t)buf[off]
                      | ((uint32_t)buf[off + 1] << 8)
                      | ((uint32_t)buf[off + 2] << 16)
                      | ((uint32_t)buf[off + 3] << 24);
        uint32_t dMax = (uint32_t)buf[off + 4]
                      | ((uint32_t)buf[off + 5] << 8)
                      | ((uint32_t)buf[off + 6] << 16)
                      | ((uint32_t)buf[off + 7] << 24);
        uint32_t dRes = (uint32_t)buf[off + 8]
                      | ((uint32_t)buf[off + 9] << 8)
                      | ((uint32_t)buf[off + 10] << 16)
                      | ((uint32_t)buf[off + 11] << 24);
        LOGI("  sub-range %d: min=%u max=%u res=%u", i, dMin, dMax, dRes);

        if (dMin == dMax) {
            // Singleton sub-range: a single discrete fixed rate.
            result.push_back((int)dMin);
        } else {
            // Range (continuous when dRes==0, stepped otherwise). In both
            // cases enumerate the standard audiophile rates that fall inside
            // [dMin, dMax]. We don't filter by dRes alignment because real
            // DACs step at 1 Hz or implement the standard rates directly.
            for (int r : commonRates) {
                if ((uint32_t)r >= dMin && (uint32_t)r <= dMax) {
                    result.push_back(r);
                }
            }
        }
    }

    if (tempClaim) libusb_release_interface(handle, acInterfaceNum);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool UsbAudioDriver::configure(int sampleRate, int channels, int bitDepth, bool preferDsd) {
    if (!opened) {
        LOGE("configure() called but not opened");
        return false;
    }

    // Stop-before-reconfigure guard
    if (streaming.load()) {
        LOGI("configure() called while streaming -- stopping first");
        stop();
    }

    LOGI("configure requested: rate=%d ch=%d bits=%d preferDsd=%d",
         sampleRate, channels, bitDepth, preferDsd ? 1 : 0);

    // Pass 1: exact match including DSD preference. When preferDsd is true we
    // only accept a DSD-flagged alt; when false we only accept a non-DSD alt.
    // Capture alts (ADC IN) are never candidates for the playback configure().
    UsbAudioFormat* best = nullptr;
    for (auto& f : formats) {
        if (f.isCapture) continue;
        if (f.sampleRate == sampleRate && f.channels == channels && f.bitDepth == bitDepth
                && f.isDsd == preferDsd) {
            best = &f;
            break;
        }
    }
    // Pass 2: exact match on (rate, channels, bits) ignoring DSD flag.
    if (!best) {
        for (auto& f : formats) {
            if (f.isCapture) continue;
            if (f.sampleRate == sampleRate && f.channels == channels && f.bitDepth == bitDepth) {
                best = &f;
                break;
            }
        }
    }
    // Relax: match rate, prefer highest bit depth
    if (!best) {
        for (auto& f : formats) {
            if (f.isCapture) continue;
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
    LOGI("Selected alt=%d (isDsd=%d) for rate=%d ch=%d bits=%d",
         best->altSetting, best->isDsd ? 1 : 0,
         best->sampleRate, best->channels, best->bitDepth);

    activeFormat = *best;
    configuredRate = sampleRate;
    configuredChannels = best->channels;
    configuredBitDepth = best->bitDepth;
    configuredSubslotSize = (best->subslotSize > 0)
            ? best->subslotSize : ((best->bitDepth + 7) / 8);
    configured = true;

    LOGI("Configured: rate=%d ch=%d bits=%d subslot=%d iface=%d alt=%d ep=0x%02x UAC%d",
         configuredRate, configuredChannels, configuredBitDepth, configuredSubslotSize,
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
    int bytesPerFrame = configuredSubslotSize * configuredChannels;
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
    int bytesPerFrame = configuredSubslotSize * configuredChannels;

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

    LOGI("Starting: iface=%d alt=%d ep=0x%02x rate=%d ch=%d bits=%d subslot=%d "
         "maxpkt=0x%04x(eff=%d) speed=%s fpp=%d bpp=%d UAC%d",
         activeFormat.interfaceNum, activeFormat.altSetting, activeFormat.endpointAddr,
         configuredRate, configuredChannels, configuredBitDepth, configuredSubslotSize,
         activeFormat.maxPacketSize, effectiveMaxPkt,
         isHighSpeed ? "High" : "Full",
         nominalFpp, bytesPerPacket, uacVersion);

    // Allocate ring buffer: 200ms of output audio (in wire-format bytes)
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

    // Set sample rate based on UAC version. Use the active format's clock
    // source so DAC+ADC devices don't accidentally set the capture clock when
    // we meant to set the playback clock.
    int csIdForStart = activeFormat.clockSourceId >= 0
            ? activeFormat.clockSourceId : clockSourceId;
    if (uacVersion >= 2 && csIdForStart >= 0) {
        if (!setSampleRateUAC2(csIdForStart, configuredRate)) {
            // DAC rejected the rate. Abort before shipping isochronous packets:
            // sending data at a rate the DAC isn't clocking for produces loud
            // noise on the analog output, which is worse than silence.
            LOGE("start(): aborting because the DAC rejected sample rate %d Hz",
                 configuredRate);
            stop();
            return false;
        }
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
    ensureEventThread();

    LOGI("USB audio streaming started (%d transfers, ring=%d bytes)", NUM_TRANSFERS, ringSize);
    return true;
}

// Event-thread refcount. The thread services libusb callbacks for BOTH the OUT
// pipeline and the capture pipeline; we share one thread per device so a single
// libusb context dispatches both directions. Each start() / startCapture() bumps
// the user count; each stop() / stopCapture() decrements it. The thread exits
// when the count hits zero.
void UsbAudioDriver::ensureEventThread() {
    int prev = eventThreadUsers.fetch_add(1, std::memory_order_acq_rel);
    if (prev > 0) {
        // Already running for another direction.
        return;
    }
    eventThread = std::thread([this]() {
        // THREAD_PRIORITY_URGENT_AUDIO equivalent (-19), no starvation risk
        if (setpriority(PRIO_PROCESS, 0, -19) != 0) {
            LOGI("setpriority(-19) failed: %s, using default", strerror(errno));
        } else {
            LOGI("Event thread: nice=-19 (urgent audio)");
        }

        LOGI("Event thread started");
        while (eventThreadUsers.load(std::memory_order_acquire) > 0) {
            struct timeval tv = {0, 10000}; // 10ms timeout
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
        LOGI("Event thread exited");
    });
}

void UsbAudioDriver::releaseEventThread() {
    int prev = eventThreadUsers.fetch_sub(1, std::memory_order_acq_rel);
    if (prev != 1) {
        // Other direction still streaming -- thread stays up.
        return;
    }
    if (eventThread.joinable()) {
        eventThread.join();
    }
}

int UsbAudioDriver::write(const uint8_t* data, int length) {
    if (!streaming.load() || !ringBuffer) return -1;
    // Align write to wire-frame boundary to prevent sample misalignment.
    // Caller is responsible for producing data already padded to subslot width
    // (e.g., DoP packer pads 24-bit-in-32 with LSB padding when subslot != 3).
    int bytesPerFrame = configuredSubslotSize * configuredChannels;
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

    // subslotSize is the on-wire byte count per sample; bitDepth is the data range.
    // When subslot > bitDepth/8 (e.g., 24-bit-in-32-bit), unused bits are at the LSB
    // end per UAC2 spec, so the data value is left-aligned in the subslot.
    int subslotBytes = configuredSubslotSize;
    int padBits = subslotBytes * 8 - configuredBitDepth;
    if (padBits < 0) padBits = 0;

    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4]; // max 4 bytes per sample
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);

        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / subslotBytes);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        int outBytes = 0;

        const float gain = softwareGain.load(std::memory_order_relaxed);
        for (int i = 0; i < batch; i++) {
            float s = data[totalConsumed + i] * gain;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;

            int32_t v;
            switch (configuredBitDepth) {
                case 16: v = (int32_t)(s * 32767.0f);      break;
                case 24: v = (int32_t)(s * 8388607.0f);    break;
                case 32: v = (int32_t)(s * 2147483647.0f); break;
                default: v = (int32_t)(s * 32767.0f);      break;
            }
            int32_t wire = v << padBits;
            for (int b = 0; b < subslotBytes; b++) {
                convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / subslotBytes;
        totalConsumed += samplesWritten;

        if (samplesWritten < batch) break;
    }

    return totalConsumed;
}

int UsbAudioDriver::writeInt16(const int16_t* data, int numSamples) {
    if (!streaming.load() || !ringBuffer) return -1;

    // Upscale 16-bit data into the DAC's wire format. The data is left-aligned
    // within the subslot (per UAC2: padding at LSB end).
    int subslotBytes = configuredSubslotSize;
    int dataShift = (subslotBytes * 8) - 16;
    if (dataShift < 0) dataShift = 0;

    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4]; // max 4 bytes per sample
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);

        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / subslotBytes);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        int outBytes = 0;

        const float gain = softwareGain.load(std::memory_order_relaxed);
        for (int i = 0; i < batch; i++) {
            int16_t s = data[totalConsumed + i];
            int32_t scaled;
            if (gain >= 0.9999f) {
                scaled = (int32_t)s;
            } else {
                float fs = (float)s * gain;
                if (fs > 32767.0f) fs = 32767.0f;
                else if (fs < -32768.0f) fs = -32768.0f;
                scaled = (int32_t)fs;
            }
            int32_t wire = scaled << dataShift;
            for (int b = 0; b < subslotBytes; b++) {
                convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / subslotBytes;
        totalConsumed += samplesWritten;

        if (samplesWritten < batch) break;
    }

    return totalConsumed;
}

int UsbAudioDriver::writeInt24Packed(const uint8_t* data, int numBytes) {
    // 24-bit packed PCM: 3 bytes little-endian signed per sample. Sample-align
    // to the next multiple of 3 from the caller; this method assumes whole samples.
    if (!streaming.load() || !ringBuffer) return -1;
    int numSamples = numBytes / 3;
    if (numSamples <= 0) return 0;

    int subslotBytes = configuredSubslotSize;
    int padBits = subslotBytes * 8 - configuredBitDepth;
    if (padBits < 0) padBits = 0;

    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4];
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);
        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / subslotBytes);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        const float gain = softwareGain.load(std::memory_order_relaxed);
        int outBytes = 0;
        for (int i = 0; i < batch; i++) {
            int off = (totalConsumed + i) * 3;
            // Sign-extend 24-bit LE to int32: load 3 bytes into bits 8..31 then arithmetic shift down.
            int32_t v = ((int32_t)data[off] << 8)
                      | ((int32_t)data[off + 1] << 16)
                      | ((int32_t)data[off + 2] << 24);
            v >>= 8;  // arithmetic shift, preserves sign

            if (gain < 0.9999f) {
                float fs = (float)v * gain;
                if (fs > 8388607.0f) fs = 8388607.0f;
                else if (fs < -8388608.0f) fs = -8388608.0f;
                v = (int32_t)fs;
            }

            // The value is 24-bit; left-align inside the DAC subslot.
            int dataShift = (subslotBytes * 8) - 24;
            if (dataShift < 0) dataShift = 0;
            int32_t wire = v << dataShift;
            for (int b = 0; b < subslotBytes; b++) {
                convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / subslotBytes;
        totalConsumed += samplesWritten;
        if (samplesWritten < batch) break;
    }
    return totalConsumed;
}

int UsbAudioDriver::writeInt32(const int32_t* data, int numSamples) {
    if (!streaming.load() || !ringBuffer) return -1;
    if (numSamples <= 0) return 0;

    int subslotBytes = configuredSubslotSize;
    // 32-bit data is already full-range; no left-align padding needed for 32-bit DAC.
    // For 24-bit DAC reception of 32-bit source, we lose 8 LSBs by simple truncation.
    int dataShift = (subslotBytes * 8) - 32;
    if (dataShift < 0) dataShift = 0;

    const int CHUNK = 512;
    uint8_t convBuf[CHUNK * 4];
    int totalConsumed = 0;

    while (totalConsumed < numSamples) {
        int batch = std::min(CHUNK, numSamples - totalConsumed);
        size_t space = ringBuffer->getFreeSpace();
        int fitSamples = (int)(space / subslotBytes);
        if (fitSamples <= 0) break;
        batch = std::min(batch, fitSamples);

        const float gain = softwareGain.load(std::memory_order_relaxed);
        int outBytes = 0;
        for (int i = 0; i < batch; i++) {
            int32_t v = data[totalConsumed + i];
            if (gain < 0.9999f) {
                // float has 24-bit mantissa -- enough for typical attenuation work.
                float fs = (float)v * gain;
                if (fs > 2147483520.0f) fs = 2147483520.0f;
                else if (fs < -2147483648.0f) fs = -2147483648.0f;
                v = (int32_t)fs;
            }
            int32_t wire = v << dataShift;
            for (int b = 0; b < subslotBytes; b++) {
                convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
            }
        }

        int written = (int)ringBuffer->write(convBuf, outBytes);
        int samplesWritten = written / subslotBytes;
        totalConsumed += samplesWritten;
        if (samplesWritten < batch) break;
    }
    return totalConsumed;
}

void UsbAudioDriver::flush() {
    if (ringBuffer) {
        ringBuffer->clear();
    }
}

void UsbAudioDriver::stop() {
    if (!streaming.load() && eventThreadUsers.load() == 0) {
        // Already stopped -- avoid double-release of the event thread refcount.
        return;
    }
    bool wasStreaming = streaming.exchange(false);

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
    // AC interface is shared with the capture pipeline -- keep it claimed while
    // capture is still streaming. close() will release it as a last step.
    if (handle && acInterfaceClaimed && !capStreaming.load()) {
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

    if (wasStreaming) {
        releaseEventThread();
    }

    LOGI("USB audio streaming stopped");
}

// =====================================================================
//  Capture pipeline (ADC -> host). Mirrors the OUT pipeline in reverse.
// =====================================================================

bool UsbAudioDriver::hasCaptureFormats() const {
    for (auto& f : formats) {
        if (f.isCapture) return true;
    }
    return false;
}

bool UsbAudioDriver::configureCapture(int sampleRate, int channels, int bitDepth) {
    if (!opened) {
        LOGE("configureCapture() called but not opened");
        return false;
    }
    if (capStreaming.load()) {
        LOGI("configureCapture() called while capturing -- stopping first");
        stopCapture();
    }

    LOGI("configureCapture requested: rate=%d ch=%d bits=%d", sampleRate, channels, bitDepth);

    // Two-pass match on capture-only alts.
    UsbAudioFormat* best = nullptr;
    for (auto& f : formats) {
        if (!f.isCapture) continue;
        if (f.sampleRate == sampleRate && f.channels == channels && f.bitDepth == bitDepth) {
            best = &f;
            break;
        }
    }
    if (!best) {
        for (auto& f : formats) {
            if (!f.isCapture) continue;
            if (f.sampleRate == sampleRate && f.channels == channels) {
                if (!best || f.bitDepth > best->bitDepth) best = &f;
            }
        }
    }
    if (!best) {
        for (auto& f : formats) {
            if (!f.isCapture) continue;
            if (f.sampleRate == sampleRate) {
                if (!best || f.bitDepth > best->bitDepth) best = &f;
            }
        }
    }
    if (!best) {
        LOGE("No matching capture format for rate=%d ch=%d bits=%d",
             sampleRate, channels, bitDepth);
        return false;
    }

    // Shared-clock devices: if playback is running on the same clock, the rate
    // is already pinned -- refuse a mismatched capture rate rather than yank
    // the playback's clock out from under it.
    if (streaming.load() && best->clockSourceId >= 0
            && best->clockSourceId == activeFormat.clockSourceId
            && sampleRate != configuredRate) {
        LOGE("shared clock between DAC and ADC; capture must use playback rate %d Hz",
             configuredRate);
        return false;
    }

    capActiveFormat = *best;
    capRate = sampleRate;
    capChannels = best->channels;
    capBitDepth = best->bitDepth;
    capSubslotSize = (best->subslotSize > 0) ? best->subslotSize : ((best->bitDepth + 7) / 8);
    capConfigured = true;

    LOGI("Configured capture: rate=%d ch=%d bits=%d subslot=%d iface=%d alt=%d ep=0x%02x clk=%d",
         capRate, capChannels, capBitDepth, capSubslotSize,
         capActiveFormat.interfaceNum, capActiveFormat.altSetting,
         capActiveFormat.endpointAddr, capActiveFormat.clockSourceId);
    return true;
}

void UsbAudioDriver::captureCallback(struct libusb_transfer* transfer) {
    auto* driver = static_cast<UsbAudioDriver*>(transfer->user_data);
    driver->handleCaptureComplete(transfer);
}

void UsbAudioDriver::handleCaptureComplete(struct libusb_transfer* transfer) {
    if (!capStreaming.load()) {
        capActiveTransfers--;
        return;
    }

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
        transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
        LOGE("Capture transfer status=%d (%s)", transfer->status,
             transfer->status == LIBUSB_TRANSFER_ERROR ? "ERROR" :
             transfer->status == LIBUSB_TRANSFER_STALL ? "STALL" :
             transfer->status == LIBUSB_TRANSFER_NO_DEVICE ? "NO_DEVICE" :
             transfer->status == LIBUSB_TRANSFER_OVERFLOW ? "OVERFLOW" :
             transfer->status == LIBUSB_TRANSFER_CANCELLED ? "CANCELLED" : "UNKNOWN");
        int remaining = --capActiveTransfers;
        if (remaining <= 0) {
            LOGE("All capture transfers failed -- stopping capture");
            capStreaming.store(false);
        }
        return;
    }

    // Iso IN packets are scattered at uniform stride. We set every packet's
    // length to capEffectiveMaxPkt at submit, so packet p starts at
    // buffer + p * capEffectiveMaxPkt; only the first actual_length bytes are
    // valid PCM. If the ring buffer is full (Java reader is too slow) we drop
    // the excess silently -- capture is producer-paced and we cannot stall
    // the USB bus.
    for (int p = 0; p < transfer->num_iso_packets; p++) {
        auto& pkt = transfer->iso_packet_desc[p];
        if (pkt.status != LIBUSB_TRANSFER_COMPLETED) continue;
        int len = pkt.actual_length;
        if (len <= 0) continue;
        uint8_t* src = transfer->buffer + (capEffectiveMaxPkt * p);
        if (capRingBuffer) capRingBuffer->write(src, len);
    }

    int idx = -1;
    for (int i = 0; i < CAP_NUM_TRANSFERS; i++) {
        if (capTransfers[i] == transfer) { idx = i; break; }
    }
    if (idx < 0) {
        int remaining = --capActiveTransfers;
        if (remaining <= 0) {
            LOGE("All capture transfers lost -- stopping");
            capStreaming.store(false);
        }
        return;
    }

    submitCaptureTransfer(idx);
}

void UsbAudioDriver::submitCaptureTransfer(int index) {
    if (!capStreaming.load()) return;
    libusb_transfer* xfr = capTransfers[index];

    // Re-arm each iso packet to the full advertised size; iso IN packets are
    // device-paced and only the actual_length on completion tells us how much
    // arrived. No data fill -- the previous callback already drained it.
    for (int p = 0; p < CAP_PACKETS_PER_TRANSFER; p++) {
        xfr->iso_packet_desc[p].length = capEffectiveMaxPkt;
        xfr->iso_packet_desc[p].actual_length = 0;
    }
    xfr->length = capEffectiveMaxPkt * CAP_PACKETS_PER_TRANSFER;

    int rc = libusb_submit_transfer(xfr);
    if (rc < 0) {
        LOGE("capture resubmit_transfer failed: %s", libusb_error_name(rc));
        int remaining = --capActiveTransfers;
        if (remaining <= 0) {
            LOGE("All capture transfers lost on resubmit -- stopping");
            capStreaming.store(false);
        }
    }
}

bool UsbAudioDriver::startCapture() {
    if (!capConfigured || !handle) {
        LOGE("startCapture() called but configured=%d handle=%p", capConfigured, handle);
        return false;
    }
    if (capStreaming.load()) {
        LOGI("startCapture() already running");
        return true;
    }

    int basePktSize = capActiveFormat.maxPacketSize & 0x7FF;
    int mult = ((capActiveFormat.maxPacketSize >> 11) & 0x03) + 1;
    capEffectiveMaxPkt = basePktSize * mult;

    bool isHighSpeed = (usbSpeed >= LIBUSB_SPEED_HIGH);
    int bytesPerFrame = capSubslotSize * capChannels;
    int nominalFpp = isHighSpeed ? (capRate / 8000) : (capRate / 1000);
    int nominalBytesPerPacket = nominalFpp * bytesPerFrame;

    LOGI("Starting capture: iface=%d alt=%d ep=0x%02x rate=%d ch=%d bits=%d subslot=%d "
         "maxpkt=0x%04x(eff=%d) speed=%s fpp=%d bpp=%d UAC%d",
         capActiveFormat.interfaceNum, capActiveFormat.altSetting, capActiveFormat.endpointAddr,
         capRate, capChannels, capBitDepth, capSubslotSize,
         capActiveFormat.maxPacketSize, capEffectiveMaxPkt,
         isHighSpeed ? "High" : "Full", nominalFpp, nominalBytesPerPacket, uacVersion);

    // Capture ring buffer: 200ms of capture audio (in wire-format bytes).
    int ringSize = capRate * bytesPerFrame / 5;
    if (ringSize < 65536) ringSize = 65536;
    delete capRingBuffer;
    capRingBuffer = new RingBuffer(ringSize);
    if (mlock(capRingBuffer->getBuffer(), capRingBuffer->getCapacity()) != 0) {
        LOGI("mlock capture ring buffer failed: %s (non-fatal)", strerror(errno));
    }

    int rc;
    // Detach kernel driver from capture AS (and AC, if not already done by OUT).
    if (acInterfaceNum >= 0 && !acInterfaceClaimed) {
        rc = libusb_detach_kernel_driver(handle, acInterfaceNum);
        LOGI("capture detach_kernel_driver(AC iface %d): %s", acInterfaceNum,
             rc == 0 ? "OK" : libusb_error_name(rc));
    }
    rc = libusb_detach_kernel_driver(handle, capActiveFormat.interfaceNum);
    LOGI("capture detach_kernel_driver(AS iface %d): %s", capActiveFormat.interfaceNum,
         rc == 0 ? "OK" : libusb_error_name(rc));

    // Claim AC interface (shared with OUT). Idempotent: skip if already held.
    if (acInterfaceNum >= 0 && !acInterfaceClaimed) {
        rc = libusb_claim_interface(handle, acInterfaceNum);
        if (rc < 0) {
            LOGI("capture claim_interface(AC %d) failed: %s (non-fatal)",
                 acInterfaceNum, libusb_error_name(rc));
        } else {
            acInterfaceClaimed = true;
        }
    }

    rc = libusb_claim_interface(handle, capActiveFormat.interfaceNum);
    if (rc < 0) {
        LOGE("capture claim_interface(%d) failed: %s",
             capActiveFormat.interfaceNum, libusb_error_name(rc));
        stopCapture();
        return false;
    }
    capInterfaceClaimed = true;

    libusb_set_interface_alt_setting(handle, capActiveFormat.interfaceNum, 0);
    if (!setInterfaceAltSetting(capActiveFormat.interfaceNum, capActiveFormat.altSetting)) {
        stopCapture();
        return false;
    }

    // Sample rate. On UAC2 we drive the ADC's clock source; on shared-clock
    // devices the rate was already pinned by playback and configureCapture
    // would have rejected a mismatch.
    int capClockId = capActiveFormat.clockSourceId >= 0
            ? capActiveFormat.clockSourceId : clockSourceId;
    if (uacVersion >= 2 && capClockId >= 0) {
        bool sharedClockAlreadySet = streaming.load()
                && capClockId == activeFormat.clockSourceId;
        if (!sharedClockAlreadySet) {
            if (!setSampleRateUAC2(capClockId, capRate)) {
                LOGE("startCapture(): ADC rejected sample rate %d Hz", capRate);
                stopCapture();
                return false;
            }
        }
    } else {
        // UAC1 capture: best-effort endpoint SET_CUR. Some devices ignore it.
        setSampleRate(capActiveFormat.endpointAddr, capRate);
    }

    // Allocate transfers. Buffer size uses effectiveMaxPkt because each iso IN
    // packet may arrive at the max size; we cannot assume the nominal fpp.
    capTransferBufSize = capEffectiveMaxPkt * CAP_PACKETS_PER_TRANSFER;
    for (int i = 0; i < CAP_NUM_TRANSFERS; i++) {
        capTransfers[i] = libusb_alloc_transfer(CAP_PACKETS_PER_TRANSFER);
        if (!capTransfers[i]) {
            LOGE("capture alloc_transfer failed");
            stopCapture();
            return false;
        }
        capTransferBuffers[i] = new uint8_t[capTransferBufSize];
        memset(capTransferBuffers[i], 0, capTransferBufSize);
        if (mlock(capTransferBuffers[i], capTransferBufSize) != 0) {
            LOGI("mlock capture transfer[%d] failed: %s (non-fatal)", i, strerror(errno));
        }

        libusb_fill_iso_transfer(capTransfers[i], handle,
            capActiveFormat.endpointAddr,
            capTransferBuffers[i], capTransferBufSize,
            CAP_PACKETS_PER_TRANSFER,
            captureCallback, this, 1000);
        libusb_set_iso_packet_lengths(capTransfers[i], capEffectiveMaxPkt);
    }

    capStreaming.store(true);
    capActiveTransfers.store(CAP_NUM_TRANSFERS);

    for (int i = 0; i < CAP_NUM_TRANSFERS; i++) {
        rc = libusb_submit_transfer(capTransfers[i]);
        if (rc < 0) {
            LOGE("capture submit_transfer[%d] failed: %s", i, libusb_error_name(rc));
            capActiveTransfers--;
        }
    }
    if (capActiveTransfers.load() <= 0) {
        LOGE("All capture transfers failed to submit");
        capStreaming.store(false);
        stopCapture();
        return false;
    }
    LOGI("%d/%d capture transfers submitted", capActiveTransfers.load(), CAP_NUM_TRANSFERS);

    ensureEventThread();

    LOGI("USB audio capture started (%d transfers, ring=%d bytes)",
         CAP_NUM_TRANSFERS, ringSize);
    return true;
}

int UsbAudioDriver::readCapture(uint8_t* out, int maxBytes) {
    if (!capStreaming.load() || !capRingBuffer) return -1;
    int bytesPerFrame = capSubslotSize * capChannels;
    if (bytesPerFrame > 0) {
        int aligned = (maxBytes / bytesPerFrame) * bytesPerFrame;
        if (aligned <= 0) return 0;
        maxBytes = aligned;
    }
    return (int)capRingBuffer->read(out, maxBytes);
}

void UsbAudioDriver::stopCapture() {
    if (!capStreaming.load() && capActiveTransfers.load() == 0
            && !capInterfaceClaimed && capTransferBuffers[0] == nullptr) {
        // Already torn down -- avoid double-release of the event thread refcount.
        return;
    }
    bool wasStreaming = capStreaming.exchange(false);

    if (ctx) {
        for (int retry = 0; retry < 50 && capActiveTransfers.load() > 0; retry++) {
            struct timeval tv = {0, 10000};
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
        for (int i = 0; i < CAP_NUM_TRANSFERS; i++) {
            if (capTransfers[i]) libusb_cancel_transfer(capTransfers[i]);
        }
        for (int retry = 0; retry < 30 && capActiveTransfers.load() > 0; retry++) {
            struct timeval tv = {0, 5000};
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
    }

    for (int i = 0; i < CAP_NUM_TRANSFERS; i++) {
        if (capTransfers[i]) {
            libusb_free_transfer(capTransfers[i]);
            capTransfers[i] = nullptr;
        }
        if (capTransferBuffers[i]) {
            if (capTransferBufSize > 0) {
                munlock(capTransferBuffers[i], capTransferBufSize);
            }
            delete[] capTransferBuffers[i];
            capTransferBuffers[i] = nullptr;
        }
    }
    capTransferBufSize = 0;

    if (handle && capInterfaceClaimed) {
        libusb_set_interface_alt_setting(handle, capActiveFormat.interfaceNum, 0);
        libusb_release_interface(handle, capActiveFormat.interfaceNum);
        capInterfaceClaimed = false;
    }
    // AC interface is shared with playback -- keep it claimed while OUT runs.
    if (handle && acInterfaceClaimed && !streaming.load()) {
        libusb_release_interface(handle, acInterfaceNum);
        acInterfaceClaimed = false;
    }

    if (capRingBuffer) {
        munlock(capRingBuffer->getBuffer(), capRingBuffer->getCapacity());
        delete capRingBuffer;
        capRingBuffer = nullptr;
    }

    capActiveTransfers.store(0);

    if (wasStreaming) {
        releaseEventThread();
    }

    LOGI("USB audio capture stopped");
}

void UsbAudioDriver::close() {
    LOGI("close() called, streaming=%d capStreaming=%d opened=%d",
         streaming.load() ? 1 : 0, capStreaming.load() ? 1 : 0, opened);

    if (capStreaming.load()) {
        stopCapture();
    }
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
    configuredSubslotSize = 0;
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
