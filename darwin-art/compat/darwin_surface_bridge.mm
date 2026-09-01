#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "darwin_surface_internal.h"
#include "darwin_android_time.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <deque>
#include <iostream>
#include <limits>
#include <new>
#include <time.h>
#include <unordered_map>

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

// Surface lifecycle and scanout are AppKit-owned, but ART will eventually run
// on a dedicated owner pthread.  Keep the marshalling primitive in this small
// bridge so callers never invoke AppKit from that worker.  The block is
// synchronous by design for create/resize/present/destroy ordering; the main
// thread must continue servicing its NSApplication run loop while a worker is
// alive (a blocking join would deadlock this path).
template <typename Function>
auto RunOnMainSync(Function&& function) -> decltype(function()) {
  if (IsMainThread()) return function();
  using Result = decltype(function());
  __block Result result{};
  dispatch_sync(dispatch_get_main_queue(), ^{
    result = function();
  });
  return result;
}

uint64_t AndroidEventTimeNanos() {
  return static_cast<uint64_t>(darwin_art::AndroidUptimeNanos());
}

bool IsValidDimension(uint32_t value) {
  return value > 0 && value <= kMaximumDimension;
}

size_t AlignRowBytes(size_t value) {
  return (value + kRowAlignment - 1) & ~(kRowAlignment - 1);
}

struct SurfaceBacking {
  IOSurfaceRef io_surface = nullptr;
  id<MTLTexture> texture = nil;
  size_t bytes_per_row = 0;
};

