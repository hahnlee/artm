#include "darwin_angle_egl.h"
#include "darwin_android_platform.h"
#include "darwin_surface_bridge.h"

#include <android/hardware_buffer.h>
#include <android/surface_control.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
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

extern "C" int sync_wait(int fd, int timeout_ms);
extern "C" int darwin_art_bionic_socket_broker_close(int fd);

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
  // Android propagates the producer color space through BufferQueue.  The
  // Darwin queue stores the same state on its ANativeWindow producer so HWUI
  // can negotiate wide-color surfaces without a host-side policy override.
  std::atomic<int32_t> dataspace{0};
  std::mutex mutex;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> locked;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> published;
  struct GpuSlot {
    AHardwareBuffer* buffer = nullptr;
    void* native_buffer = nullptr;
    bool dequeued = false;
    bool consumer_held = false;
    int release_fence = -1;
  };
  std::vector<GpuSlot> gpu_slots;
  ASurfaceControl* surface_control = nullptr;
  uint32_t imported_surface_owner_process_id = 0;
  uint32_t imported_surface_layer_id = 0;
  uint32_t next_gpu_slot = 0;
  DarwinArtAndroidNativeWindowQueueCallback queue_callback = nullptr;
  void* queue_context = nullptr;
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
struct ImportedSurfaceIdentity {
  uint32_t owner_process_id = 0;
  uint32_t layer_id = 0;
};
std::unordered_map<jlong, ImportedSurfaceIdentity>
    g_imported_surface_identities;
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
  for (auto& slot : window->gpu_slots) {
    if (slot.release_fence >= 0) {
      (void)darwin_art_bionic_socket_broker_close(slot.release_fence);
    }
    if (slot.buffer != nullptr) AHardwareBuffer_release(slot.buffer);
  }
  if (window->surface_control != nullptr) {
    ASurfaceControl_release(window->surface_control);
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
bool EnsureGpuSlots(DarwinAndroidNativeWindow* window) {
  if (window == nullptr) return false;
  const uint32_t width = static_cast<uint32_t>(
      window->width.load(std::memory_order_relaxed));
  const uint32_t height = static_cast<uint32_t>(
      window->height.load(std::memory_order_relaxed));
  if (width == 0 || height == 0) return false;
  if (!window->gpu_slots.empty()) return true;
  AHardwareBuffer_Desc description{
      .width = width,
      .height = height,
      .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
      .stride = 0,
      .rfu0 = 0,
      .rfu1 = 0,
  };
  for (int index = 0; index < 3; ++index) {
    AHardwareBuffer* buffer = nullptr;
    if (AHardwareBuffer_allocate(&description, &buffer) != 0 ||
        buffer == nullptr) {
      for (auto& slot : window->gpu_slots) {
        AHardwareBuffer_release(slot.buffer);
      }
      window->gpu_slots.clear();
      return false;
    }
    window->gpu_slots.push_back({
        .buffer = buffer,
        .native_buffer =
            darwin_art_android_hardware_buffer_native_window_buffer(buffer),
        .dequeued = false,
    });
  }
  return true;
}

int NativeWindowDequeue(AndroidNativeWindowAbi* abi, void** out_buffer,
                        int* out_fence) {
  auto* window = WindowFromAbi(abi);
  if (window == nullptr || out_buffer == nullptr || out_fence == nullptr)
    return -EINVAL;
  std::lock_guard<std::mutex> lock(window->mutex);
  if (!EnsureGpuSlots(window)) return -ENOMEM;
  for (size_t offset = 0; offset < window->gpu_slots.size(); ++offset) {
    const size_t index =
        (window->next_gpu_slot + offset) % window->gpu_slots.size();
    auto& slot = window->gpu_slots[index];
    if (slot.dequeued || slot.consumer_held) continue;
    if (slot.release_fence >= 0) {
      if (sync_wait(slot.release_fence, 0) != 0) continue;
      (void)darwin_art_bionic_socket_broker_close(slot.release_fence);
      slot.release_fence = -1;
    }
    slot.dequeued = true;
    window->next_gpu_slot =
        static_cast<uint32_t>((index + 1) % window->gpu_slots.size());
    *out_buffer = slot.native_buffer;
    *out_fence = -1;
    return 0;
  }
  return -EBUSY;
}
int NativeWindowDequeueDeprecated(AndroidNativeWindowAbi* abi,
                                  void** out_buffer) {
  int fence = -1;
  return NativeWindowDequeue(abi, out_buffer, &fence);
}
int NativeWindowUnsupportedBuffer(AndroidNativeWindowAbi*, void*) {
  return -ENOSYS;
}
DarwinAndroidNativeWindow::GpuSlot* FindGpuSlot(
    DarwinAndroidNativeWindow* window, void* native_buffer) {
  if (window == nullptr || native_buffer == nullptr) return nullptr;
  auto found = std::find_if(
      window->gpu_slots.begin(), window->gpu_slots.end(),
      [native_buffer](const DarwinAndroidNativeWindow::GpuSlot& slot) {
        return slot.native_buffer == native_buffer;
      });
  return found == window->gpu_slots.end() ? nullptr : &*found;
}

int NativeWindowQueue(AndroidNativeWindowAbi* abi, void* native_buffer,
                      int fence) {
  auto* window = WindowFromAbi(abi);
  AHardwareBuffer* buffer = nullptr;
  DarwinArtAndroidNativeWindowQueueCallback callback = nullptr;
  void* callback_context = nullptr;
  int32_t slot_index = -1;
  int32_t dataspace = 0;
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    auto* slot = FindGpuSlot(window, native_buffer);
    if (slot == nullptr || !slot->dequeued) return -EINVAL;
    slot->dequeued = false;
    buffer = slot->buffer;
    slot_index = static_cast<int32_t>(slot - window->gpu_slots.data());
    callback = window->queue_callback;
    callback_context = window->queue_context;
    dataspace = window->dataspace.load(std::memory_order_acquire);
    slot->consumer_held = callback != nullptr;
  }
  if (callback != nullptr) {
    callback(callback_context, buffer, slot_index, fence, dataspace);
    return 0;
  }
  if (window->surface_control == nullptr) {
    window->surface_control = reinterpret_cast<ASurfaceControl*>(
        darwin_art_android_surface_control_create_root("HWUI ViewRoot"));
  }
  ASurfaceTransaction* transaction = ASurfaceTransaction_create();
  if (window->surface_control == nullptr || transaction == nullptr)
    return -ENOMEM;
  if (DebugAndroidNativeWindow()) {
    uint32_t owner_process_id = 0;
    uint32_t layer_id = 0;
    const bool identified = darwin_art_android_surface_control_get_identity(
        window->surface_control, &owner_process_id, &layer_id);
    std::cerr << "ART Android ANativeWindow: queue window=" << window
              << " control=" << window->surface_control
              << " identified=" << (identified ? 1 : 0)
              << " owner=" << owner_process_id << " layer=" << layer_id
              << " slot=" << slot_index << "\n";
  }
  ASurfaceTransaction_setBuffer(transaction, window->surface_control, buffer,
                                fence);
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
  return 0;
}

