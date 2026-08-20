#include "darwin_framework_natives.h"

#include <mach/mach_time.h>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(DARWIN_ART_REAL_GRAPHICS)
#include <android/graphics/jni_runtime.h>
#include <androidicuinit/android_icu_init.h>

#include "darwin_art/android_runtime_host.h"
#include "darwin_android_graphics_registration.h"

namespace android {
extern int register_android_util_Log(JNIEnv* env);
extern int register_android_content_AssetManager(JNIEnv* env);
extern int register_android_content_StringBlock(JNIEnv* env);
extern int register_android_content_XmlBlock(JNIEnv* env);
extern int register_android_content_res_ApkAssets(JNIEnv* env);
extern int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);
}  // namespace android
#endif

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

void NativeAllocationRegistryApplyFreeFunction(JNIEnv*, jclass,
                                                jlong free_function,
                                                jlong native_ptr) {
  if (free_function == 0 || native_ptr == 0) return;
  using FreeFunction = void (*)(void*);
  reinterpret_cast<FreeFunction>(static_cast<std::uintptr_t>(free_function))(
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(native_ptr)));
}

void PerfettoNoopFinalizer(void*) {}

jlong PerfettoCategoryNativeDelete() {
  // Perfetto is intentionally disabled on the Darwin host. Return a no-op
  // finalizer address so NativeAllocationRegistry can retain its contract
  // without importing tracing or host-specific allocation state.
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      &PerfettoNoopFinalizer));
}

jlong PerfettoCategoryNativeInit(jstring, jstring, jstring) {
  // Non-zero sentinel: Java keeps the category alive, while all tracing is
  // disabled by native_is_enabled below.
  return 1;
}

void PerfettoCategoryNativeRegister(jlong) {}
void PerfettoCategoryNativeUnregister(jlong) {}
jboolean PerfettoCategoryNativeIsEnabled(jlong) {
  return JNI_FALSE;
}

jlong PerfettoTrackEventExtraNativeDelete() {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      &PerfettoNoopFinalizer));
}

jint EventLogWriteEvent(JNIEnv*, jclass, jint, jobjectArray) {
  // ServiceManager latency diagnostics are optional on the host; preserve
  // the Java call contract without importing Android's kernel event log.
  return 0;
}

std::mutex g_system_properties_mutex;
std::unordered_map<std::string, std::string> g_system_properties{
    {"ro.product.cpu.abilist", "arm64-v8a"},
    {"ro.product.cpu.abilist64", "arm64-v8a"},
    {"ro.product.cpu.abilist32", ""},
    {"ro.build.version.sdk", "36"},
    {"ro.build.version.sdk_full", "36.0"},
    {"ro.build.version.release", "16"},
    {"ro.build.version.release_or_codename", "16"},
    {"ro.build.version.codename", "REL"},
    {"ro.build.version.all_codenames", "REL"},
    {"ro.build.version.known_codenames", "REL"},
};

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

jint LogPrintln(JNIEnv* env, jclass, jint, jint, jstring, jstring message) {
  return message == nullptr ? 0 : env->GetStringLength(message);
}

jboolean TraceIsTagEnabled(JNIEnv*, jclass, jlong) { return JNI_FALSE; }

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

struct DarwinBinderHolder {};

struct DarwinAssetManager {};

struct DarwinTheme {
  explicit DarwinTheme(DarwinAssetManager* owner) : assets(owner) {}

  DarwinAssetManager* assets;
};

#if !defined(DARWIN_ART_REAL_GRAPHICS)
struct DarwinPaint {
  jint flags = 0;
  jint color = 0xff000000;
};

struct DarwinRenderNode {
  explicit DarwinRenderNode(std::string node_name)
      : name(std::move(node_name)) {}

  std::string name;
  jint left = 0;
  jint top = 0;
  jint right = 0;
  jint bottom = 0;
};
#endif

void BinderHolderFinalizer(void* holder) {
  delete static_cast<DarwinBinderHolder*>(holder);
}

void AssetManagerFinalizer(void* assets) {
  delete static_cast<DarwinAssetManager*>(assets);
}

jlong AssetManagerCreate(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(new DarwinAssetManager());
}

void AssetManagerDestroy(JNIEnv*, jclass, jlong handle) {
  AssetManagerFinalizer(reinterpret_cast<void*>(
      static_cast<std::uintptr_t>(handle)));
}

void ThemeFinalizer(void* theme) { delete static_cast<DarwinTheme*>(theme); }

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

