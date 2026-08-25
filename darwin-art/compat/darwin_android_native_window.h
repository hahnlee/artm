#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// Stable software-decoder frame published by Android's ANativeWindow API.
// The pixel allocation remains owned until release_frame is called, allowing
// Skia/Metal to upload directly from the guest decoder buffer without an
// intermediate CPU copy.
typedef struct DarwinArtAndroidNativeWindowFrame {
  const void* pixels;
  size_t size;
  uint32_t width;
  uint32_t height;
  uint32_t stride_pixels;
  int32_t format;
  uint64_t generation;
  void* owner;
} DarwinArtAndroidNativeWindowFrame;

bool darwin_art_android_ANativeWindow_acquire_frame(
    DarwinArtAndroidNativeWindowFrame* frame);
void darwin_art_android_ANativeWindow_release_frame(
    DarwinArtAndroidNativeWindowFrame* frame);

}
