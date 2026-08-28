#include "darwin_android_platform.h"

#include "darwin_angle_egl.h"
#include "darwin_art_bionic_socket_broker.h"

#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/choreographer.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/sensor.h>
#include <android/surface_control.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" int darwin_art_bionic_errno_set_from_darwin(int error);
extern "C" int darwin_art_bionic_socket_broker_pipe(
    int32_t descriptors[2]);
extern "C" intptr_t darwin_art_bionic_socket_broker_write(
    int fd, const void* bytes, size_t count);
extern "C" int darwin_art_bionic_socket_broker_close(int fd);
using DarwinArtSharedMemoryIoctl = int (*)(int, uint32_t, void*, int*, int*);
extern "C" int darwin_art_bionic_ioctl_bind_shared_memory(
    DarwinArtSharedMemoryIoctl callback);
extern "C" int darwin_art_android_shared_memory_ioctl(
    int fd, uint32_t request, void* argument, int* result,
    int* android_errno);

struct DarwinAndroidNativeBaseAbi {
  int32_t magic = ('_' << 24) | ('b' << 16) | ('f' << 8) | 'r';
  int32_t version = 0;
  void* reserved[4]{};
  void (*inc_ref)(DarwinAndroidNativeBaseAbi*) = nullptr;
  void (*dec_ref)(DarwinAndroidNativeBaseAbi*) = nullptr;
};

struct DarwinAndroidNativeWindowBufferAbi {
  DarwinAndroidNativeBaseAbi common{};
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t format = 0;
  int32_t usage_deprecated = 0;
  uintptr_t layer_count = 0;
  void* reserved[1]{};
  const void* handle = nullptr;
  uint64_t usage = 0;
  void* reserved_proc[7]{};
};

struct AHardwareBuffer {
  // Android implements AHardwareBuffer as a GraphicBuffer.  Its public EGL
  // client-buffer view is the embedded ANativeWindowBuffer at +0x10 on
  // arm64.  Native consumers such as Chromium's bundled ANGLE read this ABI
  // directly to determine dimensions, format and usage.
  void* graphic_buffer_prefix[2]{};
  DarwinAndroidNativeWindowBufferAbi native_buffer{};
  std::atomic<uint32_t> references{1};
  AHardwareBuffer_Desc description{};
  IOSurfaceRef surface = nullptr;
  std::mutex mutex;
  uint32_t locks = 0;
};

static_assert(offsetof(AHardwareBuffer, native_buffer) == 0x10);

AHardwareBuffer* HardwareBufferFromNativeBase(
    DarwinAndroidNativeBaseAbi* base) {
  return base == nullptr
             ? nullptr
             : reinterpret_cast<AHardwareBuffer*>(
                   reinterpret_cast<char*>(base) -
                   offsetof(AHardwareBuffer, native_buffer));
}

void HardwareBufferNativeIncRef(DarwinAndroidNativeBaseAbi* base) {
  AHardwareBuffer_acquire(HardwareBufferFromNativeBase(base));
}

void HardwareBufferNativeDecRef(DarwinAndroidNativeBaseAbi* base) {
  AHardwareBuffer_release(HardwareBufferFromNativeBase(base));
}

struct AInputEvent {
  uint32_t magic = 0x44414945u;
  std::atomic<uint32_t> references{1};
  int32_t type = AINPUT_EVENT_TYPE_MOTION;
  int32_t source = AINPUT_SOURCE_TOUCHSCREEN;
  int32_t action = 0;
  int32_t meta_state = 0;
  int32_t button_state = 0;
  int32_t classification = 0;
  int64_t down_time = 0;
  int64_t event_time = 0;
  int32_t pointer_id = 0;
  int32_t tool_type = AMOTION_EVENT_TOOL_TYPE_FINGER;
  float x = 0;
  float y = 0;
  float raw_x = 0;
  float raw_y = 0;
  float pressure = 1;
  float touch_major = 1;
  float touch_minor = 1;
  float orientation = 0;
};

struct ALooperRegistration {
  int fd;
  int ident;
  int events;
  ALooper_callbackFunc callback;
  void* data;
};

enum class ChoreographerCallbackKind {
  kFrame,
  kFrame64,
  kVsync,
  kRefreshRate,
};

struct ChoreographerCallback {
  std::chrono::steady_clock::time_point deadline;
  ChoreographerCallbackKind kind;
  void* callback;
  void* data;
};

struct ALooper {
  std::atomic<uint32_t> references{1};
  int options = 0;
  int wake_fd = -1;
  std::mutex mutex;
  std::vector<ALooperRegistration> registrations;
  std::vector<ChoreographerCallback> frame_callbacks;
};

struct AChoreographer {
  ALooper* looper = nullptr;
  std::mutex mutex;
  std::vector<std::pair<AChoreographer_refreshRateCallback, void*>>
      refresh_callbacks;
};

struct AChoreographerFrameCallbackData {
  int64_t frame_time_nanos = 0;
  AVsyncId vsync_id = 0;
  int64_t expected_presentation_time_nanos = 0;
  int64_t deadline_nanos = 0;
};

thread_local ALooper* g_thread_looper = nullptr;
thread_local AChoreographer* g_thread_choreographer = nullptr;

struct ASensorManager {};
struct ASensorEventQueue {};
struct ASensor {};
ASensorManager g_sensor_manager;

namespace {

struct SharedMemoryState {
  size_t size;
  int protection;
};

struct SharedMemoryMarker {
  uint64_t magic;
  uint64_t size;
  int32_t protection;
  uint32_t reserved;
};

constexpr uint64_t kSharedMemoryMarkerMagic = UINT64_C(0x444152544153484d);
constexpr char kSharedMemoryMarkerName[] = "com.darwinart.ashmem";

bool WriteSharedMemoryMarker(int fd, const SharedMemoryState& state) {
  const SharedMemoryMarker marker{kSharedMemoryMarkerMagic,
                                  static_cast<uint64_t>(state.size),
                                  state.protection, 0};
  return fsetxattr(fd, kSharedMemoryMarkerName, &marker, sizeof(marker), 0, 0) ==
         0;
}

bool ReadSharedMemoryMarker(int fd, SharedMemoryState* state) {
  if (state == nullptr) return false;
  SharedMemoryMarker marker{};
  const ssize_t size =
      fgetxattr(fd, kSharedMemoryMarkerName, &marker, sizeof(marker), 0, 0);
  if (size != static_cast<ssize_t>(sizeof(marker)) ||
      marker.magic != kSharedMemoryMarkerMagic || marker.size == 0 ||
      marker.size > std::numeric_limits<size_t>::max()) {
    return false;
  }
  constexpr int kAndroidProtectionMask = 0x1 | 0x2 | 0x4;
  if ((marker.protection & ~kAndroidProtectionMask) != 0) return false;
  *state = SharedMemoryState{static_cast<size_t>(marker.size),
                             marker.protection};
  return true;
}

std::mutex g_shared_memory_mutex;
std::unordered_map<int, SharedMemoryState> g_shared_memory;
std::mutex g_hardware_buffer_mutex;
std::unordered_map<const void*, AHardwareBuffer*> g_hardware_buffer_aliases;

constexpr uint32_t kDarwinBgraPixelFormat =
    (static_cast<uint32_t>('B') << 24) |
    (static_cast<uint32_t>('G') << 16) |
    (static_cast<uint32_t>('R') << 8) | static_cast<uint32_t>('A');

uint32_t BytesPerPixel(uint32_t format) {
  switch (format) {
    case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
      return 2;
    case AHARDWAREBUFFER_FORMAT_BLOB:
      return 1;
    default:
      return 4;
  }
}

AHardwareBuffer* WrapSurface(IOSurfaceRef surface,
                             const AHardwareBuffer_Desc& description) {
  if (surface == nullptr) return nullptr;
  auto* buffer = new (std::nothrow) AHardwareBuffer();
  if (buffer == nullptr) return nullptr;
  buffer->description = description;
  buffer->description.stride = static_cast<uint32_t>(
      IOSurfaceGetBytesPerRow(surface) / BytesPerPixel(description.format));
  buffer->native_buffer.common.version =
      sizeof(DarwinAndroidNativeWindowBufferAbi);
  buffer->native_buffer.common.inc_ref = &HardwareBufferNativeIncRef;
  buffer->native_buffer.common.dec_ref = &HardwareBufferNativeDecRef;
  buffer->native_buffer.width = static_cast<int>(description.width);
  buffer->native_buffer.height = static_cast<int>(description.height);
  buffer->native_buffer.stride = static_cast<int>(buffer->description.stride);
  buffer->native_buffer.format = static_cast<int>(description.format);
  buffer->native_buffer.usage_deprecated =
      static_cast<int>(description.usage & UINT32_MAX);
  buffer->native_buffer.layer_count = description.layers;
  buffer->native_buffer.handle = nullptr;
  buffer->native_buffer.usage = description.usage;
  buffer->surface = surface;
  {
    std::lock_guard<std::mutex> lock(g_hardware_buffer_mutex);
    g_hardware_buffer_aliases.emplace(buffer, buffer);
    // Android's EGL implementation exposes the embedded
    // ANativeWindowBuffer/GraphicBuffer client view rather than the owning
    // AHardwareBuffer address. ANGLE's host helper preserves that ABI offset.
    g_hardware_buffer_aliases.emplace(&buffer->native_buffer, buffer);
  }
  return buffer;
}

struct SurfaceControl {
  std::atomic<uint32_t> references{1};
  std::string name;
  SurfaceControl* parent = nullptr;
  // A control obtained from a Java Surface or an ANativeWindow is attached
  // to SurfaceFlinger's display tree even though its NDK parent is null.
  // Controls created through ASurfaceControl_create are ordinary children;
  // reparent(child, nullptr) detaches them and they must stop contributing to
  // composition until attached to a rooted ancestor again.
  bool composition_root = false;
  AHardwareBuffer* buffer = nullptr;
  ARect source{};
  ARect destination{};
  ARect crop{};
  bool has_geometry = false;
  bool has_crop = false;
  bool visible = true;
  int32_t position_x = 0;
  int32_t position_y = 0;
  int32_t transform = 0;
  int32_t z_order = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float alpha = 1.0f;
};

std::mutex g_surface_controls_mutex;
std::vector<SurfaceControl*> g_surface_controls;

bool IsAttachedToCompositionRoot(
    const SurfaceControl* control,
    const std::vector<SurfaceControl*>& controls) {
  // Parent links are runtime-owned identities. Bound the walk by the number
  // of live controls so a malformed/cyclic guest transaction fails detached
  // rather than looping forever or following a released pointer.
  for (size_t depth = 0; control != nullptr && depth <= controls.size();
       ++depth) {
    if (std::find(controls.begin(), controls.end(), control) == controls.end())
      return false;
    if (control->composition_root) return true;
    control = control->parent;
  }
  return false;
}

struct SurfacePresentation {
  SurfaceControl* control = nullptr;
  AHardwareBuffer* buffer = nullptr;
  ARect source{};
  ARect destination{};
  float alpha = 1.0f;
  int32_t z_order = 0;
  std::string name;
  int32_t transform = 0;
  bool has_damage = false;
  ARect damage{};
};

SurfacePresentation MakePresentation(SurfaceControl* control) {
  const auto& description = control->buffer->description;
  const ARect source = control->has_geometry
                           ? control->source
                           : (control->has_crop
                                  ? control->crop
                                  : ARect{0, 0,
                                          static_cast<int32_t>(description.width),
                                          static_cast<int32_t>(description.height)});
  const ARect destination = control->has_geometry
                                ? control->destination
                                : ARect{
                                      control->position_x,
                                      control->position_y,
                                      control->position_x +
                                          static_cast<int32_t>(
                                              (source.right - source.left) *
                                              control->scale_x),
                                      control->position_y +
                                          static_cast<int32_t>(
                                              (source.bottom - source.top) *
                                              control->scale_y)};
  AHardwareBuffer_acquire(control->buffer);
  return {.control = control,
          .buffer = control->buffer,
          .source = source,
          .destination = destination,
          .alpha = control->alpha,
          .z_order = control->z_order,
          .name = control->name,
          .transform = control->transform};
}

void SetPresentationDamage(SurfacePresentation* presentation,
                           const std::vector<ARect>& damage) {
  if (presentation == nullptr || damage.empty()) return;
  ARect bounds = damage.front();
  for (const ARect& rect : damage) {
    bounds.left = std::min(bounds.left, rect.left);
    bounds.top = std::min(bounds.top, rect.top);
    bounds.right = std::max(bounds.right, rect.right);
    bounds.bottom = std::max(bounds.bottom, rect.bottom);
  }
  // Android keeps layer geometry unchanged. Damage is buffer-coordinate
  // metadata used only to bound recomposition; it is not a crop or a frame.
  presentation->has_damage = true;
  presentation->damage = bounds;
}

struct SurfaceTransactionStats {
  std::vector<ASurfaceControl*> controls;
  std::unordered_map<ASurfaceControl*, AHardwareBuffer*> previous_buffers;