bool AllocateSurfaceBacking(id<MTLDevice> device, uint32_t width,
                            uint32_t height, SurfaceBacking* out) {
  if (device == nil || out == nullptr || !IsValidDimension(width) ||
      !IsValidDimension(height)) {
    return false;
  }
  const size_t minimum_row_bytes = static_cast<size_t>(width) * kBytesPerPixel;
  const size_t bytes_per_row = AlignRowBytes(minimum_row_bytes);
  if (bytes_per_row > std::numeric_limits<size_t>::max() /
                          static_cast<size_t>(height)) {
    return false;
  }
  NSDictionary* surface_properties = @{
    (__bridge NSString*)kIOSurfaceWidth : @(width),
    (__bridge NSString*)kIOSurfaceHeight : @(height),
    (__bridge NSString*)kIOSurfaceBytesPerElement : @(kBytesPerPixel),
    (__bridge NSString*)kIOSurfaceBytesPerRow : @(bytes_per_row),
    (__bridge NSString*)kIOSurfacePixelFormat : @(kBgraPixelFormat),
    (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
  };
  IOSurfaceRef io_surface = IOSurfaceCreate(
      (__bridge CFDictionaryRef)surface_properties);
  if (io_surface == nullptr) return false;
  const size_t actual_bytes_per_row = IOSurfaceGetBytesPerRow(io_surface);
  const size_t allocation_size = IOSurfaceGetAllocSize(io_surface);
  if (actual_bytes_per_row < minimum_row_bytes ||
      actual_bytes_per_row > std::numeric_limits<size_t>::max() /
                                 static_cast<size_t>(height) ||
      allocation_size < actual_bytes_per_row * static_cast<size_t>(height)) {
    CFRelease(io_surface);
    return false;
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                               MTLPixelFormatBGRA8Unorm
                                                       width:width
                                                      height:height
                                                   mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor
                                                    iosurface:io_surface
                                                        plane:0];
  if (texture == nil) {
    CFRelease(io_surface);
    return false;
  }
  out->io_surface = io_surface;
  out->texture = texture;
  out->bytes_per_row = actual_bytes_per_row;
  return true;
}

NSString* WindowTitle(const char* title) {
  if (title == nullptr || title[0] == '\0') {
    return @"Darwin ART Surface";
  }
  NSString* result = [NSString stringWithUTF8String:title];
  return result == nil ? @"Darwin ART Surface" : result;
}

NSImage* ApplicationIconFromEnvironment() {
  const char* path = std::getenv("DARWIN_ART_APK_APP_ICON");
  if (path == nullptr || path[0] == '\0') return nil;
  NSString* file = [NSString stringWithUTF8String:path];
  if (file == nil) return nil;
  NSData* data = [NSData dataWithContentsOfFile:file options:0 error:nil];
  NSImage* image = data == nil ? nil : [[NSImage alloc] initWithData:data];
  if (image != nil) {
    // AppKit uses the image's representations at their native size.  Keep
    // the APK's density choice intact while providing a useful dock/menu
    // size for low-density icons as well.
    image.size = NSMakeSize(128.0, 128.0);
  }
  return image;
}

void ApplyApplicationIdentity(NSApplication* application, NSWindow* window) {
  NSImage* image = ApplicationIconFromEnvironment();
  if (image == nil) return;
  application.applicationIconImage = image;
  if (window != nil) window.miniwindowImage = image;
}

CGFloat WindowScale(bool visible) {
  const char* value = std::getenv("DARWIN_ART_WINDOW_SCALE");
  return visible && value != nullptr && std::strcmp(value, "2") == 0 ? 2.0
                                                                       : 1.0;
}

uint32_t AndroidMetaState(NSEventModifierFlags flags) {
  uint32_t meta = 0;
  if ((flags & NSEventModifierFlagShift) != 0) meta |= 0x1;
  if ((flags & NSEventModifierFlagOption) != 0) meta |= 0x2;
  if ((flags & NSEventModifierFlagFunction) != 0) meta |= 0x8;
  if ((flags & NSEventModifierFlagControl) != 0) meta |= 0x1000;
  if ((flags & NSEventModifierFlagCommand) != 0) meta |= 0x10000;
  if ((flags & NSEventModifierFlagCapsLock) != 0) meta |= 0x100000;
  return meta;
}

uint32_t AndroidKeyCode(unsigned short code) {
  switch (code) {
    case 0: return 29;  // A
    case 1: return 47;  // S
    case 2: return 32;  // D
    case 3: return 34;  // F
    case 4: return 36;  // H
    case 5: return 35;  // G
    case 6: return 54;  // Z
    case 7: return 52;  // X
    case 8: return 31;  // C
    case 9: return 50;  // V
    case 11: return 30; // B
    case 12: return 45; // Q
    case 13: return 51; // W
    case 14: return 33; // E
    case 15: return 46; // R
    case 16: return 53; // Y
    case 17: return 48; // T
    case 18: return 8;  // 1
    case 19: return 9;  // 2
    case 20: return 10; // 3
    case 21: return 11; // 4
    case 22: return 13; // 6
    case 23: return 12; // 5
    case 24: return 70; // =
    case 25: return 16; // 9
    case 26: return 14; // 7
    case 27: return 69; // -
    case 28: return 15; // 8
    case 29: return 7;  // 0
    case 30: return 72; // ]
    case 31: return 43; // O
    case 32: return 49; // U
    case 33: return 71; // [
    case 34: return 37; // I
    case 35: return 44; // P
    case 36: return 66; // ENTER
    case 37: return 40; // L
    case 38: return 38; // J
    case 39: return 75; // '
    case 40: return 39; // K
    case 41: return 74; // ;
    case 42: return 73; // backslash
    case 43: return 55; // ,
    case 44: return 76; // /
    case 45: return 42; // N
    case 46: return 41; // M
    case 47: return 56; // .
    case 48: return 61; // TAB
    case 49: return 62; // SPACE
    case 50: return 68; // grave
    case 51: return 67; // DEL
    case 53: return 111; // ESCAPE
    case 54: return 118; // META_RIGHT
    case 55: return 117; // META_LEFT
    case 56: return 59;  // SHIFT_LEFT
    case 57: return 115; // CAPS_LOCK
    case 58: return 57;  // ALT_LEFT
    case 59: return 113; // CTRL_LEFT
    case 60: return 60;  // SHIFT_RIGHT
    case 61: return 58;  // ALT_RIGHT
    case 62: return 114; // CTRL_RIGHT
    case 63: return 119; // FUNCTION
    case 123: return 21; // DPAD_LEFT
    case 124: return 22; // DPAD_RIGHT
    case 125: return 20; // DPAD_DOWN
    case 126: return 19; // DPAD_UP
    default: return 0;
  }
}

uint32_t AndroidScanCode(uint32_t key_code) {
  // Linux evdev scan codes carried by Android's native InputDispatcher.
  // AppKit virtual key codes are a different namespace and must not leak into
  // KeyEvent.getScanCode().
  if (key_code == 7) return 11;
  if (key_code >= 8 && key_code <= 16) return key_code - 6;
  switch (key_code) {
    case 29: return 30; case 30: return 48; case 31: return 46;
    case 32: return 32; case 33: return 18; case 34: return 33;
    case 35: return 34; case 36: return 35; case 37: return 23;
    case 38: return 36; case 39: return 37; case 40: return 38;
    case 41: return 50; case 42: return 49; case 43: return 24;
    case 44: return 25; case 45: return 16; case 46: return 19;
    case 47: return 31; case 48: return 20; case 49: return 22;
    case 50: return 47; case 51: return 17; case 52: return 45;
    case 53: return 21; case 54: return 44; case 55: return 51;
    case 56: return 52; case 62: return 57; case 66: return 28;
    case 67: return 14; case 68: return 41; case 69: return 12;
    case 70: return 13; case 71: return 26; case 72: return 27;
    case 73: return 43; case 74: return 39; case 75: return 40;
    case 76: return 53; case 111: return 1;
    default: return 0;
  }
}

NSEventModifierFlags ModifierFlagForKey(unsigned short code) {
  switch (code) {
    case 54:
    case 55: return NSEventModifierFlagCommand;
    case 56:
    case 60: return NSEventModifierFlagShift;
    case 57: return NSEventModifierFlagCapsLock;
    case 58:
    case 61: return NSEventModifierFlagOption;
    case 59:
    case 62: return NSEventModifierFlagControl;
    case 63: return NSEventModifierFlagFunction;
    default: return 0;
  }
}

}  // namespace

