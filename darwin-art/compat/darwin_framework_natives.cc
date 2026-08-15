#include "darwin_framework_natives.h"

#include <mach/mach_time.h>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

class DarwinMessageQueue {
 public:
  void Poll(jint timeout_millis) {
    std::unique_lock lock(mutex_);
    polling_ = true;
    if (!wake_pending_) {
      if (timeout_millis < 0) {
        condition_.wait(lock, [this] { return wake_pending_; });
      } else if (timeout_millis > 0) {
        condition_.wait_for(lock, std::chrono::milliseconds(timeout_millis),
                            [this] { return wake_pending_; });
      }
    }
    wake_pending_ = false;
    polling_ = false;
  }

  void Wake() {
    std::lock_guard lock(mutex_);
    wake_pending_ = true;
    condition_.notify_one();
  }

  bool IsPolling() {
    std::lock_guard lock(mutex_);
    return polling_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool wake_pending_ = false;
  bool polling_ = false;
};

std::mutex g_system_properties_mutex;
std::unordered_map<std::string, std::string> g_system_properties;

std::optional<std::string> JavaString(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) {
    return std::nullopt;
  }
  std::string result(utf);
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

std::optional<std::string> GetSystemProperty(JNIEnv* env, jstring key) {
  const std::optional<std::string> name = JavaString(env, key);
  if (!name.has_value()) {
    return std::nullopt;
  }
  std::lock_guard lock(g_system_properties_mutex);
  const auto found = g_system_properties.find(*name);
  return found == g_system_properties.end()
             ? std::nullopt
             : std::optional<std::string>(found->second);
}

DarwinMessageQueue* ToMessageQueue(jlong handle) {
  return reinterpret_cast<DarwinMessageQueue*>(
      static_cast<std::uintptr_t>(handle));
}

jlong MessageQueueNativeInit(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(new DarwinMessageQueue());
}

void MessageQueueNativeDestroy(JNIEnv*, jclass, jlong handle) {
  delete ToMessageQueue(handle);
}

void MessageQueueNativePollOnce(JNIEnv*, jobject, jlong handle,
                                jint timeout_millis) {
  if (DarwinMessageQueue* queue = ToMessageQueue(handle); queue != nullptr) {
    queue->Poll(timeout_millis);
  }
}

void MessageQueueNativeWake(JNIEnv*, jclass, jlong handle) {
  if (DarwinMessageQueue* queue = ToMessageQueue(handle); queue != nullptr) {
    queue->Wake();
  }
}

jboolean MessageQueueNativeIsPolling(JNIEnv*, jclass, jlong handle) {
  DarwinMessageQueue* queue = ToMessageQueue(handle);
  return queue != nullptr && queue->IsPolling() ? JNI_TRUE : JNI_FALSE;
}

void MessageQueueNativeSetFileDescriptorEvents(JNIEnv*, jclass, jlong, jint,
                                               jint) {
  // File-descriptor polling will use kqueue. Activity/Handler construction does
  // not register descriptors, so the first framework gate keeps this explicit.
}

jboolean LogIsLoggable(JNIEnv*, jclass, jstring, jint priority) {
  // Android's default log threshold is INFO when no per-tag system property
  // overrides it. A Darwin property bridge can replace this policy later.
  constexpr jint kInfoPriority = 4;
  return priority >= kInfoPriority ? JNI_TRUE : JNI_FALSE;
}

jlong MachTicksToNanos(std::uint64_t ticks) {
  static mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t value{};
    mach_timebase_info(&value);
    return value;
  }();
  return static_cast<jlong>(
      (static_cast<unsigned __int128>(ticks) * timebase.numer) /
      timebase.denom);
}

jlong SystemClockUptimeNanos(JNIEnv*, jclass) {
  return MachTicksToNanos(mach_absolute_time());
}

jlong SystemClockUptimeMillis(JNIEnv* env, jclass klass) {
  return SystemClockUptimeNanos(env, klass) / 1'000'000;
}

jlong SystemClockElapsedRealtimeNanos(JNIEnv*, jclass) {
  return MachTicksToNanos(mach_continuous_time());
}

jlong SystemClockElapsedRealtime(JNIEnv* env, jclass klass) {
  return SystemClockElapsedRealtimeNanos(env, klass) / 1'000'000;
}

jlong SystemClockCurrentThreadTimeMillis(JNIEnv*, jclass) {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
    return 0;
  }
  return static_cast<jlong>(value.tv_sec) * 1'000 + value.tv_nsec / 1'000'000;
}

jstring SystemPropertiesGet(JNIEnv* env, jclass, jstring key,
                            jstring default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
  return value.has_value() ? env->NewStringUTF(value->c_str()) : default_value;
}

template <typename Integer>
Integer ParseSystemPropertyInteger(JNIEnv* env, jstring key,
                                   Integer default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
  if (!value.has_value()) {
    return default_value;
  }
  Integer parsed{};
  const auto result =
      std::from_chars(value->data(), value->data() + value->size(), parsed);
  return result.ec == std::errc{} && result.ptr == value->data() + value->size()
             ? parsed
             : default_value;
}

jint SystemPropertiesGetInt(JNIEnv* env, jclass, jstring key,
                            jint default_value) {
  return ParseSystemPropertyInteger(env, key, default_value);
}

