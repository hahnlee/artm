#pragma once

#include <jni.h>

#include <cstdint>

#include "darwin_android_native_window.h"

namespace darwin_art {

// Registers the Android EGL10 facade and the small GLES20 bootstrap surface
// used by GLSurfaceView. The implementation delegates to ANGLE's Darwin
// dylibs and keeps Android's Java handle objects as the public ABI.
bool RegisterDarwinAngleEglNatives(JNIEnv* env);

extern "C" void* darwin_art_angle_dso_symbol(const char* soname,
                                              const char* symbol);

// Receives generic SurfaceView window geometry from the framework bridge.
// This is intentionally independent of any APK class.
void ConfigureDarwinAngleHostSurface(jint x, jint y, jint width, jint height);
jint DarwinAngleHostSurfaceWidth();
jint DarwinAngleHostSurfaceHeight();

}  // namespace darwin_art

extern "C" {
void* darwin_art_android_ANativeWindow_fromSurface(void* env, void* surface);
void* darwin_art_android_ANativeWindow_create(int32_t width, int32_t height,
                                               int32_t format);
void darwin_art_android_ANativeWindow_acquire(void* window);
int32_t darwin_art_android_ANativeWindow_getFormat(void* window);
void* darwin_art_android_ANativeWindow_toSurface(void* env, void* window);
void darwin_art_android_ANativeWindow_release(void* window);
int32_t darwin_art_android_ANativeWindow_lock(void* window, void* buffer,
                                              void* dirty_bounds);
int32_t darwin_art_android_ANativeWindow_unlockAndPost(void* window);
int32_t darwin_art_android_ANativeWindow_setBuffersGeometry(
    void* window, int32_t width, int32_t height, int32_t format);

void* darwin_art_android_eglCreateWindowSurface(void* display, void* config,
                                                void* window,
                                                const int32_t* attributes);
uint32_t darwin_art_android_eglSwapBuffers(void* display, void* surface);
uint32_t darwin_art_android_eglDestroySurface(void* display, void* surface);

// Debug-only GLES forwarding hooks selected by the ELF resolver when
// DARWIN_ART_DEBUG_ANGLE is enabled. They preserve the Android GLES ABI while
// exposing whether a native renderer reaches ANGLE's texture/draw stages.
void darwin_art_android_glTexImage2D(uint32_t target, int32_t level,
                                    int32_t internal_format, int32_t width,
                                    int32_t height, int32_t border,
                                    uint32_t format, uint32_t type,
                                    const void* pixels);
void darwin_art_android_glTexSubImage2D(uint32_t target, int32_t level,
                                       int32_t x, int32_t y, int32_t width,
                                       int32_t height, uint32_t format,
                                       uint32_t type, const void* pixels);
void darwin_art_android_glDrawArrays(uint32_t mode, int32_t first,
                                     int32_t count);
void darwin_art_android_glDrawElements(uint32_t mode, int32_t count,
                                       uint32_t type, const void* indices);
void darwin_art_android_glUseProgram(uint32_t program);
bool darwin_art_android_begin_hardware_buffer_composition(void* buffer,
                                                          bool clear);
void darwin_art_android_set_hardware_buffer_composition_active(bool active);
void darwin_art_android_end_hardware_buffer_composition();
// Marks the exact BufferQueue slot displaced by a SurfaceControl transaction.
// Its persistent IOSurface remains canonical until the producer reacquires the
// slot; the next draw-FBO bind restores it into ANGLE's 2D staging texture.
void darwin_art_android_mark_hardware_buffer_released(void* buffer);
void darwin_art_android_present_hardware_buffer(
    void* queue, void* buffer, int32_t source_left, int32_t source_top,
    int32_t source_right, int32_t source_bottom, int32_t destination_left,
    int32_t destination_top, int32_t destination_right,
    int32_t destination_bottom, bool has_damage, int32_t damage_left,
    int32_t damage_top, int32_t damage_right, int32_t damage_bottom,
    float alpha);
}
