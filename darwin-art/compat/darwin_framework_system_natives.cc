#include "darwin_framework_system_natives.h"

#include <mach/mach_time.h>
#include <sys/resource.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <time.h>

namespace darwin_art::framework_system {
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

DarwinMessageQueue* ToMessageQueue(jlong handle) {
  return reinterpret_cast<DarwinMessageQueue*>(
      static_cast<std::uintptr_t>(handle));
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
  return MachTicksToNanos(mach_absolute_time());
}

jlong system_clock_uptime_millis(JNIEnv* env, jclass klass) {
  return system_clock_uptime_nanos(env, klass) / 1'000'000;
}

jlong system_clock_elapsed_realtime_nanos(JNIEnv*, jclass) {
  return MachTicksToNanos(mach_continuous_time());
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