  ~SurfaceTransactionStats() {
    for (const auto& [control, buffer] : previous_buffers) {
      (void)control;
      AHardwareBuffer_release(buffer);
    }
  }
};

int CreateSignaledSurfaceFence() {
  int32_t descriptors[2]{-1, -1};
  constexpr uint64_t kSignaledMetalFence =
      UINT64_C(0x44415257494e4653);  // "DARWINFS"
  if (darwin_art_bionic_socket_broker_pipe(descriptors) != 0) return -1;
  const intptr_t written = darwin_art_bionic_socket_broker_write(
      descriptors[1], &kSignaledMetalFence, sizeof(kSignaledMetalFence));
  (void)darwin_art_bionic_socket_broker_close(descriptors[1]);
  if (written != static_cast<intptr_t>(sizeof(kSignaledMetalFence))) {
    (void)darwin_art_bionic_socket_broker_close(descriptors[0]);
    return -1;
  }
  return descriptors[0];
}

using TransactionCallback = void (*)(void*, ASurfaceTransactionStats*);

struct SurfaceTransaction {
  struct Update {
    ASurfaceControl* opaque = nullptr;
    AHardwareBuffer* buffer = nullptr;
    bool has_buffer = false;
    ARect source{};
    ARect destination{};
    ARect crop{};
    bool has_geometry = false;
    bool has_crop = false;
    bool has_visibility = false;
    bool visible = true;
    bool has_position = false;
    int32_t position_x = 0;
    int32_t position_y = 0;
    bool has_transform = false;
    int32_t transform = 0;
    bool has_z_order = false;
    int32_t z_order = 0;
    bool has_scale = false;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    bool has_alpha = false;
    float alpha = 1.0f;
    bool has_parent = false;
    ASurfaceControl* parent = nullptr;
    bool has_damage = false;
    std::vector<ARect> damage;
  };

  std::vector<ASurfaceControl*> controls;
  std::vector<Update> updates;
  TransactionCallback commit = nullptr;
  void* commit_context = nullptr;
  TransactionCallback complete = nullptr;
  void* complete_context = nullptr;
};

SurfaceTransaction::Update* FindUpdate(SurfaceTransaction* transaction,
                                       ASurfaceControl* control) {
  if (transaction == nullptr || control == nullptr) return nullptr;
  auto found = std::find_if(
      transaction->updates.begin(), transaction->updates.end(),
      [control](const SurfaceTransaction::Update& update) {
        return update.opaque == control;
      });
  if (found != transaction->updates.end()) return &*found;
  transaction->updates.push_back({.opaque = control});
  return &transaction->updates.back();
}

ASurfaceControl* CreateSurfaceControl(ASurfaceControl* parent,
                                      const char* name,
                                      bool composition_root) {
  auto* control = new (std::nothrow) SurfaceControl();
  if (control != nullptr) {
    if (name != nullptr) control->name = name;
    control->parent = reinterpret_cast<SurfaceControl*>(parent);
    control->composition_root = composition_root;
    std::lock_guard<std::mutex> lock(g_surface_controls_mutex);
    g_surface_controls.push_back(control);
    if (std::getenv("DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS") != nullptr) {
      std::fprintf(stderr,
                   "ART Android SurfaceControl: create pid=%d control=%p "
                   "parent=%p root=%d name=%s\n",
                   getpid(), static_cast<void*>(control),
                   static_cast<void*>(parent), composition_root ? 1 : 0,
                   control->name.c_str());
    }
  }
  return reinterpret_cast<ASurfaceControl*>(control);
}

bool DebugSurfaceTransactions() {
  static const bool enabled =
      std::getenv("DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS") != nullptr;
  return enabled;
}

void Remember(SurfaceTransaction* transaction, ASurfaceControl* control) {
  if (transaction == nullptr || control == nullptr) return;
  if (std::find(transaction->controls.begin(), transaction->controls.end(),
                control) == transaction->controls.end()) {
    transaction->controls.push_back(control);
  }
  (void)FindUpdate(transaction, control);
}

void ReleaseTransactionBuffers(SurfaceTransaction* transaction) {
  if (transaction == nullptr) return;
  for (auto& update : transaction->updates) {
    if (update.buffer != nullptr) AHardwareBuffer_release(update.buffer);
    update.buffer = nullptr;
  }
  transaction->updates.clear();
}

void NoopServiceCallback(void*) {}

}  // namespace

extern "C" int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc,
                                         AHardwareBuffer** out) {
  if (desc == nullptr || out == nullptr || desc->width == 0 ||
      desc->height == 0 || desc->layers != 1) {
    return -EINVAL;
  }
  *out = nullptr;
  const uint32_t bytes_per_pixel = BytesPerPixel(desc->format);
  const size_t row_bytes = static_cast<size_t>(desc->width) * bytes_per_pixel;
  if (row_bytes / bytes_per_pixel != desc->width ||
      row_bytes > SIZE_MAX / desc->height) {
    return -EOVERFLOW;
  }
  NSDictionary* properties = @{
    (__bridge NSString*)kIOSurfaceWidth : @(desc->width),
    (__bridge NSString*)kIOSurfaceHeight : @(desc->height),
    (__bridge NSString*)kIOSurfaceBytesPerElement : @(bytes_per_pixel),
    (__bridge NSString*)kIOSurfaceBytesPerRow : @(row_bytes),
    (__bridge NSString*)kIOSurfaceAllocSize : @(row_bytes * desc->height),
    (__bridge NSString*)kIOSurfacePixelFormat : @(kDarwinBgraPixelFormat),
    (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
  };
  IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
  if (surface == nullptr) return -ENOMEM;
  AHardwareBuffer* buffer = WrapSurface(surface, *desc);
  if (buffer == nullptr) {
    CFRelease(surface);
    return -ENOMEM;
  }
  *out = buffer;
  if (std::getenv("DARWIN_ART_DEBUG_GRAPHICS_DSO") != nullptr) {
    std::fprintf(stderr,
                 "ART Android AHardwareBuffer: allocate buffer=%p surface=%p "
                 "size=%ux%u format=%u usage=%llu\n",
                 static_cast<void*>(buffer), static_cast<void*>(surface),
                 desc->width, desc->height, desc->format,
                 static_cast<unsigned long long>(desc->usage));
  }
  return 0;
}

extern "C" void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
  if (buffer != nullptr) buffer->references.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void* darwin_art_android_hardware_buffer_metal_texture(
    AHardwareBuffer* buffer, void* metal_device) {
  if (buffer == nullptr || buffer->surface == nullptr || metal_device == nullptr ||
      buffer->description.width == 0 || buffer->description.height == 0) {
    return nullptr;
  }
  return darwin_art_android_iosurface_metal_texture(
      buffer->surface, buffer->description.width, buffer->description.height,
      metal_device);
}

extern "C" void* darwin_art_android_iosurface_metal_texture(
    void* iosurface, uint32_t width, uint32_t height, void* metal_device) {
  if (iosurface == nullptr || metal_device == nullptr || width == 0 ||
      height == 0) {
    return nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                     MTLTextureUsageRenderTarget;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor
                                                  iosurface:(IOSurfaceRef)iosurface
                                                      plane:0];
  // newTextureWithDescriptor returns a +1 object in this non-ARC translation
  // unit; transfer that ownership to the opaque C handle.
  return reinterpret_cast<void*>(texture);
}

extern "C" void darwin_art_android_metal_texture_release(void* texture) {
  if (texture != nullptr) CFRelease(texture);
}

extern "C" void AHardwareBuffer_release(AHardwareBuffer* buffer) {
  if (buffer != nullptr &&
      buffer->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    {
      std::lock_guard<std::mutex> lock(g_hardware_buffer_mutex);
      std::erase_if(g_hardware_buffer_aliases,
                    [buffer](const auto& entry) {
                      return entry.second == buffer;
                    });
    }
    if (buffer->surface != nullptr) CFRelease(buffer->surface);
    delete buffer;
  }
}

extern "C" AHardwareBuffer*
darwin_art_android_hardware_buffer_from_client_buffer(void* client_buffer) {
  std::lock_guard<std::mutex> lock(g_hardware_buffer_mutex);
  auto found = g_hardware_buffer_aliases.find(client_buffer);
  return found == g_hardware_buffer_aliases.end() ? nullptr : found->second;
}

extern "C" void* darwin_art_android_hardware_buffer_iosurface(
    AHardwareBuffer* buffer) {
  if (std::getenv("DARWIN_ART_DEBUG_GRAPHICS_DSO") != nullptr) {
    std::fprintf(stderr,
                 "ART Android AHardwareBuffer: iosurface buffer=%p surface=%p\n",
                 static_cast<void*>(buffer),
                 buffer == nullptr ? nullptr : static_cast<void*>(buffer->surface));
  }
  return buffer == nullptr ? nullptr : buffer->surface;
}

extern "C" void AHardwareBuffer_describe(const AHardwareBuffer* buffer,
                                           AHardwareBuffer_Desc* out) {
  if (buffer != nullptr && out != nullptr) *out = buffer->description;
}

extern "C" int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t,
                                     int32_t, const ARect*, void** out) {
  if (buffer == nullptr || out == nullptr || buffer->surface == nullptr)
    return -EINVAL;
  std::lock_guard<std::mutex> lock(buffer->mutex);
  if (buffer->locks++ == 0 &&
      IOSurfaceLock(buffer->surface, 0, nullptr) != kIOReturnSuccess) {
    --buffer->locks;
    return -EIO;
  }
  *out = IOSurfaceGetBaseAddress(buffer->surface);
  return *out == nullptr ? -EIO : 0;
}

