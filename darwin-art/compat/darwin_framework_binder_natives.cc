#include "darwin_framework_natives.h"

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

struct DarwinParcel {
  std::vector<uint8_t> data;
  size_t position = 0;
  bool allow_fds = true;
  std::vector<jobject> binders;
};

DarwinParcel* Parcel(jlong pointer) {
  return reinterpret_cast<DarwinParcel*>(static_cast<std::uintptr_t>(pointer));
}

template <typename T>
jint ParcelWriteScalar(jlong pointer, T value) {
  auto* parcel = Parcel(pointer);
  if (parcel == nullptr) return -1;
  const size_t end = parcel->position + sizeof(T);
  if (end > parcel->data.size()) parcel->data.resize(end);
  std::memcpy(parcel->data.data() + parcel->position, &value, sizeof(T));
  parcel->position = end;
  return 0;
}

template <typename T>
T ParcelReadScalar(jlong pointer) {
  auto* parcel = Parcel(pointer);
  T value{};
  if (parcel == nullptr || parcel->position + sizeof(T) > parcel->data.size()) {
    return value;
  }
  std::memcpy(&value, parcel->data.data() + parcel->position, sizeof(T));
  parcel->position += sizeof(T);
  return value;
}

jlong ParcelCreate(JNIEnv*, jclass) {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      new (std::nothrow) DarwinParcel()));
}

void ParcelDestroy(JNIEnv* env, jclass, jlong pointer) {
  auto* parcel = Parcel(pointer);
  if (parcel == nullptr) return;
  for (jobject binder : parcel->binders) env->DeleteGlobalRef(binder);
  delete parcel;
}

void ParcelFreeBuffer(JNIEnv*, jclass, jlong pointer) {
  if (auto* parcel = Parcel(pointer); parcel != nullptr) {
    parcel->data.clear();
    parcel->position = 0;
  }
}

void ParcelMarkSensitive(jlong) {}
void ParcelMarkForBinder(JNIEnv*, jclass, jlong, jobject) {}
jboolean ParcelIsForRpc(jlong) { return JNI_FALSE; }
jint ParcelDataSize(jlong p) { return Parcel(p) == nullptr ? 0 : Parcel(p)->data.size(); }
jint ParcelDataAvail(jlong p) {
  auto* parcel = Parcel(p);
  return parcel == nullptr || parcel->position >= parcel->data.size()
      ? 0 : static_cast<jint>(parcel->data.size() - parcel->position);
}
jint ParcelDataPosition(jlong p) {
  return Parcel(p) == nullptr ? 0 : static_cast<jint>(Parcel(p)->position);
}
jint ParcelDataCapacity(jlong p) {
  return Parcel(p) == nullptr ? 0 : static_cast<jint>(Parcel(p)->data.capacity());
}
void ParcelSetDataSize(JNIEnv*, jclass, jlong p, jint size) {
  if (auto* parcel = Parcel(p); parcel != nullptr && size >= 0) {
    parcel->data.resize(static_cast<size_t>(size));
    if (parcel->position > parcel->data.size()) parcel->position = parcel->data.size();
  }
}
void ParcelSetDataPosition(jlong p, jint position) {
  if (auto* parcel = Parcel(p); parcel != nullptr && position >= 0) {
    parcel->position = std::min(static_cast<size_t>(position), parcel->data.size());
  }
}
void ParcelSetDataCapacity(JNIEnv*, jclass, jlong p, jint capacity) {
  if (auto* parcel = Parcel(p); parcel != nullptr && capacity >= 0) {
    parcel->data.reserve(static_cast<size_t>(capacity));
  }
}
jboolean ParcelPushAllowFds(jlong p, jboolean allow) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr) return JNI_FALSE;
  const bool previous = parcel->allow_fds;
  parcel->allow_fds = allow == JNI_TRUE;
  return previous ? JNI_TRUE : JNI_FALSE;
}
void ParcelRestoreAllowFds(jlong p, jboolean allow) {
  if (auto* parcel = Parcel(p); parcel != nullptr) parcel->allow_fds = allow == JNI_TRUE;
}
jint ParcelWriteInt(jlong p, jint v) { return ParcelWriteScalar(p, v); }
jint ParcelWriteLong(jlong p, jlong v) { return ParcelWriteScalar(p, v); }
jint ParcelWriteFloat(jlong p, jfloat v) { return ParcelWriteScalar(p, v); }
jint ParcelWriteDouble(jlong p, jdouble v) { return ParcelWriteScalar(p, v); }
jint ParcelReadInt(jlong p) { return ParcelReadScalar<jint>(p); }
jlong ParcelReadLong(jlong p) { return ParcelReadScalar<jlong>(p); }
jfloat ParcelReadFloat(jlong p) { return ParcelReadScalar<jfloat>(p); }
jdouble ParcelReadDouble(jlong p) { return ParcelReadScalar<jdouble>(p); }

