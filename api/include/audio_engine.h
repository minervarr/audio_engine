/*
 * audio_engine.h — the audio engine's PUBLIC PRODUCT: a stable C ABI.
 *
 * This is the one surface every host application talks to, on every platform
 * (Linux, Android, Windows). It is C (extern "C"), not C++, on purpose:
 *
 *   - ABI-stable across compilers and toolchain versions (C++ ABIs are not).
 *   - Bindable from anything: JNI (Android), P/Invoke, other languages, and
 *     trivially machine-generated glue.
 *
 * The engine itself is 100% modern C++ INSIDE (see core/ and backends/); only
 * this ~thin boundary is C. Design rules for this header (AI-friendly first):
 *   - Opaque handle (ae_engine*); no struct layout is exposed.
 *   - Every call returns an ae_result int code; ae_result_str() explains it.
 *   - Explicit units in names (_hz, _ms, _db_q8, _bytes). No ambiguous ints.
 *   - No overloading, no hidden global state, no callbacks that allocate.
 *
 * STATUS: contract sketch for Phase 2. Not yet implemented or built — the
 * ae::Engine C++ orchestrator (core/) and this wrapper (api/src) land during
 * the Android native-port phase. Declarations here define the target shape.
 */
#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine handle. Create with ae_engine_create, free with ae_engine_destroy. */
typedef struct ae_engine ae_engine;

/* Every fallible call returns one of these. Non-zero is failure. */
typedef enum ae_result {
    AE_OK = 0,
    AE_ERR_UNKNOWN = 1,
    AE_ERR_INVALID_ARG = 2,
    AE_ERR_UNSUPPORTED = 3,
    AE_ERR_DEVICE = 4,
    AE_ERR_DECODE = 5,
    AE_ERR_STATE = 6
} ae_result;

/* Human-readable, static string for a result code. Never NULL. */
const char* ae_result_str(ae_result code);

/* ABI version of this header, so a host can detect a mismatch at load time. */
uint32_t ae_abi_version(void);

/* --- engine lifecycle ---------------------------------------------------- */

/* Playback state, mirrors ae::Engine::State. */
typedef enum ae_state {
    AE_STATE_IDLE     = 0,
    AE_STATE_PREPARED = 1,
    AE_STATE_PLAYING  = 2,
    AE_STATE_PAUSED   = 3,
    AE_STATE_STOPPED  = 4
} ae_state;

/*
 * Event callbacks. They fire from the engine's output thread, so keep them
 * short and non-blocking; `user` is the pointer passed to ae_engine_set_callbacks.
 */
typedef void (*ae_prepared_cb)(void* user);
typedef void (*ae_completion_cb)(void* user);
typedef void (*ae_error_cb)(int result_code, void* user);
/* Fires when a gapless successor becomes the active track. */
typedef void (*ae_transition_cb)(void* user);

/*
 * Create an engine with the platform's default on-device output (Android: the
 * AAudio speaker path). Returns NULL on allocation/backend failure. Free with
 * ae_engine_destroy.
 */
ae_engine* ae_engine_create(void);

/* Stop playback (if any) and free the engine. NULL is ignored. */
void ae_engine_destroy(ae_engine* engine);

/* Register event callbacks. Pass NULL for any you don't want. */
void ae_engine_set_callbacks(ae_engine* engine,
                             ae_prepared_cb on_prepared,
                             ae_completion_cb on_completion,
                             ae_error_cb on_error,
                             void* user);

/* Register the gapless-transition callback (uses the same `user` as above). */
void ae_engine_set_transition_cb(ae_engine* engine, ae_transition_cb on_transition);

/* --- transport ----------------------------------------------------------- */

/*
 * Decode and play the audio at file descriptor `fd`, region [offset, offset+length).
 * Pass offset=0, length=-1 for the whole file. The engine dup()s the fd, so the
 * caller may close it after this returns. Stops any current playback first.
 */
ae_result ae_engine_play_fd(ae_engine* engine, int fd, int64_t offset, int64_t length);

/*
 * Decode and play a DSD file (.dff / .dsf) on `fd` (a whole-file descriptor).
 * DSD is USB-DAC only: switch the output to a USB DAC (ae_engine_switch_to_usb_fd)
 * first. The engine emits DoP (DSD-over-PCM), which any DSD-capable DAC accepts;
 * EQ and software volume are bypassed (a 1-bit signal can't be filtered/scaled).
 */
ae_result ae_engine_play_dsd_fd(ae_engine* engine, int fd, int64_t offset, int64_t length);

