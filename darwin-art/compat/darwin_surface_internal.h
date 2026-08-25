#pragma once

#include "darwin_surface_bridge.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstddef>
#include <atomic>
#include <mutex>

@interface DarwinArtMetalView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                    pixelSize:(CGSize)pixelSize
                 contentScale:(CGFloat)contentScale;
- (BOOL)nextPointerEvent:(DarwinArtPointerEvent*)outEvent;
- (BOOL)nextPointerEventV2:(DarwinArtPointerEventV2*)outEvent;
- (BOOL)nextKeyEventV1:(DarwinArtKeyEventV1*)outEvent;
- (void)cancelPointerStream;
- (void)updateDrawableSize;
@end

// Shared host-owned surface state. GPU-only state is deliberately opaque so
// this header can be compiled by the CPU surface bridge without any Skia
// dependency. The GPU bridge owns the object stored in gpu_state.
struct DarwinArtSurface {
  // Protects the IOSurface/Metal backing tuple while a native producer on a
  // separate Android GL thread refreshes its retained target during resize.
  mutable std::mutex backing_mutex;
  uint32_t width = 0;
  uint32_t height = 0;
  size_t bytes_per_row = 0;
  IOSurfaceRef io_surface = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> io_surface_texture = nil;
  id<MTLCommandBuffer> last_command_buffer = nil;
  NSWindow* window = nil;
  id window_delegate = nil;
  DarwinArtMetalView* view = nil;
  bool visible = false;
  bool producer_mapped = false;
  void* gpu_state = nullptr;
  // Geometry and publication state for a SurfaceView whose EGL producer
  // renders directly into io_surface.  The Android GL thread publishes only
  // after eglWaitGL(), while the HWUI/Metal thread samples the same texture.
  std::atomic<int32_t> embedded_surface_x{0};
  std::atomic<int32_t> embedded_surface_y{0};
  std::atomic<uint32_t> embedded_surface_width{0};
  std::atomic<uint32_t> embedded_surface_height{0};
  std::atomic<uint32_t> embedded_buffer_width{0};
  std::atomic<uint32_t> embedded_buffer_height{0};
  std::atomic<uint64_t> embedded_surface_frame{0};

  ~DarwinArtSurface();
};

extern DarwinArtSurface* g_active_gpu_surface;

// Weak no-op in the CPU bridge; the GPU bridge supplies the strong cleanup
// implementation when it is linked into the graphics runtime.
void darwin_art_surface_gpu_forget(DarwinArtSurface* surface);
