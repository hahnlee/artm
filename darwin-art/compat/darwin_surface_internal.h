#pragma once

#include "darwin_surface_bridge.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstddef>
#include <atomic>
#include <mutex>

struct DarwinArtSurface;

@interface DarwinArtMetalView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                    pixelSize:(CGSize)pixelSize
                 contentScale:(CGFloat)contentScale;
- (BOOL)nextPointerEvent:(DarwinArtPointerEvent*)outEvent;
- (BOOL)nextPointerEventV2:(DarwinArtPointerEventV2*)outEvent;
- (BOOL)nextKeyEventV1:(DarwinArtKeyEventV1*)outEvent;
- (BOOL)clearInputHintIfEmpty;
- (void)cancelPointerStream;
- (void)updateDrawableSize;
- (void)setOwnerSurface:(DarwinArtSurface*)surface;
- (void)signalOwnerWake;
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
  // Direct RenderThread GPU submissions are produced off the AppKit actor;
  // keep their completion handle separate from the main-actor IOSurface blit.
  id<MTLCommandBuffer> last_gpu_command_buffer = nil;
  mutable std::mutex presentation_mutex;
  bool presentation_closing = false;
  bool presentation_scheduled = false;
  uint64_t presentation_requested = 0;
  std::atomic<int32_t> last_scanout_status{DARWIN_ART_SURFACE_OK};
  // Worker-thread frame pulses request scanout through a single latest-wins
  // AppKit command. The generation/mutex pair bounds the main-queue backlog
  // to one block while guaranteeing that a request arriving during a blit
  // schedules one trailing main turn instead of being lost.
  NSWindow* window = nil;
  id window_delegate = nil;
  DarwinArtMetalView* view = nil;
  bool visible = false;
  // Updated only by AppKit callbacks and read by the ART owner thread. This
  // lets the owner observe a user close without synchronously entering the
  // AppKit main queue (the main actor already owns event pumping).
  std::atomic<bool> window_closed{false};
  // AppKit input wakes the Android owner Looper through this POD hook. The
  // callback is copied under the mutex and invoked after releasing it.
  mutable std::mutex owner_wake_mutex;
  DarwinArtSurfaceOwnerWakeCallback owner_wake_callback = nullptr;
  void* owner_wake_context = nullptr;
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
