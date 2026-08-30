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
