#include <optional>
#include <ostream>
#include <string>

#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "jni.h"
#include "palette/palette.h"
#include "runtime_image.h"
#include "unwindstack/AndroidUnwinder.h"

// This translation unit owns platform-compatibility shims that do not depend
// on the Android ELF graph, JNI proxy, or provider namespace. Keeping them
// separate makes a provider/JNI edit invalidate only its own object instead of
// recompiling the entire runtime adapter.

extern "C" palette_status_t PaletteSchedGetPriority(int32_t, int32_t* java_priority) {
  if (java_priority == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *java_priority = 5;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteSchedSetPriority(int32_t, int32_t) {
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteWriteCrashThreadStacks(const char*, size_t) {
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteDebugStoreGetString(char* result, size_t max_size) {
  if (result != nullptr && max_size != 0) {
    result[0] = '\0';
  }
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteTraceEnabled(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteTraceBegin(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceEnd() { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceIntegerValue(const char*, int32_t) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyDexFileLoaded(const char*) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyOatFileLoaded(const char*) {
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteShouldReportJniInvocations(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteNotifyBeginJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyEndJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}

extern "C" void* __hwasan_tag_pointer(const volatile void* pointer, unsigned char) {
  return const_cast<void*>(pointer);
}
extern "C" void __hwasan_handle_longjmp(const void*) {}

namespace art {
namespace hprof {
void DumpHeap(const char*, int, bool) {}
}  // namespace hprof

std::string RuntimeImage::GetRuntimeImagePath(const std::string&) { return {}; }

bool RuntimeImage::WriteImageToDisk(std::string* error_msg) {
  if (error_msg != nullptr) {
    *error_msg = "runtime images are not supported on Darwin";
  }
  return false;
}

namespace odrefresh {
bool UploadStatsIfAvailable(std::string*) { return true; }
}  // namespace odrefresh

std::ostream& operator<<(std::ostream& stream, const Intrinsics& intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kNone:
      return stream << "None";
#define PRINT_INTRINSIC(Name, ...) case Intrinsics::k##Name: return stream << #Name;
    ART_INTRINSICS_LIST(PRINT_INTRINSIC)
#undef PRINT_INTRINSIC
  }
  return stream << "Intrinsics[" << static_cast<int>(intrinsic) << "]";
}
}  // namespace art

extern "C" void SkipAddSignalHandler(bool) {}

namespace unwindstack {
bool AndroidLocalUnwinder::InternalInitialize(ErrorData&) {
  return false;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData&) {
  return false;
}
}  // namespace unwindstack
