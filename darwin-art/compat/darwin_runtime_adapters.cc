#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "jni/jni_env_ext.h"
#include "hprof/hprof.h"
#include "darwin_provider_owners.h"
#include "darwin_jni_shorty.h"
#include "darwin_runtime_adapters_internal.h"
#include "darwin_art_bionic_builtin_adapters.h"
#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_ioctl.h"
#include "darwin_art_bionic_sendfile.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_art_bionic_stdio.h"
#include "darwin_art_bionic_strftime.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"
#include "odr_statslog/odr_statslog.h"

extern "C" DarwinArtBionicSendfileTransferStatus
darwin_art_bionic_fs_sendfile_transfer(
    void* context, const DarwinArtBionicSendfileRequest* request,
    DarwinArtBionicSendfileResult* result);

namespace android {

std::atomic<int> g_elf_fixture_status{0};
std::atomic<uint32_t> g_elf_classified_trampoline_mask{0};
std::atomic<int> g_elf_fixture_lifecycle{0};
std::atomic<uint32_t> g_elf_fixture_provider_routes{0};
std::atomic<int> g_elf_fixture_namespace_lifecycle{0};

namespace {

int32_t ProxyRegisterNatives(void *context, void *clazz,
                             const DarwinArtJniNativeMethod *methods,
                             int32_t count) {
  auto *library = static_cast<ElfLibrary *>(context);
  constexpr int32_t kMaxRegularMethodsPerGraph = 32;
  if (library == nullptr || clazz == nullptr || methods == nullptr ||
      count <= 0 || count > kMaxRegularMethodsPerGraph) {
    return DARWIN_ART_JNI_ERR;
  }
  JNIEnv *art_env = CurrentArtEnv();
  if (library->trampolines != nullptr || library->proxy == nullptr ||
      art_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  constexpr std::array<std::pair<const char *, const char *>, 8>
      kFixtureExpected = {{
          {"nativeAdd", "(IJI)J"},
          {"nativeSpill", kDarwinArtElfJniFixtureSpillSignature},
          {"nativeUsesEnv", "()I"},
          {"nativeNarrowStack", "(IIIIIIZBCSIJLjava/lang/Object;)I"},
          {"nativeEcho", "(Ljava/lang/Object;)Ljava/lang/Object;"},
          {"nativeFloat", "(F)F"},
          {"nativeDouble", "(D)D"},
          {"nativeVoid", "()V"},
      }};
  if (library->fixture_graph &&
      count != static_cast<int32_t>(kFixtureExpected.size())) {
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
         std::strcmp(methods[index].signature,
                     kFixtureExpected[index].second) != 0)) {
      return DARWIN_ART_JNI_ERR;
    }
    if (art_env->GetStaticMethodID(static_cast<jclass>(clazz),
                                   methods[index].name,
                                   methods[index].signature) == nullptr) {
      return DARWIN_ART_JNI_ERR;
    }
  }
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfCapturedRegistration,
                                  std::memory_order_relaxed);
  }
  JavaVM *proxy_vm =
      static_cast<JavaVM *>(darwin_art_jni_proxy_java_vm(library->proxy));
  void *proxy_env = nullptr;
  if (proxy_vm == nullptr ||
      proxy_vm->GetEnv(&proxy_env, JNI_VERSION_1_6) != JNI_OK ||
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
    requests[index] = {methods[index].function, shorties[index].c_str(),
                       uint32_t{1} << index};
  }
  std::string trampoline_error;
  auto *trampolines = darwin_art::android_jni::CreateRegularTrampolines(
      proxy_env, requests.data(), requests.size(), &trampoline_error);
  if (trampolines == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  bool all_entries_valid = true;
  for (size_t index = 0; index < requests.size(); ++index) {
    void *entry = darwin_art::android_jni::TrampolineEntry(trampolines, index);
    all_entries_valid = all_entries_valid && entry != nullptr &&
                        darwin_art::android_jni::TrampolineEntryMask(entry) ==
                            (uint32_t{1} << index);
  }
  if (darwin_art::android_jni::TrampolineGeneration(trampolines) == 0 ||
      darwin_art::android_jni::TrampolineCount(trampolines) !=
          requests.size() ||
      !all_entries_valid) {
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
    return DARWIN_ART_JNI_ERR;
  }
  if (library->fixture_graph) {
    void *native_add_entry =
        darwin_art::android_jni::TrampolineEntry(trampolines, 0);
    void *native_spill_entry =
        darwin_art::android_jni::TrampolineEntry(trampolines, 1);
    void *native_uses_env_entry =
        darwin_art::android_jni::TrampolineEntry(trampolines, 2);
    const auto *native_add_bytes =
        static_cast<const uint8_t *>(native_add_entry);
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
        darwin_art::android_jni::TrampolineEntryMask(native_add_bytes + 4) !=
            0) {
      darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
      return DARWIN_ART_JNI_ERR;
    }
  }
  std::vector<JNINativeMethod> bridged_methods(static_cast<size_t>(count));
  for (size_t index = 0; index < bridged_methods.size(); ++index) {
    bridged_methods[index] = {
        const_cast<char *>(methods[index].name),
        const_cast<char *>(methods[index].signature),
        darwin_art::android_jni::TrampolineEntry(trampolines, index)};
  }
  const jint status = art_env->RegisterNatives(
      static_cast<jclass>(clazz), bridged_methods.data(),
      static_cast<jint>(bridged_methods.size()));
  if (status != JNI_OK) {
    jthrowable registration_failure = art_env->ExceptionOccurred();
    if (registration_failure != nullptr) {
      art_env->ExceptionClear();
    }
    art_env->UnregisterNatives(static_cast<jclass>(clazz));
    jthrowable rollback_failure = art_env->ExceptionOccurred();
    if (rollback_failure != nullptr) {
      art_env->ExceptionClear();
    }
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
  library->trampolines = trampolines;
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfInstalledRegistration,
                                  std::memory_order_relaxed);
  }
  return DARWIN_ART_JNI_OK;
}

