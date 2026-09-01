#pragma once

#include <cstddef>
#include <cstdint>

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
// Vulkan's Android external-memory contract exposes FORMAT_R8G8B8A8_UNORM.
// Use an RGBA Metal view of the same IOSurface instead of the BGRA view used
// by EGL and the host compositor.
extern "C" void* darwin_art_android_hardware_buffer_vulkan_metal_texture(
    AHardwareBuffer* buffer, void* metal_device);
extern "C" void* darwin_art_android_iosurface_metal_texture(
    void* iosurface, uint32_t width, uint32_t height, void* metal_device);
extern "C" void darwin_art_android_metal_texture_release(void* texture);
// Creates a Metal shared event on the supplied MTLDevice and reserves the
// next monotonically increasing signal value. The returned event is retained
// until release. A fence descriptor created from it becomes readable only
// when ANGLE/Metal signals that exact value.
extern "C" void* darwin_art_android_metal_shared_event_create(
    void* metal_device, uint64_t* signal_value);
extern "C" int darwin_art_android_metal_shared_event_fence_fd(
    void* shared_event, uint64_t signal_value);
// Returns the next unsignaled Metal event value. The caller uses this value
// for one Vulkan binary-semaphore operation.
extern "C" uint64_t darwin_art_android_metal_shared_event_next_value(
    void* shared_event);
// Consumes an Android sync fence descriptor without blocking the Vulkan
// caller. The shared event is signaled from a background waiter after the
// fence becomes readable, allowing MoltenVK to keep the wait on the GPU.
extern "C" int darwin_art_android_metal_shared_event_import_fence(
    void* shared_event, uint64_t signal_value, int fence_fd);
extern "C" void darwin_art_android_metal_shared_event_release(
    void* shared_event);
extern "C" void* darwin_art_android_hardware_buffer_native_window_buffer(
    AHardwareBuffer* buffer);
extern "C" AHardwareBuffer*
darwin_art_android_hardware_buffer_from_client_buffer(void* client_buffer);
extern "C" void* darwin_art_android_hardware_buffer_iosurface(
    AHardwareBuffer* buffer);

// Framework SurfaceControl JNI and NDK libandroid clients share one retained
// compositor graph. These entry points create the display-root identity and
// reset a reusable transaction without exposing the private implementation.
extern "C" void* darwin_art_android_surface_control_create_root(
    const char* name);
extern "C" bool darwin_art_android_surface_control_get_identity(
    void* control, uint32_t* owner_process_id, uint32_t* layer_id);
extern "C" void darwin_art_android_surface_transaction_clear(
    void* transaction);
extern "C" void darwin_art_android_surface_transaction_merge(
    void* destination, void* source);
extern "C" void darwin_art_android_surface_transaction_set_relative_layer(
    void* transaction, void* control, void* relative_to, int32_t z);
