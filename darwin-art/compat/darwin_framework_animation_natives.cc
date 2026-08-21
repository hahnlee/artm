#include "darwin_framework_natives.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {


// DisplayEventReceiver is the clock edge behind Choreographer, ValueAnimator,
// and framework RippleDrawable.  Android normally gets this edge from
// SurfaceFlinger.  The Darwin host has no SurfaceFlinger, so provide the same
// Java callback contract from a 60 Hz monotonic scheduler.  Keeping the
// receiver in a lease map is important: NativeAllocationRegistry may finalize
// it while a scheduled callback is already waiting on the host thread.
class DarwinDisplayEventReceiver {
 public:
  DarwinDisplayEventReceiver(JavaVM* vm, jobject receiver_weak,
                             jclass receiver_class, jmethodID reference_get,
                             jmethodID dispatch_vsync)
      : vm_(vm),
        receiver_weak_(receiver_weak),
        receiver_class_(receiver_class),
        reference_get_(reference_get),
        dispatch_vsync_(dispatch_vsync) {}

  ~DarwinDisplayEventReceiver() = default;

  void Dispose(JNIEnv* env) {
    std::unique_lock lock(mutex_);
    disposed_ = true;
    condition_.wait(lock, [this] { return callbacks_ == 0; });
    if (receiver_weak_ != nullptr) {
      env->DeleteGlobalRef(receiver_weak_);
      receiver_weak_ = nullptr;
    }
    if (receiver_class_ != nullptr) {
      env->DeleteGlobalRef(receiver_class_);
      receiver_class_ = nullptr;
    }
  }

  void FinalizeFromAnyThread() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
      if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return;
      }
      attached = true;
    }
    Dispose(env);
    if (attached) {
      vm_->DetachCurrentThread();
    }
  }

  void Schedule() {
    std::shared_ptr<DarwinDisplayEventReceiver> keep_alive = SharedFromThis();
    if (keep_alive == nullptr) {
      return;
    }
    std::thread([keep_alive = std::move(keep_alive)] {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      keep_alive->Dispatch();
    }).detach();
  }

  void Dispatch() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
      if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return;
      }
      attached = true;
    }

    jobject weak = nullptr;
    {
      std::lock_guard lock(mutex_);
      if (disposed_ || receiver_weak_ == nullptr) {
        if (attached) {
          vm_->DetachCurrentThread();
        }
        return;
      }
      ++callbacks_;
      weak = receiver_weak_;
    }

    jobject receiver = env->CallObjectMethod(weak, reference_get_);
    if (receiver != nullptr && !env->ExceptionCheck()) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const jlong timestamp = static_cast<jlong>(now.tv_sec) * 1'000'000'000LL +
                             static_cast<jlong>(now.tv_nsec);
      env->CallVoidMethod(receiver, dispatch_vsync_, timestamp, 0, 0);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(receiver);
    } else if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }

    {
      std::lock_guard lock(mutex_);
      if (--callbacks_ == 0) {
        condition_.notify_all();
      }
    }
    if (attached) {
      vm_->DetachCurrentThread();
    }
  }

  void SetSelf(const std::shared_ptr<DarwinDisplayEventReceiver>& self) {
    std::lock_guard lock(mutex_);
    self_ = self;
  }

 private:
  std::shared_ptr<DarwinDisplayEventReceiver> SharedFromThis() {
    std::lock_guard lock(mutex_);
    if (disposed_) {
      return nullptr;
    }
    return self_.lock();
  }

  JavaVM* vm_;
  jobject receiver_weak_;
  jclass receiver_class_;
  jmethodID reference_get_;
  jmethodID dispatch_vsync_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::weak_ptr<DarwinDisplayEventReceiver> self_;
  size_t callbacks_ = 0;
  bool disposed_ = false;
};

std::mutex g_display_receiver_mutex;
std::unordered_map<jlong, std::shared_ptr<DarwinDisplayEventReceiver>>
    g_display_receivers;

std::shared_ptr<DarwinDisplayEventReceiver> FindDisplayReceiver(jlong handle) {
  std::lock_guard lock(g_display_receiver_mutex);
  const auto found = g_display_receivers.find(handle);
  return found == g_display_receivers.end() ? nullptr : found->second;
}