int NativeWindowCancel(AndroidNativeWindowAbi* abi, void* native_buffer,
                       int fence) {
  auto* window = WindowFromAbi(abi);
  if (fence >= 0) (void)darwin_art_bionic_socket_broker_close(fence);
  std::lock_guard<std::mutex> lock(window->mutex);
  auto* slot = FindGpuSlot(window, native_buffer);
  if (slot == nullptr || !slot->dequeued) return -EINVAL;
  slot->dequeued = false;
  return 0;
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
    case 17:  // NATIVE_WINDOW_IS_VALID
      *value = 1;
      return 0;
    case 20:  // NATIVE_WINDOW_DATASPACE
      *value = window->dataspace.load(std::memory_order_acquire);
      return 0;
    default:
      return -ENOENT;
  }
}

int NativeWindowPerform(AndroidNativeWindowAbi* abi, int operation, ...) {
  if (abi == nullptr) return -EINVAL;
  auto* window = WindowFromAbi(abi);
  if (operation == 19) {  // NATIVE_WINDOW_SET_BUFFERS_DATASPACE
    va_list arguments;
    va_start(arguments, operation);
    const int32_t dataspace = va_arg(arguments, int32_t);
    va_end(arguments);
    window->dataspace.store(dataspace, std::memory_order_release);
  }
  // The Darwin BufferQueue accepts the remaining producer configuration as
  // advisory state until each operation has a corresponding Composer field.
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
  window->abi.dequeue_buffer_deprecated = &NativeWindowDequeueDeprecated;
  window->abi.lock_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.queue_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.query = &NativeWindowQuery;
  window->abi.perform = &NativeWindowPerform;
  window->abi.cancel_buffer_deprecated = &NativeWindowUnsupportedBuffer;
  window->abi.dequeue_buffer = &NativeWindowDequeue;
  window->abi.queue_buffer = &NativeWindowQueue;
  window->abi.cancel_buffer = &NativeWindowCancel;
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
    const auto imported = g_imported_surface_identities.find(identity);
    if (imported != g_imported_surface_identities.end()) {
      window->imported_surface_owner_process_id = imported->second.owner_process_id;
      window->imported_surface_layer_id = imported->second.layer_id;
    }
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
  window->java_surface_identity = reinterpret_cast<jlong>(window);
  {
    std::lock_guard<std::mutex> lock(g_android_native_window_mutex);
    g_android_native_windows_by_surface.emplace(window->java_surface_identity,
                                                 window);
  }
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

extern "C" void darwin_art_android_ANativeWindow_set_queue_callback(
    void* opaque, DarwinArtAndroidNativeWindowQueueCallback callback,
    void* context) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) return;
  std::lock_guard<std::mutex> lock(window->mutex);
  window->queue_callback = callback;
  window->queue_context = context;
}

