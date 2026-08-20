#ifndef DARWIN_ART_DARWIN_ART_H_
#define DARWIN_ART_DARWIN_ART_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DARWIN_ART_ABI_VERSION 1u

// Process lifecycle errors are kept outside the existing run-stage values.
// Shutdown is intentionally not idempotent: a duplicate call is reported so
// the host cannot accidentally hide ownership bugs.
#define DARWIN_ART_STATUS_PROCESS_ALREADY_STARTED 66
#define DARWIN_ART_STATUS_SHUTDOWN_NOT_READY 67
#define DARWIN_ART_STATUS_SHUTDOWN_WRONG_THREAD 68
#define DARWIN_ART_STATUS_SHUTDOWN_ALREADY_COMPLETED 69
#define DARWIN_ART_STATUS_SHUTDOWN_FAILED 70

#if defined(__GNUC__)
#define DARWIN_ART_EXPORT __attribute__((visibility("default")))
#else
#define DARWIN_ART_EXPORT
#endif

// The frame memory is borrowed only for the duration of this callback. The
// host must copy it or retain a separately negotiated surface before returning.
typedef int32_t (*darwin_art_frame_callback_t)(
    void* context,
    const uint32_t* argb_pixels,
    uint32_t width,
    uint32_t height,
    size_t stride_bytes);

// Optional Rust-owned provider lifecycle hooks.  When both callbacks are
// present, provider activation/deactivation is delegated to the host owner;
// when absent, the engine uses its built-in compatibility owner.
typedef int32_t (*darwin_art_provider_acquire_t)(
    void* context, uint32_t provider_kind, int32_t authority_fd);
typedef int32_t (*darwin_art_provider_release_t)(
    void* context, uint32_t provider_kind);

typedef struct darwin_art_process_config {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* core_oj_jar;
  const char* core_libart_jar;
  const char* framework_jar;
  const char* core_icu4j_jar;
  const char* app_dex;
  uint64_t heap_initial_bytes;
  uint64_t heap_maximum_bytes;
  void* host_context;
  darwin_art_frame_callback_t frame_callback;
  void* provider_context;
  darwin_art_provider_acquire_t provider_acquire;
  darwin_art_provider_release_t provider_release;
} darwin_art_process_config_t;

typedef struct darwin_art_process_result {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t hello_answer;
  int32_t native_round_trip;
  int32_t arraycopy_result;
  int32_t activity_probe_result;
  int32_t lifecycle_result;
  uint32_t frame_width;
  uint32_t frame_height;
} darwin_art_process_result_t;

// Runs exactly one ART instance in the current process. Version 1 deliberately
// exposes a one-shot operation: Runtime::Create is process-global, and the
// existing Darwin port does not yet promise safe re-creation after shutdown.
// A return value of zero is success; nonzero values are stable process-run
// stages and are also diagnosed on stderr.
DARWIN_ART_EXPORT int32_t darwin_art_run_process(
    const darwin_art_process_config_t* config,
    darwin_art_process_result_t* result);

// Dispatches one host pointer sample into the retained Android root view and
// emits a refreshed frame through frame_callback. Actions are 0=down, 1=up,
// and 2=move. This first input ABI supports generic clickable View semantics;
// full MotionEvent/native input semantics remain a later subsystem boundary.
DARWIN_ART_EXPORT int32_t darwin_art_dispatch_pointer(
    uint32_t action,
    float x,
    float y);

// Tears down the Runtime created by darwin_art_run_process. Once Runtime::Create
// succeeded, the host must call this exactly once after run_process returns,
// including when run_process returned a nonzero stage. It must be called on the
// same native thread; frame callbacks must return before shutdown begins.
//
// The function deletes outstanding process global references, destroys ART,
// and only then releases the registered application DexFile storage. A zero
// return is success. NOT_READY means no live Runtime exists (including a create
// failure); WRONG_THREAD leaves it available for the owning thread; an
// ALREADY_COMPLETED result identifies a duplicate shutdown call.
DARWIN_ART_EXPORT int32_t darwin_art_shutdown_process(void);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // DARWIN_ART_DARWIN_ART_H_
