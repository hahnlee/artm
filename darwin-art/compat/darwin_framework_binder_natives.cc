#include "darwin_framework_natives.h"

#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <unistd.h>

namespace {

struct DarwinBinderHolder {};

void BinderHolderFinalizer(void* holder) {
  delete static_cast<DarwinBinderHolder*>(holder);
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

jint BinderGetCallingPid() { return static_cast<jint>(getpid()); }

// These identity operations are @CriticalNative in Android 16, so their
// callback ABI intentionally has no JNIEnv/jclass pair.
jlong BinderClearCallingIdentity() { return 0; }
void BinderRestoreCallingIdentity(jlong) {}
void BinderFlushPendingCommands() {}

thread_local jint g_binder_thread_strict_mode_policy = 0;

jint BinderGetThreadStrictModePolicy() {
  return g_binder_thread_strict_mode_policy;
}

void BinderSetThreadStrictModePolicy(jint policy) {
  g_binder_thread_strict_mode_policy = policy;
}

struct DarwinInputChannelState {
  explicit DarwinInputChannelState(std::string channel_name)
      : name(std::move(channel_name)) {}

  std::string name;
};

struct DarwinInputChannel {
  std::shared_ptr<DarwinInputChannelState> state;
  bool server = false;
  bool disposed = false;
};

void InputChannelFinalizer(void* pointer) {
  delete static_cast<DarwinInputChannel*>(pointer);
}

jlong InputChannelGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&InputChannelFinalizer);
}

DarwinInputChannel* InputChannel(jlong pointer) {
  return reinterpret_cast<DarwinInputChannel*>(
      static_cast<std::uintptr_t>(pointer));
}

void InputChannelDispose(JNIEnv*, jclass, jlong pointer) {
  if (auto* channel = InputChannel(pointer); channel != nullptr) {
    channel->disposed = true;
  }
}

jlong InputChannelDup(JNIEnv*, jobject, jlong pointer) {
  const auto* channel = InputChannel(pointer);
  if (channel == nullptr || channel->disposed || channel->state == nullptr) {
    return 0;
  }
  auto* duplicate = new (std::nothrow)
      DarwinInputChannel{channel->state, channel->server, false};
  return static_cast<jlong>(
      reinterpret_cast<std::uintptr_t>(duplicate));
}

jstring InputChannelGetName(JNIEnv* env, jobject, jlong pointer) {
  const auto* channel = InputChannel(pointer);
  return channel == nullptr || channel->state == nullptr
             ? nullptr
             : env->NewStringUTF(channel->state->name.c_str());
}

jobject InputChannelGetToken(JNIEnv*, jobject, jlong) { return nullptr; }

jlongArray InputChannelOpenPair(JNIEnv* env, jclass, jstring name) {
  const char* utf = name == nullptr ? nullptr : env->GetStringUTFChars(name, nullptr);
  auto state = std::make_shared<DarwinInputChannelState>(
      utf == nullptr ? "darwin-art-input" : utf);
  if (utf != nullptr) env->ReleaseStringUTFChars(name, utf);
  auto* client = new (std::nothrow) DarwinInputChannel{state, false, false};
  auto* server = new (std::nothrow) DarwinInputChannel{state, true, false};
  if (client == nullptr || server == nullptr) {
    delete client;
    delete server;
    return nullptr;
  }
  jlong values[] = {
      static_cast<jlong>(reinterpret_cast<std::uintptr_t>(client)),
      static_cast<jlong>(reinterpret_cast<std::uintptr_t>(server)),
  };
  jlongArray result = env->NewLongArray(2);
  if (result != nullptr) env->SetLongArrayRegion(result, 0, 2, values);
  return result;
}

jlong InputChannelReadParcel(JNIEnv*, jobject, jobject) { return 0; }
void InputChannelWriteParcel(JNIEnv*, jobject, jobject, jlong) {}

struct DarwinInputReceiver {
  jobject weak_receiver = nullptr;
  std::shared_ptr<DarwinInputChannelState> channel;
  jint next_sequence = 1;
  jint last_finished_sequence = 0;
  bool last_finished_handled = false;
  bool focused = false;
  bool touch_mode = false;
};

