#include "darwin_angle_egl.h"
#include "darwin_surface_bridge.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace {
struct DarwinAndroidNativeWindowBuffer {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_pixels = 0;
  int32_t format = 1;
  uint64_t generation = 0;
};

struct AndroidNativeBaseAbi {
  int32_t magic;
  int32_t version;
  void* reserved[4];
  void (*inc_ref)(AndroidNativeBaseAbi* base);
  void (*dec_ref)(AndroidNativeBaseAbi* base);
};

struct AndroidNativeWindowAbi {
  AndroidNativeBaseAbi common;
  uint32_t flags;
  int32_t min_swap_interval;
  int32_t max_swap_interval;
  float xdpi;
  float ydpi;
  intptr_t oem[4];
  int (*set_swap_interval)(AndroidNativeWindowAbi*, int);
  int (*dequeue_buffer_deprecated)(AndroidNativeWindowAbi*, void**);
  int (*lock_buffer_deprecated)(AndroidNativeWindowAbi*, void*);
  int (*queue_buffer_deprecated)(AndroidNativeWindowAbi*, void*);
  int (*query)(const AndroidNativeWindowAbi*, int, int*);
  int (*perform)(AndroidNativeWindowAbi*, int, ...);
  int (*cancel_buffer_deprecated)(AndroidNativeWindowAbi*, void*);
  int (*dequeue_buffer)(AndroidNativeWindowAbi*, void**, int*);
  int (*queue_buffer)(AndroidNativeWindowAbi*, void*, int);
  int (*cancel_buffer)(AndroidNativeWindowAbi*, void*, int);
};

struct DarwinAndroidNativeWindow {
  // Must remain first. HWUI receives this object as a real ANativeWindow and
  // uses its Android native-base refcount/query ABI before handing it to the
  // render thread.
  AndroidNativeWindowAbi abi{};
  jlong java_surface_identity = 0;
  std::atomic<uint32_t> references{1};
  std::atomic<int32_t> width{0};
  std::atomic<int32_t> height{0};
  std::atomic<int32_t> format{1};
  std::mutex mutex;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> locked;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> published;
};

struct AndroidNativeWindowBufferAbi {
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  void* bits;
  uint32_t reserved[6];
};

std::mutex g_android_native_window_mutex;
DarwinAndroidNativeWindow* g_android_native_window_published = nullptr;
std::unordered_map<jlong, DarwinAndroidNativeWindow*>
    g_android_native_windows_by_surface;
std::atomic<uint64_t> g_android_native_window_generation{0};

bool DebugAndroidNativeWindow() {
  const char* value = std::getenv("DARWIN_ART_DEBUG_ANATIVEWINDOW");
  return value != nullptr && std::strcmp(value, "0") != 0;
}

uint32_t AndroidNativeWindowBytesPerPixel(int32_t format) {
  // android/native_window.h: RGBA_8888=1, RGBX_8888=2, RGB_565=4.
  return format == 4 ? 2u : 4u;
}

DarwinAndroidNativeWindow* WindowFromAbi(AndroidNativeWindowAbi* abi) {
  return reinterpret_cast<DarwinAndroidNativeWindow*>(abi);
}

const DarwinAndroidNativeWindow* WindowFromAbi(
    const AndroidNativeWindowAbi* abi) {
  return reinterpret_cast<const DarwinAndroidNativeWindow*>(abi);
}

void ReleaseNativeWindow(DarwinAndroidNativeWindow* window) {
  if (window == nullptr ||
      window->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    if (g_android_native_window_published == window) {
      g_android_native_window_published = nullptr;
    }
    if (window->java_surface_identity != 0) {
      auto found = g_android_native_windows_by_surface.find(
          window->java_surface_identity);
      if (found != g_android_native_windows_by_surface.end() &&
          found->second == window) {
        g_android_native_windows_by_surface.erase(found);
      }
    }
  }
  delete window;
}

void NativeWindowIncRef(AndroidNativeBaseAbi* base) {
  if (base == nullptr) return;
  auto* window = reinterpret_cast<DarwinAndroidNativeWindow*>(base);
  window->references.fetch_add(1, std::memory_order_relaxed);
}

void NativeWindowDecRef(AndroidNativeBaseAbi* base) {
  ReleaseNativeWindow(
      reinterpret_cast<DarwinAndroidNativeWindow*>(base));
}

int NativeWindowSetSwapInterval(AndroidNativeWindowAbi*, int) { return 0; }
int NativeWindowUnsupportedDequeue(AndroidNativeWindowAbi*, void**, int*) {
  return -ENOSYS;
}
int NativeWindowUnsupportedDequeueDeprecated(AndroidNativeWindowAbi*, void**) {
  return -ENOSYS;
}
int NativeWindowUnsupportedBuffer(AndroidNativeWindowAbi*, void*) {
  return -ENOSYS;
}
int NativeWindowUnsupportedBufferWithFence(AndroidNativeWindowAbi*, void*,
                                           int) {
  return -ENOSYS;
}