ae_result ae_engine_pause(ae_engine* engine);
ae_result ae_engine_resume(ae_engine* engine);
ae_result ae_engine_stop(ae_engine* engine);

/*
 * Queue a successor for gapless playback. Same fd contract as ae_engine_play_fd
 * (the engine dup()s it). When the current track ends, a format-matching
 * successor takes over with no gap (encoder padding/delay trimmed) and fires the
 * transition callback; a mismatched one is ignored and the track completes.
 */
ae_result ae_engine_enqueue_next_fd(ae_engine* engine, int fd, int64_t offset, int64_t length);

/* Seek the current track. Flushes the pipeline and resumes at position_ms. */
ae_result ae_engine_seek_ms(ae_engine* engine, int64_t position_ms);

/* Total duration of the current track in ms, or -1 if unknown / not playing. */
int64_t ae_engine_duration_ms(ae_engine* engine);

/* Current state, or AE_STATE_IDLE if engine is NULL. */
ae_state ae_engine_state(ae_engine* engine);

/* Playback position in milliseconds, or 0 if not playing / engine is NULL. */
int64_t ae_engine_position_ms(ae_engine* engine);

/* --- output routing ------------------------------------------------------ */

/*
 * Switch the live output to a direct-USB DAC opened on `fd`, with no gap: the
 * decode pipeline keeps running and only the drain target changes. `fd` is a
 * USB device file descriptor (the host app resolves USB permission and passes
 * the fd; the engine does NOT take ownership — keep it valid until you switch
 * away or stop). Volume state carries over to the new device.
 */
ae_result ae_engine_switch_to_usb_fd(ae_engine* engine, int fd);

/* Switch the live output back to the platform's on-device speaker (AAudio). */
ae_result ae_engine_switch_to_speaker(ae_engine* engine);

/* --- volume -------------------------------------------------------------- */

/* Volume control path. Applies only to the USB DAC sink; the speaker path
 * defers to the OS stream volume and ignores these. */
typedef enum ae_volume_mode {
    AE_VOLUME_AUTO     = 0,  /* hardware Feature Unit if present, else software */
    AE_VOLUME_HARDWARE = 1,  /* force UAC Feature Unit (greyed out if absent)   */
    AE_VOLUME_SOFTWARE = 2,  /* force software gain (PCM only)                  */
    AE_VOLUME_EXTERNAL = 3   /* DAC pinned to unity; downstream amp/knob rules  */
} ae_volume_mode;

/* Set the slider position, 0.0 (mute) .. 1.0 (unity / bit-perfect). */
ae_result ae_engine_set_volume(ae_engine* engine, float linear01);

/* Select the volume path (see ae_volume_mode). */
ae_result ae_engine_set_volume_mode(ae_engine* engine, ae_volume_mode mode);

/* 1 if the active output exposes a hardware volume (UAC Feature Unit), else 0. */
int ae_engine_has_hardware_volume(ae_engine* engine);

/* --- equalizer ----------------------------------------------------------- */

/*
 * Configure the parametric EQ applied to decoded PCM before the sink. `coeffs`
 * is num_filters * 5 doubles: [b0, b1, b2, a1, a2] per biquad (a0 normalized to
 * 1), computed by the host for the current sample rate. `preamp` is a linear
 * pre-gain. `enabled` (0/1) toggles the whole chain. Applied engine-wide,
 * independent of the active output. Safe to call live to retune.
 */
ae_result ae_engine_set_eq(ae_engine* engine, int num_filters,
                           const double* coeffs, double preamp, int enabled);

/* --- capture ------------------------------------------------------------- */
/*
 * On-device audio capture (Android: AAudio microphone input), independent of
 * playback. The host app owns the RECORD_AUDIO runtime permission. Lifecycle:
 * create -> configure -> start -> read* -> stop -> destroy. Emits 16-bit PCM.
 */
typedef struct ae_capture ae_capture;

ae_capture* ae_capture_create(void);
void        ae_capture_destroy(ae_capture* capture);

/* Negotiate the capture format; the device may adjust it (query is implicit —
 * read() delivers whatever the granted stream produces). */
ae_result ae_capture_configure(ae_capture* capture, int sample_rate_hz, int channels);
ae_result ae_capture_start(ae_capture* capture);

/* Drain up to max_bytes of interleaved 16-bit PCM. Returns bytes read, 0 if
 * nothing is buffered yet, or negative on failure. */
int ae_capture_read(ae_capture* capture, uint8_t* out, int max_bytes);
ae_result ae_capture_stop(ae_capture* capture);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AUDIO_ENGINE_H */