extern "C" int AHardwareBuffer_lockPlanes(AHardwareBuffer* buffer,
                                            uint64_t usage, int32_t fence,
                                            const ARect* rect,
                                            AHardwareBuffer_Planes* out) {
  if (out == nullptr) return -EINVAL;
  std::memset(out, 0, sizeof(*out));
  void* data = nullptr;
  const int result = AHardwareBuffer_lock(buffer, usage, fence, rect, &data);
  if (result != 0) return result;
  out->planeCount = 1;
  out->planes[0].data = data;
  out->planes[0].pixelStride = BytesPerPixel(buffer->description.format);
  out->planes[0].rowStride =
      static_cast<uint32_t>(IOSurfaceGetBytesPerRow(buffer->surface));
  return 0;
}

extern "C" int AHardwareBuffer_unlock(AHardwareBuffer* buffer,
                                       int32_t* fence) {
  if (buffer == nullptr || buffer->surface == nullptr) return -EINVAL;
  std::lock_guard<std::mutex> lock(buffer->mutex);
  if (buffer->locks == 0) return -EINVAL;
  if (--buffer->locks == 0 &&
      IOSurfaceUnlock(buffer->surface, 0, nullptr) != kIOReturnSuccess)
    return -EIO;
  if (fence != nullptr) *fence = -1;
  return 0;
}

struct HardwareBufferWire {
  uint32_t magic;
  uint32_t surface_id;
  AHardwareBuffer_Desc description;
};

extern "C" int AHardwareBuffer_sendHandleToUnixSocket(
    const AHardwareBuffer* buffer, int socket_fd) {
  if (buffer == nullptr || buffer->surface == nullptr) return -EINVAL;
  const HardwareBufferWire wire{0x44414842u, IOSurfaceGetID(buffer->surface),
                                buffer->description};
  const intptr_t written =
      darwin_art_bionic_socket_broker_write(socket_fd, &wire, sizeof(wire));
  return written == sizeof(wire) ? 0 : -EIO;
}

extern "C" int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd,
                                                          AHardwareBuffer** out) {
  if (out == nullptr) return -EINVAL;
  *out = nullptr;
  HardwareBufferWire wire{};
  const intptr_t read =
      darwin_art_bionic_socket_broker_read(socket_fd, &wire, sizeof(wire));
  if (read != sizeof(wire) || wire.magic != 0x44414842u) return -EIO;
  IOSurfaceRef surface = IOSurfaceLookup(wire.surface_id);
  if (surface == nullptr) return -ENOENT;
  *out = WrapSurface(surface, wire.description);
  if (*out == nullptr) {
    CFRelease(surface);
    return -ENOMEM;
  }
  return 0;
}

extern "C" AHardwareBuffer* AHardwareBuffer_fromHardwareBuffer(JNIEnv*,
                                                                 jobject) {
  return nullptr;
}

