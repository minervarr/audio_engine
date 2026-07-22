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

/*
 * NOTE: The transport surface (create/destroy, play_fd, pause, resume, seek_ms,
 * stop, gapless queue_fd, volume, EQ, capability queries, and the callback
 * registration for prepared/completion/error/transition) is specified during
 * the Phase 2 design and added here incrementally. Kept intentionally minimal
 * until that design is approved, so nothing here is speculative.
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AUDIO_ENGINE_H */
