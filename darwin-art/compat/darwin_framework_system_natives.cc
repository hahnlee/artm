#include "darwin_framework_system_natives.h"

#include "darwin_android_platform.h"
#include "darwin_android_time.h"

#include <sys/resource.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <time.h>

namespace darwin_art::framework_system {
namespace {

class DarwinMessageQueue {
 public:
  DarwinMessageQueue()
      : looper_(darwin_art_android_platform_prepare_current_looper()) {}

  void Poll(jint timeout_millis) {
    polling_.store(true, std::memory_order_release);
    (void)darwin_art_android_platform_poll_current_looper_timeout(
        timeout_millis);
    polling_.store(false, std::memory_order_release);
  }

  void Wake() {
    darwin_art_android_platform_wake_looper(looper_);
  }

  bool IsPolling() {
    return polling_.load(std::memory_order_acquire);
  }

 private:
  void* looper_ = nullptr;
  std::atomic<bool> polling_{false};
};

DarwinMessageQueue* ToMessageQueue(jlong handle) {
  return reinterpret_cast<DarwinMessageQueue*>(
      static_cast<std::uintptr_t>(handle));
}

jlong TimevalToMillis(const timeval& value) {
  return static_cast<jlong>(value.tv_sec) * 1000 + value.tv_usec / 1000;
}

}  // namespace

jlong process_get_elapsed_cpu_time(JNIEnv*, jclass) {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return TimevalToMillis(usage.ru_utime) + TimevalToMillis(usage.ru_stime);
}

jint event_log_write_event(JNIEnv*, jclass, jint, jobjectArray) {
  // ServiceManager latency diagnostics are optional on the host; preserve
  // the Java call contract without importing Android's kernel event log.
  return 0;
}

jlong message_queue_native_init(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(new DarwinMessageQueue());
}

void message_queue_native_destroy(JNIEnv*, jclass, jlong handle) {
  delete ToMessageQueue(handle);
}

void message_queue_native_poll_once(JNIEnv*, jobject, jlong handle,
                                    jint timeout_millis) {
  if (DarwinMessageQueue* queue = ToMessageQueue(handle); queue != nullptr) {
    queue->Poll(timeout_millis);
  }
}

void message_queue_native_wake(JNIEnv*, jclass, jlong handle) {
  if (DarwinMessageQueue* queue = ToMessageQueue(handle); queue != nullptr) {
    queue->Wake();
  }
}

jboolean message_queue_native_is_polling(JNIEnv*, jclass, jlong handle) {
  DarwinMessageQueue* queue = ToMessageQueue(handle);
  return queue != nullptr && queue->IsPolling() ? JNI_TRUE : JNI_FALSE;
}

void message_queue_native_set_file_descriptor_events(JNIEnv*, jclass, jlong,
                                                     jint, jint) {
  // File-descriptor polling will use kqueue. Activity/Handler construction does
  // not register descriptors, so the first framework gate keeps this explicit.
}

jboolean log_is_loggable(JNIEnv*, jclass, jstring, jint priority) {
  constexpr jint kInfoPriority = 4;
  return priority >= kInfoPriority ? JNI_TRUE : JNI_FALSE;
}

jint log_println(JNIEnv* env, jclass, jint, jint, jstring, jstring message) {
  return message == nullptr ? 0 : env->GetStringLength(message);
}

jboolean trace_is_tag_enabled(JNIEnv*, jclass, jlong) { return JNI_FALSE; }

jlong system_clock_uptime_nanos(JNIEnv*, jclass) {
  return darwin_art::AndroidUptimeNanos();
}

jlong system_clock_uptime_millis(JNIEnv* env, jclass klass) {
  return system_clock_uptime_nanos(env, klass) / 1'000'000;
}

jlong system_clock_elapsed_realtime_nanos(JNIEnv*, jclass) {
  return darwin_art::AndroidElapsedRealtimeNanos();
}

jlong system_clock_elapsed_realtime(JNIEnv* env, jclass klass) {
  return system_clock_elapsed_realtime_nanos(env, klass) / 1'000'000;
}

jlong system_clock_current_thread_time_millis(JNIEnv*, jclass) {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
    return 0;
  }
  return static_cast<jlong>(value.tv_sec) * 1'000 + value.tv_nsec / 1'000'000;
}

}  // namespace darwin_art::framework_system