jboolean InputReceiverConsume(JNIEnv*, jclass, jlong, jlong) {
  return JNI_FALSE;
}
void InputReceiverDispose(JNIEnv* env, jclass, jlong pointer) {
  auto* receiver = reinterpret_cast<DarwinInputReceiver*>(
      static_cast<std::uintptr_t>(pointer));
  if (receiver == nullptr) return;
  if (receiver->weak_receiver != nullptr) {
    env->DeleteGlobalRef(receiver->weak_receiver);
  }
  delete receiver;
}
jstring InputReceiverDump(JNIEnv* env, jclass, jlong, jstring) {
  return env->NewStringUTF("");
}
void InputReceiverFinish(JNIEnv*, jclass, jlong pointer, jint sequence,
                         jboolean handled) {
  auto* receiver = reinterpret_cast<DarwinInputReceiver*>(
      static_cast<std::uintptr_t>(pointer));
  if (receiver != nullptr) {
    receiver->last_finished_sequence = sequence;
    receiver->last_finished_handled = handled == JNI_TRUE;
  }
  if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
    std::cerr << "ART Android InputEvent finish sequence=" << sequence
              << " handled=" << (handled == JNI_TRUE ? 1 : 0) << "\n";
  }
}
jlong InputReceiverInit(JNIEnv* env, jclass, jobject weak_receiver,
                        jobject input_channel, jobject) {
  jclass channel_class = env->GetObjectClass(input_channel);
  jfieldID pointer_field = channel_class == nullptr
                               ? nullptr
                               : env->GetFieldID(channel_class, "mPtr", "J");
  const jlong pointer = pointer_field == nullptr
                            ? 0
                            : env->GetLongField(input_channel, pointer_field);
  const auto* channel = InputChannel(pointer);
  env->DeleteLocalRef(channel_class);
  if (channel == nullptr || channel->disposed || channel->state == nullptr ||
      weak_receiver == nullptr || env->ExceptionCheck()) {
    return 0;
  }
  auto* receiver = new (std::nothrow) DarwinInputReceiver{
      env->NewGlobalRef(weak_receiver), channel->state, 1, 0, false, false,
      false};
  if (receiver == nullptr || receiver->weak_receiver == nullptr) {
    delete receiver;
    return 0;
  }
  return static_cast<jlong>(
      reinterpret_cast<std::uintptr_t>(receiver));
}
jboolean InputReceiverProbablyHasInput(JNIEnv*, jclass, jlong) {
  return JNI_FALSE;
}
void InputReceiverReportTimeline(JNIEnv*, jclass, jlong, jint, jlong, jlong) {}

struct DarwinKeyCharacterMap {
  jint device_id;
};

DarwinKeyCharacterMap* KeyMap(jlong pointer) {
  return reinterpret_cast<DarwinKeyCharacterMap*>(
      static_cast<std::uintptr_t>(pointer));
}

void KeyMapApplyOverlay(JNIEnv*, jclass, jlong, jstring, jstring) {}
void KeyMapDispose(JNIEnv*, jclass, jlong pointer) { delete KeyMap(pointer); }
jboolean KeyMapEquals(JNIEnv*, jclass, jlong left, jlong right) {
  const auto* lhs = KeyMap(left);
  const auto* rhs = KeyMap(right);
  return lhs != nullptr && rhs != nullptr && lhs->device_id == rhs->device_id
             ? JNI_TRUE
             : JNI_FALSE;
}
jchar KeyMapGetCharacter(JNIEnv*, jclass, jlong, jint key_code,
                         jint meta_state) {
  const bool shift = (meta_state & 0x1) != 0;
  if (key_code >= 29 && key_code <= 54) {
    const char base = static_cast<char>('a' + (key_code - 29));
    return static_cast<jchar>(shift ? base - ('a' - 'A') : base);
  }
  if (key_code >= 7 && key_code <= 16) {
    static constexpr char kPlain[] = "0123456789";
    static constexpr char kShifted[] = ")!@#$%^&*(";
    const size_t index = static_cast<size_t>(key_code - 7);
    return static_cast<jchar>(shift ? kShifted[index] : kPlain[index]);
  }
  switch (key_code) {
    case 55: return shift ? '<' : ',';
    case 56: return shift ? '>' : '.';
    case 62: return ' ';
    case 68: return shift ? '~' : '`';
    case 69: return shift ? '_' : '-';
    case 70: return shift ? '+' : '=';
    case 71: return shift ? '{' : '[';
    case 72: return shift ? '}' : ']';
    case 73: return shift ? '|' : '\\';
    case 74: return shift ? ':' : ';';
    case 75: return shift ? '"' : '\'';
    case 76: return shift ? '?' : '/';
    default: return 0;
  }
}
jchar KeyMapGetDisplayLabel(JNIEnv*, jclass, jlong, jint) { return 0; }
jobjectArray KeyMapGetEvents(JNIEnv*, jclass, jlong, jcharArray) { return nullptr; }
jboolean KeyMapGetFallbackAction(JNIEnv*, jclass, jlong, jint, jint, jobject) {
  return JNI_FALSE;
}
jint KeyMapGetKeyboardType(JNIEnv*, jclass, jlong) {
  // KeyCharacterMap.FULL: macOS normally supplies a connected physical
  // keyboard, not Android's synthetic VIRTUAL_KEYBOARD device.
  return 4;
}
jint KeyMapGetMappedKey(JNIEnv*, jclass, jlong, jint) { return 0; }
jchar KeyMapGetMatch(JNIEnv*, jclass, jlong, jint, jcharArray, jint) { return 0; }
jchar KeyMapGetNumber(JNIEnv*, jclass, jlong, jint) { return 0; }
jobject KeyMapObtainEmpty(JNIEnv* env, jclass klass, jint device_id) {
  auto* map = new (std::nothrow) DarwinKeyCharacterMap{device_id};
  if (map == nullptr) return nullptr;
  jmethodID constructor = env->GetMethodID(klass, "<init>", "(J)V");
  jobject result =
      constructor == nullptr
          ? nullptr
          : env->NewObject(klass, constructor,
                           static_cast<jlong>(reinterpret_cast<std::uintptr_t>(map)));
  if (result == nullptr || env->ExceptionCheck()) {
    delete map;
    return nullptr;
  }
  return result;
}
jlong KeyMapReadFromParcel(JNIEnv*, jclass, jobject) {
  auto* map = new (std::nothrow) DarwinKeyCharacterMap{1};
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(map));
}
void KeyMapWriteToParcel(JNIEnv*, jclass, jlong, jobject) {}

