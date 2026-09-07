#include <android/native_window.h>

#include <cerrno>
#include <cstdint>

namespace {
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
};

constexpr int kNativeWindowIsValid = 17;
constexpr int kNativeWindowSetBuffersDataspace = 19;
constexpr int kNativeWindowDataspace = 20;
}  // namespace

extern "C" int32_t ANativeWindow_setBuffersDataSpace(
    ANativeWindow* native_window, int32_t dataspace) {
  auto* window = reinterpret_cast<AndroidNativeWindowAbi*>(native_window);
  int valid = 0;
  if (window == nullptr || window->query == nullptr ||
      window->perform == nullptr ||
      window->query(window, kNativeWindowIsValid, &valid) != 0 || valid == 0) {
    return -EINVAL;
  }
  return window->perform(window, kNativeWindowSetBuffersDataspace, dataspace);
}

extern "C" int32_t ANativeWindow_getBuffersDataSpace(
    ANativeWindow* native_window) {
  auto* window = reinterpret_cast<AndroidNativeWindowAbi*>(native_window);
  int dataspace = 0;
  if (window == nullptr || window->query == nullptr ||
      window->query(window, kNativeWindowDataspace, &dataspace) != 0) {
    return -EINVAL;
  }
  return dataspace;
}

// Frame-rate hints are advisory on Android: SurfaceFlinger may change the
// display mode, but rendering remains correct when the hint is ignored.  The
// Metal compositor owns the display cadence in Darwin ART, so accept the
// NDK calls as successful no-ops rather than leaving the API unresolved for
// engines (notably Unity/Swappy) that resolve them lazily.
extern "C" int32_t ANativeWindow_setFrameRate(
    ANativeWindow* native_window, float, int8_t) {
  auto* window = reinterpret_cast<AndroidNativeWindowAbi*>(native_window);
  if (window == nullptr || window->query == nullptr) return -EINVAL;
  int valid = 0;
  return window->query(window, kNativeWindowIsValid, &valid) == 0 && valid
             ? 0
             : -EINVAL;
}

extern "C" int32_t ANativeWindow_setFrameRateWithChangeStrategy(
    ANativeWindow* native_window, float frame_rate, int8_t compatibility,
    int8_t) {
  return ANativeWindow_setFrameRate(native_window, frame_rate, compatibility);
}
