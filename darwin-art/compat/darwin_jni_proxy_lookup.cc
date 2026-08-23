#include <cstring>
#include <iostream>
#include <string>

#include "darwin_jni_shorty.h"
#include "darwin_runtime_adapters_internal.h"

namespace android {

void* ProxyCurrentEnv(void*) { return CurrentArtEnv(); }

void* ProxyFindClass(void* context, const char* name) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || name == nullptr) {
    return nullptr;
  }
  void* clazz = nullptr;
  if (library->app_loader != nullptr) {
    std::cerr << "DARWIN JNI ProxyFindClass app-loader name=" << name << "\n";
    jclass loader_class = art_env->FindClass("java/lang/ClassLoader");
    jmethodID load_class = loader_class == nullptr
                                ? nullptr
                                : art_env->GetMethodID(
                                      loader_class, "loadClass",
                                      "(Ljava/lang/String;)Ljava/lang/Class;");
    std::string binary(name);
    for (char& ch : binary) {
      if (ch == '/') ch = '.';
    }
    jstring binary_name = art_env->NewStringUTF(binary.c_str());
    clazz = load_class == nullptr || binary_name == nullptr
                ? nullptr
                : art_env->CallObjectMethod(
                      static_cast<jobject>(library->app_loader), load_class,
                      binary_name);
    if (art_env->ExceptionCheck()) art_env->ExceptionClear();
    std::cerr << "DARWIN JNI ProxyFindClass result=" << clazz << "\n";
    if (binary_name != nullptr) art_env->DeleteLocalRef(binary_name);
    if (loader_class != nullptr) art_env->DeleteLocalRef(loader_class);
  } else {
    std::cerr << "DARWIN JNI ProxyFindClass boot name=" << name << "\n";
    clazz = art_env->FindClass(name);
  }
  if (library->fixture_graph && clazz != nullptr &&
      std::strcmp(name, "darwin/art/nativefixture/NativeFixture") == 0) {
    g_elf_fixture_status.fetch_or(kElfFoundFixtureClass, std::memory_order_relaxed);
  }
  return clazz;
}

}  // namespace android