int NativeWindowQuery(const AndroidNativeWindowAbi* abi, int what, int* value) {
  if (abi == nullptr || value == nullptr) return -EINVAL;
  const auto* window = WindowFromAbi(abi);
  switch (what) {
    case 0:  // NATIVE_WINDOW_WIDTH
    case 7:  // NATIVE_WINDOW_DEFAULT_WIDTH
      *value = window->width.load(std::memory_order_relaxed);
      return 0;
    case 1:  // NATIVE_WINDOW_HEIGHT
    case 8:  // NATIVE_WINDOW_DEFAULT_HEIGHT
      *value = window->height.load(std::memory_order_relaxed);
      return 0;
    case 2:  // NATIVE_WINDOW_FORMAT
      *value = window->format.load(std::memory_order_relaxed);
      return 0;
    case 3:  // NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS
      *value = 0;
      return 0;
    case 4:  // NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER
      *value = 1;
      return 0;
    case 6:  // NATIVE_WINDOW_TRANSFORM_HINT
      *value = 0;
      return 0;
    default:
      return -ENOENT;
  }
}

int NativeWindowPerform(AndroidNativeWindowAbi*, int, ...) {
  // The detached Metal compositor has no BufferQueue producer configuration.
  // Accept advisory configuration requests; queries continue through query().
  return 0;
}

void InitializeNativeWindowAbi(DarwinAndroidNativeWindow* window) {
  constexpr int32_t kAndroidNativeWindowMagic =
      ('_' << 24) | ('w' << 16) | ('n' << 8) | 'd';
  window->abi.common.magic = kAndroidNativeWindowMagic;
  window->abi.common.version = sizeof(AndroidNativeWindowAbi);
  window->abi.common.inc_ref = &NativeWindowIncRef;
  window->abi.common.dec_ref = &NativeWindowDecRef;
  window->abi.min_swap_interval = 0;
  window->abi.max_swap_interval = 1;
  window->abi.set_swap_interval = &NativeWindowSetSwapInterval;
  window->abi.dequeue_buffer_deprecated =
      &NativeWindowUnsupportedDequeueDeprecated;
  window->abi.lock_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.queue_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.query = &NativeWindowQuery;
  window->abi.perform = &NativeWindowPerform;
  window->abi.cancel_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.dequeue_buffer = &NativeWindowUnsupportedDequeue;
  window->abi.queue_buffer = &NativeWindowUnsupportedBufferWithFence;
  window->abi.cancel_buffer = &NativeWindowUnsupportedBufferWithFence;
}
}  // namespace

extern "C" void* darwin_art_android_ANativeWindow_fromSurface(void* opaque_env,
                                                                void* surface) {
  auto* env = static_cast<JNIEnv*>(opaque_env);
  const jobject java_surface = static_cast<jobject>(surface);
  jlong identity = 0;
  jclass surface_class =
      env == nullptr || java_surface == nullptr
          ? nullptr
          : env->GetObjectClass(java_surface);
  jfieldID native_object =
      surface_class == nullptr
          ? nullptr
          : env->GetFieldID(surface_class, "mNativeObject", "J");
  if (native_object != nullptr && !env->ExceptionCheck()) {
    identity = env->GetLongField(java_surface, native_object);
  }
  if (surface_class != nullptr) env->DeleteLocalRef(surface_class);
  if (env != nullptr && env->ExceptionCheck()) env->ExceptionClear();
  if (identity != 0) {
    std::lock_guard<std::mutex> lock(g_android_native_window_mutex);
    auto found = g_android_native_windows_by_surface.find(identity);
    if (found != g_android_native_windows_by_surface.end()) {
      found->second->references.fetch_add(1, std::memory_order_relaxed);
      return found->second;
    }
  }
  auto* window = new DarwinAndroidNativeWindow();
  InitializeNativeWindowAbi(window);
  window->java_surface_identity = identity;
  window->width.store(darwin_art::DarwinAngleHostSurfaceWidth(),
                      std::memory_order_relaxed);
  window->height.store(darwin_art::DarwinAngleHostSurfaceHeight(),
                       std::memory_order_relaxed);
  if (identity != 0) {
    std::lock_guard<std::mutex> lock(g_android_native_window_mutex);
    auto [found, inserted] =
        g_android_native_windows_by_surface.emplace(identity, window);
    if (!inserted) {
      found->second->references.fetch_add(1, std::memory_order_relaxed);
      delete window;
      return found->second;
    }
  }
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: fromSurface pid=" << getpid()
              << " javaSurface=" << surface << " identity=0x" << std::hex
              << identity << std::dec << " window=" << window
              << " size=" << window->width.load(std::memory_order_relaxed)
              << "x" << window->height.load(std::memory_order_relaxed)
              << "\n";
  }
  return window;
}

extern "C" void* darwin_art_android_ANativeWindow_create(
    int32_t width, int32_t height, int32_t format) {
  auto* window = new (std::nothrow) DarwinAndroidNativeWindow();
  if (window == nullptr) return nullptr;
  InitializeNativeWindowAbi(window);
  window->width.store(width, std::memory_order_relaxed);
  window->height.store(height, std::memory_order_relaxed);
  window->format.store(format, std::memory_order_relaxed);
  return window;
}