namespace {
const AInputEvent* Input(const AInputEvent* event) {
  return event != nullptr && event->magic == 0x44414945u ? event : nullptr;
}
float MotionAxis(const AInputEvent* event, int32_t axis) {
  event = Input(event);
  if (event == nullptr) return 0;
  switch (axis) {
    case AMOTION_EVENT_AXIS_X: return event->x;
    case AMOTION_EVENT_AXIS_Y: return event->y;
    case AMOTION_EVENT_AXIS_PRESSURE: return event->pressure;
    case AMOTION_EVENT_AXIS_TOUCH_MAJOR: return event->touch_major;
    case AMOTION_EVENT_AXIS_TOUCH_MINOR: return event->touch_minor;
    case AMOTION_EVENT_AXIS_ORIENTATION: return event->orientation;
    default: return 0;
  }
}

int64_t MonotonicNanos() {
  // AChoreographer timestamps use the same CLOCK_MONOTONIC timebase exposed
  // by Android's clock_gettime. On Darwin, libc++ steady_clock is based on a
  // different Mach clock whose suspend offset can differ by seconds; exposing
  // it to Android makes Chromium reject begin frames as being in the future.
  struct timespec now {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

int DispatchDueFrameCallbacks(ALooper* looper) {
  if (looper == nullptr) return 0;
  std::vector<ChoreographerCallback> callbacks;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(looper->mutex);
    auto callback = looper->frame_callbacks.begin();
    while (callback != looper->frame_callbacks.end()) {
      if (callback->deadline > now) {
        ++callback;
        continue;
      }
      callbacks.push_back(*callback);
      callback = looper->frame_callbacks.erase(callback);
    }
  }
  static std::atomic<AVsyncId> next_vsync_id{1};
  static std::atomic<uint32_t> dispatched_count{0};
  for (const ChoreographerCallback& callback : callbacks) {
    const int64_t frame_time = MonotonicNanos();
    const uint32_t sequence =
        dispatched_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (std::getenv("DARWIN_ART_DEBUG_CHOREOGRAPHER") != nullptr &&
        sequence <= 64) {
      std::fprintf(stderr,
                   "ART Android Choreographer: dispatch sequence=%u kind=%u "
                   "frame_ns=%lld pid=%d\n",
                   sequence, static_cast<unsigned>(callback.kind),
                   static_cast<long long>(frame_time), getpid());
    }
    switch (callback.kind) {
      case ChoreographerCallbackKind::kFrame:
        reinterpret_cast<AChoreographer_frameCallback>(callback.callback)(
            static_cast<long>(frame_time), callback.data);
        break;
      case ChoreographerCallbackKind::kFrame64:
        reinterpret_cast<AChoreographer_frameCallback64>(callback.callback)(
            frame_time, callback.data);
        break;
      case ChoreographerCallbackKind::kVsync: {
        AChoreographerFrameCallbackData data{
            frame_time, next_vsync_id.fetch_add(1, std::memory_order_relaxed),
            frame_time + 16'666'667, frame_time + 15'000'000};
        reinterpret_cast<AChoreographer_vsyncCallback>(callback.callback)(
            &data, callback.data);
        break;
      }
      case ChoreographerCallbackKind::kRefreshRate:
        reinterpret_cast<AChoreographer_refreshRateCallback>(callback.callback)(
            16'666'667, callback.data);
        break;
    }
  }
  return static_cast<int>(callbacks.size());
}

int NextFrameCallbackDelayMillis(ALooper* looper) {
  if (looper == nullptr) return -1;
  std::lock_guard<std::mutex> lock(looper->mutex);
  if (looper->frame_callbacks.empty()) return -1;
  const auto next = std::min_element(
      looper->frame_callbacks.begin(), looper->frame_callbacks.end(),
      [](const auto& left, const auto& right) {
        return left.deadline < right.deadline;
      });
  const auto delay = next->deadline - std::chrono::steady_clock::now();
  if (delay <= std::chrono::steady_clock::duration::zero()) return 0;
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();
  return static_cast<int>(std::min<int64_t>(milliseconds + 1, INT_MAX));
}

void ScheduleChoreographerCallback(AChoreographer* choreographer,
                                   ChoreographerCallbackKind kind,
                                   void* callback, void* data,
                                   uint32_t delay_millis) {
  if (choreographer == nullptr || choreographer->looper == nullptr ||
      callback == nullptr)
    return;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(delay_millis) +
                        (delay_millis == 0 ? std::chrono::milliseconds(16)
                                           : std::chrono::milliseconds(0));
  static std::atomic<uint32_t> scheduled_count{0};
  const uint32_t sequence =
      scheduled_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (std::getenv("DARWIN_ART_DEBUG_CHOREOGRAPHER") != nullptr &&
      sequence <= 64) {
    std::fprintf(stderr,
                 "ART Android Choreographer: schedule sequence=%u kind=%u "
                 "delay_ms=%u pid=%d\n",
                 sequence, static_cast<unsigned>(kind), delay_millis,
                 getpid());
  }
  {
    std::lock_guard<std::mutex> lock(choreographer->looper->mutex);
    choreographer->looper->frame_callbacks.push_back(
        {deadline, kind, callback, data});
  }
  ALooper_wake(choreographer->looper);
}
}  // namespace

extern "C" int32_t AInputEvent_getType(const AInputEvent* event) {
  event = Input(event); return event == nullptr ? 0 : event->type;
}
extern "C" int32_t AInputEvent_getSource(const AInputEvent* event) {
  event = Input(event); return event == nullptr ? 0 : event->source;
}
extern "C" void AInputEvent_release(const AInputEvent* event) {
  auto* owned = const_cast<AInputEvent*>(Input(event));
  if (owned != nullptr &&
      owned->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
    delete owned;
}
extern "C" int32_t AMotionEvent_getAction(const AInputEvent* e) { e=Input(e); return e?e->action:0; }
extern "C" int32_t AMotionEvent_getMetaState(const AInputEvent* e) { e=Input(e); return e?e->meta_state:0; }
extern "C" int32_t AMotionEvent_getButtonState(const AInputEvent* e) { e=Input(e); return e?e->button_state:0; }
extern "C" int32_t AMotionEvent_getClassification(const AInputEvent* e) { e=Input(e); return e?e->classification:0; }
extern "C" int64_t AMotionEvent_getDownTime(const AInputEvent* e) { e=Input(e); return e?e->down_time:0; }
extern "C" int64_t AMotionEvent_getEventTime(const AInputEvent* e) { e=Input(e); return e?e->event_time:0; }
extern "C" size_t AMotionEvent_getPointerCount(const AInputEvent* e) { return Input(e)?1:0; }
extern "C" int32_t AMotionEvent_getPointerId(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->pointer_id:-1; }
extern "C" int32_t AMotionEvent_getToolType(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->tool_type:AMOTION_EVENT_TOOL_TYPE_UNKNOWN; }
extern "C" float AMotionEvent_getRawX(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->raw_x:0; }
extern "C" float AMotionEvent_getRawY(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->raw_y:0; }
extern "C" float AMotionEvent_getX(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->x:0; }
extern "C" float AMotionEvent_getY(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->y:0; }
extern "C" float AMotionEvent_getPressure(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->pressure:0; }
extern "C" float AMotionEvent_getTouchMajor(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->touch_major:0; }
extern "C" float AMotionEvent_getTouchMinor(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->touch_minor:0; }
extern "C" float AMotionEvent_getOrientation(const AInputEvent* e, size_t i) { e=Input(e); return e&&i==0?e->orientation:0; }
extern "C" float AMotionEvent_getAxisValue(const AInputEvent* e, int32_t axis, size_t i) { return i==0?MotionAxis(e,axis):0; }
extern "C" size_t AMotionEvent_getHistorySize(const AInputEvent*) { return 0; }
extern "C" int64_t AMotionEvent_getHistoricalEventTime(const AInputEvent*, size_t) { return 0; }
extern "C" float AMotionEvent_getHistoricalX(const AInputEvent*, size_t, size_t) { return 0; }
extern "C" float AMotionEvent_getHistoricalY(const AInputEvent*, size_t, size_t) { return 0; }
extern "C" float AMotionEvent_getHistoricalTouchMajor(const AInputEvent*, size_t, size_t) { return 0; }

extern "C" ALooper* ALooper_forThread() { return g_thread_looper; }
extern "C" ALooper* ALooper_prepare(int options) {
  if (g_thread_looper == nullptr) {
    g_thread_looper = new (std::nothrow) ALooper();
    if (g_thread_looper != nullptr) {
      g_thread_looper->options = options;
      // EFD_NONBLOCK | EFD_CLOEXEC in Android's ABI. The broker implements
      // eventfd over a datagram socketpair while preserving guest fd identity.
      g_thread_looper->wake_fd =
          darwin_art_bionic_socket_broker_eventfd(0, 0x80800);
      if (g_thread_looper->wake_fd < 0) {
        delete g_thread_looper;
        g_thread_looper = nullptr;
      }
    }
  }
  return g_thread_looper;
}
extern "C" void ALooper_acquire(ALooper* looper) {
  if (looper != nullptr) looper->references.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void ALooper_release(ALooper* looper) {
  // The thread association is a process-lifetime safety reference. This
  // mirrors ART's other opaque compatibility tokens and prevents a borrowed
  // ALooper_forThread pointer from becoming dangling.
  if (looper != nullptr && looper->references.load(std::memory_order_acquire) > 1)
    looper->references.fetch_sub(1, std::memory_order_acq_rel);
}
extern "C" int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                              ALooper_callbackFunc callback, void* data) {
  if (looper == nullptr || fd < 0 ||
      (callback == nullptr &&
       (looper->options & ALOOPER_PREPARE_ALLOW_NON_CALLBACKS) == 0))
    return -1;
  std::lock_guard<std::mutex> lock(looper->mutex);
  auto found = std::find_if(looper->registrations.begin(),
                            looper->registrations.end(),
                            [fd](const auto& value) { return value.fd == fd; });
  const ALooperRegistration registration{fd, ident, events, callback, data};
  if (found == looper->registrations.end())
    looper->registrations.push_back(registration);
  else
    *found = registration;
  return 1;
}
extern "C" int ALooper_removeFd(ALooper* looper, int fd) {
  if (looper == nullptr) return -1;
  std::lock_guard<std::mutex> lock(looper->mutex);
  const auto old_size = looper->registrations.size();
  std::erase_if(looper->registrations,
                [fd](const auto& value) { return value.fd == fd; });
  return looper->registrations.size() == old_size ? 0 : 1;
}
extern "C" void ALooper_wake(ALooper* looper) {
  if (looper == nullptr || looper->wake_fd < 0) return;
  const uint64_t value = 1;
  (void)darwin_art_bionic_socket_broker_write(looper->wake_fd, &value,
                                               sizeof(value));
}
extern "C" int ALooper_pollOnce(int timeout_ms, int* out_fd,
                                 int* out_events, void** out_data) {
  if (out_fd != nullptr) *out_fd = 0;
  if (out_events != nullptr) *out_events = 0;
  if (out_data != nullptr) *out_data = nullptr;
  ALooper* looper = g_thread_looper;
  if (looper == nullptr) return ALOOPER_POLL_ERROR;
  if (DispatchDueFrameCallbacks(looper) > 0) return ALOOPER_POLL_CALLBACK;
  const int frame_delay = NextFrameCallbackDelayMillis(looper);
  if (frame_delay >= 0 && (timeout_ms < 0 || frame_delay < timeout_ms))
    timeout_ms = frame_delay;
  std::vector<ALooperRegistration> registrations;
  {
    std::lock_guard<std::mutex> lock(looper->mutex);
    registrations = looper->registrations;
  }
  std::vector<DarwinArtBionicPollFd> descriptors;
  descriptors.reserve(registrations.size() + 1);
  descriptors.push_back({looper->wake_fd, 0x0001, 0});
  for (const auto& registration : registrations) {
    int16_t poll_events = 0;
    if ((registration.events & ALOOPER_EVENT_INPUT) != 0) poll_events |= 0x0001;
    if ((registration.events & ALOOPER_EVENT_OUTPUT) != 0) poll_events |= 0x0004;
    descriptors.push_back({registration.fd, poll_events, 0});
  }
  const int ready = darwin_art_bionic_socket_broker_poll(
      descriptors.data(), descriptors.size(), timeout_ms);
  if (ready < 0) return ALOOPER_POLL_ERROR;
  if (ready == 0) {
    return DispatchDueFrameCallbacks(looper) > 0 ? ALOOPER_POLL_CALLBACK
                                                 : ALOOPER_POLL_TIMEOUT;
  }
  if ((descriptors[0].revents & 0x0001) != 0) {
    uint64_t value = 0;
    (void)darwin_art_bionic_socket_broker_read(looper->wake_fd, &value,
                                                sizeof(value));
    return ALOOPER_POLL_WAKE;
  }
  bool invoked_callback = false;
  for (size_t index = 1; index < descriptors.size(); ++index) {
    const int16_t poll_events = descriptors[index].revents;
    if (poll_events == 0) continue;
    int events = 0;
    if ((poll_events & 0x0001) != 0) events |= ALOOPER_EVENT_INPUT;
    if ((poll_events & 0x0004) != 0) events |= ALOOPER_EVENT_OUTPUT;
    if ((poll_events & 0x0008) != 0) events |= ALOOPER_EVENT_ERROR;
    if ((poll_events & 0x0010) != 0) events |= ALOOPER_EVENT_HANGUP;
    if ((poll_events & 0x0020) != 0) events |= ALOOPER_EVENT_INVALID;
    const auto& registration = registrations[index - 1];
    if (registration.callback != nullptr) {
      invoked_callback = true;
      if (registration.callback(registration.fd, events, registration.data) == 0)
        ALooper_removeFd(looper, registration.fd);
    } else {
      if (out_fd != nullptr) *out_fd = registration.fd;
      if (out_events != nullptr) *out_events = events;
      if (out_data != nullptr) *out_data = registration.data;
      return registration.ident;
    }
  }
  return invoked_callback ? ALOOPER_POLL_CALLBACK : ALOOPER_POLL_WAKE;
}

extern "C" AChoreographer* AChoreographer_getInstance() {
  ALooper* looper = ALooper_forThread();
  if (looper == nullptr) return nullptr;
  if (g_thread_choreographer == nullptr) {
    g_thread_choreographer = new (std::nothrow) AChoreographer();
    if (g_thread_choreographer != nullptr)
      g_thread_choreographer->looper = looper;
  }
  return g_thread_choreographer;
}

extern "C" void AChoreographer_postFrameCallback(
    AChoreographer* choreographer, AChoreographer_frameCallback callback,
    void* data) {
  ScheduleChoreographerCallback(choreographer,
                               ChoreographerCallbackKind::kFrame,
                               reinterpret_cast<void*>(callback), data, 0);
}

extern "C" void AChoreographer_postFrameCallbackDelayed(
    AChoreographer* choreographer, AChoreographer_frameCallback callback,
    void* data, long delay_millis) {
  ScheduleChoreographerCallback(
      choreographer, ChoreographerCallbackKind::kFrame,
      reinterpret_cast<void*>(callback), data,
      delay_millis <= 0 ? 0 : static_cast<uint32_t>(delay_millis));
}

extern "C" void AChoreographer_postFrameCallback64(
    AChoreographer* choreographer, AChoreographer_frameCallback64 callback,
    void* data) {
  ScheduleChoreographerCallback(choreographer,
                               ChoreographerCallbackKind::kFrame64,
                               reinterpret_cast<void*>(callback), data, 0);
}

extern "C" void AChoreographer_postFrameCallbackDelayed64(
    AChoreographer* choreographer, AChoreographer_frameCallback64 callback,
    void* data, uint32_t delay_millis) {
  ScheduleChoreographerCallback(choreographer,
                               ChoreographerCallbackKind::kFrame64,
                               reinterpret_cast<void*>(callback), data,
                               delay_millis);
}

extern "C" void AChoreographer_postVsyncCallback(
    AChoreographer* choreographer, AChoreographer_vsyncCallback callback,
    void* data) {
  ScheduleChoreographerCallback(choreographer,
                               ChoreographerCallbackKind::kVsync,
                               reinterpret_cast<void*>(callback), data, 0);
}

extern "C" void AChoreographer_registerRefreshRateCallback(
    AChoreographer* choreographer,
    AChoreographer_refreshRateCallback callback, void* data) {
  if (choreographer == nullptr || callback == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(choreographer->mutex);
    choreographer->refresh_callbacks.emplace_back(callback, data);
  }
  ScheduleChoreographerCallback(choreographer,
                               ChoreographerCallbackKind::kRefreshRate,
                               reinterpret_cast<void*>(callback), data, 0);
}

extern "C" void AChoreographer_unregisterRefreshRateCallback(
    AChoreographer* choreographer,
    AChoreographer_refreshRateCallback callback, void* data) {
  if (choreographer == nullptr) return;
  std::lock_guard<std::mutex> lock(choreographer->mutex);
  std::erase(choreographer->refresh_callbacks, std::make_pair(callback, data));
}

extern "C" int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(
    const AChoreographerFrameCallbackData* data) {
  return data == nullptr ? 0 : data->frame_time_nanos;
}
extern "C" size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(
    const AChoreographerFrameCallbackData*) { return 1; }
extern "C" size_t
AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(
    const AChoreographerFrameCallbackData*) { return 0; }
extern "C" AVsyncId AChoreographerFrameCallbackData_getFrameTimelineVsyncId(
    const AChoreographerFrameCallbackData* data, size_t index) {
  return data == nullptr || index != 0 ? -1 : data->vsync_id;
}
extern "C" int64_t
AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
    const AChoreographerFrameCallbackData* data, size_t index) {
  return data == nullptr || index != 0 ? 0
                                      : data->expected_presentation_time_nanos;
}
extern "C" int64_t
AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(
    const AChoreographerFrameCallbackData* data, size_t index) {
  return data == nullptr || index != 0 ? 0 : data->deadline_nanos;
}

extern "C" int darwin_art_android_platform_poll_current_looper() {
  if (g_thread_looper == nullptr) return 0;
  int dispatched = 0;
  for (int iteration = 0; iteration < 128; ++iteration) {
    const int result = ALooper_pollOnce(0, nullptr, nullptr, nullptr);
    if (result == ALOOPER_POLL_CALLBACK || result == ALOOPER_POLL_WAKE) {
      ++dispatched;
      continue;
    }
    if (result == ALOOPER_POLL_TIMEOUT) return dispatched;
    return result == ALOOPER_POLL_ERROR ? -1 : dispatched;
  }
  return dispatched;
}

extern "C" void* darwin_art_android_platform_prepare_current_looper() {
  return ALooper_prepare(0);
}

extern "C" int darwin_art_android_platform_poll_current_looper_timeout(
    int timeout_ms) {
  return ALooper_pollOnce(timeout_ms, nullptr, nullptr, nullptr);
}

extern "C" void darwin_art_android_platform_wake_looper(void* looper) {
  ALooper_wake(static_cast<ALooper*>(looper));
}

extern "C" ASensorManager* ASensorManager_getInstanceForPackage(const char*) {
  return &g_sensor_manager;
}
extern "C" const ASensor* ASensorManager_getDefaultSensor(ASensorManager*, int) {
  return nullptr;
}
extern "C" ASensorEventQueue* ASensorManager_createEventQueue(
    ASensorManager* manager, ALooper* looper, int, ALooper_callbackFunc, void*) {
  if (manager == nullptr || looper == nullptr) return nullptr;
  return new (std::nothrow) ASensorEventQueue();
}
extern "C" int ASensorManager_destroyEventQueue(ASensorManager*, ASensorEventQueue* queue) {
  delete queue; return 0;
}
extern "C" int ASensorEventQueue_enableSensor(ASensorEventQueue*, const ASensor*) { return -EINVAL; }
extern "C" int ASensorEventQueue_disableSensor(ASensorEventQueue*, const ASensor*) { return -EINVAL; }
extern "C" int ASensorEventQueue_setEventRate(ASensorEventQueue*, const ASensor*, int32_t) { return -EINVAL; }
extern "C" ssize_t ASensorEventQueue_getEvents(ASensorEventQueue*, ASensorEvent*, size_t) { return 0; }
extern "C" int ASensor_getMinDelay(ASensor const*) { return 0; }

extern "C" ASurfaceControl* ASurfaceControl_createFromWindow(ANativeWindow*,
                                                               const char* name) {
  return CreateSurfaceControl(nullptr, name, true);
}

extern "C" ASurfaceControl* ASurfaceControl_create(ASurfaceControl* parent,
                                                     const char* name) {
  return CreateSurfaceControl(parent, name, false);
}

extern "C" ASurfaceControl* ASurfaceControl_fromJava(JNIEnv*, jobject) {
  return CreateSurfaceControl(nullptr, "java-surface-control", true);
}

extern "C" void ASurfaceControl_release(ASurfaceControl* opaque) {
  auto* control = reinterpret_cast<SurfaceControl*>(opaque);
  if (control != nullptr &&
      control->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    {
      std::lock_guard<std::mutex> lock(g_surface_controls_mutex);
      std::erase(g_surface_controls, control);
      for (SurfaceControl* child : g_surface_controls) {
        if (child->parent == control) child->parent = nullptr;
      }
    }
    if (control->buffer != nullptr) AHardwareBuffer_release(control->buffer);
    delete control;
  }
}

extern "C" ASurfaceTransaction* ASurfaceTransaction_create() {
  return reinterpret_cast<ASurfaceTransaction*>(new (std::nothrow) SurfaceTransaction());
}

extern "C" void ASurfaceTransaction_delete(ASurfaceTransaction* opaque) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  ReleaseTransactionBuffers(transaction);
  delete transaction;
}