jlong SystemPropertiesGetLong(JNIEnv* env, jclass, jstring key,
                              jlong default_value) {
  return ParseSystemPropertyInteger(env, key, default_value);
}

jboolean SystemPropertiesGetBoolean(JNIEnv* env, jclass, jstring key,
                                    jboolean default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
  if (!value.has_value()) {
    return default_value;
  }
  if (*value == "1" || *value == "true" || *value == "on" || *value == "yes") {
    return JNI_TRUE;
  }
  if (*value == "0" || *value == "false" || *value == "off" || *value == "no") {
    return JNI_FALSE;
  }
  return default_value;
}

jlong SystemPropertiesFind(JNIEnv*, jclass, jstring) { return 0; }

jstring SystemPropertiesGetByHandle(JNIEnv* env, jclass, jlong) {
  return env->NewStringUTF("");
}

jint SystemPropertiesGetIntByHandle(jlong, jint default_value) {
  return default_value;
}

jlong SystemPropertiesGetLongByHandle(jlong, jlong default_value) {
  return default_value;
}

jboolean SystemPropertiesGetBooleanByHandle(jlong, jboolean default_value) {
  return default_value;
}

void SystemPropertiesSet(JNIEnv* env, jclass, jstring key, jstring value) {
  const std::optional<std::string> name = JavaString(env, key);
  const std::optional<std::string> text = JavaString(env, value);
  if (!name.has_value() || !text.has_value()) {
    return;
  }
  std::lock_guard lock(g_system_properties_mutex);
  g_system_properties[*name] = *text;
}

void SystemPropertiesNoOp(JNIEnv*, jclass) {}

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

bool RegisterFrameworkNatives(JNIEnv* env) {
  JNINativeMethod message_queue_methods[] = {
      {const_cast<char*>("nativeInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&MessageQueueNativeInit)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&MessageQueueNativeDestroy)},
      {const_cast<char*>("nativePollOnce"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&MessageQueueNativePollOnce)},
      {const_cast<char*>("nativeWake"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&MessageQueueNativeWake)},
      {const_cast<char*>("nativeIsPolling"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&MessageQueueNativeIsPolling)},
      {const_cast<char*>("nativeSetFileDescriptorEvents"),
       const_cast<char*>("(JII)V"),
       reinterpret_cast<void*>(&MessageQueueNativeSetFileDescriptorEvents)},
  };
  if (!Register(env, "android/os/MessageQueue", message_queue_methods,
                static_cast<jint>(std::size(message_queue_methods)))) {
    return false;
  }

  JNINativeMethod log_methods[] = {
      {const_cast<char*>("isLoggable"),
       const_cast<char*>("(Ljava/lang/String;I)Z"),
       reinterpret_cast<void*>(&LogIsLoggable)},
  };
  if (!Register(env, "android/util/Log", log_methods,
                static_cast<jint>(std::size(log_methods)))) {
    return false;
  }

  JNINativeMethod system_clock_methods[] = {
      {const_cast<char*>("currentThreadTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemClockCurrentThreadTimeMillis)},
      {const_cast<char*>("elapsedRealtime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemClockElapsedRealtime)},
      {const_cast<char*>("elapsedRealtimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemClockElapsedRealtimeNanos)},
      {const_cast<char*>("uptimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemClockUptimeMillis)},
      {const_cast<char*>("uptimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemClockUptimeNanos)},
  };
  if (!Register(env, "android/os/SystemClock", system_clock_methods,
                static_cast<jint>(std::size(system_clock_methods)))) {
    return false;
  }

  JNINativeMethod system_properties_methods[] = {
      {const_cast<char*>("native_get"),
       const_cast<char*>(
           "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemPropertiesGet)},
      {const_cast<char*>("native_get_int"),
       const_cast<char*>("(Ljava/lang/String;I)I"),
       reinterpret_cast<void*>(&SystemPropertiesGetInt)},
      {const_cast<char*>("native_get_long"),
       const_cast<char*>("(Ljava/lang/String;J)J"),
       reinterpret_cast<void*>(&SystemPropertiesGetLong)},
      {const_cast<char*>("native_get_boolean"),
       const_cast<char*>("(Ljava/lang/String;Z)Z"),
       reinterpret_cast<void*>(&SystemPropertiesGetBoolean)},
      {const_cast<char*>("native_find"),
       const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&SystemPropertiesFind)},
      {const_cast<char*>("native_get"),
       const_cast<char*>("(J)Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemPropertiesGetByHandle)},
      {const_cast<char*>("native_get_int"), const_cast<char*>("(JI)I"),
       reinterpret_cast<void*>(&SystemPropertiesGetIntByHandle)},
      {const_cast<char*>("native_get_long"), const_cast<char*>("(JJ)J"),
       reinterpret_cast<void*>(&SystemPropertiesGetLongByHandle)},
      {const_cast<char*>("native_get_boolean"), const_cast<char*>("(JZ)Z"),
       reinterpret_cast<void*>(&SystemPropertiesGetBooleanByHandle)},
      {const_cast<char*>("native_set"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&SystemPropertiesSet)},
      {const_cast<char*>("native_add_change_callback"),
       const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SystemPropertiesNoOp)},
      {const_cast<char*>("native_report_sysprop_change"),
       const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SystemPropertiesNoOp)},
  };
  return Register(env, "android/os/SystemProperties", system_properties_methods,
                  static_cast<jint>(std::size(system_properties_methods)));
}

}  // namespace darwin_art
