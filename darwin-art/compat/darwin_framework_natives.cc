#include "darwin_framework_natives.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>

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
  return Register(env, "android/util/Log", log_methods,
                  static_cast<jint>(std::size(log_methods)));
}

}  // namespace darwin_art
