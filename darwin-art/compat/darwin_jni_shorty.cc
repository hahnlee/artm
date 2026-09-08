#include <cstring>
#include <cstdint>
#include <string>

#include "darwin_jni_shorty.h"
#include "jni/jni_env_ext.h"
#include "thread.h"

namespace android {

JNIEnv* CurrentArtEnv() {
  art::Thread* self = art::Thread::Current();
  return self == nullptr ? nullptr : static_cast<JNIEnv*>(self->GetJniEnv());
}

bool CurrentGenericJniFrame(uint64_t* managed_sp) {
  if (managed_sp == nullptr) return false;
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) return false;
  const art::ManagedStack* stack = self->GetManagedStack();
  // The callback may be reached through a Darwin native-registration bridge
  // that does not publish the usual quick-frame registry. The managed-stack
  // node is still authoritative at this boundary; inspect its untagged frame
  // pointer directly and let the unwind consumer validate the frame payload.
  if (stack == nullptr) return false;
  *managed_sp = reinterpret_cast<uint64_t>(stack->GetTopQuickFrameKnownNotTagged());
  return *managed_sp != 0;
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
