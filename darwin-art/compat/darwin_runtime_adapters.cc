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