jlong AssetManagerGetThemeFreeFunction(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&ThemeFinalizer);
}

jlong AssetManagerThemeCreate(JNIEnv*, jclass, jlong assets_handle) {
  auto* assets = reinterpret_cast<DarwinAssetManager*>(
      static_cast<std::uintptr_t>(assets_handle));
  return reinterpret_cast<std::uintptr_t>(new DarwinTheme(assets));
}

void AssetManagerThemeApplyStyle(JNIEnv*, jclass, jlong, jlong, jint,
                                 jboolean) {}

void AssetManagerThemeCopy(JNIEnv*, jclass, jlong, jlong, jlong, jlong) {}

jint AssetManagerThemeGetAttributeValue(JNIEnv*, jclass, jlong, jlong, jint,
                                        jobject, jboolean) {
  return 0;
}

jint AssetManagerThemeGetChangingConfigurations(JNIEnv*, jclass, jlong) {
  return 0;
}

void AssetManagerThemeRebase(JNIEnv*, jclass, jlong, jlong, jintArray,
                             jbooleanArray, jint) {}

void AssetManagerApplyStyle(JNIEnv*, jclass, jlong, jlong, jint, jint, jlong,
                            jintArray, jlong, jlong) {
  // Java's TypedArray storage is zero-initialized. Leaving it untouched is an
  // empty style result, so every framework lookup observes its documented
  // default while the real framework-res table parser is brought up.
}

void AssetManagerApplyStyleWithArray(JNIEnv*, jclass, jlong, jlong, jint, jint,
                                     jlong, jintArray, jintArray, jintArray) {}

jboolean AssetManagerResolveAttrs(JNIEnv*, jclass, jlong, jlong, jint, jint,
                                  jintArray, jintArray, jintArray,
                                  jintArray) {
  return JNI_TRUE;
}

jboolean AssetManagerRetrieveAttributes(JNIEnv*, jclass, jlong, jlong,
                                         jintArray, jintArray, jintArray) {
  return JNI_TRUE;
}

void AssetManagerSetApkAssets(JNIEnv*, jclass, jlong, jobjectArray, jboolean,
                              jboolean) {}

void AssetManagerSetConfiguration(
    JNIEnv*, jclass, jlong, jint, jint, jstring, jobjectArray, jint, jint,
    jint, jint, jint, jint, jint, jint, jint, jint, jint, jint, jint, jint,
    jint, jint, jboolean) {}

jobjectArray AssetManagerGetLocales(JNIEnv* env, jclass, jlong, jboolean) {
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray result =
      string_class == nullptr ? nullptr : env->NewObjectArray(0, string_class, nullptr);
  env->DeleteLocalRef(string_class);
  return result;
}

jlong BinderGetNativeHolder(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(new DarwinBinderHolder());
}

jlong BinderGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&BinderHolderFinalizer);
}

jint BinderGetCallingUid() {
  // Treat the host bridge as Android's system UID until per-app identities are
  // introduced with the Binder compatibility layer.
  return 1000;
}

jobject CreateDarwinContextBinder(JNIEnv* env) {
  // There is no system_server on the Darwin host.  The fixture installs a
  // process-local IServiceManager/IDisplayManager pair whose only real answer
  // is a 360x640, 60 Hz display; all unrelated services return null/defaults.
  jclass bridge = env->FindClass("dev/darwinart/simple/DarwinServiceBridge");
  if (bridge == nullptr) {
    env->ExceptionClear();
    jclass thread_class = env->FindClass("java/lang/Thread");
    jmethodID current_thread = thread_class == nullptr
                                   ? nullptr
                                   : env->GetStaticMethodID(
                                         thread_class, "currentThread",
                                         "()Ljava/lang/Thread;");
    jobject thread = current_thread == nullptr
                         ? nullptr
                         : env->CallStaticObjectMethod(thread_class,
                                                       current_thread);
    jmethodID get_loader = thread_class == nullptr
                               ? nullptr
                               : env->GetMethodID(
                                     thread_class, "getContextClassLoader",
                                     "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader == nullptr
                         ? nullptr
                         : env->CallObjectMethod(thread, get_loader);
    jclass loader_class = loader == nullptr
                              ? nullptr
                              : env->GetObjectClass(loader);
    jmethodID load_class = loader_class == nullptr
                               ? nullptr
                               : env->GetMethodID(
                                     loader_class, "loadClass",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("dev.darwinart.simple.DarwinServiceBridge");
    jobject loaded = load_class == nullptr
                         ? nullptr
                         : env->CallObjectMethod(loader, load_class, name);
    if (!env->ExceptionCheck()) {
      bridge = static_cast<jclass>(loaded);
    } else {
      env->ExceptionClear();
      env->DeleteLocalRef(loaded);
    }
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(thread);
    env->DeleteLocalRef(thread_class);
  }
  jmethodID create = bridge == nullptr
                         ? nullptr
                         : env->GetStaticMethodID(bridge, "createContextBinder",
                                                  "()Landroid/os/Binder;");
  jobject result = create == nullptr
                       ? nullptr
                       : env->CallStaticObjectMethod(bridge, create);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(bridge);
  return result;
}

jobject BinderInternalGetContextObject(JNIEnv* env, jclass) {
  return CreateDarwinContextBinder(env);
}

jobject ServiceManagerProxyGetNativeServiceManager(JNIEnv* env, jobject) {
  return CreateDarwinContextBinder(env);
}

#if !defined(DARWIN_ART_REAL_GRAPHICS)
void RenderNodeFinalizer(void* render_node) {
  delete static_cast<DarwinRenderNode*>(render_node);
}

jlong RenderNodeCreate(JNIEnv* env, jclass, jstring name) {
  std::optional<std::string> node_name = JavaString(env, name);
  return reinterpret_cast<std::uintptr_t>(
      new DarwinRenderNode(node_name.value_or("")));
}

jlong RenderNodeGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&RenderNodeFinalizer);
}