void DisplayEventReceiverRelease(void* raw) {
  const jlong handle = static_cast<jlong>(reinterpret_cast<std::uintptr_t>(raw));
  std::shared_ptr<DarwinDisplayEventReceiver> receiver;
  {
    std::lock_guard lock(g_display_receiver_mutex);
    const auto found = g_display_receivers.find(handle);
    if (found == g_display_receivers.end()) {
      return;
    }
    receiver = std::move(found->second);
    g_display_receivers.erase(found);
  }
  // The finalizer normally runs on a Java thread, but this also handles an
  // ART shutdown callback from a detached host thread.
  if (receiver != nullptr) {
    receiver->FinalizeFromAnyThread();
  }
}

jlong DisplayEventReceiverNativeInit(JNIEnv* env, jclass, jobject receiver_weak,
                                     jobject, jobject, jint, jint, jlong) {
  if (receiver_weak == nullptr) {
    return 0;
  }
  JavaVM* vm = nullptr;
  if (env->GetJavaVM(&vm) != JNI_OK) {
    return 0;
  }
  jclass reference_class = env->FindClass("java/lang/ref/Reference");
  jmethodID reference_get = reference_class == nullptr
                                ? nullptr
                                : env->GetMethodID(
                                      reference_class, "get",
                                      "()Ljava/lang/Object;");
  jobject receiver = reference_get == nullptr
                         ? nullptr
                         : env->CallObjectMethod(receiver_weak, reference_get);
  if (receiver == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(reference_class);
    env->DeleteLocalRef(receiver);
    return 0;
  }
  jclass receiver_class_local = env->GetObjectClass(receiver);
  jmethodID dispatch_vsync =
      receiver_class_local == nullptr
          ? nullptr
          : env->GetMethodID(receiver_class_local, "dispatchVsync", "(JJI)V");
  if (dispatch_vsync == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(receiver_class_local);
    env->DeleteLocalRef(receiver);
    env->DeleteLocalRef(reference_class);
    return 0;
  }
  jobject weak_global = env->NewGlobalRef(receiver_weak);
  jclass receiver_class_global =
      static_cast<jclass>(env->NewGlobalRef(receiver_class_local));
  env->DeleteLocalRef(receiver_class_local);
  env->DeleteLocalRef(receiver);
  env->DeleteLocalRef(reference_class);
  if (weak_global == nullptr || receiver_class_global == nullptr) {
    env->DeleteGlobalRef(weak_global);
    env->DeleteGlobalRef(receiver_class_global);
    return 0;
  }
  auto receiver_state = std::make_shared<DarwinDisplayEventReceiver>(
      vm, weak_global, receiver_class_global, reference_get, dispatch_vsync);
  const jlong handle = static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      receiver_state.get()));
  receiver_state->SetSelf(receiver_state);
  {
    std::lock_guard lock(g_display_receiver_mutex);
    g_display_receivers.emplace(handle, std::move(receiver_state));
  }
  return handle;
}

void DisplayEventReceiverNativeScheduleVsync(JNIEnv*, jclass, jlong handle) {
  if (auto receiver = FindDisplayReceiver(handle); receiver != nullptr) {
    receiver->Schedule();
  }
}

jobject DisplayEventReceiverNativeGetLatestVsyncEventData(JNIEnv*, jclass,
                                                            jlong) {
  // Choreographer accepts a null timeline and uses the timestamp/count path.
  // A host display has no SurfaceFlinger frame-timeline prediction to expose.
  return nullptr;
}

jlong PerfettoCategoryNativeGetExtraPtr(jlong) { return 0; }

jlong DisplayEventReceiverNativeFinalizer(JNIEnv*, jclass) {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      &DisplayEventReceiverRelease));
}

// PropertyValuesHolder's JNI helpers are used by ValueAnimator and
// RippleDrawable. A jmethodID is already an opaque stable JNI handle, so the
// host can preserve Android's lookup/call ABI without exposing ART internals.
jlong PropertyGetMethod(JNIEnv* env, jclass, jclass target, jstring name,
                        const char* signature) {
  if (target == nullptr || name == nullptr) return 0;
  const char* utf_name = env->GetStringUTFChars(name, nullptr);
  if (utf_name == nullptr) return 0;
  jmethodID method = env->GetMethodID(target, utf_name, signature);
  env->ReleaseStringUTFChars(name, utf_name);
  return reinterpret_cast<jlong>(method);
}