@implementation DarwinArtMetalView {
  CAMetalLayer* _metalLayer;
  std::deque<DarwinArtPointerEventV2> _pointerEvents;
  std::deque<DarwinArtKeyEventV1> _keyEvents;
  // AppKit is the producer, while the future ART worker consumes packets.
  // Keep the ABI packets independent of the NSView and make the mailbox
  // safe to drain without touching AppKit objects.
  std::mutex _eventMutex;
  std::unordered_map<unsigned short, uint64_t> _keyDownTimes;
  std::unordered_map<unsigned short, uint32_t> _keyRepeatCounts;
  BOOL _pointerActive;
  uint64_t _nextPointerSequence;
  uint64_t _nextKeySequence;
  uint64_t _downTimeNanos;
}

- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                    pixelSize:(CGSize)pixelSize
                 contentScale:(CGFloat)contentScale {
  self = [super initWithFrame:frame];
  if (self != nil) {
    self.wantsLayer = YES;
    _metalLayer = [CAMetalLayer layer];
    _metalLayer.device = device;
    _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The current presenter uses the CAMetalLayer drawable as a blit
    // destination. Metal forbids blits into framebuffer-only textures.
    _metalLayer.framebufferOnly = NO;
    _metalLayer.contentsScale = contentScale;
    _metalLayer.drawableSize = pixelSize;
    self.layer = _metalLayer;
    _pointerActive = NO;
    _nextPointerSequence = 1;
    _nextKeySequence = 1;
    _downTimeNanos = 0;
  }
  return self;
}

- (BOOL)isFlipped {
  return YES;
}

- (CAMetalLayer*)metalLayer {
  return _metalLayer;
}

- (void)updateDrawableSize {
  if (_metalLayer == nil) return;
  const CGFloat scale = _metalLayer.contentsScale > 0.0
                            ? _metalLayer.contentsScale
                            : 1.0;
  const NSRect bounds = self.bounds;
  _metalLayer.drawableSize = CGSizeMake(
      std::max<CGFloat>(1.0, std::ceil(bounds.size.width * scale)),
      std::max<CGFloat>(1.0, std::ceil(bounds.size.height * scale)));
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)enqueuePointerEvent:(NSEvent*)event
                     action:(DarwinArtPointerAction)action {
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  const NSRect bounds = self.bounds;
  // Keep the stream alive while a button is held even after the cursor leaves
  // the view. Android receives those coordinates and decides whether the
  // gesture remains owned by the child; dropping them here makes an eventual
  // mouseUp indistinguishable from a lost pointer. A new DOWN while the old
  // stream is still active is repaired with an explicit CANCEL.
  if (action == DARWIN_ART_POINTER_DOWN && _pointerActive) {
    [self cancelPointerStream];
  }
  // NSEvent coordinates are AppKit points, while Android's retained view is
  // laid out in the CAMetalLayer drawable's backing pixels.  Derive the
  // mapping from the live layer instead of assuming the launcher's requested
  // scale is still the presentation scale after a Retina/resize transition.
  const CGSize drawable_size = _metalLayer.drawableSize;
  const CGFloat x_scale = bounds.size.width > 0.0
                              ? drawable_size.width / bounds.size.width
                              : 1.0;
  const CGFloat y_scale = bounds.size.height > 0.0
                              ? drawable_size.height / bounds.size.height
                              : 1.0;
  // DarwinArtMetalView is flipped, so convertPoint already returns a
  // top-left-origin Y coordinate. Flipping it a second time made a click near
  // the top of the window arrive near the bottom of Android/Blink (for
  // example input-field y=188 became body y=888 on a 1280 px surface).
  const CGFloat android_y = point.y;
  const uint64_t event_time_nanos = AndroidEventTimeNanos();
  if (action == DARWIN_ART_POINTER_DOWN) _downTimeNanos = event_time_nanos;
  {
    std::lock_guard<std::mutex> lock(_eventMutex);
    _pointerEvents.push_back(DarwinArtPointerEventV2{
        .version = 2,
        .size = static_cast<uint32_t>(sizeof(DarwinArtPointerEventV2)),
        .action = static_cast<uint32_t>(action),
        // Android app players conventionally translate the primary host click
        // into a touchscreen stream. A distinct mouse packet remains available
        // in ABI v2 for explicit external-mouse integrations.
        .flags = 0,
        .sequence = _nextPointerSequence++,
        .event_time_nanos = event_time_nanos,
        .down_time_nanos = _downTimeNanos,
        .pointer_id = 0,
        .pointer_count = 1,
        .x = static_cast<float>(point.x * x_scale),
        .y = static_cast<float>(android_y * y_scale),
        .raw_x = static_cast<float>(point.x * x_scale),
        .raw_y = static_cast<float>(android_y * y_scale),
        .pressure = 1.0f,
        .size_value = 1.0f,
    });
  }
  if (action == DARWIN_ART_POINTER_DOWN) {
    _pointerActive = YES;
  } else if (action == DARWIN_ART_POINTER_UP ||
             action == DARWIN_ART_POINTER_CANCEL) {
    _pointerActive = NO;
    _downTimeNanos = 0;
  }
}

