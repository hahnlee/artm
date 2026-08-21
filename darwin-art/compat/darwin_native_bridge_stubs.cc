#include <cstdlib>

#include "jni.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"

// NativeBridge/NativeLoader process hooks are deliberately capability-closed
// on Darwin. They have no ELF graph or provider state; keeping these ABI stubs
// in their own object avoids recompiling the graph adapter for a platform hook
// change and makes the unsupported bridge policy explicit.

namespace android {
extern "C" {

bool LoadNativeBridge(const char* library, const NativeBridgeRuntimeCallbacks*) {
  // An empty name means ART explicitly requested no translation bridge.
  return library == nullptr || library[0] == '\0';
}

bool PreInitializeNativeBridge(const char*, const char*) { return true; }
void PreZygoteForkNativeBridge() {}
bool InitializeNativeBridge(JNIEnv*, const char*) { return true; }
bool NativeBridgeInitialized() { return false; }
uint32_t NativeBridgeGetVersion() { return 0; }
void UnloadNativeBridge() {}

void NativeLoaderFreeErrorMessage(char* message) {
  std::free(message);
}

void ResetNativeLoader() {}

}  // extern "C"
}  // namespace android
