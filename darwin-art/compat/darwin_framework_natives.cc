#include "darwin_framework_natives.h"
#include "darwin_framework_system_natives.h"

#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
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
  using namespace framework_system;
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
