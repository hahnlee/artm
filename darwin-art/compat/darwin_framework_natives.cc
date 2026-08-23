#include "darwin_framework_natives.h"
#include "darwin_framework_system_natives.h"
#include "darwin_motion_event_natives.h"

#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
#include <atomic>
#include <vector>

namespace {

void NativeAllocationRegistryApplyFreeFunction(JNIEnv*, jclass,
                                                jlong free_function,
                                                jlong native_ptr) {
  if (free_function == 0 || native_ptr == 0) return;
  using FreeFunction = void (*)(void*);
  reinterpret_cast<FreeFunction>(static_cast<std::uintptr_t>(free_function))(
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(native_ptr)));
}

// Android's HandlerThread calls this before starting framework animation and
// GL worker loops. Darwin does not expose Android's numeric scheduler
// priorities, so keep the call successful and leave the host QoS unchanged.
void ProcessSetThreadPriority(JNIEnv*, jclass, jint) {}

// SurfaceView is part of the stock Skottie layout.  The Darwin window owns
// the actual CAMetalLayer, so SurfaceControl's server-side handle is not
// materialized; keep the Java NativeAllocationRegistry contract valid with a
// process-local no-op finalizer until a real compositor transaction is used.
void SurfaceControlNoopFinalizer(void*) {}
jlong SurfaceControlNativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&SurfaceControlNoopFinalizer);
}
jlong NextEglHandle();
jlong SurfaceControlNativeCreateTransaction(JNIEnv*, jclass) { return NextEglHandle(); }

std::atomic<jlong> g_egl_handle{1};

jlong NextEglHandle() { return g_egl_handle.fetch_add(1, std::memory_order_relaxed); }

void EglNativeClassInit(JNIEnv*, jclass) {}
void GlNativeClassInit(JNIEnv*, jclass) {}
jlong EglCreateContext(JNIEnv*, jobject, jobject, jobject, jobject, jintArray) {
  return NextEglHandle();
}
jlong EglCreatePbufferSurface(JNIEnv*, jobject, jobject, jintArray) {
  return NextEglHandle();
}
void EglCreatePixmapSurface(JNIEnv*, jobject, jobject, jobject, jobject, jintArray) {}
jlong EglCreateWindowSurface(JNIEnv*, jobject, jobject, jobject, jobject, jintArray) {
  return NextEglHandle();
}
jlong EglCreateWindowSurfaceTexture(JNIEnv*, jobject, jobject, jobject, jobject,
                                     jintArray) {
  return NextEglHandle();
}
jlong EglGetCurrentContext(JNIEnv*, jobject) { return 1; }
jlong EglGetCurrentDisplay(JNIEnv*, jobject) { return 1; }
jlong EglGetCurrentSurface(JNIEnv*, jobject, jint) { return 1; }
jlong EglGetDisplay(JNIEnv*, jobject, jobject) { return 1; }
jint EglGetInitCount(JNIEnv*, jclass, jobject) { return 1; }

jobject NewEglConfig(JNIEnv* env, jlong handle) {
  jclass config = env->FindClass("com/google/android/gles_jni/EGLConfigImpl");
  if (config == nullptr) return nullptr;
  jmethodID ctor = env->GetMethodID(config, "<init>", "(J)V");
  jobject result = ctor == nullptr ? nullptr : env->NewObject(config, ctor, handle);
  env->DeleteLocalRef(config);
  return result;
}

jboolean EglChooseConfig(JNIEnv* env, jobject, jobject, jintArray, jobjectArray configs,
                         jint config_size, jintArray count) {
  if (count != nullptr && env->GetArrayLength(count) > 0) {
    jint one = 1;
    env->SetIntArrayRegion(count, 0, 1, &one);
  }
  if (configs != nullptr && config_size > 0 && env->GetArrayLength(configs) > 0) {
    jobject config = NewEglConfig(env, NextEglHandle());
    if (config == nullptr) return JNI_FALSE;
    env->SetObjectArrayElement(configs, 0, config);
    env->DeleteLocalRef(config);
  }
  return JNI_TRUE;
}