jlong PropertyGetFloatMethod(JNIEnv* env, jclass declaring, jclass target,
                             jstring name) {
  return PropertyGetMethod(env, declaring, target, name, "(F)V");
}
jlong PropertyGetIntMethod(JNIEnv* env, jclass declaring, jclass target,
                           jstring name) {
  return PropertyGetMethod(env, declaring, target, name, "(I)V");
}
jlong PropertyGetMultipleFloatMethod(JNIEnv* env, jclass declaring,
                                     jclass target, jstring name, jint count) {
  std::string signature("(");
  signature.append(static_cast<std::size_t>(count > 0 ? count : 0), 'F');
  signature.append(")V");
  return PropertyGetMethod(env, declaring, target, name, signature.c_str());
}
jlong PropertyGetMultipleIntMethod(JNIEnv* env, jclass declaring, jclass target,
                                   jstring name, jint count) {
  std::string signature("(");
  signature.append(static_cast<std::size_t>(count > 0 ? count : 0), 'I');
  signature.append(")V");
  return PropertyGetMethod(env, declaring, target, name, signature.c_str());
}

void PropertyCallFloatMethod(JNIEnv* env, jclass, jobject object, jlong method,
                             jfloat value) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), value);
}
void PropertyCallIntMethod(JNIEnv* env, jclass, jobject object, jlong method,
                           jint value) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), value);
}
void PropertyCallTwoFloatMethod(JNIEnv* env, jclass, jobject object, jlong method,
                                jfloat a, jfloat b) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), a, b);
}
void PropertyCallFourFloatMethod(JNIEnv* env, jclass, jobject object, jlong method,
                                 jfloat a, jfloat b, jfloat c, jfloat d) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), a, b, c,
                        d);
}
void PropertyCallMultipleFloatMethod(JNIEnv* env, jclass, jobject object,
                                     jlong method,
                                     jfloatArray values) {
  if (object == nullptr || method == 0 || values == nullptr) return;
  const jsize count = env->GetArrayLength(values);
  std::vector<jfloat> source(static_cast<std::size_t>(count));
  env->GetFloatArrayRegion(values, 0, count, source.data());
  std::vector<jvalue> arguments(static_cast<std::size_t>(count));
  for (jsize index = 0; index < count; ++index) {
    arguments[static_cast<std::size_t>(index)].f =
        source[static_cast<std::size_t>(index)];
  }
  env->CallVoidMethodA(object, reinterpret_cast<jmethodID>(method),
                       arguments.data());
}
void PropertyCallTwoIntMethod(JNIEnv* env, jclass, jobject object, jlong method,
                              jint a,
                              jint b) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), a, b);
}
void PropertyCallFourIntMethod(JNIEnv* env, jclass, jobject object, jlong method,
                               jint a,
                               jint b, jint c, jint d) {
  if (object != nullptr && method != 0)
    env->CallVoidMethod(object, reinterpret_cast<jmethodID>(method), a, b, c,
                        d);
}
void PropertyCallMultipleIntMethod(JNIEnv* env, jclass, jobject object,
                                   jlong method,
                                   jintArray values) {
  if (object == nullptr || method == 0 || values == nullptr) return;
  const jsize count = env->GetArrayLength(values);
  std::vector<jint> source(static_cast<std::size_t>(count));
  env->GetIntArrayRegion(values, 0, count, source.data());
  std::vector<jvalue> arguments(static_cast<std::size_t>(count));
  for (jsize index = 0; index < count; ++index) {
    arguments[static_cast<std::size_t>(index)].i =
        source[static_cast<std::size_t>(index)];
  }
  env->CallVoidMethodA(object, reinterpret_cast<jmethodID>(method),
                       arguments.data());
}

void PerfettoNoopFinalizer(void*) {}

jlong PerfettoCategoryNativeDelete() {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      &PerfettoNoopFinalizer));
}

jlong PerfettoCategoryNativeInit(jstring, jstring, jstring) { return 1; }
void PerfettoCategoryNativeRegister(jlong) {}
void PerfettoCategoryNativeUnregister(jlong) {}
jboolean PerfettoCategoryNativeIsEnabled(jlong) { return JNI_FALSE; }

jlong PerfettoTrackEventExtraNativeDelete() {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      &PerfettoNoopFinalizer));
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

