#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "darwin_jni_shorty.h"
#include "darwin_runtime_adapters_internal.h"
#include "jni/jni_env_ext.h"

namespace android {

int32_t ProxyRegisterNatives(void* context,
                             void* clazz,
                             const DarwinArtJniNativeMethod* methods,
                             int32_t count) {
  auto* library = static_cast<ElfLibrary*>(context);
  constexpr int32_t kMaxRegularMethodsPerGraph = 4096;
  if (library == nullptr || clazz == nullptr || methods == nullptr ||
      count <= 0 || count > kMaxRegularMethodsPerGraph) {
    std::cerr << "DARWIN JNI RegisterNatives reject: bad arguments count=" << count << "\n";
    return DARWIN_ART_JNI_ERR;
  }
  JNIEnv* art_env = CurrentArtEnv();
  if (library->proxy == nullptr || art_env == nullptr) {
    std::cerr << "DARWIN JNI RegisterNatives reject: proxy/env missing proxy="
              << library->proxy << " env=" << art_env << "\n";
    return DARWIN_ART_JNI_ERR;
  }
  constexpr std::array<std::pair<const char*, const char*>, 8> kFixtureExpected = {{
      {"nativeAdd", "(IJI)J"},
      {"nativeSpill", kDarwinArtElfJniFixtureSpillSignature},
      {"nativeUsesEnv", "()I"},
      {"nativeNarrowStack", "(IIIIIIZBCSIJLjava/lang/Object;)I"},
      {"nativeEcho", "(Ljava/lang/Object;)Ljava/lang/Object;"},
      {"nativeFloat", "(F)F"},
      {"nativeDouble", "(D)D"},
      {"nativeVoid", "()V"},
  }};
  if (library->fixture_graph && count != static_cast<int32_t>(kFixtureExpected.size())) {
    return DARWIN_ART_JNI_ERR;
  }
  for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
    if (methods[index].name == nullptr || methods[index].signature == nullptr ||
        methods[index].function == nullptr ||
        !darwin_art_image_registry::ContainsAddress(
            library->image_registry,
            reinterpret_cast<uintptr_t>(methods[index].function))) {
      return DARWIN_ART_JNI_ERR;
    }
    if (library->fixture_graph &&
        (std::strcmp(methods[index].name, kFixtureExpected[index].first) != 0 ||
         std::strcmp(methods[index].signature, kFixtureExpected[index].second) != 0)) {
      return DARWIN_ART_JNI_ERR;
    }
    jmethodID method = art_env->GetStaticMethodID(
        static_cast<jclass>(clazz), methods[index].name, methods[index].signature);
    if (method == nullptr && art_env->ExceptionCheck()) {
      art_env->ExceptionClear();
    }
    // JNI_OnLoad commonly registers a static utility class followed by an
    // instance class.  The old bridge only checked GetStaticMethodID, which
    // made the second, perfectly valid RegisterNatives call fail closed.
    if (method == nullptr) {
      method = art_env->GetMethodID(static_cast<jclass>(clazz), methods[index].name,
                                    methods[index].signature);
      if (method == nullptr && art_env->ExceptionCheck()) {
        art_env->ExceptionClear();
      }
    }
    if (method == nullptr) {
      std::cerr << "DARWIN JNI RegisterNatives reject: method missing name="
                << methods[index].name << " sig=" << methods[index].signature << "\n";
      return DARWIN_ART_JNI_ERR;
    }
  }
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfCapturedRegistration, std::memory_order_relaxed);
  }
  JavaVM* proxy_vm =
      static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy));
  void* proxy_env = nullptr;
  if (proxy_vm == nullptr || proxy_vm->GetEnv(&proxy_env, JNI_VERSION_1_6) != JNI_OK ||
      proxy_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  std::vector<std::string> shorties(static_cast<size_t>(count));
  std::vector<darwin_art::android_jni::TrampolineRequest> requests(
      static_cast<size_t>(count));
  for (size_t index = 0; index < requests.size(); ++index) {
    if (!DescriptorToShorty(methods[index].signature, &shorties[index])) {
      return DARWIN_ART_JNI_ERR;
    }
    const uint32_t entry_mask =
        library->fixture_graph ? (uint32_t{1} << index) : uint32_t{1};
    requests[index] = {
        methods[index].function, shorties[index].c_str(), entry_mask};
    std::cerr << "DARWIN JNI RegisterNatives method name="
              << methods[index].name << " sig=" << methods[index].signature
              << " target=" << methods[index].function << "\n";
  }
  std::string trampoline_error;
  auto* trampolines = darwin_art::android_jni::CreateRegularTrampolines(
      proxy_env, requests.data(), requests.size(), &trampoline_error);
  if (trampolines == nullptr) {
    std::cerr << "DARWIN JNI RegisterNatives reject: trampoline create " << trampoline_error
              << "\n";
    return DARWIN_ART_JNI_ERR;
  }
  bool all_entries_valid = true;
  for (size_t index = 0; index < requests.size(); ++index) {
    void* entry = darwin_art::android_jni::TrampolineEntry(trampolines, index);
    all_entries_valid = all_entries_valid && entry != nullptr &&
                        darwin_art::android_jni::TrampolineEntryMask(entry) ==
                            requests[index].entry_mask;
  }
  if (darwin_art::android_jni::TrampolineGeneration(trampolines) == 0 ||
      darwin_art::android_jni::TrampolineCount(trampolines) != requests.size() ||
      !all_entries_valid) {
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
    return DARWIN_ART_JNI_ERR;
  }
  if (library->fixture_graph) {
    void* native_add_entry = darwin_art::android_jni::TrampolineEntry(trampolines, 0);
    void* native_spill_entry = darwin_art::android_jni::TrampolineEntry(trampolines, 1);
    void* native_uses_env_entry = darwin_art::android_jni::TrampolineEntry(trampolines, 2);
    const auto* native_add_bytes = static_cast<const uint8_t*>(native_add_entry);
    if (!darwin_art::android_jni::IsTrampolineEntry(native_add_entry) ||
        !darwin_art::android_jni::IsTrampolineEntry(native_spill_entry) ||
        !darwin_art::android_jni::IsTrampolineEntry(native_uses_env_entry) ||
        darwin_art::android_jni::IsTrampolineEntry(native_add_bytes + 4) ||
        darwin_art::android_jni::TrampolineEntryMask(native_add_entry) !=
            kFixtureNativeAddEntryMask ||
        darwin_art::android_jni::TrampolineEntryMask(native_spill_entry) !=
            kFixtureNativeSpillEntryMask ||
        darwin_art::android_jni::TrampolineEntryMask(native_uses_env_entry) !=
            kFixtureNativeUsesEnvEntryMask ||
        darwin_art::android_jni::TrampolineEntryMask(native_add_bytes + 4) != 0) {
      darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
      return DARWIN_ART_JNI_ERR;
    }
  }
  std::vector<JNINativeMethod> bridged_methods(static_cast<size_t>(count));
  for (size_t index = 0; index < bridged_methods.size(); ++index) {
    bridged_methods[index] = {
        const_cast<char*>(methods[index].name), const_cast<char*>(methods[index].signature),
        darwin_art::android_jni::TrampolineEntry(trampolines, index)};
  }
  const jint status = art_env->RegisterNatives(
      static_cast<jclass>(clazz), bridged_methods.data(),
      static_cast<jint>(bridged_methods.size()));
  if (status != JNI_OK) {
    std::cerr << "DARWIN JNI RegisterNatives reject: ART status=" << status
              << " class=" << clazz << " count=" << count << "\n";
    jthrowable registration_failure = art_env->ExceptionOccurred();
    if (registration_failure != nullptr) art_env->ExceptionClear();
    art_env->UnregisterNatives(static_cast<jclass>(clazz));
    jthrowable rollback_failure = art_env->ExceptionOccurred();
    if (rollback_failure != nullptr) art_env->ExceptionClear();
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
    if (registration_failure != nullptr) {
      art_env->Throw(registration_failure);
      art_env->DeleteLocalRef(registration_failure);
    } else if (rollback_failure != nullptr) {
      art_env->Throw(rollback_failure);
    }
    art_env->DeleteLocalRef(rollback_failure);
    return DARWIN_ART_JNI_ERR;
  }
  {
    std::lock_guard<std::mutex> lock(library->trampoline_mutex);
    if (library->trampolines == nullptr) {
      library->trampolines = trampolines;
    }
    library->trampoline_sets.push_back(trampolines);
  }
  std::cerr << "DARWIN JNI RegisterNatives installed count=" << count
            << " sets=" << library->trampoline_sets.size() << "\n";
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfInstalledRegistration, std::memory_order_relaxed);
  }
  return DARWIN_ART_JNI_OK;
}

}  // namespace android
