#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "darwin_surface_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace {

static_assert(sizeof(bool) == 1);
static_assert(sizeof(DarwinArtSurfaceResult) == 4);
static_assert(sizeof(DarwinArtSurfaceCreateInfo) == 24);
static_assert(offsetof(DarwinArtSurfaceCreateInfo, width) == 0);
static_assert(offsetof(DarwinArtSurfaceCreateInfo, height) == 4);
static_assert(offsetof(DarwinArtSurfaceCreateInfo, title) == 8);
static_assert(offsetof(DarwinArtSurfaceCreateInfo, visible) == 16);

constexpr uint32_t kBytesPerPixel = 4;
constexpr size_t kRowAlignment = 64;
constexpr uint32_t kMaximumDimension = 16384;
constexpr uint32_t kBgraPixelFormat =
    (static_cast<uint32_t>('B') << 24) |
    (static_cast<uint32_t>('G') << 16) |
    (static_cast<uint32_t>('R') << 8) |
    static_cast<uint32_t>('A');

bool IsMainThread() {
  return [NSThread isMainThread];
}

bool IsValidDimension(uint32_t value) {
  return value > 0 && value <= kMaximumDimension;
}

size_t AlignRowBytes(size_t value) {
  return (value + kRowAlignment - 1) & ~(kRowAlignment - 1);
}

NSString* WindowTitle(const char* title) {
  if (title == nullptr || title[0] == '\0') {
    return @"Darwin ART Surface";
  }
  NSString* result = [NSString stringWithUTF8String:title];
  return result == nil ? @"Darwin ART Surface" : result;
}

}  // namespace

@interface DarwinArtMetalView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                    pixelSize:(CGSize)pixelSize;
@end

@implementation DarwinArtMetalView {
  CAMetalLayer* _metalLayer;
}

- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                    pixelSize:(CGSize)pixelSize {
  self = [super initWithFrame:frame];
  if (self != nil) {
    self.wantsLayer = YES;
    _metalLayer = [CAMetalLayer layer];
    _metalLayer.device = device;
    _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The current presenter uses the CAMetalLayer drawable as a blit
    // destination. Metal forbids blits into framebuffer-only textures.
    _metalLayer.framebufferOnly = NO;
    _metalLayer.contentsScale = 1.0;
    _metalLayer.drawableSize = pixelSize;
    self.layer = _metalLayer;
  }
  return self;
}

- (BOOL)isFlipped {
  return YES;
}

- (CAMetalLayer*)metalLayer {
  return _metalLayer;
}

@end

struct DarwinArtSurface {
  uint32_t width = 0;
  uint32_t height = 0;
  size_t bytes_per_row = 0;
  IOSurfaceRef io_surface = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> io_surface_texture = nil;
  id<MTLCommandBuffer> last_command_buffer = nil;
  NSWindow* window = nil;
  DarwinArtMetalView* view = nil;
  bool visible = false;
  bool producer_mapped = false;

  ~DarwinArtSurface() {
    // Metal textures created from an IOSurface may consult that IOSurface
    // while they are being released. ARC destroys C++ fields after the
    // destructor body, so release the Objective-C owners explicitly before
    // dropping our Core Foundation reference.
    last_command_buffer = nil;
    io_surface_texture = nil;
    command_queue = nil;
    device = nil;
    view = nil;
    window = nil;
    if (io_surface != nullptr) {
      CFRelease(io_surface);
      io_surface = nullptr;
    }
  }
};