void ParcelWriteBytes(JNIEnv* env, jclass, jlong p, jbyteArray bytes,
                      jint offset, jint length) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr || bytes == nullptr || offset < 0 || length < 0) return;
  ParcelWriteInt(p, length);
  const size_t end = parcel->position + static_cast<size_t>(length);
  if (end > parcel->data.size()) parcel->data.resize(end);
  env->GetByteArrayRegion(bytes, offset, length,
                          reinterpret_cast<jbyte*>(parcel->data.data() + parcel->position));
  parcel->position = end;
}

jbyteArray ParcelCreateByteArray(JNIEnv* env, jclass, jlong p) {
  const jint length = ParcelReadInt(p);
  auto* parcel = Parcel(p);
  if (parcel == nullptr || length < 0 ||
      parcel->position + static_cast<size_t>(length) > parcel->data.size()) return nullptr;
  jbyteArray result = env->NewByteArray(length);
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, length,
        reinterpret_cast<const jbyte*>(parcel->data.data() + parcel->position));
    parcel->position += static_cast<size_t>(length);
  }
  return result;
}

jboolean ParcelReadByteArray(JNIEnv* env, jclass, jlong p, jbyteArray output,
                             jint length) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr || output == nullptr || length < 0 ||
      parcel->position + static_cast<size_t>(length) > parcel->data.size()) {
    return JNI_FALSE;
  }
  env->SetByteArrayRegion(output, 0, length,
      reinterpret_cast<const jbyte*>(parcel->data.data() + parcel->position));
  parcel->position += static_cast<size_t>(length);
  return JNI_TRUE;
}

void ParcelWriteString(JNIEnv* env, jclass, jlong p, jstring value) {
  if (value == nullptr) { ParcelWriteInt(p, -1); return; }
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) { ParcelWriteInt(p, -1); return; }
  const jint length = static_cast<jint>(std::strlen(utf));
  ParcelWriteInt(p, length);
  auto* parcel = Parcel(p);
  const size_t end = parcel->position + static_cast<size_t>(length);
  if (end > parcel->data.size()) parcel->data.resize(end);
  std::memcpy(parcel->data.data() + parcel->position, utf, length);
  parcel->position = end;
  env->ReleaseStringUTFChars(value, utf);
}

jstring ParcelReadString(JNIEnv* env, jclass, jlong p) {
  const jint length = ParcelReadInt(p);
  auto* parcel = Parcel(p);
  if (parcel == nullptr || length < 0 ||
      parcel->position + static_cast<size_t>(length) > parcel->data.size()) return nullptr;
  std::string value(reinterpret_cast<const char*>(parcel->data.data() + parcel->position),
                    static_cast<size_t>(length));
  parcel->position += static_cast<size_t>(length);
  return env->NewStringUTF(value.c_str());
}

void ParcelWriteStrongBinder(JNIEnv* env, jclass, jlong p, jobject binder) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr) return;
  if (binder == nullptr) { ParcelWriteInt(p, -1); return; }
  parcel->binders.push_back(env->NewGlobalRef(binder));
  ParcelWriteInt(p, static_cast<jint>(parcel->binders.size() - 1));
}

jobject ParcelReadStrongBinder(JNIEnv* env, jclass, jlong p) {
  auto* parcel = Parcel(p);
  const jint index = ParcelReadInt(p);
  return parcel == nullptr || index < 0 ||
      static_cast<size_t>(index) >= parcel->binders.size()
      ? nullptr : env->NewLocalRef(parcel->binders[static_cast<size_t>(index)]);
}

jbyteArray ParcelMarshall(JNIEnv* env, jclass, jlong p) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr) return nullptr;
  jbyteArray result = env->NewByteArray(static_cast<jsize>(parcel->data.size()));
  if (result != nullptr && !parcel->data.empty()) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(parcel->data.size()),
        reinterpret_cast<const jbyte*>(parcel->data.data()));
  }
  return result;
}

void ParcelUnmarshall(JNIEnv* env, jclass, jlong p, jbyteArray bytes,
                      jint offset, jint length) {
  auto* parcel = Parcel(p);
  if (parcel == nullptr || bytes == nullptr || offset < 0 || length < 0) return;
  parcel->data.resize(static_cast<size_t>(length));
  env->GetByteArrayRegion(bytes, offset, length,
                          reinterpret_cast<jbyte*>(parcel->data.data()));
  parcel->position = 0;
}

