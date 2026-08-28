#pragma once

#include <cstddef>

// Exact, closed libandroid.so provider used by the Android ELF resolver.
// Returns null for every symbol not implemented by this compatibility layer.
extern "C" void* darwin_art_android_platform_symbol(const char* symbol);

// Drains callbacks registered on the current thread's Android native ALooper
// without blocking. The detached host calls this at the same boundary where
// Android MessageQueue.nativePollOnce would normally service ALooper fds.
extern "C" int darwin_art_android_platform_poll_current_looper();

// Android's Java MessageQueue and native ALooper are two views of one poll
// loop. These helpers let the framework JNI bridge share that loop without
// exposing the private host-side ALooper representation.
extern "C" void* darwin_art_android_platform_prepare_current_looper();
extern "C" int darwin_art_android_platform_poll_current_looper_timeout(
    int timeout_ms);
extern "C" void darwin_art_android_platform_wake_looper(void* looper);

// SCM_RIGHTS transports the Darwin file description but not Android ashmem's
// region-wide protection metadata. Binder carries this sidecar between app
// processes and re-adopts the received descriptor into the local provider.
extern "C" int darwin_art_android_shared_memory_get_info(
    int fd, size_t* size, int* protection);
extern "C" int darwin_art_android_shared_memory_adopt(
    int fd, size_t size, int protection);

// Creates a Metal 2D texture over the exact IOSurface storage owned by an
// AHardwareBuffer. The device must be the MTLDevice exported by ANGLE's EGL
// display. The returned Objective-C object is retained until the matching
// release call.
struct AHardwareBuffer;
extern "C" void* darwin_art_android_hardware_buffer_metal_texture(
    AHardwareBuffer* buffer, void* metal_device);
extern "C" void* darwin_art_android_iosurface_metal_texture(
    void* iosurface, uint32_t width, uint32_t height, void* metal_device);
extern "C" void darwin_art_android_metal_texture_release(void* texture);