jboolean EglCopyBuffers(JNIEnv*, jobject, jobject, jobject, jobject) { return JNI_TRUE; }
jboolean EglDestroyContext(JNIEnv*, jobject, jobject, jobject) { return JNI_TRUE; }
jboolean EglDestroySurface(JNIEnv*, jobject, jobject, jobject) { return JNI_TRUE; }
jboolean EglGetConfigAttrib(JNIEnv* env, jobject, jobject, jobject, jint attr,
                            jintArray value) {
  if (value != nullptr && env->GetArrayLength(value) > 0) {
    jint answer = (attr == 0x3024 || attr == 0x3023 || attr == 0x3022 || attr == 0x3021)
                     ? 8 : (attr == 0x3025 || attr == 0x3026 ? 0 : 4);
    env->SetIntArrayRegion(value, 0, 1, &answer);
  }
  return JNI_TRUE;
}
jboolean EglGetConfigs(JNIEnv* env, jobject self, jobject display, jobjectArray configs,
                       jint size, jintArray count) {
  return EglChooseConfig(env, self, display, nullptr, configs, size, count);
}
jint EglGetError(JNIEnv*, jobject) { return 0x3000; }
jboolean EglInitialize(JNIEnv* env, jobject, jobject, jintArray version) {
  if (version != nullptr && env->GetArrayLength(version) >= 2) {
    jint values[2] = {1, 5};
    env->SetIntArrayRegion(version, 0, 2, values);
  }
  return JNI_TRUE;
}
jboolean EglMakeCurrent(JNIEnv*, jobject, jobject, jobject, jobject, jobject) {
  return JNI_TRUE;
}
jboolean EglQueryContext(JNIEnv* env, jobject, jobject, jobject, jint, jintArray value) {
  if (value != nullptr && env->GetArrayLength(value) > 0) {
    jint answer = 2;
    env->SetIntArrayRegion(value, 0, 1, &answer);
  }
  return JNI_TRUE;
}
jstring EglQueryString(JNIEnv* env, jobject, jobject, jint) {
  return env->NewStringUTF("Darwin Metal EGL");
}
jboolean EglQuerySurface(JNIEnv* env, jobject, jobject, jobject, jint, jintArray value) {
  if (value != nullptr && env->GetArrayLength(value) > 0) {
    jint answer = 1;
    env->SetIntArrayRegion(value, 0, 1, &answer);
  }
  return JNI_TRUE;
}
jboolean EglReleaseThread(JNIEnv*, jobject) { return JNI_TRUE; }
jboolean EglSwapBuffers(JNIEnv*, jobject, jobject, jobject) { return JNI_TRUE; }
jboolean EglTerminate(JNIEnv*, jobject, jobject) { return JNI_TRUE; }
jboolean EglWaitGL(JNIEnv*, jobject) { return JNI_TRUE; }
jboolean EglWaitNative(JNIEnv*, jobject, jint, jobject) { return JNI_TRUE; }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
struct DarwinPaint {
  jint flags = 0;
  jint color = 0xff000000;
};
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
void PaintFinalizer(void* paint) { delete static_cast<DarwinPaint*>(paint); }

jlong PaintInit() {
  return reinterpret_cast<std::uintptr_t>(new DarwinPaint());
}

jlong PaintGetNativeFinalizer() {
  return reinterpret_cast<std::uintptr_t>(&PaintFinalizer);
}

void PaintSetFlags(jlong handle, jint flags) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->flags = flags;
  }
}

void PaintSetElegantTextHeight(jlong, jint) {}

jint PaintSetTextLocales(JNIEnv*, jclass, jlong, jstring) { return 0; }

void PaintSetColor(jlong handle, jint color) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->color = color;
  }
}
#endif

bool Register(JNIEnv* env, const char* class_name, JNINativeMethod* methods,
              jint method_count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) {
    return false;
  }
  const bool registered =
      env->RegisterNatives(klass, methods, method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}

}  // namespace

// Keep the dynamic JNI fallback as well as the explicit table registration.
// Some framework threads resolve Process natives before the framework table is
// visible through their boot-class loader.
extern "C" JNIEXPORT void Java_android_os_Process_setThreadPriority(
    JNIEnv*, jclass, jint) {}

namespace darwin_art {

bool RegisterFrameworkSupportNatives(JNIEnv* env) {
  JNINativeMethod native_allocation_methods[] = {
      {const_cast<char*>("applyFreeFunction"), const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&NativeAllocationRegistryApplyFreeFunction)},
  };
  return Register(env, "libcore/util/NativeAllocationRegistry",
                  native_allocation_methods,
                  static_cast<jint>(std::size(native_allocation_methods)));
}