jint ParcelCompareData(JNIEnv*, jclass, jlong left, jlong right) {
  auto* a = Parcel(left);
  auto* b = Parcel(right);
  if (a == nullptr || b == nullptr) return a == b ? 0 : a == nullptr ? -1 : 1;
  if (a->data == b->data) return 0;
  return std::lexicographical_compare(a->data.begin(), a->data.end(),
                                      b->data.begin(), b->data.end()) ? -1 : 1;
}

void ParcelAppendFrom(JNIEnv*, jclass, jlong destination, jlong source,
                      jint offset, jint length) {
  auto* to = Parcel(destination);
  auto* from = Parcel(source);
  if (to == nullptr || from == nullptr || offset < 0 || length < 0 ||
      static_cast<size_t>(offset) + static_cast<size_t>(length) > from->data.size()) return;
  to->data.insert(to->data.end(), from->data.begin() + offset,
                  from->data.begin() + offset + length);
  to->position = to->data.size();
}

void ParcelSignalException(JNIEnv*, jclass, jint) {}

jboolean ParcelHasBinders(JNIEnv*, jclass, jlong p) {
  return Parcel(p) != nullptr && !Parcel(p)->binders.empty() ? JNI_TRUE : JNI_FALSE;
}
jboolean ParcelHasBindersRange(JNIEnv* env, jclass cls, jlong p, jint, jint) {
  return ParcelHasBinders(env, cls, p);
}
jboolean ParcelHasFileDescriptors(jlong) { return JNI_FALSE; }
jboolean ParcelHasFileDescriptorsRange(JNIEnv*, jclass, jlong, jint, jint) {
  return JNI_FALSE;
}
jboolean ParcelReplaceWorkSource(jlong, jint) { return JNI_FALSE; }
jint ParcelReadWorkSource(jlong) { return -1; }
jlong ParcelOpenAshmemSize(jlong) { return 0; }

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
  JNINativeMethod parcel_methods[] = {
      {const_cast<char*>("nativeMarkSensitive"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&ParcelMarkSensitive)},
      {const_cast<char*>("nativeMarkForBinder"),
       const_cast<char*>("(JLandroid/os/IBinder;)V"),
       reinterpret_cast<void*>(&ParcelMarkForBinder)},
      {const_cast<char*>("nativeIsForRpc"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&ParcelIsForRpc)},
      {const_cast<char*>("nativeDataSize"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelDataSize)},
      {const_cast<char*>("nativeDataAvail"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelDataAvail)},
      {const_cast<char*>("nativeDataPosition"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelDataPosition)},
      {const_cast<char*>("nativeDataCapacity"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelDataCapacity)},
      {const_cast<char*>("nativeSetDataSize"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&ParcelSetDataSize)},
      {const_cast<char*>("nativeSetDataPosition"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&ParcelSetDataPosition)},
      {const_cast<char*>("nativeSetDataCapacity"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&ParcelSetDataCapacity)},
      {const_cast<char*>("nativePushAllowFds"), const_cast<char*>("(JZ)Z"),
       reinterpret_cast<void*>(&ParcelPushAllowFds)},
      {const_cast<char*>("nativeRestoreAllowFds"), const_cast<char*>("(JZ)V"),
       reinterpret_cast<void*>(&ParcelRestoreAllowFds)},
      {const_cast<char*>("nativeWriteByteArray"), const_cast<char*>("(J[BII)V"),
       reinterpret_cast<void*>(&ParcelWriteBytes)},
      {const_cast<char*>("nativeWriteBlob"), const_cast<char*>("(J[BII)V"),
       reinterpret_cast<void*>(&ParcelWriteBytes)},
      {const_cast<char*>("nativeWriteInt"), const_cast<char*>("(JI)I"),
       reinterpret_cast<void*>(&ParcelWriteInt)},
      {const_cast<char*>("nativeWriteLong"), const_cast<char*>("(JJ)I"),
       reinterpret_cast<void*>(&ParcelWriteLong)},
      {const_cast<char*>("nativeWriteFloat"), const_cast<char*>("(JF)I"),
       reinterpret_cast<void*>(&ParcelWriteFloat)},
      {const_cast<char*>("nativeWriteDouble"), const_cast<char*>("(JD)I"),
       reinterpret_cast<void*>(&ParcelWriteDouble)},
      {const_cast<char*>("nativeSignalExceptionForError"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&ParcelSignalException)},
      {const_cast<char*>("nativeWriteString8"),
       const_cast<char*>("(JLjava/lang/String;)V"),
       reinterpret_cast<void*>(&ParcelWriteString)},
      {const_cast<char*>("nativeWriteString16"),
       const_cast<char*>("(JLjava/lang/String;)V"),
       reinterpret_cast<void*>(&ParcelWriteString)},
      {const_cast<char*>("nativeWriteStrongBinder"),
       const_cast<char*>("(JLandroid/os/IBinder;)V"),
       reinterpret_cast<void*>(&ParcelWriteStrongBinder)},
      {const_cast<char*>("nativeCreateByteArray"), const_cast<char*>("(J)[B"),
       reinterpret_cast<void*>(&ParcelCreateByteArray)},
      {const_cast<char*>("nativeReadByteArray"), const_cast<char*>("(J[BI)Z"),
       reinterpret_cast<void*>(&ParcelReadByteArray)},
      {const_cast<char*>("nativeReadBlob"), const_cast<char*>("(J)[B"),
       reinterpret_cast<void*>(&ParcelCreateByteArray)},
      {const_cast<char*>("nativeReadInt"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelReadInt)},
      {const_cast<char*>("nativeReadLong"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&ParcelReadLong)},
      {const_cast<char*>("nativeReadFloat"), const_cast<char*>("(J)F"),
       reinterpret_cast<void*>(&ParcelReadFloat)},
      {const_cast<char*>("nativeReadDouble"), const_cast<char*>("(J)D"),
       reinterpret_cast<void*>(&ParcelReadDouble)},
      {const_cast<char*>("nativeReadString8"),
       const_cast<char*>("(J)Ljava/lang/String;"),
       reinterpret_cast<void*>(&ParcelReadString)},
      {const_cast<char*>("nativeReadString16"),
       const_cast<char*>("(J)Ljava/lang/String;"),
       reinterpret_cast<void*>(&ParcelReadString)},
      {const_cast<char*>("nativeReadStrongBinder"),
       const_cast<char*>("(J)Landroid/os/IBinder;"),
       reinterpret_cast<void*>(&ParcelReadStrongBinder)},
      {const_cast<char*>("nativeCreate"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&ParcelCreate)},
      {const_cast<char*>("nativeFreeBuffer"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&ParcelFreeBuffer)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&ParcelDestroy)},
      {const_cast<char*>("nativeMarshall"), const_cast<char*>("(J)[B"),
       reinterpret_cast<void*>(&ParcelMarshall)},
      {const_cast<char*>("nativeUnmarshall"), const_cast<char*>("(J[BII)V"),
       reinterpret_cast<void*>(&ParcelUnmarshall)},
      {const_cast<char*>("nativeCompareData"), const_cast<char*>("(JJ)I"),
       reinterpret_cast<void*>(&ParcelCompareData)},
      {const_cast<char*>("nativeAppendFrom"), const_cast<char*>("(JJII)V"),
       reinterpret_cast<void*>(&ParcelAppendFrom)},
      {const_cast<char*>("nativeHasFileDescriptors"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&ParcelHasFileDescriptors)},
      {const_cast<char*>("nativeHasFileDescriptorsInRange"),
       const_cast<char*>("(JII)Z"),
       reinterpret_cast<void*>(&ParcelHasFileDescriptorsRange)},
      {const_cast<char*>("nativeHasBinders"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&ParcelHasBinders)},
      {const_cast<char*>("nativeHasBindersInRange"), const_cast<char*>("(JII)Z"),
       reinterpret_cast<void*>(&ParcelHasBindersRange)},
      {const_cast<char*>("nativeWriteInterfaceToken"),
       const_cast<char*>("(JLjava/lang/String;)V"),
       reinterpret_cast<void*>(&ParcelWriteString)},
      {const_cast<char*>("nativeEnforceInterface"),
       const_cast<char*>("(JLjava/lang/String;)V"),
       reinterpret_cast<void*>(&ParcelWriteString)},
      {const_cast<char*>("nativeReplaceCallingWorkSourceUid"),
       const_cast<char*>("(JI)Z"),
       reinterpret_cast<void*>(&ParcelReplaceWorkSource)},
      {const_cast<char*>("nativeReadCallingWorkSourceUid"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&ParcelReadWorkSource)},
      {const_cast<char*>("nativeGetOpenAshmemSize"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&ParcelOpenAshmemSize)},
  };
  if (!Register(env, "android/os/Parcel", parcel_methods,
                static_cast<jint>(std::size(parcel_methods)))) {
    return false;
  }

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