- (void)mouseDown:(NSEvent*)event {
  [self enqueuePointerEvent:event action:DARWIN_ART_POINTER_DOWN];
}

- (void)mouseUp:(NSEvent*)event {
  [self enqueuePointerEvent:event action:DARWIN_ART_POINTER_UP];
}

- (void)mouseDragged:(NSEvent*)event {
  [self enqueuePointerEvent:event action:DARWIN_ART_POINTER_MOVE];
}

- (void)enqueueKeyEvent:(NSEvent*)event action:(uint32_t)action {
  const unsigned short scan_code = event.keyCode;
  const uint32_t key_code = AndroidKeyCode(scan_code);
  if (key_code == 0) return;
  const uint64_t event_time_nanos = AndroidEventTimeNanos();
  if (action == 0 && !event.isARepeat) {
    _keyDownTimes[scan_code] = event_time_nanos;
    _keyRepeatCounts[scan_code] = 0;
  }
  const auto down = _keyDownTimes.find(scan_code);
  const uint64_t down_time_nanos =
      down == _keyDownTimes.end() ? event_time_nanos : down->second;
  uint32_t repeat_count = 0;
  if (action == 0 && event.isARepeat) {
    repeat_count = ++_keyRepeatCounts[scan_code];
  }
  NSString* characters = event.characters;
  const uint32_t unicode_char =
      characters.length == 0 ? 0 : [characters characterAtIndex:0];
  {
    std::lock_guard<std::mutex> lock(_eventMutex);
    _keyEvents.push_back(DarwinArtKeyEventV1{
        .version = 1,
        .size = static_cast<uint32_t>(sizeof(DarwinArtKeyEventV1)),
        .action = action,
        // KeyEvent.FLAG_FROM_SYSTEM, as set by Android InputDispatcher for a
        // connected physical keyboard.
        .flags = 0x8,
        .sequence = _nextKeySequence++,
        .event_time_nanos = event_time_nanos,
        .down_time_nanos = down_time_nanos,
        .key_code = key_code,
        .scan_code = AndroidScanCode(key_code),
        .meta_state = AndroidMetaState(event.modifierFlags),
        .repeat_count = repeat_count,
        .device_id = 1,
        .source = 0x101,
        .unicode_char = unicode_char,
    });
  }
  if (action == 1) {
    _keyDownTimes.erase(scan_code);
    _keyRepeatCounts.erase(scan_code);
  }
}

- (void)keyDown:(NSEvent*)event {
  [self enqueueKeyEvent:event action:0];
}

- (void)keyUp:(NSEvent*)event {
  [self enqueueKeyEvent:event action:1];
}

- (void)flagsChanged:(NSEvent*)event {
  const NSEventModifierFlags flag = ModifierFlagForKey(event.keyCode);
  if (flag == 0) return;
  const uint32_t action = (event.modifierFlags & flag) != 0 ? 0 : 1;
  [self enqueueKeyEvent:event action:action];
}

- (BOOL)nextPointerEvent:(DarwinArtPointerEvent*)outEvent {
  if (outEvent == nullptr) return NO;
  DarwinArtPointerEventV2 event = {};
  if (![self nextPointerEventV2:&event]) return NO;
  *outEvent = DarwinArtPointerEvent{
      .action = event.action, .x = event.x, .y = event.y};
  return YES;
}

- (BOOL)nextPointerEventV2:(DarwinArtPointerEventV2*)outEvent {
  if (outEvent == nullptr) return NO;
  std::lock_guard<std::mutex> lock(_eventMutex);
  if (_pointerEvents.empty()) return NO;
  *outEvent = _pointerEvents.front();
  _pointerEvents.pop_front();
  return YES;
}

- (BOOL)nextKeyEventV1:(DarwinArtKeyEventV1*)outEvent {
  if (outEvent == nullptr) return NO;
  std::lock_guard<std::mutex> lock(_eventMutex);
  if (_keyEvents.empty()) return NO;
  *outEvent = _keyEvents.front();
  _keyEvents.pop_front();
  return YES;
}