DarwinArtSurfaceResult darwin_art_surface_map_producer(
    DarwinArtSurface* surface,
    DarwinArtSurfaceProducerMapping* out_mapping) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr || out_mapping == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  *out_mapping = DarwinArtSurfaceProducerMapping{};
  if (surface->producer_mapped) {
    return DARWIN_ART_SURFACE_PRODUCER_ALREADY_MAPPED;
  }

  [surface->last_command_buffer waitUntilCompleted];
  if (surface->last_command_buffer != nil &&
      surface->last_command_buffer.status == MTLCommandBufferStatusError) {
    return DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED;
  }
  if (IOSurfaceLock(surface->io_surface, 0, nullptr) != kIOReturnSuccess) {
    return DARWIN_ART_SURFACE_ALLOCATION_FAILED;
  }
  void* base_address = IOSurfaceGetBaseAddress(surface->io_surface);
  if (base_address == nullptr) {
    IOSurfaceUnlock(surface->io_surface, 0, nullptr);
    return DARWIN_ART_SURFACE_ALLOCATION_FAILED;
  }

  surface->producer_mapped = true;
  *out_mapping = DarwinArtSurfaceProducerMapping{
      .base_address = base_address,
      .bytes_per_row = surface->bytes_per_row,
      .allocation_size = IOSurfaceGetAllocSize(surface->io_surface),
      .width = surface->width,
      .height = surface->height,
  };
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurfaceResult darwin_art_surface_unmap_producer(
    DarwinArtSurface* surface) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  if (!surface->producer_mapped) {
    return DARWIN_ART_SURFACE_PRODUCER_NOT_MAPPED;
  }
  if (IOSurfaceUnlock(surface->io_surface, 0, nullptr) != kIOReturnSuccess) {
    return DARWIN_ART_SURFACE_ALLOCATION_FAILED;
  }
  surface->producer_mapped = false;
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurface* darwin_art_surface_create(
    const DarwinArtSurfaceCreateInfo* create_info,
    DarwinArtSurfaceResult* out_result) {
  auto finish = [out_result](DarwinArtSurfaceResult result,
                             DarwinArtSurface* surface) {
    if (out_result != nullptr) {
      *out_result = result;
    }
    return surface;
  };

  if (!IsMainThread()) {
    return finish(DARWIN_ART_SURFACE_NOT_MAIN_THREAD, nullptr);
  }
  if (create_info == nullptr || !IsValidDimension(create_info->width) ||
      !IsValidDimension(create_info->height)) {
    return finish(DARWIN_ART_SURFACE_INVALID_ARGUMENT, nullptr);
  }

  const size_t minimum_row_bytes =
      static_cast<size_t>(create_info->width) * kBytesPerPixel;
  const size_t bytes_per_row = AlignRowBytes(minimum_row_bytes);
  if (bytes_per_row > std::numeric_limits<size_t>::max() /
                          static_cast<size_t>(create_info->height)) {
    return finish(DARWIN_ART_SURFACE_INVALID_ARGUMENT, nullptr);
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return finish(DARWIN_ART_SURFACE_METAL_UNAVAILABLE, nullptr);
    }
    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (command_queue == nil) {
      return finish(DARWIN_ART_SURFACE_METAL_UNAVAILABLE, nullptr);
    }

    NSDictionary* surface_properties = @{
      (__bridge NSString*)kIOSurfaceWidth : @(create_info->width),
      (__bridge NSString*)kIOSurfaceHeight : @(create_info->height),
      (__bridge NSString*)kIOSurfaceBytesPerElement : @(kBytesPerPixel),
      (__bridge NSString*)kIOSurfaceBytesPerRow : @(bytes_per_row),
      (__bridge NSString*)kIOSurfacePixelFormat : @(kBgraPixelFormat),
    };
    IOSurfaceRef io_surface = IOSurfaceCreate(
        (__bridge CFDictionaryRef)surface_properties);
    if (io_surface == nullptr) {
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }
    const size_t actual_bytes_per_row = IOSurfaceGetBytesPerRow(io_surface);
    const size_t allocation_size = IOSurfaceGetAllocSize(io_surface);
    if (actual_bytes_per_row < minimum_row_bytes ||
        actual_bytes_per_row > std::numeric_limits<size_t>::max() /
                                   static_cast<size_t>(create_info->height) ||
        allocation_size < actual_bytes_per_row *
                              static_cast<size_t>(create_info->height)) {
      CFRelease(io_surface);
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }

    MTLTextureDescriptor* texture_descriptor =
        [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:create_info->width
                                        height:create_info->height
                                     mipmapped:NO];
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> io_surface_texture =
        [device newTextureWithDescriptor:texture_descriptor
                               iosurface:io_surface
                                   plane:0];
    if (io_surface_texture == nil) {
      CFRelease(io_surface);
      return finish(DARWIN_ART_SURFACE_METAL_UNAVAILABLE, nullptr);
    }

    DarwinArtSurface* surface = new (std::nothrow) DarwinArtSurface();
    if (surface == nullptr) {
      CFRelease(io_surface);
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }
    surface->width = create_info->width;
    surface->height = create_info->height;
    surface->bytes_per_row = actual_bytes_per_row;
    surface->io_surface = io_surface;
    surface->device = device;
    surface->command_queue = command_queue;
    surface->io_surface_texture = io_surface_texture;
    surface->visible = create_info->visible;

    NSApplication* application = NSApplication.sharedApplication;
    if (create_info->visible) {
      [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    }
    NSRect frame = NSMakeRect(0, 0, create_info->width, create_info->height);
    surface->window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (surface->window == nil) {
      delete surface;
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }
    // The opaque handle, not AppKit's close operation, owns the window.
    surface->window.releasedWhenClosed = NO;
    surface->window.title = WindowTitle(create_info->title);
    surface->view = [[DarwinArtMetalView alloc]
        initWithFrame:frame
               device:device
            pixelSize:CGSizeMake(create_info->width, create_info->height)];
    if (surface->view == nil || surface->view.metalLayer == nil) {
      delete surface;
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }
    surface->window.contentView = surface->view;
    [surface->window center];
    if (create_info->visible) {
      [surface->window makeKeyAndOrderFront:nil];
      [application activateIgnoringOtherApps:YES];
    }

    if (IOSurfaceLock(io_surface, 0, nullptr) == kIOReturnSuccess) {
      void* base_address = IOSurfaceGetBaseAddress(io_surface);
      if (base_address != nullptr) {
        std::memset(base_address, 0,
                    actual_bytes_per_row *
                        static_cast<size_t>(create_info->height));
      }
      IOSurfaceUnlock(io_surface, 0, nullptr);
    }
    return finish(DARWIN_ART_SURFACE_OK, surface);
  }
}

