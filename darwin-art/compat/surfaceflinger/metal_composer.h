#pragma once

#include <cstddef>
#include <cstdint>

struct DarwinArtMetalComposerLayer {
  uint32_t owner_process_id = 0;
  uint32_t layer_id = 0;
  uint32_t parent_owner_process_id = 0;
  uint32_t parent_id = 0;
  uint64_t what = 0;
  uint32_t flags = 0;
  uint32_t mask = 0;
  uint32_t transform = 0;
  // True when the native producer stored row zero at the bottom of its
  // IOSurface. This is buffer metadata, independent of layer/window names.
  bool producer_bottom_left = false;
  void* iosurface = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int32_t source_left = 0;
  int32_t source_top = 0;
  int32_t source_right = 0;
  int32_t source_bottom = 0;
  int32_t destination_left = 0;
  int32_t destination_top = 0;
  int32_t destination_right = 0;
  int32_t destination_bottom = 0;
  int32_t z = 0;
  float alpha = 1.0f;
};

// Darwin's HWC/Composer backend. Every layer and the display target remain
// IOSurface-backed Metal textures. The command buffer waits on the producer
// event in the GPU timeline and signals a retained completion event after the
// atomic display composition. The caller owns completion_event.
extern "C" bool darwin_art_metal_composer_compose(
    void* metal_device, void* target_iosurface, uint32_t target_width,
    uint32_t target_height, const DarwinArtMetalComposerLayer* layers,
    size_t layer_count, void* producer_event, uint64_t producer_value,
    void** completion_event, uint64_t* completion_value);