- (void)cancelPointerStream {
  if (!_pointerActive) return;
  const uint64_t event_time_nanos = AndroidEventTimeNanos();
  {
    std::lock_guard<std::mutex> lock(_eventMutex);
    _pointerEvents.push_back(DarwinArtPointerEventV2{
        .version = 2,
        .size = static_cast<uint32_t>(sizeof(DarwinArtPointerEventV2)),
        .action = DARWIN_ART_POINTER_CANCEL,
        .flags = 0,
        .sequence = _nextPointerSequence++,
        .event_time_nanos = event_time_nanos,
        .down_time_nanos = _downTimeNanos,
        .pointer_id = 0,
        .pointer_count = 1,
        .x = 0.0f,
        .y = 0.0f,
        .raw_x = 0.0f,
        .raw_y = 0.0f,
        .pressure = 0.0f,
        .size_value = 0.0f,
    });
    _pointerActive = NO;
    _downTimeNanos = 0;
  }
}

@end

@interface DarwinArtSurfaceWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, weak) DarwinArtMetalView* view;
@property(nonatomic, assign) DarwinArtSurface* surface;
@end

static DarwinArtSurfaceResult ResizeSurfaceBacking(DarwinArtSurface* surface,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   bool update_window);

@implementation DarwinArtSurfaceWindowDelegate
- (void)windowDidResignKey:(NSNotification*)notification {
  (void)notification;
  [self.view cancelPointerStream];
}
- (void)windowWillClose:(NSNotification*)notification {
  (void)notification;
  [self.view cancelPointerStream];
}
- (void)windowDidResize:(NSNotification*)notification {
  (void)notification;
  [self.view cancelPointerStream];
  [self.view updateDrawableSize];
  if (self.surface == nullptr || self.view == nil) return;
  const CGFloat scale = self.view.metalLayer.contentsScale > 0.0
                            ? self.view.metalLayer.contentsScale
                            : 1.0;
  const NSRect bounds = self.view.bounds;
  const uint32_t width = static_cast<uint32_t>(std::max<CGFloat>(
      1.0, std::ceil(bounds.size.width * scale)));
  const uint32_t height = static_cast<uint32_t>(std::max<CGFloat>(
      1.0, std::ceil(bounds.size.height * scale)));
  const DarwinArtSurfaceResult result =
      ResizeSurfaceBacking(self.surface, width, height, false);
  if (result != DARWIN_ART_SURFACE_OK) {
    std::cerr << "DARWIN_ART window resize failed status=" << result
              << " width=" << width << " height=" << height << "\n";
  } else {
    std::cerr << "DARWIN_ART window resize pixels=" << width << "x"
              << height << "\n";
  }
}
@end