bool RegisterFrameworkAnimationNatives(JNIEnv* env) {
  JNINativeMethod display_event_receiver_methods[] = {
      {const_cast<char*>("nativeInit"),
       const_cast<char*>(
           "(Ljava/lang/ref/WeakReference;Ljava/lang/ref/WeakReference;"
           "Landroid/os/MessageQueue;IIJ)J"),
       reinterpret_cast<void*>(&DisplayEventReceiverNativeInit)},
      {const_cast<char*>("nativeGetDisplayEventReceiverFinalizer"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&DisplayEventReceiverNativeFinalizer)},
      {const_cast<char*>("nativeScheduleVsync"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&DisplayEventReceiverNativeScheduleVsync)},
      {const_cast<char*>("nativeGetLatestVsyncEventData"),
       const_cast<char*>(
           "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;"),
       reinterpret_cast<void*>(&DisplayEventReceiverNativeGetLatestVsyncEventData)},
  };
  if (!Register(env, "android/view/DisplayEventReceiver",
                display_event_receiver_methods,
                static_cast<jint>(std::size(display_event_receiver_methods)))) {
    return false;
  }

  JNINativeMethod property_values_holder_methods[] = {
      {const_cast<char*>("nCallFloatMethod"),
       const_cast<char*>("(Ljava/lang/Object;JF)V"),
       reinterpret_cast<void*>(&PropertyCallFloatMethod)},
      {const_cast<char*>("nCallFourFloatMethod"),
       const_cast<char*>("(Ljava/lang/Object;JFFFF)V"),
       reinterpret_cast<void*>(&PropertyCallFourFloatMethod)},
      {const_cast<char*>("nCallFourIntMethod"),
       const_cast<char*>("(Ljava/lang/Object;JIIII)V"),
       reinterpret_cast<void*>(&PropertyCallFourIntMethod)},
      {const_cast<char*>("nCallIntMethod"),
       const_cast<char*>("(Ljava/lang/Object;JI)V"),
       reinterpret_cast<void*>(&PropertyCallIntMethod)},
      {const_cast<char*>("nCallMultipleFloatMethod"),
       const_cast<char*>("(Ljava/lang/Object;J[F)V"),
       reinterpret_cast<void*>(&PropertyCallMultipleFloatMethod)},
      {const_cast<char*>("nCallMultipleIntMethod"),
       const_cast<char*>("(Ljava/lang/Object;J[I)V"),
       reinterpret_cast<void*>(&PropertyCallMultipleIntMethod)},
      {const_cast<char*>("nCallTwoFloatMethod"),
       const_cast<char*>("(Ljava/lang/Object;JFF)V"),
       reinterpret_cast<void*>(&PropertyCallTwoFloatMethod)},
      {const_cast<char*>("nCallTwoIntMethod"),
       const_cast<char*>("(Ljava/lang/Object;JII)V"),
       reinterpret_cast<void*>(&PropertyCallTwoIntMethod)},
      {const_cast<char*>("nGetFloatMethod"),
       const_cast<char*>("(Ljava/lang/Class;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&PropertyGetFloatMethod)},
      {const_cast<char*>("nGetIntMethod"),
       const_cast<char*>("(Ljava/lang/Class;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&PropertyGetIntMethod)},
      {const_cast<char*>("nGetMultipleFloatMethod"),
       const_cast<char*>("(Ljava/lang/Class;Ljava/lang/String;I)J"),
       reinterpret_cast<void*>(&PropertyGetMultipleFloatMethod)},
      {const_cast<char*>("nGetMultipleIntMethod"),
       const_cast<char*>("(Ljava/lang/Class;Ljava/lang/String;I)J"),
       reinterpret_cast<void*>(&PropertyGetMultipleIntMethod)},
  };
  if (!Register(env, "android/animation/PropertyValuesHolder",
                property_values_holder_methods,
                static_cast<jint>(std::size(property_values_holder_methods)))) {
    return false;
  }

  JNINativeMethod perfetto_category_methods[] = {
      {const_cast<char*>("native_init"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeInit)},
      {const_cast<char*>("native_delete"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeDelete)},
      {const_cast<char*>("native_get_extra_ptr"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeGetExtraPtr)},
      {const_cast<char*>("native_register"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeRegister)},
      {const_cast<char*>("native_unregister"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeUnregister)},
      {const_cast<char*>("native_is_enabled"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&PerfettoCategoryNativeIsEnabled)},
  };
  if (!Register(env, "android/os/PerfettoTrace$Category",
                perfetto_category_methods,
                static_cast<jint>(std::size(perfetto_category_methods)))) {
    return false;
  }
  JNINativeMethod perfetto_extra_methods[] = {
      {const_cast<char*>("native_delete"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PerfettoTrackEventExtraNativeDelete)},
  };
  return Register(env, "android/os/PerfettoTrackEventExtra", perfetto_extra_methods,
                  static_cast<jint>(std::size(perfetto_extra_methods)));
}

}  // namespace darwin_art