extern "C" void ASurfaceTransaction_apply(ASurfaceTransaction* opaque) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  if (transaction == nullptr) return;
  SurfaceTransactionStats stats{.controls = transaction->controls};
  if (DebugSurfaceTransactions()) {
    std::fprintf(stderr,
                 "ART Android SurfaceTransaction: apply pid=%d controls=%zu\n",
                 getpid(), transaction->controls.size());
  }
  std::vector<SurfacePresentation> presentations;
  {
    std::lock_guard<std::mutex> controls_lock(g_surface_controls_mutex);
    for (auto& update : transaction->updates) {
      auto* control = reinterpret_cast<SurfaceControl*>(update.opaque);
      if (control == nullptr) continue;
      if (update.has_buffer) {
        if (control->buffer != nullptr && control->buffer != update.buffer) {
          AHardwareBuffer_acquire(control->buffer);
          stats.previous_buffers.emplace(update.opaque, control->buffer);
        }
        if (control->buffer != nullptr)
          AHardwareBuffer_release(control->buffer);
        control->buffer = update.buffer;
        update.buffer = nullptr;
      }
      if (update.has_geometry) {
        control->source = update.source;
        control->destination = update.destination;
        control->has_geometry = true;
      }
      if (update.has_crop) {
        control->crop = update.crop;
        control->has_crop = true;
      }
      if (update.has_visibility) control->visible = update.visible;
      if (update.has_position) {
        control->position_x = update.position_x;
        control->position_y = update.position_y;
      }
      if (update.has_transform) control->transform = update.transform;
      if (update.has_z_order) control->z_order = update.z_order;
      if (update.has_scale) {
        control->scale_x = update.scale_x;
        control->scale_y = update.scale_y;
      }
      if (update.has_alpha) control->alpha = update.alpha;
      if (update.has_parent) {
        control->parent = reinterpret_cast<SurfaceControl*>(update.parent);
        // createFromWindow/fromJava controls start attached to the display,
        // but an explicit reparent replaces that initial attachment. In
        // particular, reparent(control, nullptr) means detach; retaining the
        // root bit would keep Chromium's old tab surface in composition.
        control->composition_root = false;
      }
    }

    // SurfaceFlinger composes the current layer tree, not just the controls
    // carrying new buffers in this transaction. Re-blending only an updated
    // translucent layer over the previous display accumulates its color and
    // leaves pixels from hidden/moved layers behind. Keep each AHardwareBuffer
    // as the retained layer backing, then rebuild the host target entirely on
    // the GPU for every committed transaction.
    for (SurfaceControl* control : g_surface_controls) {
      if (control->visible && control->buffer != nullptr &&
          IsAttachedToCompositionRoot(control, g_surface_controls))
        presentations.push_back(MakePresentation(control));
    }
  }
  std::stable_sort(
      presentations.begin(), presentations.end(),
      [](const SurfacePresentation& left, const SurfacePresentation& right) {
        return left.z_order < right.z_order;
      });
  const bool composition_started =
      !presentations.empty() &&
      darwin_art_android_begin_hardware_buffer_composition(
          presentations.front().buffer, true);
  if (composition_started) {
    for (const auto& presentation : presentations) {
      if (DebugSurfaceTransactions()) {
        std::fprintf(stderr,
                     "ART Android SurfaceTransaction: present pid=%d name=%s "
                     "source=[%d,%d,%d,%d] destination=[%d,%d,%d,%d] "
                     "alpha=%.3f z=%d transform=%d\n",
                     getpid(), presentation.name.c_str(),
                     presentation.source.left, presentation.source.top,
                     presentation.source.right, presentation.source.bottom,
                     presentation.destination.left, presentation.destination.top,
                     presentation.destination.right,
                     presentation.destination.bottom, presentation.alpha,
                     presentation.z_order, presentation.transform);
      }
      darwin_art_android_present_hardware_buffer(
          presentation.control, presentation.buffer, presentation.source.left,
          presentation.source.top, presentation.source.right,
          presentation.source.bottom, presentation.destination.left,
          presentation.destination.top, presentation.destination.right,
          presentation.destination.bottom, presentation.has_damage,
          presentation.damage.left, presentation.damage.top,
          presentation.damage.right, presentation.damage.bottom,
          presentation.alpha);
    }
    darwin_art_android_end_hardware_buffer_composition();
  }
  for (const auto& presentation : presentations) {
    AHardwareBuffer_release(presentation.buffer);
  }
  auto* stats_opaque = reinterpret_cast<ASurfaceTransactionStats*>(&stats);
  if (transaction->commit != nullptr)
    transaction->commit(transaction->commit_context, stats_opaque);
  if (transaction->complete != nullptr)
    transaction->complete(transaction->complete_context, stats_opaque);
  transaction->controls.clear();
  ReleaseTransactionBuffers(transaction);
}