std::atomic<jint> g_next_key_event_id{1};

jint KeyEventNextId(JNIEnv*, jclass) {
  return g_next_key_event_id.fetch_add(1, std::memory_order_relaxed);
}

jint KeyEventCodeFromString(JNIEnv* env, jclass, jstring value) {
  if (value == nullptr) return 0;
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) return 0;
  jint result = 0;
  if (std::strncmp(utf, "KEYCODE_", 8) == 0) {
    char* end = nullptr;
    const long parsed = std::strtol(utf + 8, &end, 10);
    if (end != utf + 8 && *end == '\0' && parsed >= 0 && parsed <= INT32_MAX) {
      result = static_cast<jint>(parsed);
    }
  }
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

jstring KeyEventCodeToString(JNIEnv* env, jclass, jint key_code) {
  const std::string value = std::to_string(key_code);
  return env->NewStringUTF(value.c_str());
}

jobject CreateDarwinContextBinder(JNIEnv* env) {
  // There is no system_server on the Darwin host. The fixture installs a
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

bool FocusFrameworkViewRoot(JNIEnv* env, jobject view_root) {
  if (env == nullptr || view_root == nullptr) return false;
  jclass view_root_class = env->GetObjectClass(view_root);
  jfieldID receiver_field =
      view_root_class == nullptr
          ? nullptr
          : env->GetFieldID(
                view_root_class, "mInputEventReceiver",
                "Landroid/view/ViewRootImpl$WindowInputEventReceiver;");
  jobject java_receiver = receiver_field == nullptr
                              ? nullptr
                              : env->GetObjectField(view_root, receiver_field);
  jclass receiver_class = java_receiver == nullptr
                              ? nullptr
                              : env->GetObjectClass(java_receiver);
  jfieldID pointer_field =
      receiver_class == nullptr
          ? nullptr
          : env->GetFieldID(receiver_class, "mReceiverPtr", "J");
  const jlong pointer = pointer_field == nullptr
                            ? 0
                            : env->GetLongField(java_receiver, pointer_field);
  auto* receiver = reinterpret_cast<DarwinInputReceiver*>(
      static_cast<std::uintptr_t>(pointer));
  jclass weak_class =
      receiver == nullptr || receiver->weak_receiver == nullptr
          ? nullptr
          : env->GetObjectClass(receiver->weak_receiver);
  jmethodID weak_get =
      weak_class == nullptr
          ? nullptr
          : env->GetMethodID(weak_class, "get", "()Ljava/lang/Object;");
  jobject target = weak_get == nullptr
                       ? nullptr
                       : env->CallObjectMethod(receiver->weak_receiver, weak_get);
  jclass input_receiver_class =
      env->FindClass("android/view/InputEventReceiver");
  jmethodID focus =
      input_receiver_class == nullptr
          ? nullptr
          : env->GetMethodID(input_receiver_class, "onFocusEvent", "(Z)V");
  if (target != nullptr && focus != nullptr && !env->ExceptionCheck()) {
    env->CallVoidMethod(target, focus, JNI_TRUE);
  }
  const bool focused = target != nullptr && focus != nullptr &&
                       !env->ExceptionCheck();
  if (receiver != nullptr && focused) receiver->focused = true;
  if (input_receiver_class != nullptr) {
    env->DeleteLocalRef(input_receiver_class);
  }
  if (target != nullptr) env->DeleteLocalRef(target);
  if (weak_class != nullptr) env->DeleteLocalRef(weak_class);
  if (receiver_class != nullptr) env->DeleteLocalRef(receiver_class);
  if (java_receiver != nullptr) env->DeleteLocalRef(java_receiver);
  if (view_root_class != nullptr) env->DeleteLocalRef(view_root_class);
  return focused;
}

bool DispatchFrameworkInputEvent(JNIEnv* env, jobject view_root, jobject event,
                                 bool* handled) {
  if (env == nullptr || view_root == nullptr || event == nullptr) return false;
  if (handled != nullptr) *handled = false;
  jclass view_root_class = env->GetObjectClass(view_root);
  jfieldID receiver_field =
      view_root_class == nullptr
          ? nullptr
          : env->GetFieldID(
                view_root_class, "mInputEventReceiver",
                "Landroid/view/ViewRootImpl$WindowInputEventReceiver;");
  jobject java_receiver =
      receiver_field == nullptr
          ? nullptr
          : env->GetObjectField(view_root, receiver_field);
  jclass receiver_class = java_receiver == nullptr
                              ? nullptr
                              : env->GetObjectClass(java_receiver);
  jfieldID pointer_field =
      receiver_class == nullptr
          ? nullptr
          : env->GetFieldID(receiver_class, "mReceiverPtr", "J");
  const jlong pointer = pointer_field == nullptr
                            ? 0
                            : env->GetLongField(java_receiver, pointer_field);
  auto* receiver = reinterpret_cast<DarwinInputReceiver*>(
      static_cast<std::uintptr_t>(pointer));
  if (receiver == nullptr || receiver->channel == nullptr ||
      receiver->weak_receiver == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(receiver_class);
    env->DeleteLocalRef(java_receiver);
    env->DeleteLocalRef(view_root_class);
    return false;
  }
  jclass weak_class = env->GetObjectClass(receiver->weak_receiver);
  jmethodID weak_get =
      weak_class == nullptr
          ? nullptr
          : env->GetMethodID(weak_class, "get", "()Ljava/lang/Object;");
  jobject target = weak_get == nullptr
                       ? nullptr
                       : env->CallObjectMethod(receiver->weak_receiver, weak_get);
  jclass input_receiver_class = env->FindClass("android/view/InputEventReceiver");
  jmethodID focus =
      input_receiver_class == nullptr
          ? nullptr
          : env->GetMethodID(input_receiver_class, "onFocusEvent", "(Z)V");
  if (!receiver->focused && target != nullptr && focus != nullptr &&
      !env->ExceptionCheck()) {
    env->CallVoidMethod(target, focus, JNI_TRUE);
    jmethodID window_focus_changed = env->GetMethodID(
        view_root_class, "windowFocusChanged", "(Z)V");
    jmethodID handle_focus = env->GetMethodID(
        view_root_class, "handleWindowFocusChanged", "()V");
    if (window_focus_changed != nullptr && handle_focus != nullptr &&
        !env->ExceptionCheck()) {
      env->CallVoidMethod(view_root, window_focus_changed, JNI_TRUE);
      env->CallVoidMethod(view_root, handle_focus);
    }
    receiver->focused = !env->ExceptionCheck();
  }
  jclass motion_event_class = env->FindClass("android/view/MotionEvent");
  const bool is_motion =
      motion_event_class != nullptr && env->IsInstanceOf(event, motion_event_class);
  if (is_motion && !receiver->touch_mode && target != nullptr &&
      input_receiver_class != nullptr && !env->ExceptionCheck()) {
    jmethodID touch_mode = env->GetMethodID(
        input_receiver_class, "onTouchModeChanged", "(Z)V");
    jmethodID view_root_touch_mode = env->GetMethodID(
        view_root_class, "touchModeChanged", "(Z)V");
    jmethodID handle_touch_mode = env->GetMethodID(
        view_root_class, "handleWindowTouchModeChanged", "()V");
    if (touch_mode != nullptr && view_root_touch_mode != nullptr &&
        handle_touch_mode != nullptr &&
        !env->ExceptionCheck()) {
      env->CallVoidMethod(target, touch_mode, JNI_TRUE);
      env->CallVoidMethod(view_root, view_root_touch_mode, JNI_TRUE);
      env->CallVoidMethod(view_root, handle_touch_mode);
      receiver->touch_mode = !env->ExceptionCheck();
    }
  }
  jmethodID dispatch =
      input_receiver_class == nullptr
          ? nullptr
          : env->GetMethodID(input_receiver_class, "dispatchInputEvent",
                             "(ILandroid/view/InputEvent;)V");
  if (target != nullptr && dispatch != nullptr && !env->ExceptionCheck()) {
    const jint sequence = receiver->next_sequence++;
    receiver->last_finished_sequence = 0;
    receiver->last_finished_handled = false;
    env->CallVoidMethod(target, dispatch, sequence, event);
    if (handled != nullptr && receiver->last_finished_sequence == sequence) {
      *handled = receiver->last_finished_handled;
    }
  }
  const bool delivered = target != nullptr && dispatch != nullptr &&
                         !env->ExceptionCheck();
  env->DeleteLocalRef(input_receiver_class);
  env->DeleteLocalRef(motion_event_class);
  env->DeleteLocalRef(target);
  env->DeleteLocalRef(weak_class);
  env->DeleteLocalRef(receiver_class);
  env->DeleteLocalRef(java_receiver);
  env->DeleteLocalRef(view_root_class);
  return delivered;
}

bool RegisterFrameworkBinderNatives(JNIEnv* env) {
  JNINativeMethod binder_methods[] = {
      {const_cast<char*>("getNativeBBinderHolder"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&BinderGetNativeHolder)},
      {const_cast<char*>("getNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&BinderGetNativeFinalizer)},
      {const_cast<char*>("getCallingUid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&BinderGetCallingUid)},
      {const_cast<char*>("getCallingPid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&BinderGetCallingPid)},
      {const_cast<char*>("clearCallingIdentity"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&BinderClearCallingIdentity)},
      {const_cast<char*>("restoreCallingIdentity"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&BinderRestoreCallingIdentity)},
      {const_cast<char*>("flushPendingCommands"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&BinderFlushPendingCommands)},
      {const_cast<char*>("getThreadStrictModePolicy"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&BinderGetThreadStrictModePolicy)},
      {const_cast<char*>("setThreadStrictModePolicy"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&BinderSetThreadStrictModePolicy)},
  };
  if (!Register(env, "android/os/Binder", binder_methods,
                static_cast<jint>(std::size(binder_methods)))) {
    return false;
  }

  JNINativeMethod binder_internal_methods[] = {
      {const_cast<char*>("getContextObject"),
       const_cast<char*>("()Landroid/os/IBinder;"),
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

  JNINativeMethod input_channel_methods[] = {
      {const_cast<char*>("nativeDup"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&InputChannelDup)},
      {const_cast<char*>("nativeGetFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&InputChannelGetFinalizer)},
      {const_cast<char*>("nativeGetName"),
       const_cast<char*>("(J)Ljava/lang/String;"),
       reinterpret_cast<void*>(&InputChannelGetName)},
      {const_cast<char*>("nativeGetToken"),
       const_cast<char*>("(J)Landroid/os/IBinder;"),
       reinterpret_cast<void*>(&InputChannelGetToken)},
      {const_cast<char*>("nativeOpenInputChannelPair"),
       const_cast<char*>("(Ljava/lang/String;)[J"),
       reinterpret_cast<void*>(&InputChannelOpenPair)},
      {const_cast<char*>("nativeReadFromParcel"),
       const_cast<char*>("(Landroid/os/Parcel;)J"),
       reinterpret_cast<void*>(&InputChannelReadParcel)},
      {const_cast<char*>("nativeWriteToParcel"),
       const_cast<char*>("(Landroid/os/Parcel;J)V"),
       reinterpret_cast<void*>(&InputChannelWriteParcel)},
      {const_cast<char*>("nativeDispose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&InputChannelDispose)},
  };
  if (!Register(env, "android/view/InputChannel", input_channel_methods,
                static_cast<jint>(std::size(input_channel_methods)))) {
    return false;
  }

  JNINativeMethod input_receiver_methods[] = {
      {const_cast<char*>("nativeConsumeBatchedInputEvents"),
       const_cast<char*>("(JJ)Z"),
       reinterpret_cast<void*>(&InputReceiverConsume)},
      {const_cast<char*>("nativeDispose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&InputReceiverDispose)},
      {const_cast<char*>("nativeDump"),
       const_cast<char*>("(JLjava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&InputReceiverDump)},
      {const_cast<char*>("nativeFinishInputEvent"),
       const_cast<char*>("(JIZ)V"),
       reinterpret_cast<void*>(&InputReceiverFinish)},
      {const_cast<char*>("nativeInit"),
       const_cast<char*>("(Ljava/lang/ref/WeakReference;"
                         "Landroid/view/InputChannel;Landroid/os/MessageQueue;)J"),
       reinterpret_cast<void*>(&InputReceiverInit)},
      {const_cast<char*>("nativeProbablyHasInput"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&InputReceiverProbablyHasInput)},
      {const_cast<char*>("nativeReportTimeline"),
       const_cast<char*>("(JIJJ)V"),
       reinterpret_cast<void*>(&InputReceiverReportTimeline)},
  };
  if (!Register(env, "android/view/InputEventReceiver", input_receiver_methods,
                static_cast<jint>(std::size(input_receiver_methods)))) {
    return false;
  }

  JNINativeMethod key_character_map_methods[] = {
      {const_cast<char*>("nativeApplyOverlay"),
       const_cast<char*>("(JLjava/lang/String;Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&KeyMapApplyOverlay)},
      {const_cast<char*>("nativeDispose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&KeyMapDispose)},
      {const_cast<char*>("nativeEquals"), const_cast<char*>("(JJ)Z"),
       reinterpret_cast<void*>(&KeyMapEquals)},
      {const_cast<char*>("nativeGetCharacter"), const_cast<char*>("(JII)C"),
       reinterpret_cast<void*>(&KeyMapGetCharacter)},
      {const_cast<char*>("nativeGetDisplayLabel"), const_cast<char*>("(JI)C"),
       reinterpret_cast<void*>(&KeyMapGetDisplayLabel)},
      {const_cast<char*>("nativeGetEvents"),
       const_cast<char*>("(J[C)[Landroid/view/KeyEvent;"),
       reinterpret_cast<void*>(&KeyMapGetEvents)},
      {const_cast<char*>("nativeGetFallbackAction"),
       const_cast<char*>("(JIILandroid/view/KeyCharacterMap$FallbackAction;)Z"),
       reinterpret_cast<void*>(&KeyMapGetFallbackAction)},
      {const_cast<char*>("nativeGetKeyboardType"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&KeyMapGetKeyboardType)},
      {const_cast<char*>("nativeGetMappedKey"), const_cast<char*>("(JI)I"),
       reinterpret_cast<void*>(&KeyMapGetMappedKey)},
      {const_cast<char*>("nativeGetMatch"), const_cast<char*>("(JI[CI)C"),
       reinterpret_cast<void*>(&KeyMapGetMatch)},
      {const_cast<char*>("nativeGetNumber"), const_cast<char*>("(JI)C"),
       reinterpret_cast<void*>(&KeyMapGetNumber)},
      {const_cast<char*>("nativeObtainEmptyKeyCharacterMap"),
       const_cast<char*>("(I)Landroid/view/KeyCharacterMap;"),
       reinterpret_cast<void*>(&KeyMapObtainEmpty)},
      {const_cast<char*>("nativeReadFromParcel"),
       const_cast<char*>("(Landroid/os/Parcel;)J"),
       reinterpret_cast<void*>(&KeyMapReadFromParcel)},
      {const_cast<char*>("nativeWriteToParcel"),
       const_cast<char*>("(JLandroid/os/Parcel;)V"),
       reinterpret_cast<void*>(&KeyMapWriteToParcel)},
  };
  if (!Register(env, "android/view/KeyCharacterMap",
                key_character_map_methods,
                static_cast<jint>(std::size(key_character_map_methods)))) {
    return false;
  }
  JNINativeMethod key_event_methods[] = {
      {const_cast<char*>("nativeKeyCodeFromString"),
       const_cast<char*>("(Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&KeyEventCodeFromString)},
      {const_cast<char*>("nativeKeyCodeToString"),
       const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&KeyEventCodeToString)},
      {const_cast<char*>("nativeNextId"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&KeyEventNextId)},
  };
  return Register(env, "android/view/KeyEvent", key_event_methods,
                  static_cast<jint>(std::size(key_event_methods)));
}

}  // namespace darwin_art