bool RegisterFrameworkNatives(JNIEnv* env) {
  if (!RegisterMotionEventNatives(env)) {
    return false;
  }
  using namespace framework_system;
  JNINativeMethod process_methods[] = {
      {const_cast<char*>("setThreadPriority"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&ProcessSetThreadPriority)},
  };
  if (!Register(env, "android/os/Process", process_methods,
                static_cast<jint>(std::size(process_methods)))) {
    return false;
  }

  JNINativeMethod surface_control_methods[] = {
      {const_cast<char*>("nativeGetNativeSurfaceControlFinalizer"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceControlNativeGetFinalizer)},
      {const_cast<char*>("nativeGetNativeTransactionFinalizer"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceControlNativeGetFinalizer)},
      {const_cast<char*>("nativeCreateTransaction"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceControlNativeCreateTransaction)},
  };
  if (!Register(env, "android/view/SurfaceControl", surface_control_methods,
                static_cast<jint>(std::size(surface_control_methods)))) {
    return false;
  }

  JNINativeMethod egl_methods[] = {
      {const_cast<char*>("_nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&EglNativeClassInit)},
      {const_cast<char*>("_eglCreateContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljavax/microedition/khronos/egl/EGLContext;[I)J"),
       reinterpret_cast<void*>(&EglCreateContext)},
      {const_cast<char*>("_eglCreatePbufferSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;[I)J"),
       reinterpret_cast<void*>(&EglCreatePbufferSurface)},
      {const_cast<char*>("_eglCreatePixmapSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)V"),
       reinterpret_cast<void*>(&EglCreatePixmapSurface)},
      {const_cast<char*>("_eglCreateWindowSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)J"),
       reinterpret_cast<void*>(&EglCreateWindowSurface)},
      {const_cast<char*>("_eglCreateWindowSurfaceTexture"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)J"),
       reinterpret_cast<void*>(&EglCreateWindowSurfaceTexture)},
      {const_cast<char*>("_eglGetCurrentContext"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&EglGetCurrentContext)},
      {const_cast<char*>("_eglGetCurrentDisplay"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&EglGetCurrentDisplay)},
      {const_cast<char*>("_eglGetCurrentSurface"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&EglGetCurrentSurface)},
      {const_cast<char*>("_eglGetDisplay"), const_cast<char*>("(Ljava/lang/Object;)J"),
       reinterpret_cast<void*>(&EglGetDisplay)},
      {const_cast<char*>("getInitCount"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;)I"),
       reinterpret_cast<void*>(&EglGetInitCount)},
      {const_cast<char*>("eglChooseConfig"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[I[Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglChooseConfig)},
      {const_cast<char*>("eglCopyBuffers"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;Ljava/lang/Object;)Z"),
       reinterpret_cast<void*>(&EglCopyBuffers)},
      {const_cast<char*>("eglDestroyContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLContext;)Z"),
       reinterpret_cast<void*>(&EglDestroyContext)},
      {const_cast<char*>("eglDestroySurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;)Z"),
       reinterpret_cast<void*>(&EglDestroySurface)},
      {const_cast<char*>("eglGetConfigAttrib"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglGetConfigAttrib)},
      {const_cast<char*>("eglGetConfigs"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglGetConfigs)},
      {const_cast<char*>("eglGetError"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&EglGetError)},
      {const_cast<char*>("eglInitialize"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[I)Z"),
       reinterpret_cast<void*>(&EglInitialize)},
      {const_cast<char*>("eglMakeCurrent"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLContext;)Z"),
       reinterpret_cast<void*>(&EglMakeCurrent)},
      {const_cast<char*>("eglQueryContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLContext;I[I)Z"),
       reinterpret_cast<void*>(&EglQueryContext)},
      {const_cast<char*>("eglQueryString"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&EglQueryString)},
      {const_cast<char*>("eglQuerySurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;I[I)Z"),
       reinterpret_cast<void*>(&EglQuerySurface)},
      {const_cast<char*>("eglReleaseThread"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&EglReleaseThread)},
      {const_cast<char*>("eglSwapBuffers"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;)Z"),
       reinterpret_cast<void*>(&EglSwapBuffers)},
      {const_cast<char*>("eglTerminate"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;)Z"),
       reinterpret_cast<void*>(&EglTerminate)},
      {const_cast<char*>("eglWaitGL"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&EglWaitGL)},
      {const_cast<char*>("eglWaitNative"),
       const_cast<char*>("(ILjava/lang/Object;)Z"),
       reinterpret_cast<void*>(&EglWaitNative)},
  };
  if (!Register(env, "com/google/android/gles_jni/EGLImpl", egl_methods,
                static_cast<jint>(std::size(egl_methods)))) {
    return false;
  }
  JNINativeMethod gl_methods[] = {
      {const_cast<char*>("_nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&GlNativeClassInit)},
  };
  if (!Register(env, "com/google/android/gles_jni/GLImpl", gl_methods,
                static_cast<jint>(std::size(gl_methods)))) {
    return false;
  }

  JNINativeMethod message_queue_methods[] = {
      {const_cast<char*>("nativeInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&message_queue_native_init)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_destroy)},
      {const_cast<char*>("nativePollOnce"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&message_queue_native_poll_once)},
      {const_cast<char*>("nativeWake"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_wake)},
      {const_cast<char*>("nativeIsPolling"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&message_queue_native_is_polling)},
      {const_cast<char*>("nativeSetFileDescriptorEvents"),
       const_cast<char*>("(JII)V"),
       reinterpret_cast<void*>(&message_queue_native_set_file_descriptor_events)},
  };
  if (!Register(env, "android/os/MessageQueue", message_queue_methods,
                static_cast<jint>(std::size(message_queue_methods)))) {
    return false;
  }

  if (!RegisterFrameworkAnimationNatives(env)) {
    return false;
  }

  JNINativeMethod event_log_methods[] = {
      {const_cast<char*>("writeEvent"),
       const_cast<char*>("(I[Ljava/lang/Object;)I"),
       reinterpret_cast<void*>(&event_log_write_event)},
  };
  if (!Register(env, "android/util/EventLog", event_log_methods,
                static_cast<jint>(std::size(event_log_methods)))) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod log_methods[] = {
      {const_cast<char*>("isLoggable"),
       const_cast<char*>("(Ljava/lang/String;I)Z"),
       reinterpret_cast<void*>(&log_is_loggable)},
      {const_cast<char*>("println_native"),
       const_cast<char*>(
           "(IILjava/lang/String;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&log_println)},
  };
  if (!Register(env, "android/util/Log", log_methods,
                static_cast<jint>(std::size(log_methods)))) {
    return false;
  }
#endif

  JNINativeMethod trace_methods[] = {
      {const_cast<char*>("nativeIsTagEnabled"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&trace_is_tag_enabled)},
  };
  if (!Register(env, "android/os/Trace", trace_methods,
                static_cast<jint>(std::size(trace_methods)))) {
    return false;
  }

  JNINativeMethod system_clock_methods[] = {
      {const_cast<char*>("currentThreadTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_current_thread_time_millis)},
      {const_cast<char*>("elapsedRealtime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime)},
      {const_cast<char*>("elapsedRealtimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime_nanos)},
      {const_cast<char*>("uptimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_millis)},
      {const_cast<char*>("uptimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_nanos)},
  };
  if (!Register(env, "android/os/SystemClock", system_clock_methods,
                static_cast<jint>(std::size(system_clock_methods)))) {
    return false;
  }

  if (!RegisterFrameworkBinderNatives(env)) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  if (!RegisterFrameworkRenderNodeNatives(env)) {
    return false;
  }

  JNINativeMethod paint_methods[] = {
      {const_cast<char*>("nInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintInit)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintGetNativeFinalizer)},
      {const_cast<char*>("nSetFlags"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetFlags)},
      {const_cast<char*>("nSetElegantTextHeight"),
       const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetElegantTextHeight)},
      {const_cast<char*>("nSetTextLocales"),
       const_cast<char*>("(JLjava/lang/String;)I"),
       reinterpret_cast<void*>(&PaintSetTextLocales)},
      {const_cast<char*>("nSetColor"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetColor)},
  };
  if (!Register(env, "android/graphics/Paint", paint_methods,
                static_cast<jint>(std::size(paint_methods)))) {
    return false;
  }
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  if (!RegisterFrameworkAssetManagerNatives(env)) {
    return false;
  }
#endif

  if (!RegisterFrameworkSystemPropertyNatives(env)) {
    return false;
  }

  return true;
}

}  // namespace darwin_art