extern "C" void ASurfaceTransaction_setOnCommit(ASurfaceTransaction* opaque,
                                                  void* context,
                                                  TransactionCallback callback) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  if (transaction != nullptr) {
    transaction->commit = callback;
    transaction->commit_context = context;
  }
}

extern "C" void ASurfaceTransaction_setOnComplete(ASurfaceTransaction* opaque,
                                                    void* context,
                                                    TransactionCallback callback) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  if (transaction != nullptr) {
    transaction->complete = callback;
    transaction->complete_context = context;
  }
}

extern "C" void ASurfaceTransactionStats_getASurfaceControls(
    ASurfaceTransactionStats* opaque, ASurfaceControl*** out, size_t* count) {
  if (out == nullptr || count == nullptr) return;
  auto* stats = reinterpret_cast<SurfaceTransactionStats*>(opaque);
  *count = stats == nullptr ? 0 : stats->controls.size();
  if (*count == 0) { *out = nullptr; return; }
  *out = static_cast<ASurfaceControl**>(std::malloc(*count * sizeof(**out)));
  if (*out == nullptr) { *count = 0; return; }
  std::memcpy(*out, stats->controls.data(), *count * sizeof(**out));
}

extern "C" void ASurfaceTransactionStats_releaseASurfaceControls(
    ASurfaceControl** controls) { std::free(controls); }
extern "C" int ASurfaceTransactionStats_getPreviousReleaseFenceFd(
    ASurfaceTransactionStats* opaque, ASurfaceControl* control) {
  auto* stats = reinterpret_cast<SurfaceTransactionStats*>(opaque);
  if (stats == nullptr || control == nullptr ||
      std::find(stats->controls.begin(), stats->controls.end(), control) ==
          stats->controls.end()) {
    return -1;
  }
  auto previous = stats->previous_buffers.find(control);
  if (previous == stats->previous_buffers.end()) return -1;
  // SurfaceFlinger returns a release fence for the buffer displaced by this
  // transaction. Presentation above is submitted to ANGLE's Metal queue before
  // callbacks run, and the producer fence path has already completed the
  // staging-to-IOSurface publish. A broker sync-file marker preserves the
  // Android ownership/lifetime flow so Chromium waits at its next BeginAccess
  // and our EGL import can restore that exact persistent AHB slot.
  darwin_art_android_mark_hardware_buffer_released(previous->second);
  return CreateSignaledSurfaceFence();
}
extern "C" int ASurfaceTransactionStats_getPresentFenceFd(
    ASurfaceTransactionStats*) { return -1; }
extern "C" int64_t ASurfaceTransactionStats_getLatchTime(
    ASurfaceTransactionStats*) { return 0; }
extern "C" int64_t ASurfaceTransactionStats_getAcquireTime(
    ASurfaceTransactionStats*, ASurfaceControl*) { return -1; }

#define SURFACE_CONTROL_SETTER(name, signature, control_arg) \
  extern "C" void name signature {                           \
    Remember(reinterpret_cast<SurfaceTransaction*>(transaction), control_arg); \
  }

extern "C" void ASurfaceTransaction_reparent(
    ASurfaceTransaction* opaque, ASurfaceControl* control,
    ASurfaceControl* parent) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_parent = true;
    update->parent = parent;
  }
  if (DebugSurfaceTransactions()) {
    std::fprintf(stderr,
                 "ART Android SurfaceTransaction: reparent pid=%d "
                 "control=%p parent=%p\n",
                 getpid(), static_cast<void*>(control),
                 static_cast<void*>(parent));
  }
}
extern "C" void ASurfaceTransaction_setVisibility(
    ASurfaceTransaction* opaque, ASurfaceControl* control,
    enum ASurfaceTransactionVisibility visibility) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_visibility = true;
    update->visible = visibility == ASURFACE_TRANSACTION_VISIBILITY_SHOW;
  }
}
extern "C" void ASurfaceTransaction_setZOrder(ASurfaceTransaction* opaque,
                                                ASurfaceControl* control,
                                                int32_t z_order) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_z_order = true;
    update->z_order = z_order;
  }
}
extern "C" void ASurfaceTransaction_setBuffer(
    ASurfaceTransaction* transaction, ASurfaceControl* control,
    AHardwareBuffer* buffer, int fence_fd) {
  auto* state = reinterpret_cast<SurfaceTransaction*>(transaction);
  Remember(state, control);
  auto* update = FindUpdate(state, control);
  if (update != nullptr) {
    if (update->buffer != nullptr) AHardwareBuffer_release(update->buffer);
    update->buffer = buffer;
    update->has_buffer = true;
    if (buffer != nullptr) AHardwareBuffer_acquire(buffer);
  }
  if (fence_fd >= 0)
    (void)darwin_art_bionic_socket_broker_close(fence_fd);
  if (!DebugSurfaceTransactions()) return;
  const uint32_t surface_id =
      buffer == nullptr || buffer->surface == nullptr
          ? 0
          : IOSurfaceGetID(buffer->surface);
  const uint32_t width = buffer == nullptr ? 0 : buffer->description.width;
  const uint32_t height = buffer == nullptr ? 0 : buffer->description.height;
  std::fprintf(stderr,
               "ART Android SurfaceTransaction: setBuffer pid=%d control=%p "
               "buffer=%p iosurface=%u size=%ux%u fence=%d\n",
               getpid(), static_cast<void*>(control), static_cast<void*>(buffer),
               surface_id, width, height, fence_fd);
}
extern "C" void ASurfaceTransaction_setGeometry(
    ASurfaceTransaction* opaque, ASurfaceControl* control, const ARect& source,
    const ARect& destination, int32_t transform) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->source = source;
    update->destination = destination;
    update->has_geometry = true;
    update->transform = transform;
    update->has_transform = true;
  }
  if (DebugSurfaceTransactions()) {
    std::fprintf(stderr,
                 "ART Android SurfaceTransaction: geometry pid=%d control=%p "
                 "source=[%d,%d,%d,%d] destination=[%d,%d,%d,%d] "
                 "transform=%d\n",
                 getpid(), static_cast<void*>(control), source.left,
                 source.top, source.right, source.bottom, destination.left,
                 destination.top, destination.right, destination.bottom,
                 transform);
  }
}
extern "C" void ASurfaceTransaction_setCrop(ASurfaceTransaction* opaque,
                                               ASurfaceControl* control,
                                               const ARect& crop) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->crop = crop;
    update->has_crop = true;
  }
}
extern "C" void ASurfaceTransaction_setPosition(ASurfaceTransaction* opaque,
                                                   ASurfaceControl* control,
                                                   int32_t x, int32_t y) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_position = true;
    update->position_x = x;
    update->position_y = y;
  }
  if (DebugSurfaceTransactions()) {
    std::fprintf(stderr,
                 "ART Android SurfaceTransaction: position pid=%d control=%p "
                 "position=%d,%d\n",
                 getpid(), static_cast<void*>(control), x, y);
  }
}
extern "C" void ASurfaceTransaction_setBufferTransform(
    ASurfaceTransaction* opaque, ASurfaceControl* control, int32_t transform) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_transform = true;
    update->transform = transform;
  }
}
extern "C" void ASurfaceTransaction_setScale(ASurfaceTransaction* opaque,
                                                ASurfaceControl* control,
                                                float x, float y) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_scale = true;
    update->scale_x = x;
    update->scale_y = y;
  }
}
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setBufferTransparency,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, enum ASurfaceTransactionTransparency), control)
extern "C" void ASurfaceTransaction_setDamageRegion(
    ASurfaceTransaction* opaque, ASurfaceControl* control,
    const ARect* rects, uint32_t count) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update == nullptr) return;
  update->has_damage = true;
  update->damage.clear();
  if (rects != nullptr && count != 0)
    update->damage.assign(rects, rects + count);
  if (!DebugSurfaceTransactions()) return;
  std::fprintf(stderr,
               "ART Android SurfaceTransaction: damage pid=%d control=%p "
               "rects=%u",
               getpid(), static_cast<void*>(control), count);
  for (const ARect& rect : update->damage) {
    std::fprintf(stderr, " [%d,%d,%d,%d]", rect.left, rect.top, rect.right,
                 rect.bottom);
  }
  std::fprintf(stderr, "\n");
}
extern "C" void ASurfaceTransaction_setBufferAlpha(ASurfaceTransaction* opaque,
                                                      ASurfaceControl* control,
                                                      float alpha) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  auto* update = FindUpdate(transaction, control);
  if (update != nullptr) {
    update->has_alpha = true;
    update->alpha = alpha;
  }
}
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setBufferDataSpace,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, enum ADataSpace), control)
extern "C" void ASurfaceTransaction_setColor(
    ASurfaceTransaction* opaque, ASurfaceControl* control, float red,
    float green, float blue, float alpha, enum ADataSpace) {
  auto* transaction = reinterpret_cast<SurfaceTransaction*>(opaque);
  Remember(transaction, control);
  if (DebugSurfaceTransactions()) {
    std::fprintf(stderr,
                 "ART Android SurfaceTransaction: color pid=%d control=%p "
                 "rgba=%.3f,%.3f,%.3f,%.3f\n",
                 getpid(), static_cast<void*>(control), red, green, blue,
                 alpha);
  }
}
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setHdrMetadata_smpte2086,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, struct AHdrMetadata_smpte2086*), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setHdrMetadata_cta861_3,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, struct AHdrMetadata_cta861_3*), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setExtendedRangeBrightness,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, float, float), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setDesiredHdrHeadroom,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, float), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setFrameRate,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, float, int8_t), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setFrameRateWithChangeStrategy,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, float, int8_t, int8_t), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_clearFrameRate,
  (ASurfaceTransaction* transaction, ASurfaceControl* control), control)
