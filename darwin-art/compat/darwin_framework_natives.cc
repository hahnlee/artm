#include "darwin_framework_natives.h"
#include "darwin_angle_egl.h"
#include "darwin_framework_system_natives.h"
#include "darwin_motion_event_natives.h"

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <atomic>
#include <new>
#include <unordered_map>
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
void SurfaceControlNativeSetTransformHint(JNIEnv*, jclass, jlong, jint) {}
void SurfaceControlNativeApplyTransaction(JNIEnv*, jclass, jlong, jboolean,
                                          jboolean) {}

std::atomic<jlong> g_egl_handle{1};

struct DarwinVelocitySample {
  jlong time_ms = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct DarwinVelocityTracker {
  std::unordered_map<jint, std::vector<DarwinVelocitySample>> samples;
  std::unordered_map<jint, std::array<float, 2>> computed;
  jint active_pointer_id = -1;
};

void AddVelocitySample(DarwinVelocityTracker* tracker, jint pointer_id,
                       jlong time_ms, float x, float y) {
  if (tracker == nullptr || time_ms < 0 || !std::isfinite(x) ||
      !std::isfinite(y)) {
    return;
  }
  auto& samples = tracker->samples[pointer_id];
  if (!samples.empty() && samples.back().time_ms == time_ms) {
    samples.back() = {time_ms, x, y};
  } else {
    samples.push_back({time_ms, x, y});
  }
  const jlong horizon = time_ms - 200;
  samples.erase(std::remove_if(samples.begin(), samples.end(),
                               [horizon](const DarwinVelocitySample& sample) {
                                 return sample.time_ms < horizon;
                               }),
                samples.end());
  if (samples.size() > 20) {
    samples.erase(samples.begin(), samples.end() - 20);
  }
  if (tracker->active_pointer_id < 0) tracker->active_pointer_id = pointer_id;
}

float EstimateVelocity(const std::vector<DarwinVelocitySample>& samples,
                       jint axis, jint units, float max_velocity) {
  if (samples.size() < 2 || units <= 0) return 0.0f;
  const jlong newest = samples.back().time_ms;
  size_t first = 0;
  while (first + 1 < samples.size() && samples[first].time_ms < newest - 100) {
    ++first;
  }
  const size_t count = samples.size() - first;
  if (count < 2) return 0.0f;
  double mean_time = 0.0;
  double mean_position = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    mean_time += static_cast<double>(samples[index].time_ms - newest);
    mean_position += axis == 0 ? samples[index].x : samples[index].y;
  }
  mean_time /= static_cast<double>(count);
  mean_position /= static_cast<double>(count);
  double numerator = 0.0;
  double denominator = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    const double time = static_cast<double>(samples[index].time_ms - newest) -
                        mean_time;
    const double position =
        static_cast<double>(axis == 0 ? samples[index].x : samples[index].y) -
        mean_position;
    numerator += time * position;
    denominator += time * time;
  }
  if (denominator <= 0.0) return 0.0f;
  const float velocity =
      static_cast<float>((numerator / denominator) * units);
  return std::clamp(velocity, -max_velocity, max_velocity);
}

jlong VelocityTrackerInitialize(JNIEnv*, jclass, jint) {
  auto* tracker = new (std::nothrow) DarwinVelocityTracker();
  return reinterpret_cast<jlong>(tracker);
}

void VelocityTrackerDispose(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<DarwinVelocityTracker*>(handle);
}

void VelocityTrackerAddMovement(JNIEnv* env, jclass, jlong handle, jobject event) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr || event == nullptr) return;
  jclass event_class = env->GetObjectClass(event);
  jmethodID get_pointer_count =
      env->GetMethodID(event_class, "getPointerCount", "()I");
  jmethodID get_pointer_id = env->GetMethodID(event_class, "getPointerId", "(I)I");
  jmethodID get_history_size =
      env->GetMethodID(event_class, "getHistorySize", "()I");
  jmethodID get_historical_time =
      env->GetMethodID(event_class, "getHistoricalEventTime", "(I)J");
  jmethodID get_historical_x =
      env->GetMethodID(event_class, "getHistoricalX", "(II)F");
  jmethodID get_historical_y =
      env->GetMethodID(event_class, "getHistoricalY", "(II)F");
  jmethodID get_event_time = env->GetMethodID(event_class, "getEventTime", "()J");
  jmethodID get_x = env->GetMethodID(event_class, "getX", "(I)F");
  jmethodID get_y = env->GetMethodID(event_class, "getY", "(I)F");
  if (env->ExceptionCheck() || get_pointer_count == nullptr ||
      get_pointer_id == nullptr || get_history_size == nullptr ||
      get_historical_time == nullptr || get_historical_x == nullptr ||
      get_historical_y == nullptr || get_event_time == nullptr ||
      get_x == nullptr || get_y == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(event_class);
    return;
  }
  const jint pointer_count = env->CallIntMethod(event, get_pointer_count);
  const jint history_size = env->CallIntMethod(event, get_history_size);
  for (jint pointer_index = 0; pointer_index < pointer_count; ++pointer_index) {
    const jint pointer_id =
        env->CallIntMethod(event, get_pointer_id, pointer_index);
    for (jint history_index = 0; history_index < history_size; ++history_index) {
      AddVelocitySample(
          tracker, pointer_id,
          env->CallLongMethod(event, get_historical_time, history_index),
          env->CallFloatMethod(event, get_historical_x, pointer_index,
                               history_index),
          env->CallFloatMethod(event, get_historical_y, pointer_index,
                               history_index));
    }
    AddVelocitySample(tracker, pointer_id,
                      env->CallLongMethod(event, get_event_time),
                      env->CallFloatMethod(event, get_x, pointer_index),
                      env->CallFloatMethod(event, get_y, pointer_index));
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(event_class);
}

