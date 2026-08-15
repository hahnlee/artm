#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <ostream>

#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"
#include "palette/palette.h"
#include "unwindstack/AndroidUnwinder.h"

namespace android {
extern "C" {

bool LoadNativeBridge(const char* library, const NativeBridgeRuntimeCallbacks*) {
  // An empty name means that ART explicitly requested no translation bridge.
  return library == nullptr || library[0] == '\0';
}

void UnloadNativeBridge() {}

void* NativeBridgeGetTrampoline2(void*, const char*, const char*, uint32_t, JNICallType) {
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void*) {
  return false;
}

void* OpenNativeLibrary(JNIEnv*,
                        int32_t,
                        const char* path,
                        jobject,
                        const char*,
                        jstring,
                        bool* needs_native_bridge,
                        char** error_msg) {
  if (needs_native_bridge != nullptr) {
    *needs_native_bridge = false;
  }
  void* handle = path == nullptr ? nullptr : dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr && error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library load failed" : message);
  }
  return handle;
}

bool CloseNativeLibrary(void* handle, bool, char** error_msg) {
  if (handle == nullptr || dlclose(handle) == 0) {
    return true;
  }
  if (error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library close failed" : message);
  }
  return false;
}

void NativeLoaderFreeErrorMessage(char* message) {
  std::free(message);
}

void ResetNativeLoader() {}

}  // extern "C"
}  // namespace android

extern "C" palette_status_t PaletteSchedGetPriority(int32_t, int32_t* java_priority) {
  if (java_priority == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *java_priority = 5;
  return PALETTE_STATUS_OK;
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
extern "C" palette_status_t PaletteNotifyDexFileLoaded(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteNotifyOatFileLoaded(const char*) { return PALETTE_STATUS_OK; }

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

namespace unwindstack {
bool AndroidLocalUnwinder::InternalInitialize(ErrorData&) {
  return false;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData&) {
  return false;
}
}  // namespace unwindstack
