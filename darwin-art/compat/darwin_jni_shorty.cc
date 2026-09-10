#include <cstring>
#include <cstdint>
#include <string>

#include "darwin_jni_shorty.h"
#include "jni/jni_env_ext.h"
#include "jni/java_vm_ext.h"
#include "runtime.h"
#include "thread.h"

namespace android {

namespace {
struct LazyAttachment {
  art::JavaVMExt* vm = nullptr;
  ~LazyAttachment() {
    if (vm != nullptr) {
      (void)vm->DetachCurrentThread();
    }
  }
};
thread_local LazyAttachment g_lazy_attachment;
}  // namespace

JNIEnv* CurrentArtEnv() {
  art::Runtime* runtime = art::Runtime::Current();
  if (runtime == nullptr || runtime->GetJavaVM() == nullptr) return nullptr;
  art::JavaVMExt* vm = runtime->GetJavaVM();
  art::Thread* self = art::Thread::Current();
  if (self != nullptr) {
    JNIEnv* env = static_cast<JNIEnv*>(self->GetJniEnv());
    if (env != nullptr) return env;
  }
  // Native Chromium worker entry points can reach the JNI proxy before their
  // Android pthread wrapper runs. Preserve GetEnv's JNI_EDETACHED contract,
  // then attach only at this callback boundary and record that ownership in
  // the TLS lease above.
  JNIEnv* existing = nullptr;
  const jint state = vm->GetEnv(reinterpret_cast<void**>(&existing), JNI_VERSION_1_6);
  if (state != JNI_EDETACHED) return state == JNI_OK ? existing : nullptr;
  JNIEnv* attached = nullptr;
  if (vm->AttachCurrentThreadAsDaemon(&attached, nullptr) != JNI_OK) {
    return nullptr;
  }
  g_lazy_attachment.vm = vm;
  return attached;
}

bool CurrentGenericJniFrame(uint64_t* managed_sp) {
  if (managed_sp == nullptr) return false;
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) return false;
  const art::ManagedStack* stack = self->GetManagedStack();
  // The callback may be reached through a Darwin native-registration bridge
  // that does not publish the usual quick-frame registry. Generic-JNI marks
  // the ManagedStack pointer with the AOSP JNI tag, so use GetTopQuickFrame()
  // to strip that tag rather than the DCHECK-only KnownNotTagged accessor.
  if (stack == nullptr) return false;
  if (!stack->HasTopQuickFrame() || !stack->GetTopQuickFrameGenericJniTag()) return false;
  *managed_sp = reinterpret_cast<uint64_t>(stack->GetTopQuickFrame());
  return *managed_sp != 0;
}

bool CurrentInterpreterFrame(uint64_t* shadow_frame) {
  if (shadow_frame == nullptr) return false;
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) return false;
  const art::ManagedStack* stack = self->GetManagedStack();
  if (stack == nullptr || !stack->HasTopShadowFrame()) return false;
  *shadow_frame = reinterpret_cast<uint64_t>(stack->GetTopShadowFrame());
  return *shadow_frame != 0;
}

namespace {

bool ParseDescriptorType(const char** cursor, bool allow_void, char* shorty_type) {
  const char* current = *cursor;
  switch (*current) {
    case 'V':
      if (!allow_void) return false;
      *shorty_type = 'V';
      *cursor = current + 1;
      return true;
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
      *shorty_type = *current;
      *cursor = current + 1;
      return true;
    case 'L': {
      const char* end = std::strchr(current + 1, ';');
      if (end == nullptr || end == current + 1) return false;
      *shorty_type = 'L';
      *cursor = end + 1;
      return true;
    }
    case '[': {
      do {
        ++current;
      } while (*current == '[');
      char component = 0;
      if (!ParseDescriptorType(&current, false, &component)) return false;
      *shorty_type = 'L';
      *cursor = current;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

bool DescriptorToShorty(const char* descriptor, std::string* shorty) {
  if (descriptor == nullptr || shorty == nullptr || descriptor[0] != '(') {
    return false;
  }
  const char* cursor = descriptor + 1;
  std::string arguments;
  while (*cursor != ')') {
    char type = 0;
    if (*cursor == '\0' || !ParseDescriptorType(&cursor, false, &type)) {
      return false;
    }
    arguments.push_back(type);
  }
  ++cursor;
  char return_type = 0;
  if (!ParseDescriptorType(&cursor, true, &return_type) || *cursor != '\0') {
    return false;
  }
  shorty->assign(1, return_type);
  shorty->append(arguments);
  return true;
}

}  // namespace android