thread_local ElfLibrary* g_pending_on_load = nullptr;
thread_local ElfLibrary* g_pending_on_unload = nullptr;

jint ElfJniOnLoadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_load, nullptr);
  if (library == nullptr || library->jni_on_load == 0 || library->proxy == nullptr) {
    return JNI_ERR;
  }
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfOnLoadCalled, std::memory_order_relaxed);
  }
  using AndroidJniOnLoad = jint (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnLoad>(library->jni_on_load);
  // JNI_OnLoad itself has only two register arguments, so its Android and
  // Darwin AArch64 calling sequences coincide. The ELF library sees only the
  // closed proxy VM and never ART's real JavaVM function table.
  const jint result =
      function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
  return result;
}

void ElfJniOnUnloadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_unload, nullptr);
  if (library == nullptr || library->jni_on_unload == 0 || library->proxy == nullptr) {
    return;
  }
  using AndroidJniOnUnload = void (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnUnload>(library->jni_on_unload);
  function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
}

}  // namespace

extern "C" {

void* NativeBridgeGetTrampoline2(void* handle,
                                 const char* name,
                                 const char*,
                                 uint32_t,
                                 JNICallType) {
  ElfLibrary* library = AsElfLibrary(handle);
  if (library == nullptr || name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "JNI_OnLoad") == 0 && library->jni_on_load != 0) {
    if (g_pending_on_load != nullptr) {
      return nullptr;
    }
    g_pending_on_load = library;
    return reinterpret_cast<void*>(&ElfJniOnLoadTrampoline);
  }
  if (std::strcmp(name, "JNI_OnUnload") == 0 && library->jni_on_unload != 0) {
    if (g_pending_on_unload != nullptr) {
      return nullptr;
    }
    g_pending_on_unload = library;
    return reinterpret_cast<void*>(&ElfJniOnUnloadTrampoline);
  }
  // Ordinary JNI methods need a NativeBridgeGetTrampoline2 per-shorty PCS
  // repacker. Returning null is an explicit capability failure, never a raw
  // Android function pointer or Darwin global-symbol fallback.
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* pointer) {
  // The trampoline allocator owns the process-wide exact-entry classifier.
  // The mask below is only fixture acceptance instrumentation; generic graph
  // entries are classified by the same registry without contributing to it.
  const uint32_t entry_mask =
      darwin_art::android_jni::TrampolineEntryMask(pointer);
  const uint32_t observed =
      g_elf_classified_trampoline_mask.fetch_or(entry_mask,
                                                std::memory_order_relaxed) |
      entry_mask;
  if (observed == kFixtureAllEntryMask) {
    g_elf_fixture_status.fetch_or(kElfClassifiedTrampolines,
                                  std::memory_order_relaxed);
  }
  return entry_mask != 0;
}

}  // extern "C"
}  // namespace android

extern "C" int darwin_art_elf_jni_fixture_registration_status() {
  return android::g_elf_fixture_status.load(std::memory_order_relaxed);
}

extern "C" int darwin_art_elf_jni_fixture_lifecycle_status() {
  return android::g_elf_fixture_lifecycle.load(std::memory_order_relaxed);
}

extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status() {
  return android::g_elf_fixture_namespace_lifecycle.load(
      std::memory_order_relaxed);
}