extern "C" void darwin_art_android_ANativeWindow_acquire(void* opaque) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window != nullptr) window->references.fetch_add(1, std::memory_order_relaxed);
}

extern "C" int32_t darwin_art_android_ANativeWindow_getFormat(void* opaque) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  return window == nullptr ? 0 : window->format.load(std::memory_order_relaxed);
}

extern "C" void* darwin_art_android_ANativeWindow_toSurface(void*, void*) {
  // The framework Surface wrapper is created by the Java bridge. Native
  // callers still retain and render through the stable ANativeWindow token.
  return nullptr;
}

extern "C" void darwin_art_android_ANativeWindow_release(void* opaque) {
  ReleaseNativeWindow(static_cast<DarwinAndroidNativeWindow*>(opaque));
}

extern "C" int32_t darwin_art_android_ANativeWindow_lock(
    void* opaque, void* buffer, void*) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  auto* native_buffer = static_cast<AndroidNativeWindowBufferAbi*>(buffer);
  if (window == nullptr || native_buffer == nullptr) return -22;
  const int32_t width = window->width.load(std::memory_order_relaxed);
  const int32_t height = window->height.load(std::memory_order_relaxed);
  const int32_t format = window->format.load(std::memory_order_relaxed);
  if (width <= 0 || height <= 0 ||
      (format != 1 && format != 2 && format != 4)) {
    return -22;
  }
  const uint32_t stride =
      (static_cast<uint32_t>(width) + 15u) & ~uint32_t{15};
  const size_t row_bytes =
      static_cast<size_t>(stride) * AndroidNativeWindowBytesPerPixel(format);
  if (row_bytes > SIZE_MAX / static_cast<size_t>(height)) return -12;
  auto storage = std::make_shared<DarwinAndroidNativeWindowBuffer>();
  storage->width = static_cast<uint32_t>(width);
  storage->height = static_cast<uint32_t>(height);
  storage->stride_pixels = stride;
  storage->format = format;
  try {
    storage->pixels.resize(row_bytes * static_cast<size_t>(height));
  } catch (const std::bad_alloc&) {
    return -12;
  }
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    if (window->locked != nullptr) return -16;
    window->locked = storage;
  }
  *native_buffer = AndroidNativeWindowBufferAbi{
      .width = width,
      .height = height,
      .stride = static_cast<int32_t>(stride),
      .format = format,
      .bits = storage->pixels.data(),
      .reserved = {},
  };
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: lock " << width << "x" << height
              << " stride=" << stride << " format=" << format << "\n";
  }
  return 0;
}

extern "C" int32_t darwin_art_android_ANativeWindow_unlockAndPost(
    void* opaque) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) return -22;
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    if (window->locked == nullptr) return -22;
    window->locked->generation =
        g_android_native_window_generation.fetch_add(
            1, std::memory_order_acq_rel) +
        1;
    window->published = std::move(window->locked);
  }
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    g_android_native_window_published = window;
  }
  darwin_art_surface_gpu_publish_embedded(darwin_art_surface_active_gpu());
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: post generation="
              << g_android_native_window_generation.load(
                     std::memory_order_relaxed)
              << "\n";
  }
  return 0;
}

extern "C" int32_t darwin_art_android_ANativeWindow_setBuffersGeometry(
    void* opaque, int32_t width, int32_t height, int32_t format) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) return -22;
  if (width > 0) window->width.store(width, std::memory_order_relaxed);
  if (height > 0) window->height.store(height, std::memory_order_relaxed);
  if (format != 0) window->format.store(format, std::memory_order_relaxed);
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: geometry " << width << "x"
              << height << " format=" << format << "\n";
  }
  return 0;
}

extern "C" bool darwin_art_android_ANativeWindow_acquire_frame(
    DarwinArtAndroidNativeWindowFrame* frame) {
  if (frame == nullptr) return false;
  *frame = DarwinArtAndroidNativeWindowFrame{};
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> storage;
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    DarwinAndroidNativeWindow* window = g_android_native_window_published;
    if (window == nullptr) return false;
    std::lock_guard<std::mutex> lock(window->mutex);
    storage = window->published;
  }
  if (storage == nullptr || storage->pixels.empty()) return false;
  auto* owner = new (std::nothrow)
      std::shared_ptr<DarwinAndroidNativeWindowBuffer>(std::move(storage));
  if (owner == nullptr) return false;
  const auto& held = **owner;
  *frame = DarwinArtAndroidNativeWindowFrame{
      .pixels = held.pixels.data(),
      .size = held.pixels.size(),
      .width = held.width,
      .height = held.height,
      .stride_pixels = held.stride_pixels,
      .format = held.format,
      .generation = held.generation,
      .owner = owner,
  };
  return true;
}

extern "C" void darwin_art_android_ANativeWindow_release_frame(
    DarwinArtAndroidNativeWindowFrame* frame) {
  if (frame == nullptr) return;
  delete static_cast<
      std::shared_ptr<DarwinAndroidNativeWindowBuffer>*>(frame->owner);
  *frame = DarwinArtAndroidNativeWindowFrame{};
}