SURFACE_CONTROL_SETTER(ASurfaceTransaction_setEnableBackPressure,
  (ASurfaceTransaction* transaction, ASurfaceControl* control, bool), control)

extern "C" void ASurfaceTransaction_setDesiredPresentTime(ASurfaceTransaction*, int64_t) {}
extern "C" void ASurfaceTransaction_setFrameTimeline(ASurfaceTransaction*, AVsyncId) {}

using ServiceCallback = void (*)(void*, void*);
extern "C" void ANativeService_setOnBindCallback(ServiceCallback, void*) {}
extern "C" void ANativeService_setOnDestroyCallback(ServiceCallback, void*) {}
extern "C" void ANativeService_setOnRebindCallback(ServiceCallback, void*) {}
extern "C" void ANativeService_setOnUnbindCallback(ServiceCallback, void*) {}

extern "C" int ASharedMemory_create(const char*, size_t size) {
  if (darwin_art_bionic_ioctl_bind_shared_memory(
          &darwin_art_android_shared_memory_ioctl) != 0) {
    darwin_art_bionic_errno_set_from_darwin(EIO);
    return -1;
  }
  if (size == 0) {
    darwin_art_bionic_errno_set_from_darwin(EINVAL);
    return -1;
  }
  char path[] = "/tmp/darwin-art-ashmem.XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    darwin_art_bionic_errno_set_from_darwin(errno);
    return -1;
  }
  // Android ashmem objects are anonymous file descriptors: unlinking the
  // Darwin backing file immediately gives the descriptor the same lifetime.
  (void)unlink(path);
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    const int error = errno;
    (void)close(fd);
    darwin_art_bionic_errno_set_from_darwin(error);
    return -1;
  }
  const SharedMemoryState state{size, PROT_READ | PROT_WRITE};
  if (!WriteSharedMemoryMarker(fd, state)) {
    const int error = errno;
    (void)close(fd);
    darwin_art_bionic_errno_set_from_darwin(error);
    return -1;
  }
  try {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.insert_or_assign(fd, state);
  } catch (...) {
    (void)close(fd);
    darwin_art_bionic_errno_set_from_darwin(ENOMEM);
    return -1;
  }
  if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
    std::fprintf(stderr, "ART Android ashmem: create fd=%d size=%zu\n", fd, size);
  if (size == 65536 &&
      std::getenv("DARWIN_ART_DEBUG_STOP_AT_ASHMEM_64K") != nullptr) {
    std::fprintf(stderr,
                 "ART Android ashmem: diagnostic wait pid=%d fd=%d size=%zu\n",
                 getpid(), fd, size);
    volatile int diagnostic_wait = 1;
    while (diagnostic_wait != 0) {
    }
  }
  return fd;
}

extern "C" int ASharedMemory_setProt(int fd, int protection) {
  constexpr int kAndroidProtectionMask = 0x1 | 0x2 | 0x4;
  if (fd < 0 || (protection & ~kAndroidProtectionMask) != 0) {
    darwin_art_bionic_errno_set_from_darwin(EINVAL);
    return -1;
  }
  SharedMemoryState state{};
  if (!ReadSharedMemoryMarker(fd, &state)) {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.erase(fd);
    if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
      std::fprintf(stderr, "ART Android ashmem: setProt missing fd=%d\n", fd);
    darwin_art_bionic_errno_set_from_darwin(EBADF);
    return -1;
  }
  // Ashmem protection can only be reduced after publication.
  if ((protection | state.protection) != state.protection) {
    darwin_art_bionic_errno_set_from_darwin(EINVAL);
    return -1;
  }
  state.protection = protection;
  if (!WriteSharedMemoryMarker(fd, state)) {
    darwin_art_bionic_errno_set_from_darwin(errno);
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.insert_or_assign(fd, state);
  }
  // Darwin has no ashmem-wide future-mapping protection seal. Individual
  // mappings still receive the requested protection through mmap/mprotect.
  return 0;
}

extern "C" int darwin_art_android_shared_memory_close(int fd) {
  SharedMemoryState state{};
  const bool is_shared_memory = ReadSharedMemoryMarker(fd, &state);
  {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.erase(fd);
  }
  if (!is_shared_memory) {
    if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
      std::fprintf(stderr, "ART Android ashmem: close miss fd=%d\n", fd);
    return 0;
  }
  const int result = close(fd) == 0 ? 1 : -1;
  if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
    std::fprintf(stderr, "ART Android ashmem: close fd=%d result=%d\n", fd,
                 result);
  return result;
}

extern "C" int darwin_art_android_shared_memory_dup(int fd) {
  SharedMemoryState state{};
  if (!ReadSharedMemoryMarker(fd, &state)) {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.erase(fd);
    return -2;
  }
  const int duplicate = dup(fd);
  if (duplicate < 0) return -1;
  try {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.insert_or_assign(duplicate, state);
  } catch (...) {
    (void)close(duplicate);
    return -1;
  }
  if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
    std::fprintf(stderr, "ART Android ashmem: dup fd=%d new=%d\n", fd,
                 duplicate);
  return duplicate;
}

extern "C" int darwin_art_android_shared_memory_get_info(
    int fd, size_t* size, int* protection) {
  if (size == nullptr || protection == nullptr) return -1;
  SharedMemoryState marked{};
  if (ReadSharedMemoryMarker(fd, &marked)) {
    try {
      std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
      g_shared_memory.insert_or_assign(fd, marked);
    } catch (...) {
      return -1;
    }
    *size = marked.size;
    *protection = marked.protection;
    return 1;
  }
  std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
  g_shared_memory.erase(fd);
  return 0;
}

extern "C" int darwin_art_android_shared_memory_adopt(
    int fd, size_t size, int protection) {
  constexpr int kAndroidProtectionMask = 0x1 | 0x2 | 0x4;
  if (fd < 0 || size == 0 || (protection & ~kAndroidProtectionMask) != 0) {
    return -1;
  }
  try {
    const SharedMemoryState state{size, protection};
    if (!WriteSharedMemoryMarker(fd, state)) return -1;
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.insert_or_assign(fd, state);
    return 0;
  } catch (...) {
    return -1;
  }
}