jboolean RenderNodeSetLeftTopRightBottom(jlong handle, jint left, jint top,
                                         jint right, jint bottom) {
  auto* node = reinterpret_cast<DarwinRenderNode*>(
      static_cast<std::uintptr_t>(handle));
  if (node == nullptr) {
    return JNI_FALSE;
  }
  const bool changed = node->left != left || node->top != top ||
                       node->right != right || node->bottom != bottom;
  node->left = left;
  node->top = top;
  node->right = right;
  node->bottom = bottom;
  return changed ? JNI_TRUE : JNI_FALSE;
}

jboolean RenderNodeHasIdentityMatrix(jlong) {
  // The software bridge has no transform mutators yet, so every node remains
  // in its default identity state.
  return JNI_TRUE;
}
#endif

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

#if defined(DARWIN_ART_REAL_GRAPHICS)
bool SetJavaSystemProperty(JNIEnv* env, const char* key, const char* value) {
  jclass system = env->FindClass("java/lang/System");
  jmethodID set_property =
      system == nullptr
          ? nullptr
          : env->GetStaticMethodID(
                system, "setProperty",
                "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  jstring java_key = set_property == nullptr ? nullptr : env->NewStringUTF(key);
  jstring java_value =
      java_key == nullptr ? nullptr : env->NewStringUTF(value);
  jobject previous =
      java_value == nullptr || env->ExceptionCheck()
          ? nullptr
          : env->CallStaticObjectMethod(system, set_property, java_key,
                                        java_value);
  const bool success = !env->ExceptionCheck();
  env->DeleteLocalRef(previous);
  env->DeleteLocalRef(java_value);
  env->DeleteLocalRef(java_key);
  env->DeleteLocalRef(system);
  return success;
}

bool ConfigureLayoutlibGraphicsRegistrar(JNIEnv* env) {
  static_assert(darwin_art::android_graphics::kNativeClassCount == 51);
  return SetJavaSystemProperty(env, "method_binding_format", "") &&
         SetJavaSystemProperty(
             env, darwin_art::android_graphics::kNativeClassesPropertyName,
             darwin_art::android_graphics::kNativeClassesCsv);
}
#endif

}  // namespace

namespace darwin_art {

FrameworkGraphicsBackend GetFrameworkGraphicsBackend() {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  return FrameworkGraphicsBackend::kAndroidGraphics;
#else
  return FrameworkGraphicsBackend::kProbeCanvas;
#endif
}

bool InitializeFrameworkGraphicsRuntime() {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  // This must run before ART initializes java.lang.System. Its static
  // initializer publishes ICU/Unicode/CLDR versions and rejects a null CLDR
  // value, while AOSP ICU on Darwin requires explicit external-data setup.
  android_icu_init();
#endif
  return true;
}

void ShutdownFrameworkGraphicsRuntime() {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  // The ICU JNI unload hook runs first and calls u_cleanup(); release the
  // external Android ICU data mapping only after no ICU consumer remains.
  android_icu_cleanup();
#endif
}

bool InstallFrameworkResourceRuntime(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  return darwin_art_android_runtime_install(env) ==
         DARWIN_ART_ANDROID_RUNTIME_OK;
#else
  (void)env;
  return true;
#endif
}

bool ShutdownFrameworkResourceRuntime(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  return darwin_art_android_runtime_uninstall(env) ==
         DARWIN_ART_ANDROID_RUNTIME_OK;
#else
  (void)env;
  return true;
#endif
}

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