DarwinArtSurfaceResult darwin_art_surface_update(
    DarwinArtSurface* surface,
    const void* bgra_pixels,
    size_t source_bytes_per_row) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr || bgra_pixels == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  const size_t copy_bytes = static_cast<size_t>(surface->width) *
                            kBytesPerPixel;
  if (source_bytes_per_row < copy_bytes) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  DarwinArtSurfaceProducerMapping mapping{};
  DarwinArtSurfaceResult result =
      darwin_art_surface_map_producer(surface, &mapping);
  if (result != DARWIN_ART_SURFACE_OK) {
    return result;
  }

  const auto* source = static_cast<const uint8_t*>(bgra_pixels);
  auto* destination = static_cast<uint8_t*>(mapping.base_address);
  for (uint32_t row = 0; row < surface->height; ++row) {
    std::memcpy(destination + static_cast<size_t>(row) * mapping.bytes_per_row,
                source + static_cast<size_t>(row) * source_bytes_per_row,
                copy_bytes);
  }
  return darwin_art_surface_unmap_producer(surface);
}

DarwinArtSurfaceResult darwin_art_surface_present(
    DarwinArtSurface* surface) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  if (surface->producer_mapped) {
    return DARWIN_ART_SURFACE_PRODUCER_ALREADY_MAPPED;
  }
  if (!surface->visible) {
    return DARWIN_ART_SURFACE_OK;
  }

  @autoreleasepool {
    id<CAMetalDrawable> drawable = [surface->view.metalLayer nextDrawable];
    if (drawable == nil) {
      return DARWIN_ART_SURFACE_DRAWABLE_UNAVAILABLE;
    }
    if (drawable.texture.width < surface->width ||
        drawable.texture.height < surface->height) {
      return DARWIN_ART_SURFACE_DRAWABLE_UNAVAILABLE;
    }
    id<MTLCommandBuffer> command_buffer =
        [surface->command_queue commandBuffer];
    if (command_buffer == nil) {
      return DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED;
    }
    id<MTLBlitCommandEncoder> encoder =
        [command_buffer blitCommandEncoder];
    if (encoder == nil) {
      return DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED;
    }

    MTLOrigin origin = MTLOriginMake(0, 0, 0);
    MTLSize size = MTLSizeMake(surface->width, surface->height, 1);
    [encoder copyFromTexture:surface->io_surface_texture
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:origin
                  sourceSize:size
                   toTexture:drawable.texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:origin];
    [encoder endEncoding];
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
    surface->last_command_buffer = command_buffer;
  }
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurfaceResult darwin_art_surface_pump_events(
    DarwinArtSurface* surface,
    double seconds) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr || !std::isfinite(seconds) || seconds < 0.0 ||
      seconds > 30.0) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  if (surface->visible && !surface->window.visible) {
    return DARWIN_ART_SURFACE_WINDOW_CLOSED;
  }
  if (seconds == 0.0) {
    return DARWIN_ART_SURFACE_OK;
  }

  @autoreleasepool {
    NSApplication* application = NSApplication.sharedApplication;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (deadline.timeIntervalSinceNow > 0.0) {
      if (surface->visible && !surface->window.visible) {
        return DARWIN_ART_SURFACE_WINDOW_CLOSED;
      }
      const NSTimeInterval slice_seconds =
          std::min<NSTimeInterval>(0.016, deadline.timeIntervalSinceNow);
      NSDate* slice_deadline =
          [NSDate dateWithTimeIntervalSinceNow:slice_seconds];
      NSEvent* event = [application nextEventMatchingMask:NSEventMaskAny
                                                untilDate:slice_deadline
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
      if (event != nil) {
        [application sendEvent:event];
      }
      [application updateWindows];
    }
  }
  return surface->visible && !surface->window.visible
             ? DARWIN_ART_SURFACE_WINDOW_CLOSED
             : DARWIN_ART_SURFACE_OK;
}

DarwinArtSurfaceResult darwin_art_surface_destroy(
    DarwinArtSurface* surface) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  DarwinArtSurfaceResult unmap_result = DARWIN_ART_SURFACE_OK;
  if (surface->producer_mapped) {
    unmap_result = darwin_art_surface_unmap_producer(surface);
  }
  [surface->last_command_buffer waitUntilCompleted];
  const bool gpu_failed =
      surface->last_command_buffer != nil &&
      surface->last_command_buffer.status == MTLCommandBufferStatusError;
  [surface->window orderOut:nil];
  [surface->window close];
  delete surface;
  if (unmap_result != DARWIN_ART_SURFACE_OK) {
    return unmap_result;
  }
  return gpu_failed ? DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED
                    : DARWIN_ART_SURFACE_OK;
}