void VelocityTrackerClear(JNIEnv*, jclass, jlong handle) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr) return;
  tracker->samples.clear();
  tracker->computed.clear();
  tracker->active_pointer_id = -1;
}

void VelocityTrackerComputeCurrentVelocity(JNIEnv*, jclass, jlong handle,
                                           jint units, jfloat max_velocity) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr) return;
  tracker->computed.clear();
  for (const auto& [pointer_id, samples] : tracker->samples) {
    tracker->computed[pointer_id] = {
        EstimateVelocity(samples, 0, units, max_velocity),
        EstimateVelocity(samples, 1, units, max_velocity)};
    if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
      std::cerr << "ART Android VelocityTracker pointer=" << pointer_id
                << " samples=" << samples.size()
                << " vx=" << tracker->computed[pointer_id][0]
                << " vy=" << tracker->computed[pointer_id][1] << "\n";
    }
  }
}

jfloat VelocityTrackerGetVelocity(JNIEnv*, jclass, jlong handle, jint axis,
                                  jint pointer_id) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr || (axis != 0 && axis != 1)) return 0.0f;
  if (pointer_id == -1) pointer_id = tracker->active_pointer_id;
  const auto velocity = tracker->computed.find(pointer_id);
  return velocity == tracker->computed.end() ? 0.0f : velocity->second[axis];
}
jboolean VelocityTrackerIsAxisSupported(JNIEnv*, jclass, jint axis) {
  return axis == 0 || axis == 1 ? JNI_TRUE : JNI_FALSE;
}

jlong NextEglHandle() { return g_egl_handle.fetch_add(1, std::memory_order_relaxed); }

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
extern "C" JNIEXPORT jlong Java_android_os_Process_getElapsedCpuTime(
    JNIEnv* env, jclass clazz) {
  return darwin_art::framework_system::process_get_elapsed_cpu_time(env, clazz);
}

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
  JNINativeMethod velocity_tracker_methods[] = {
      {const_cast<char*>("nativeInitialize"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&VelocityTrackerInitialize)},
      {const_cast<char*>("nativeDispose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&VelocityTrackerDispose)},
      {const_cast<char*>("nativeAddMovement"),
       const_cast<char*>("(JLandroid/view/MotionEvent;)V"),
       reinterpret_cast<void*>(&VelocityTrackerAddMovement)},
      {const_cast<char*>("nativeClear"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&VelocityTrackerClear)},
      {const_cast<char*>("nativeComputeCurrentVelocity"), const_cast<char*>("(JIF)V"),
       reinterpret_cast<void*>(&VelocityTrackerComputeCurrentVelocity)},
      {const_cast<char*>("nativeGetVelocity"), const_cast<char*>("(JII)F"),
       reinterpret_cast<void*>(&VelocityTrackerGetVelocity)},
      {const_cast<char*>("nativeIsAxisSupported"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&VelocityTrackerIsAxisSupported)},
  };
  if (!Register(env, "android/view/VelocityTracker", velocity_tracker_methods,
                static_cast<jint>(std::size(velocity_tracker_methods)))) {
    return false;
  }
  using namespace framework_system;
  JNINativeMethod process_methods[] = {
      {const_cast<char*>("setThreadPriority"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&ProcessSetThreadPriority)},
      {const_cast<char*>("getElapsedCpuTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&process_get_elapsed_cpu_time)},
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
      {const_cast<char*>("nativeApplyTransaction"),
       const_cast<char*>("(JZZ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeApplyTransaction)},
      {const_cast<char*>("nativeSetTransformHint"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetTransformHint)},
  };
  if (!Register(env, "android/view/SurfaceControl", surface_control_methods,
                static_cast<jint>(std::size(surface_control_methods)))) {
    return false;
  }

  if (!darwin_art::RegisterDarwinAngleEglNatives(env)) {
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

  if (!RegisterFrameworkSqliteNatives(env)) {
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