extern "C" int darwin_art_android_shared_memory_fcntl(
    int fd, int command, intptr_t argument, int* result) {
  if (result == nullptr) return 0;
  SharedMemoryState state{};
  if (!ReadSharedMemoryMarker(fd, &state)) {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.erase(fd);
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
  g_shared_memory.insert_or_assign(fd, state);
  auto found = g_shared_memory.find(fd);
  if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
    std::fprintf(stderr,
                 "ART Android ashmem: fcntl fd=%d command=%d argument=%lld\n",
                 fd, command, static_cast<long long>(argument));
  constexpr int kAndroidFDupfd = 0;
  constexpr int kAndroidFGetfd = 1;
  constexpr int kAndroidFSetfd = 2;
  constexpr int kAndroidFGetfl = 3;
  constexpr int kAndroidFSetfl = 4;
  constexpr int kAndroidFAddSeals = 1033;
  constexpr int kAndroidFGetSeals = 1034;
  constexpr int kAndroidFDupfdCloexec = 1030;
  constexpr int kSealShrink = 0x2;
  constexpr int kSealGrow = 0x4;
  constexpr int kSealFutureWrite = 0x10;
  if (command == kAndroidFGetSeals) {
    *result = kSealShrink | kSealGrow |
              ((found->second.protection & PROT_WRITE) == 0
                   ? kSealFutureWrite
                   : 0);
    if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
      std::fprintf(stderr,
                   "ART Android ashmem: fcntl fd=%d F_GET_SEALS result=%#x "
                   "protection=%#x\n",
                   fd, *result, found->second.protection);
    return 1;
  }
  if (command == kAndroidFAddSeals) {
    if ((argument & kSealFutureWrite) != 0) {
      found->second.protection &= ~PROT_WRITE;
      if (!WriteSharedMemoryMarker(fd, found->second)) {
        *result = -1;
        return 1;
      }
    }
    *result = 0;
    if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
      std::fprintf(stderr,
                   "ART Android ashmem: fcntl fd=%d F_ADD_SEALS result=0 "
                   "protection=%#x\n",
                   fd, found->second.protection);
    return 1;
  }
  int host_command = -1;
  switch (command) {
    case kAndroidFDupfd: host_command = F_DUPFD; break;
    case kAndroidFDupfdCloexec: host_command = F_DUPFD_CLOEXEC; break;
    case kAndroidFGetfd: host_command = F_GETFD; break;
    case kAndroidFSetfd: host_command = F_SETFD; break;
    case kAndroidFGetfl: host_command = F_GETFL; break;
    case kAndroidFSetfl: host_command = F_SETFL; break;
    default:
      *result = -1;
      return 1;
  }
  *result = (command == kAndroidFGetfd || command == kAndroidFGetfl)
                ? fcntl(fd, host_command)
                : fcntl(fd, host_command, argument);
  if (*result >= 0 &&
      (command == kAndroidFDupfd || command == kAndroidFDupfdCloexec)) {
    g_shared_memory.emplace(*result, found->second);
  }
  return 1;
}

extern "C" int darwin_art_android_shared_memory_ioctl(
    int fd, uint32_t request, void*, int* result, int* android_errno) {
  if (result == nullptr || android_errno == nullptr) return 0;
  SharedMemoryState state{};
  if (!ReadSharedMemoryMarker(fd, &state)) {
    std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
    g_shared_memory.erase(fd);
    if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
      std::fprintf(stderr, "ART Android ashmem: ioctl miss fd=%d request=%#x\n",
                   fd, request);
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_shared_memory_mutex);
  g_shared_memory.insert_or_assign(fd, state);
  const auto found = g_shared_memory.find(fd);
  if (std::getenv("DARWIN_ART_DEBUG_SHARED_MEMORY") != nullptr)
    std::fprintf(stderr, "ART Android ashmem: ioctl fd=%d request=%#x\n", fd,
                 request);
  constexpr uint32_t kAshmemGetSize = 0x00007704;
  constexpr uint32_t kAshmemGetProtectionMask = 0x00007706;
  if (request == kAshmemGetSize) {
    *result = found->second.size > static_cast<size_t>(INT_MAX)
                  ? -1
                  : static_cast<int>(found->second.size);
    *android_errno = *result < 0 ? EOVERFLOW : 0;
    return 1;
  }
  if (request == kAshmemGetProtectionMask) {
    *result = found->second.protection;
    *android_errno = 0;
    return 1;
  }
  *result = -1;
  *android_errno = ENOTTY;
  return 1;
}

extern "C" void* darwin_art_android_platform_symbol(const char* symbol) {
  if (symbol == nullptr) return nullptr;
#define ROUTE(name) if (std::strcmp(symbol, #name) == 0) return reinterpret_cast<void*>(&name)
  ROUTE(AHardwareBuffer_acquire);
  ROUTE(AHardwareBuffer_allocate);
  ROUTE(AHardwareBuffer_describe);
  ROUTE(AHardwareBuffer_fromHardwareBuffer);
  ROUTE(AHardwareBuffer_lock);
  ROUTE(AHardwareBuffer_lockPlanes);
  ROUTE(AHardwareBuffer_recvHandleFromUnixSocket);
  ROUTE(AHardwareBuffer_release);
  ROUTE(AHardwareBuffer_sendHandleToUnixSocket);
  ROUTE(AHardwareBuffer_unlock);
  ROUTE(AChoreographerFrameCallbackData_getFrameTimeNanos);
  ROUTE(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos);
  ROUTE(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos);
  ROUTE(AChoreographerFrameCallbackData_getFrameTimelineVsyncId);
  ROUTE(AChoreographerFrameCallbackData_getFrameTimelinesLength);
  ROUTE(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex);
  ROUTE(AChoreographer_getInstance);
  ROUTE(AChoreographer_postFrameCallback);
  ROUTE(AChoreographer_postFrameCallback64);
  ROUTE(AChoreographer_postFrameCallbackDelayed);
  ROUTE(AChoreographer_postFrameCallbackDelayed64);
  ROUTE(AChoreographer_postVsyncCallback);
  ROUTE(AChoreographer_registerRefreshRateCallback);
  ROUTE(AChoreographer_unregisterRefreshRateCallback);
  ROUTE(AInputEvent_getSource);
  ROUTE(AInputEvent_getType);
  ROUTE(AInputEvent_release);
  ROUTE(AMotionEvent_getAction);
  ROUTE(AMotionEvent_getAxisValue);
  ROUTE(AMotionEvent_getButtonState);
  ROUTE(AMotionEvent_getClassification);
  ROUTE(AMotionEvent_getDownTime);
  ROUTE(AMotionEvent_getEventTime);
  ROUTE(AMotionEvent_getHistoricalEventTime);
  ROUTE(AMotionEvent_getHistoricalTouchMajor);
  ROUTE(AMotionEvent_getHistoricalX);
  ROUTE(AMotionEvent_getHistoricalY);
  ROUTE(AMotionEvent_getHistorySize);
  ROUTE(AMotionEvent_getMetaState);
  ROUTE(AMotionEvent_getOrientation);
  ROUTE(AMotionEvent_getPointerCount);
  ROUTE(AMotionEvent_getPointerId);
  ROUTE(AMotionEvent_getPressure);
  ROUTE(AMotionEvent_getRawX);
  ROUTE(AMotionEvent_getRawY);
  ROUTE(AMotionEvent_getToolType);
  ROUTE(AMotionEvent_getTouchMajor);
  ROUTE(AMotionEvent_getTouchMinor);
  ROUTE(AMotionEvent_getX);
  ROUTE(AMotionEvent_getY);
  ROUTE(ALooper_acquire);
  ROUTE(ALooper_addFd);
  ROUTE(ALooper_forThread);
  ROUTE(ALooper_pollOnce);
  ROUTE(ALooper_prepare);
  ROUTE(ALooper_release);
  ROUTE(ALooper_removeFd);
  ROUTE(ALooper_wake);
  ROUTE(ASensorEventQueue_disableSensor);
  ROUTE(ASensorEventQueue_enableSensor);
  ROUTE(ASensorEventQueue_getEvents);
  ROUTE(ASensorEventQueue_setEventRate);
  ROUTE(ASensorManager_createEventQueue);
  ROUTE(ASensorManager_destroyEventQueue);
  ROUTE(ASensorManager_getDefaultSensor);
  ROUTE(ASensorManager_getInstanceForPackage);
  ROUTE(ASensor_getMinDelay);
  ROUTE(ASharedMemory_create);
  ROUTE(ASharedMemory_setProt);
  ROUTE(ASurfaceControl_create);
  ROUTE(ASurfaceControl_createFromWindow);
  ROUTE(ASurfaceControl_fromJava);
  ROUTE(ASurfaceControl_release);
  ROUTE(ASurfaceTransactionStats_getASurfaceControls);
  ROUTE(ASurfaceTransactionStats_getLatchTime);
  ROUTE(ASurfaceTransactionStats_getAcquireTime);
  ROUTE(ASurfaceTransactionStats_getPresentFenceFd);
  ROUTE(ASurfaceTransactionStats_getPreviousReleaseFenceFd);
  ROUTE(ASurfaceTransactionStats_releaseASurfaceControls);
  ROUTE(ASurfaceTransaction_apply);
  ROUTE(ASurfaceTransaction_create);
  ROUTE(ASurfaceTransaction_delete);
  ROUTE(ASurfaceTransaction_reparent);
  ROUTE(ASurfaceTransaction_setBuffer);
  ROUTE(ASurfaceTransaction_setBufferAlpha);
  ROUTE(ASurfaceTransaction_setBufferDataSpace);
  ROUTE(ASurfaceTransaction_setBufferTransform);
  ROUTE(ASurfaceTransaction_setBufferTransparency);
  ROUTE(ASurfaceTransaction_setColor);
  ROUTE(ASurfaceTransaction_setCrop);
  ROUTE(ASurfaceTransaction_setDamageRegion);
  ROUTE(ASurfaceTransaction_setDesiredHdrHeadroom);
  ROUTE(ASurfaceTransaction_setDesiredPresentTime);
  ROUTE(ASurfaceTransaction_setEnableBackPressure);
  ROUTE(ASurfaceTransaction_setExtendedRangeBrightness);
  ROUTE(ASurfaceTransaction_setFrameTimeline);
  ROUTE(ASurfaceTransaction_setFrameRate);
  ROUTE(ASurfaceTransaction_setFrameRateWithChangeStrategy);
  ROUTE(ASurfaceTransaction_clearFrameRate);
  ROUTE(ASurfaceTransaction_setGeometry);
  ROUTE(ASurfaceTransaction_setHdrMetadata_cta861_3);
  ROUTE(ASurfaceTransaction_setHdrMetadata_smpte2086);
  ROUTE(ASurfaceTransaction_setOnCommit);
  ROUTE(ASurfaceTransaction_setOnComplete);
  ROUTE(ASurfaceTransaction_setPosition);
  ROUTE(ASurfaceTransaction_setScale);
  ROUTE(ASurfaceTransaction_setVisibility);
  ROUTE(ASurfaceTransaction_setZOrder);
  ROUTE(ANativeService_setOnBindCallback);
  ROUTE(ANativeService_setOnDestroyCallback);
  ROUTE(ANativeService_setOnRebindCallback);
  ROUTE(ANativeService_setOnUnbindCallback);
  if (std::strcmp(symbol, "ANativeWindow_acquire") == 0)
    return reinterpret_cast<void*>(&darwin_art_android_ANativeWindow_acquire);
  if (std::strcmp(symbol, "ANativeWindow_fromSurface") == 0)
    return reinterpret_cast<void*>(&darwin_art_android_ANativeWindow_fromSurface);
  if (std::strcmp(symbol, "ANativeWindow_getFormat") == 0)
    return reinterpret_cast<void*>(&darwin_art_android_ANativeWindow_getFormat);
  if (std::strcmp(symbol, "ANativeWindow_release") == 0)
    return reinterpret_cast<void*>(&darwin_art_android_ANativeWindow_release);
  if (std::strcmp(symbol, "ANativeWindow_toSurface") == 0)
    return reinterpret_cast<void*>(&darwin_art_android_ANativeWindow_toSurface);
#undef ROUTE
  return nullptr;
}
