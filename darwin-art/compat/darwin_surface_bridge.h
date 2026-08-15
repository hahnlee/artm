#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque owner of one persistent NSWindow, CAMetalLayer, IOSurface, and Metal
// texture. All API calls currently require the macOS main thread.
typedef struct DarwinArtSurface DarwinArtSurface;

typedef enum DarwinArtSurfaceResult {
  DARWIN_ART_SURFACE_OK = 0,
  DARWIN_ART_SURFACE_INVALID_ARGUMENT = 1,
  DARWIN_ART_SURFACE_NOT_MAIN_THREAD = 2,
  DARWIN_ART_SURFACE_ALLOCATION_FAILED = 3,
  DARWIN_ART_SURFACE_METAL_UNAVAILABLE = 4,
  DARWIN_ART_SURFACE_DRAWABLE_UNAVAILABLE = 5,
  DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED = 6,
  DARWIN_ART_SURFACE_WINDOW_CLOSED = 7,
  DARWIN_ART_SURFACE_PRODUCER_ALREADY_MAPPED = 8,
  DARWIN_ART_SURFACE_PRODUCER_NOT_MAPPED = 9,
} DarwinArtSurfaceResult;

typedef struct DarwinArtSurfaceCreateInfo {
  uint32_t width;
  uint32_t height;
  // UTF-8, copied during creation. Null selects a default title.
  const char* title;
  // When false, the persistent window is created but not ordered onscreen.
  // Headless build and ABI tests should set this to false.
  bool visible;
} DarwinArtSurfaceCreateInfo;

typedef struct DarwinArtSurfaceProducerMapping {
  // Stable IOSurface CPU mapping. Valid only between a successful producer
  // map and its matching unmap.
  void* base_address;
  size_t bytes_per_row;
  size_t allocation_size;
  uint32_t width;
  uint32_t height;
} DarwinArtSurfaceProducerMapping;

// Creates persistent Apple graphics objects. The returned handle owns them
// until darwin_art_surface_destroy(). Returns null on failure and writes the
// reason to out_result when it is non-null.
DarwinArtSurface* darwin_art_surface_create(
    const DarwinArtSurfaceCreateInfo* create_info,
    DarwinArtSurfaceResult* out_result);

// Waits for the previous GPU presentation to finish, then locks the IOSurface
// for a CPU producer. Only one producer mapping may be active per surface.
DarwinArtSurfaceResult darwin_art_surface_map_producer(
    DarwinArtSurface* surface,
    DarwinArtSurfaceProducerMapping* out_mapping);

// Flush producer writes to the IOSurface and ends the active CPU mapping.
DarwinArtSurfaceResult darwin_art_surface_unmap_producer(
    DarwinArtSurface* surface);

// Copies one BGRA8-premultiplied CPU frame into the already allocated
// IOSurface. source_bytes_per_row may exceed width * 4 but may not be smaller.
// This API is intentionally independent of Java arrays. A future Skia bridge
// should prefer map_producer()/unmap_producer() to avoid this staging copy.
DarwinArtSurfaceResult darwin_art_surface_update(
    DarwinArtSurface* surface,
    const void* bgra_pixels,
    size_t source_bytes_per_row);

// Blits the persistent IOSurface-backed Metal texture into the next drawable
// of the persistent CAMetalLayer and schedules it for presentation.
DarwinArtSurfaceResult darwin_art_surface_present(
    DarwinArtSurface* surface);

// Runs the NSApplication event pump in slices no longer than 16 ms. seconds
// must be finite and in the inclusive range 0..30. A zero duration performs
// only a close-state check. WINDOW_CLOSED means the user closed the window;
// the handle remains valid and must still be destroyed by its owner.
DarwinArtSurfaceResult darwin_art_surface_pump_events(
    DarwinArtSurface* surface,
    double seconds);

// Waits for the most recently submitted command buffer, closes the window,
// and releases all owned Apple objects.
DarwinArtSurfaceResult darwin_art_surface_destroy(
    DarwinArtSurface* surface);

#ifdef __cplusplus
}  // extern "C"
#endif