  // Choreographer and RippleDrawable use this registration to turn host
  // monotonic ticks into the same dispatchVsync callback Android receives
  // from SurfaceFlinger.  Keeping it next to MessageQueue preserves the
  // framework startup dependency order.
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
  if (!Register(env, "android/os/PerfettoTrackEventExtra",
                perfetto_extra_methods,
                static_cast<jint>(std::size(perfetto_extra_methods)))) {
    return false;
  }

  JNINativeMethod event_log_methods[] = {
      {const_cast<char*>("writeEvent"),
       const_cast<char*>("(I[Ljava/lang/Object;)I"),
       reinterpret_cast<void*>(&EventLogWriteEvent)},
  };
  if (!Register(env, "android/util/EventLog", event_log_methods,
                static_cast<jint>(std::size(event_log_methods)))) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod log_methods[] = {
      {const_cast<char*>("isLoggable"),
       const_cast<char*>("(Ljava/lang/String;I)Z"),
       reinterpret_cast<void*>(&LogIsLoggable)},
      {const_cast<char*>("println_native"),
       const_cast<char*>(
           "(IILjava/lang/String;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&LogPrintln)},
  };
  if (!Register(env, "android/util/Log", log_methods,
                static_cast<jint>(std::size(log_methods)))) {
    return false;
  }
#endif

  JNINativeMethod trace_methods[] = {
      {const_cast<char*>("nativeIsTagEnabled"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&TraceIsTagEnabled)},
  };
  if (!Register(env, "android/os/Trace", trace_methods,
                static_cast<jint>(std::size(trace_methods)))) {
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

  JNINativeMethod binder_methods[] = {
      {const_cast<char*>("getNativeBBinderHolder"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&BinderGetNativeHolder)},
      {const_cast<char*>("getNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&BinderGetNativeFinalizer)},
      {const_cast<char*>("getCallingUid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&BinderGetCallingUid)},
  };
  if (!Register(env, "android/os/Binder", binder_methods,
                static_cast<jint>(std::size(binder_methods)))) {
    return false;
  }

  JNINativeMethod binder_internal_methods[] = {
      {const_cast<char*>("getContextObject"), const_cast<char*>("()Landroid/os/IBinder;"),
       reinterpret_cast<void*>(&BinderInternalGetContextObject)},
  };
  if (!Register(env, "com/android/internal/os/BinderInternal",
                binder_internal_methods,
                static_cast<jint>(std::size(binder_internal_methods)))) {
    return false;
  }

  JNINativeMethod service_manager_proxy_methods[] = {
      {const_cast<char*>("getNativeServiceManager"),
       const_cast<char*>("()Landroid/os/IBinder;"),
       reinterpret_cast<void*>(&ServiceManagerProxyGetNativeServiceManager)},
  };
  if (!Register(env, "android/os/ServiceManagerProxy",
                service_manager_proxy_methods,
                static_cast<jint>(std::size(service_manager_proxy_methods)))) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod render_node_methods[] = {
      {const_cast<char*>("nCreate"),
       const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&RenderNodeCreate)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&RenderNodeGetNativeFinalizer)},
      {const_cast<char*>("nSetLeftTopRightBottom"),
       const_cast<char*>("(JIIII)Z"),
       reinterpret_cast<void*>(&RenderNodeSetLeftTopRightBottom)},
      {const_cast<char*>("nHasIdentityMatrix"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&RenderNodeHasIdentityMatrix)},
  };
  if (!Register(env, "android/graphics/RenderNode", render_node_methods,
                static_cast<jint>(std::size(render_node_methods)))) {
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
  JNINativeMethod asset_manager_methods[] = {
      {const_cast<char*>("nativeCreate"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&AssetManagerCreate)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&AssetManagerDestroy)},
      {const_cast<char*>("nativeGetThemeFreeFunction"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&AssetManagerGetThemeFreeFunction)},
      {const_cast<char*>("nativeThemeCreate"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&AssetManagerThemeCreate)},
      {const_cast<char*>("nativeThemeApplyStyle"),
       const_cast<char*>("(JJIZ)V"),
       reinterpret_cast<void*>(&AssetManagerThemeApplyStyle)},
      {const_cast<char*>("nativeThemeCopy"), const_cast<char*>("(JJJJ)V"),
       reinterpret_cast<void*>(&AssetManagerThemeCopy)},
      {const_cast<char*>("nativeThemeGetAttributeValue"),
       const_cast<char*>("(JJILandroid/util/TypedValue;Z)I"),
       reinterpret_cast<void*>(&AssetManagerThemeGetAttributeValue)},
      {const_cast<char*>("nativeThemeGetChangingConfigurations"),
       const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&AssetManagerThemeGetChangingConfigurations)},
      {const_cast<char*>("nativeThemeRebase"),
       const_cast<char*>("(JJ[I[ZI)V"),
       reinterpret_cast<void*>(&AssetManagerThemeRebase)},
      {const_cast<char*>("nativeApplyStyle"),
       const_cast<char*>("(JJIIJ[IJJ)V"),
       reinterpret_cast<void*>(&AssetManagerApplyStyle)},
      {const_cast<char*>("nativeApplyStyleWithArray"),
       const_cast<char*>("(JJIIJ[I[I[I)V"),
       reinterpret_cast<void*>(&AssetManagerApplyStyleWithArray)},
      {const_cast<char*>("nativeResolveAttrs"),
       const_cast<char*>("(JJII[I[I[I[I)Z"),
       reinterpret_cast<void*>(&AssetManagerResolveAttrs)},
      {const_cast<char*>("nativeRetrieveAttributes"),
       const_cast<char*>("(JJ[I[I[I)Z"),
       reinterpret_cast<void*>(&AssetManagerRetrieveAttributes)},
      {const_cast<char*>("nativeSetApkAssets"),
       const_cast<char*>("(J[Landroid/content/res/ApkAssets;ZZ)V"),
       reinterpret_cast<void*>(&AssetManagerSetApkAssets)},
      {const_cast<char*>("nativeSetConfiguration"),
       const_cast<char*>(
           "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIZ)V"),
       reinterpret_cast<void*>(&AssetManagerSetConfiguration)},
      {const_cast<char*>("nativeGetLocales"),
       const_cast<char*>("(JZ)[Ljava/lang/String;"),
       reinterpret_cast<void*>(&AssetManagerGetLocales)},
  };
  if (!Register(env, "android/content/res/AssetManager",
                asset_manager_methods,
                static_cast<jint>(std::size(asset_manager_methods)))) {
    return false;
  }
#endif

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
  if (!Register(env, "android/os/SystemProperties", system_properties_methods,
                static_cast<jint>(std::size(system_properties_methods)))) {
    return false;
  }

  return true;
}

bool RegisterFrameworkResourceNatives(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  // Preserve AndroidRuntime.cpp's ownership and registration order. These four
  // tables replace the temporary AssetManager table as one atomic resource
  // subsystem; mixing either AssetManager native-handle representation would
  // make Theme/ApkAssets jlong values type-unsafe.
  return android::register_android_util_Log(env) >= 0 &&
         android::register_android_content_AssetManager(env) >= 0 &&
         android::register_android_content_StringBlock(env) >= 0 &&
         android::register_android_content_XmlBlock(env) >= 0 &&
         android::register_android_content_res_ApkAssets(env) >= 0 &&
         android::register_com_android_internal_util_VirtualRefBasePtr(env) >= 0;
#else
  // The baseline probe registered its deliberately small AssetManager table in
  // RegisterFrameworkNatives().
  (void)env;
  return true;
#endif
}

bool RegisterFrameworkGraphicsNatives(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  // Registration is atomic at the upstream libandroid_runtime boundary. In
  // particular, never register DarwinPaint/DarwinRenderNode alongside native
  // Canvas or Bitmap: their jlong values have unrelated C++ object layouts and
  // eventually cross through Canvas/RenderNode drawing APIs. The caller must
  // invoke this after Runtime::FinishMinimalForDarwinProbe(): the Darwin host
  // uses LayoutlibLoader, whose registrar calls managed System.getProperty().
  // The generated header is derived from LayoutlibLoader's source map; a real
  // build deliberately cannot compile without that verified 51-class input.
  if (!ConfigureLayoutlibGraphicsRegistrar(env)) {
    return false;
  }
  init_android_graphics();
  return register_android_graphics_classes(env) >= 0;
#else
  // ProbeCanvas, DarwinPaint, and DarwinRenderNode were registered atomically
  // by RegisterFrameworkNatives().
  (void)env;
  return true;
#endif
}

}  // namespace darwin_art
