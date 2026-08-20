#include "runtime_elf_probe.h"

#include <cstdint>

namespace android {
enum JNICallType {
  kJNICallTypeRegular = 1,
};
extern "C" void* NativeBridgeGetTrampoline2(void* handle, const char* name,
                                             const char* shorty, uint32_t len,
                                             JNICallType jni_call_type);
extern "C" void* OpenNativeLibrary(JNIEnv* env, int32_t target_sdk_version,
                                    const char* path, jobject class_loader,
                                    const char* caller_location,
                                    jstring library_path,
                                    bool* needs_native_bridge,
                                    char** error_msg);
extern "C" bool CloseNativeLibrary(void* handle, bool needs_native_bridge,
                                    char** error_msg);
extern "C" void NativeLoaderFreeErrorMessage(char* message);
}  // namespace android

namespace darwin_art_elf_probe {

bool run_android_elf_self_test(JNIEnv* env, JavaVM* vm, jobject class_loader,
                               const char* path, std::string* error) {
  bool needs_native_bridge = false;
  char* open_error = nullptr;
  void* handle = android::OpenNativeLibrary(
      env, 35, path, class_loader, nullptr, nullptr, &needs_native_bridge,
      &open_error);
  if (handle == nullptr || open_error != nullptr || !needs_native_bridge) {
    *error = open_error == nullptr ? "Android ELF open failed without detail"
                                   : open_error;
    android::NativeLoaderFreeErrorMessage(open_error);
    if (handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(handle, needs_native_bridge,
                                        &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    return false;
  }
  android::NativeLoaderFreeErrorMessage(open_error);

  void* entry = android::NativeBridgeGetTrampoline2(
      handle, "JNI_OnLoad", nullptr, 0, android::kJNICallTypeRegular);
  using JniOnLoad = jint (*)(JavaVM*, void*);
  const jint version = entry == nullptr
                           ? JNI_ERR
                           : reinterpret_cast<JniOnLoad>(entry)(vm, nullptr);

  char* close_error = nullptr;
  const bool closed =
      android::CloseNativeLibrary(handle, needs_native_bridge, &close_error);
  if (version != JNI_VERSION_1_6 || !closed || close_error != nullptr) {
    *error = close_error == nullptr
                 ? "self-testing JNI_OnLoad returned " +
                       std::to_string(static_cast<int>(version))
                 : close_error;
    android::NativeLoaderFreeErrorMessage(close_error);
    return false;
  }
  android::NativeLoaderFreeErrorMessage(close_error);
  return true;
}

}  // namespace darwin_art_elf_probe