DarwinArtSurface::~DarwinArtSurface() {
    // Metal textures created from an IOSurface may consult that IOSurface
    // while they are being released. ARC destroys C++ fields after the
    // destructor body, so release the Objective-C owners explicitly before
    // dropping our Core Foundation reference.
    last_command_buffer = nil;
    if (window != nil) window.delegate = nil;
    window_delegate = nil;
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

DarwinArtSurface* g_active_gpu_surface = nullptr;

__attribute__((weak)) void darwin_art_surface_gpu_forget(
    DarwinArtSurface*) {}

// CPU/runtime links retain the surface ABI but deliberately do not link
// Ganesh or Metal.  Keep the optional GPU capability closed in that flavor;
// the graphics link supplies the strong implementations from
// darwin_surface_gpu_bridge.mm.
__attribute__((weak)) DarwinArtSurface* darwin_art_surface_active_gpu() {
  return nullptr;
}

__attribute__((weak)) void darwin_art_surface_set_active_gpu(
    DarwinArtSurface*) {}

bool darwin_art_surface_gpu_acquire_iosurface(
    DarwinArtSurface* surface, void** iosurface, uint32_t* width,
    uint32_t* height) {
  if (surface == nullptr || iosurface == nullptr || width == nullptr ||
      height == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(surface->backing_mutex);
  if (surface->io_surface == nullptr) return false;
  CFRetain(surface->io_surface);
  *iosurface = surface->io_surface;
  *width = surface->width;
  *height = surface->height;
  return true;
}

uint32_t darwin_art_surface_gpu_iosurface_id(DarwinArtSurface* surface) {
  if (surface == nullptr) return 0;
  std::lock_guard<std::mutex> lock(surface->backing_mutex);
  return surface->io_surface == nullptr ? 0 : IOSurfaceGetID(surface->io_surface);
}

bool darwin_art_surface_gpu_lookup_iosurface(
    uint32_t surface_id, void** iosurface, uint32_t* width,
    uint32_t* height) {
  if (surface_id == 0 || iosurface == nullptr || width == nullptr ||
      height == nullptr) {
    return false;
  }
  IOSurfaceRef found = IOSurfaceLookup(surface_id);
  if (found == nullptr) return false;
  const size_t found_width = IOSurfaceGetWidth(found);
  const size_t found_height = IOSurfaceGetHeight(found);
  if (found_width == 0 || found_height == 0 ||
      found_width > UINT32_MAX || found_height > UINT32_MAX) {
    CFRelease(found);
    return false;
  }
  *iosurface = found;
  *width = static_cast<uint32_t>(found_width);
  *height = static_cast<uint32_t>(found_height);
  return true;
}

void darwin_art_surface_gpu_release_iosurface(void* iosurface) {
  if (iosurface != nullptr) CFRelease(static_cast<CFTypeRef>(iosurface));
}

void darwin_art_surface_gpu_configure_embedded(
    DarwinArtSurface* surface, int32_t x, int32_t y, uint32_t width,
    uint32_t height) {
  if (surface == nullptr) return;
  surface->embedded_surface_x.store(x, std::memory_order_relaxed);
  surface->embedded_surface_y.store(y, std::memory_order_relaxed);
  surface->embedded_surface_width.store(width, std::memory_order_relaxed);
  surface->embedded_surface_height.store(height, std::memory_order_release);
}

bool darwin_art_surface_gpu_get_embedded_geometry(
    DarwinArtSurface* surface, int32_t* x, int32_t* y, uint32_t* width,
    uint32_t* height) {
  if (surface == nullptr || x == nullptr || y == nullptr || width == nullptr ||
      height == nullptr) {
    return false;
  }
  *height = surface->embedded_surface_height.load(std::memory_order_acquire);
  *width = surface->embedded_surface_width.load(std::memory_order_relaxed);
  *x = surface->embedded_surface_x.load(std::memory_order_relaxed);
  *y = surface->embedded_surface_y.load(std::memory_order_relaxed);
  return *width > 0 && *height > 0;
}

void darwin_art_surface_gpu_set_iosurface_composition_active(
    void* iosurface, bool active) {
  if (iosurface == nullptr) return;
  IOSurfaceSetValue(static_cast<IOSurfaceRef>(iosurface),
                    CFSTR("dev.darwinart.surface-composition-active"),
                    active ? kCFBooleanTrue : kCFBooleanFalse);
}

bool darwin_art_surface_gpu_is_iosurface_composition_active(void* iosurface) {
  if (iosurface == nullptr) return true;
  CFTypeRef value = IOSurfaceCopyValue(
      static_cast<IOSurfaceRef>(iosurface),
      CFSTR("dev.darwinart.surface-composition-active"));
  if (value == nullptr) return true;
  const bool active = value == kCFBooleanTrue ||
                      (CFGetTypeID(value) == CFBooleanGetTypeID() &&
                       CFBooleanGetValue(static_cast<CFBooleanRef>(value)));
  CFRelease(value);
  return active;
}

void darwin_art_surface_gpu_publish_embedded(DarwinArtSurface* surface) {
  if (surface == nullptr) return;
  surface->embedded_surface_frame.fetch_add(1, std::memory_order_release);
}

void darwin_art_surface_gpu_set_embedded_buffer_extent(
    DarwinArtSurface* surface, uint32_t width, uint32_t height) {
  if (surface == nullptr) return;
  surface->embedded_buffer_width.store(width, std::memory_order_relaxed);
  surface->embedded_buffer_height.store(height, std::memory_order_release);
}

__attribute__((weak)) bool darwin_art_surface_gpu_composite_embedded(
    DarwinArtSurface*, void*) {
  return false;
}

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

static DarwinArtSurface* CreateSurfaceOnMain(
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
      (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
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
    const CGFloat window_scale = WindowScale(create_info->visible);
    NSRect frame = NSMakeRect(0, 0, create_info->width / window_scale,
                              create_info->height / window_scale);
    // Headless surfaces still need a Metal view/layer for GPU work, but must
    // not allocate an AppKit window. A large number of probes and hidden
    // rendering surfaces are created with visible=false; creating an
    // NSWindow for each one makes tests leak apparent windows and needlessly
    // couples offscreen rendering to AppKit window management.
    if (create_info->visible) {
      surface->window = [[NSWindow alloc]
          initWithContentRect:frame
                    styleMask:(NSWindowStyleMaskTitled |
                               NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable)
                      backing:NSBackingStoreBuffered
                        defer:NO];
      if (surface->window == nil) {
        delete surface;
        return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
      }
      // The opaque handle, not AppKit's close operation, owns the window.
      surface->window.releasedWhenClosed = NO;
      surface->window.title = WindowTitle(create_info->title);
      ApplyApplicationIdentity(application, surface->window);
    }
    surface->view = [[DarwinArtMetalView alloc]
        initWithFrame:frame
               device:device
            pixelSize:CGSizeMake(create_info->width, create_info->height)
         contentScale:window_scale];
    if (surface->view == nil || surface->view.metalLayer == nil) {
      delete surface;
      return finish(DARWIN_ART_SURFACE_ALLOCATION_FAILED, nullptr);
    }
    if (create_info->visible) {
      DarwinArtSurfaceWindowDelegate* delegate =
          [[DarwinArtSurfaceWindowDelegate alloc] init];
      delegate.view = surface->view;
      delegate.surface = surface;
      surface->window_delegate = delegate;
      surface->window.delegate = delegate;
      surface->window.contentView = surface->view;
      [surface->window makeFirstResponder:surface->view];
      [surface->window center];
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

DarwinArtSurface* darwin_art_surface_create(
    const DarwinArtSurfaceCreateInfo* create_info,
    DarwinArtSurfaceResult* out_result) {
  if (create_info == nullptr) {
    if (out_result != nullptr) *out_result = DARWIN_ART_SURFACE_INVALID_ARGUMENT;
    return nullptr;
  }
  // The caller owns this POD for the duration of the synchronous dispatch.
  // Copying it here prevents a worker from exposing a mutable request while
  // the AppKit block is queued.
  const DarwinArtSurfaceCreateInfo info = *create_info;
  return RunOnMainSync([&] { return CreateSurfaceOnMain(&info, out_result); });
}

static DarwinArtSurfaceResult ResizeSurfaceBacking(DarwinArtSurface* surface,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   bool update_window) {
  if (!IsMainThread()) return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  if (surface == nullptr || !IsValidDimension(width) ||
      !IsValidDimension(height)) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  if (surface->width == width && surface->height == height) return DARWIN_ART_SURFACE_OK;
  if (surface->producer_mapped) {
    return DARWIN_ART_SURFACE_PRODUCER_ALREADY_MAPPED;
  }
  [surface->last_command_buffer waitUntilCompleted];
  if (surface->last_command_buffer != nil &&
      surface->last_command_buffer.status == MTLCommandBufferStatusError) {
    return DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED;
  }
  SurfaceBacking backing;
  if (!AllocateSurfaceBacking(surface->device, width, height, &backing)) {
    return DARWIN_ART_SURFACE_ALLOCATION_FAILED;
  }
  IOSurfaceRef old_surface = nullptr;
  {
    std::lock_guard<std::mutex> lock(surface->backing_mutex);
    old_surface = surface->io_surface;
    surface->io_surface = backing.io_surface;
    surface->io_surface_texture = backing.texture;
    surface->bytes_per_row = backing.bytes_per_row;
    surface->width = width;
    surface->height = height;
    surface->last_command_buffer = nil;
  }
  if (old_surface != nullptr) CFRelease(old_surface);
  if (surface->view != nil) {
    [surface->view updateDrawableSize];
    if (update_window && surface->window != nil) {
      const CGFloat scale = surface->view.metalLayer.contentsScale > 0.0
                                ? surface->view.metalLayer.contentsScale
                                : 1.0;
      // NSWindow's setContentSize: preserves the lower-left frame origin.
      // Android relayouts can transiently grow a Surface and then restore its
      // previous size (Chromium does this while creating a tab).  Anchoring
      // that sequence at the lower edge walks the window upward, eventually
      // leaving its title bar off-screen.  Desktop windows conventionally
      // keep their top-left placement across content-size changes, so retain
      // that point explicitly while the Android buffer is reallocated.
      const NSPoint top_left =
          NSMakePoint(NSMinX(surface->window.frame),
                      NSMaxY(surface->window.frame));
      [surface->window setContentSize:
                         NSMakeSize(static_cast<CGFloat>(width) / scale,
                                    static_cast<CGFloat>(height) / scale)];
      [surface->window setFrameTopLeftPoint:top_left];
    }
  }
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurfaceResult darwin_art_surface_resize(
    DarwinArtSurface* surface, uint32_t width, uint32_t height) {
  return RunOnMainSync([&] {
    return ResizeSurfaceBacking(surface, width, height, true);
  });
}

DarwinArtSurfaceResult darwin_art_surface_set_title(
    DarwinArtSurface* surface, const char* title) {
  return RunOnMainSync([&] {
    if (surface == nullptr || title == nullptr || title[0] == '\0') {
      return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
    }
    if (surface->window != nil) surface->window.title = WindowTitle(title);
    return DARWIN_ART_SURFACE_OK;
  });
}

char* darwin_art_host_open_document(const char* mime_type) {
  if (!IsMainThread()) return nullptr;
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = YES;
    panel.title = @"Open Android document";
    panel.prompt = @"Open";
    if (mime_type != nullptr && strncmp(mime_type, "image/", 6) == 0) {
      panel.allowedFileTypes = @[@"jpg", @"jpeg", @"png", @"gif", @"webp"];
    }
    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
      return nullptr;
    }
    const char* path = panel.URL.fileSystemRepresentation;
    return path == nullptr ? nullptr : strdup(path);
  }
}

char* darwin_art_host_save_document(const char* mime_type,
                                    const char* suggested_name) {
  if (!IsMainThread()) return nullptr;
  @autoreleasepool {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.canCreateDirectories = YES;
    panel.title = @"Save Android document";
    panel.prompt = @"Save";
    if (mime_type != nullptr && strncmp(mime_type, "image/", 6) == 0) {
      panel.allowedFileTypes = @[@"jpg", @"jpeg", @"png", @"gif", @"webp"];
    }
    if (suggested_name != nullptr && suggested_name[0] != '\0') {
      NSString* name = [NSString stringWithUTF8String:suggested_name];
      if (name != nil) panel.nameFieldStringValue = name;
    }
    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
      return nullptr;
    }
    const char* path = panel.URL.fileSystemRepresentation;
    return path == nullptr ? nullptr : strdup(path);
  }
}

void darwin_art_host_document_path_free(char* path) {
  free(path);
}

bool darwin_art_surface_get_size(DarwinArtSurface* surface,
                                 uint32_t* width, uint32_t* height) {
  if (surface == nullptr || width == nullptr || height == nullptr) return false;
  std::lock_guard<std::mutex> lock(surface->backing_mutex);
  *width = surface->width;
  *height = surface->height;
  return true;
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

static DarwinArtSurfaceResult PresentSurfaceOnMain(
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

DarwinArtSurfaceResult darwin_art_surface_present(DarwinArtSurface* surface) {
  return RunOnMainSync([&] { return PresentSurfaceOnMain(surface); });
}

static DarwinArtSurfaceResult PumpSurfaceEventsOnMain(
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
    [surface->view cancelPointerStream];
    return DARWIN_ART_SURFACE_WINDOW_CLOSED;
  }
  if (seconds == 0.0) {
    return DARWIN_ART_SURFACE_OK;
  }

  @autoreleasepool {
    NSApplication* application = NSApplication.sharedApplication;
    // Window creation establishes the initial key window. Do not reassert
    // activation here: doing so every pump slice steals focus from other
    // macOS applications and, after a close event, can immediately bring the
    // closing Android window back in front. AppKit naturally makes the
    // surface key again when the user explicitly clicks it.
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (deadline.timeIntervalSinceNow > 0.0) {
      if (surface->visible && !surface->window.visible) {
        [surface->view cancelPointerStream];
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
  if (surface->visible && !surface->window.visible) {
    [surface->view cancelPointerStream];
    return DARWIN_ART_SURFACE_WINDOW_CLOSED;
  }
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurfaceResult darwin_art_surface_pump_events(
    DarwinArtSurface* surface, double seconds) {
  return RunOnMainSync([&] { return PumpSurfaceEventsOnMain(surface, seconds); });
}

int32_t darwin_art_appkit_pump_events(double seconds) {
  if (!IsMainThread() || !std::isfinite(seconds) || seconds < 0.0 ||
      seconds > 30.0) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  @autoreleasepool {
    NSApplication* application = NSApplication.sharedApplication;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (deadline.timeIntervalSinceNow > 0.0) {
      const NSTimeInterval slice_seconds =
          std::min<NSTimeInterval>(0.016, deadline.timeIntervalSinceNow);
      NSDate* slice_deadline =
          [NSDate dateWithTimeIntervalSinceNow:slice_seconds];
      NSEvent* event = [application nextEventMatchingMask:NSEventMaskAny
                                                untilDate:slice_deadline
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
      if (event != nil) [application sendEvent:event];
      [application updateWindows];
    }
  }
  return DARWIN_ART_SURFACE_OK;
}

bool darwin_art_surface_next_pointer_event(
    DarwinArtSurface* surface,
    DarwinArtPointerEvent* out_event) {
  if (surface == nullptr || out_event == nullptr) {
    return false;
  }
  return [surface->view nextPointerEvent:out_event] == YES;
}

bool darwin_art_surface_next_pointer_event_v2(
    DarwinArtSurface* surface,
    DarwinArtPointerEventV2* out_event) {
  if (surface == nullptr || out_event == nullptr) {
    return false;
  }
  return [surface->view nextPointerEventV2:out_event] == YES;
}

bool darwin_art_surface_next_key_event_v1(
    DarwinArtSurface* surface,
    DarwinArtKeyEventV1* out_event) {
  if (surface == nullptr || out_event == nullptr) {
    return false;
  }
  return [surface->view nextKeyEventV1:out_event] == YES;
}

static DarwinArtSurfaceResult DestroySurfaceOnMain(
    DarwinArtSurface* surface) {
  if (!IsMainThread()) {
    return DARWIN_ART_SURFACE_NOT_MAIN_THREAD;
  }
  if (surface == nullptr) {
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  darwin_art_surface_gpu_forget(surface);
  if (g_active_gpu_surface == surface) g_active_gpu_surface = nullptr;
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

DarwinArtSurfaceResult darwin_art_surface_destroy(DarwinArtSurface* surface) {
  return RunOnMainSync([&] { return DestroySurfaceOnMain(surface); });
}