extern "C" void darwin_art_android_ANativeWindow_release_consumer_slot(
    void* opaque, int32_t slot_index, int release_fence) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) {
    if (release_fence >= 0)
      (void)darwin_art_bionic_socket_broker_close(release_fence);
    return;
  }
  std::lock_guard<std::mutex> lock(window->mutex);
  if (slot_index < 0 ||
      static_cast<size_t>(slot_index) >= window->gpu_slots.size()) {
    if (release_fence >= 0)
      (void)darwin_art_bionic_socket_broker_close(release_fence);
    return;
  }
  auto& slot = window->gpu_slots[static_cast<size_t>(slot_index)];
  if (slot.release_fence >= 0)
    (void)darwin_art_bionic_socket_broker_close(slot.release_fence);
  slot.release_fence = release_fence;
  slot.consumer_held = false;
}

extern "C" void darwin_art_android_ANativeWindow_set_surface_control(
    void* opaque, void* opaque_control) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  auto* control = static_cast<ASurfaceControl*>(opaque_control);
  if (window == nullptr) return;
  if (control != nullptr) ASurfaceControl_acquire(control);
  ASurfaceControl* previous = nullptr;
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    previous = window->surface_control;
    window->surface_control = control;
  }
  if (previous != nullptr) ASurfaceControl_release(previous);
}

extern "C" bool darwin_art_android_ANativeWindow_get_surface_control_identity(
    void* opaque, uint32_t* owner_process_id, uint32_t* layer_id) {
  if (opaque == nullptr || owner_process_id == nullptr || layer_id == nullptr) {
    return false;
  }
  DarwinAndroidNativeWindow* window = nullptr;
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    const auto found = g_android_native_windows_by_surface.find(
        reinterpret_cast<jlong>(opaque));
    if (found == g_android_native_windows_by_surface.end()) return false;
    window = found->second;
  }
  std::lock_guard<std::mutex> lock(window->mutex);
  return window->surface_control != nullptr &&
         darwin_art_android_surface_control_get_identity(
             window->surface_control, owner_process_id, layer_id);
}

extern "C" void
darwin_art_android_ANativeWindow_register_imported_surface_identity(
    int64_t surface_identity, uint32_t owner_process_id, uint32_t layer_id) {
  if (surface_identity == 0 || owner_process_id == 0 || layer_id == 0) return;
  std::lock_guard<std::mutex> lock(g_android_native_window_mutex);
  g_imported_surface_identities[surface_identity] = {
      .owner_process_id = owner_process_id,
      .layer_id = layer_id,
  };
  const auto found = g_android_native_windows_by_surface.find(surface_identity);
  if (found != g_android_native_windows_by_surface.end()) {
    found->second->imported_surface_owner_process_id = owner_process_id;
    found->second->imported_surface_layer_id = layer_id;
  }
}

extern "C" bool
darwin_art_android_ANativeWindow_get_imported_surface_identity(
    void* opaque, uint32_t* owner_process_id, uint32_t* layer_id) {
  const auto* window = static_cast<const DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr || owner_process_id == nullptr || layer_id == nullptr ||
      window->imported_surface_owner_process_id == 0 ||
      window->imported_surface_layer_id == 0) {
    return false;
  }
  *owner_process_id = window->imported_surface_owner_process_id;
  *layer_id = window->imported_surface_layer_id;
  return true;
}

extern "C" bool darwin_art_android_ANativeWindow_release_if_managed(
    void* opaque) {
  if (opaque == nullptr) return false;
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  {
    std::lock_guard<std::mutex> lock(g_android_native_window_mutex);
    const auto found = g_android_native_windows_by_surface.find(
        reinterpret_cast<jlong>(opaque));
    if (found == g_android_native_windows_by_surface.end() ||
        found->second != window) {
      return false;
    }
  }
  ReleaseNativeWindow(window);
  return true;
}

extern "C" int32_t
darwin_art_android_ANativeWindow_dequeue_hardware_buffer(
    void* opaque, AHardwareBuffer** out_buffer, void** out_native_buffer,
    int* out_fence) {
  if (opaque == nullptr || out_buffer == nullptr ||
      out_native_buffer == nullptr || out_fence == nullptr) {
    return -EINVAL;
  }
  *out_buffer = nullptr;
  *out_native_buffer = nullptr;
  *out_fence = -1;
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  const int result = NativeWindowDequeue(&window->abi, out_native_buffer,
                                         out_fence);
  if (result != 0) return result;
  *out_buffer = darwin_art_android_hardware_buffer_from_client_buffer(
      *out_native_buffer);
  if (*out_buffer == nullptr) {
    (void)NativeWindowCancel(&window->abi, *out_native_buffer, *out_fence);
    *out_native_buffer = nullptr;
    *out_fence = -1;
    return -EINVAL;
  }
  return 0;
}

extern "C" int32_t darwin_art_android_ANativeWindow_queue_hardware_buffer(
    void* opaque, void* native_buffer, int fence) {
  if (opaque == nullptr) return -EINVAL;
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  return NativeWindowQueue(&window->abi, native_buffer, fence);
}

extern "C" int32_t darwin_art_android_ANativeWindow_cancel_hardware_buffer(
    void* opaque, void* native_buffer, int fence) {
  if (opaque == nullptr) return -EINVAL;
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  return NativeWindowCancel(&window->abi, native_buffer, fence);
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
