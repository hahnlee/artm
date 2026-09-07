#include <cstdlib>

#include "jni.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"

// Keep libnativebridge's process state machine source-identical to the pinned
// Android 16 implementation. Darwin's Android-ELF graph remains a lower-level
// trampoline backend; it does not replace NativeBridge lifecycle semantics.
#ifndef ABI_STRING
#define ABI_STRING "arm64"
#endif
#define NativeBridgeGetTrampoline2 AospNativeBridgeGetTrampoline2
#define NativeBridgeIsNativeBridgeFunctionPointer \
  AospNativeBridgeIsNativeBridgeFunctionPointer
namespace android {
extern "C" void* AospNativeBridgeGetTrampoline2(
    void* handle,
    const char* name,
    const char* shorty,
    uint32_t len,
    JNICallType call_type);
}
#include "../_aosp/art-native-library-control-flow/libnativebridge/native_bridge.cc"
#undef NativeBridgeIsNativeBridgeFunctionPointer
#undef NativeBridgeGetTrampoline2

namespace android {
extern "C" {

void* DarwinNativeBridgeGetTrampoline2(void* handle,
                                       const char* name,
                                       const char* shorty,
                                       uint32_t len,
                                       JNICallType call_type);
bool DarwinNativeBridgeIsNativeBridgeFunctionPointer(const void* pointer);

void* NativeBridgeGetTrampoline2(void* handle,
                                 const char* name,
                                 const char* shorty,
                                 uint32_t len,
                                 JNICallType call_type) {
  if (void* trampoline = DarwinNativeBridgeGetTrampoline2(
          handle, name, shorty, len, call_type);
      trampoline != nullptr) {
    return trampoline;
  }
  return AospNativeBridgeGetTrampoline2(handle, name, shorty, len, call_type);
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* pointer) {
  return DarwinNativeBridgeIsNativeBridgeFunctionPointer(pointer) ||
         AospNativeBridgeIsNativeBridgeFunctionPointer(pointer);
}

void NativeLoaderFreeErrorMessage(char* message) {
  std::free(message);
}

// Bionic's NativeLoader initializes linker-namespace policy here. Darwin's
// namespace graph is initialized lazily by OpenNativeLibrary, but ART's public
// JNI_CreateJavaVM entry point still owns and calls this AOSP lifecycle hook.
void InitializeNativeLoader() {}

void ResetNativeLoader() {}

}  // extern "C"
}  // namespace android
