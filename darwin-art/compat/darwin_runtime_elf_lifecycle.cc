#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "darwin_provider_owners.h"
#include "darwin_runtime_adapters_internal.h"

namespace android {
namespace {

std::mutex& CachedElfMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<std::string, DarwinArtElfGraphHandle*>& CachedElfGraphs() {
  // Android app native namespaces normally live for the ClassLoader/process
  // lifetime. Retained graph handles deliberately follow that lifetime so a
  // later dlopen can bind against the exact earlier DSO mapping and state.
  static auto* graphs =
      new std::unordered_map<std::string, DarwinArtElfGraphHandle*>();
  return *graphs;
}

void SetCacheError(DarwinArtElfErrorBuffer* error, const std::string& message) {
  if (error == nullptr) return;
  error->required = message.size() + 1;
  if (error->data == nullptr || error->capacity == 0) return;
  const size_t copied = std::min(message.size(), error->capacity - 1);
  std::memcpy(error->data, message.data(), copied);
  error->data[copied] = '\0';
}

std::string ElfError(DarwinArtElfStatus status,
                     const DarwinArtElfErrorBuffer& error) {
  std::ostringstream stream;
  stream << darwin_art_elf_status_name(status);
  if (error.data != nullptr && error.data[0] != '\0') {
    stream << ": " << error.data;
  }
  return stream.str();
}

}  // namespace

std::vector<std::string> SnapshotCachedElfSonames() {
  std::lock_guard<std::mutex> lock(CachedElfMutex());
  std::vector<std::string> result;
  result.reserve(CachedElfGraphs().size());
  for (const auto& [soname, graph] : CachedElfGraphs()) {
    if (graph != nullptr) result.push_back(soname);
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool RegisterCachedElfGraph(const char* root_soname,
                            DarwinArtElfGraphHandle* graph,
                            std::string* error) {
  if (root_soname == nullptr || root_soname[0] == '\0' || graph == nullptr) {
    if (error != nullptr) *error = "invalid ELF namespace cache registration";
    return false;
  }
  std::lock_guard<std::mutex> lock(CachedElfMutex());
  if (CachedElfGraphs().contains(root_soname)) return true;
  DarwinArtElfGraphHandle* retained = nullptr;
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer buffer{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status =
      darwin_art_elf_graph_clone(graph, &retained, &buffer);
  if (status != DARWIN_ART_ELF_OK || retained == nullptr) {
    if (error != nullptr) *error = ElfError(status, buffer);
    return false;
  }
  CachedElfGraphs().emplace(root_soname, retained);
  std::fprintf(stderr, "DARWIN ELF namespace: retained root=%s\n", root_soname);
  return true;
}

DarwinArtElfResolveStatus ResolveCachedElfProvider(
    const DarwinArtElfSymbolRequest* request,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error) {
  if (request == nullptr || request->symbol == nullptr || out_address == nullptr) {
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  std::lock_guard<std::mutex> lock(CachedElfMutex());
  auto lookup = [&](const char* soname) -> DarwinArtElfResolveStatus {
    const auto found = soname == nullptr ? CachedElfGraphs().end()
                                         : CachedElfGraphs().find(soname);
    if (found == CachedElfGraphs().end()) return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
    std::array<char, 1024> storage{};
    DarwinArtElfErrorBuffer buffer{storage.data(), storage.size(), 0};
    const DarwinArtElfStatus status = darwin_art_elf_graph_lookup_root_symbol(
        found->second, request->symbol, out_address, &buffer);
    if (status == DARWIN_ART_ELF_OK) return DARWIN_ART_ELF_RESOLVE_FOUND;
    if (status == DARWIN_ART_ELF_SYMBOL_NOT_FOUND) {
      return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
    }
    SetCacheError(error, ElfError(status, buffer));
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  };
  if (request->version_soname != nullptr) return lookup(request->version_soname);
  for (size_t index = 0; index < request->needed_library_count; ++index) {
    const DarwinArtElfResolveStatus status =
        lookup(request->needed_libraries[index]);
    if (status != DARWIN_ART_ELF_RESOLVE_NOT_FOUND) return status;
  }
  return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
}

void DestroyRuntimeElfTrampolines(ElfLibrary* library) {
  if (library == nullptr) return;
  std::lock_guard<std::mutex> lock(library->trampoline_mutex);
  for (auto* trampolines : library->trampoline_sets) {
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
  }
  library->trampoline_sets.clear();
  library->exported_jni_trampolines.clear();
  library->trampolines = nullptr;
}

int PublishRuntimeElfImage(void* context, uintptr_t start, uintptr_t end) {
  ElfLibrary* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || library->dso_lifecycle == nullptr ||
      library->image_registry == nullptr ||
      darwin_art_image_registry::Publish(library->image_registry, start, end) !=
          0) {
    return -1;
  }
  if (darwin_art_bionic_dso_lifecycle_publish_image(
          library->dso_lifecycle, start, end) == 0) {
    std::fprintf(stderr, "DARWIN ELF loader: published image=[0x%llx,0x%llx)\n",
                 static_cast<unsigned long long>(start),
                 static_cast<unsigned long long>(end));
    return 0;
  }
  if (darwin_art_image_registry::RollbackPublish(library->image_registry, start,
                                                  end) != 0) {
    std::abort();
  }
  return -1;
}

int FinalizeRuntimeElfImage(void* context, uintptr_t start, uintptr_t end) {
  ElfLibrary* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || library->dso_lifecycle == nullptr ||
      library->image_registry == nullptr ||
      darwin_art_bionic_dso_lifecycle_finalize_image(
          library->dso_lifecycle, start, end) != 0) {
    return -1;
  }
  return darwin_art_image_registry::Finalize(library->image_registry, start,
                                              end);
}

int DropRuntimeElfGraph(void* value, void* context) {
  auto* graph = static_cast<DarwinArtElfGraphHandle*>(value);
  auto* library = static_cast<ElfLibrary*>(context);
  if (graph == nullptr) return -1;
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer error{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status = darwin_art_elf_graph_unload(&graph, &error);
  if (status == DARWIN_ART_ELF_OK && library != nullptr) {
    library->graph = nullptr;
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(4, std::memory_order_relaxed);
    }
  }
  return status == DARWIN_ART_ELF_OK ? 0 : -1;
}

int DropRuntimeAndroidUnwindProvider(void* value, void* context) {
  auto* handle = static_cast<DarwinArtElfHandle*>(value);
  auto* library = static_cast<ElfLibrary*>(context);
  if (handle == nullptr) return -1;
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer error{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status = darwin_art_elf_unload(&handle, &error);
  if (status == DARWIN_ART_ELF_OK && library != nullptr) {
    library->android_unwind_provider = nullptr;
  }
  return status == DARWIN_ART_ELF_OK ? 0 : -1;
}

int DropRuntimeElfLibrary(void* value, void*) {
  auto* library = static_cast<ElfLibrary*>(value);
  if (library == nullptr || library->magic != kElfLibraryMagic) return -1;
  if (library->app_loader != nullptr) {
    if (JNIEnv* env = static_cast<JNIEnv*>(ProxyCurrentEnv(library))) {
      env->DeleteGlobalRef(static_cast<jobject>(library->app_loader));
    }
    library->app_loader = nullptr;
  }
  DestroyRuntimeElfTrampolines(library);
  TeardownProviderNamespace(library);
  for (void** handle : {&library->gles_provider, &library->egl_provider,
                        &library->z_provider}) {
    if (*handle != nullptr) {
      dlclose(*handle);
      *handle = nullptr;
    }
  }
  library->magic = 0;
  delete library;
  return 0;
}

int DropRuntimeElfImageRegistry(void* value, void*) {
  if (value == nullptr) return -1;
  darwin_art_image_registry::Destroy(
      static_cast<darwin_art_image_registry::Owner*>(value));
  return 0;
}

int DropRuntimeDsoLifecycle(void* value, void*) {
  if (value == nullptr) return -1;
  darwin_art_bionic_dso_lifecycle_owner_destroy(
      static_cast<DarwinArtBionicDsoLifecycleOwner*>(value));
  return 0;
}

int DropRuntimeProviderNamespace(void* value, void* context) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (value == nullptr) return -1;
  const auto status = darwin_art_bionic_namespace_teardown(
      static_cast<DarwinArtBionicNamespace*>(value));
  if (status != DARWIN_ART_BIONIC_NAMESPACE_OK) return -1;
  if (library != nullptr && library->fixture_graph) {
    g_elf_fixture_namespace_lifecycle.store(5, std::memory_order_relaxed);
  }
  darwin_art_bionic_namespace_destroy(
      static_cast<DarwinArtBionicNamespace*>(value));
  return 0;
}

int DropRuntimeProviderKind(void* value, void*) {
  const auto kind = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
  switch (kind) {
    case static_cast<uint32_t>(darwin_art::providers::Kind::Filesystem):
      darwin_art::providers::release_filesystem();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Network):
      darwin_art::providers::release_network();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Stdio):
      darwin_art::providers::release_stdio();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Ioctl):
      darwin_art::providers::release_ioctl();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Strftime):
      darwin_art::providers::release_strftime();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Sendfile):
      darwin_art::providers::release_sendfile();
      return 0;
    case static_cast<uint32_t>(darwin_art::providers::Kind::Vm):
      darwin_art::providers::release_vm();
      return 0;
    default:
      return -1;
  }
}

void TeardownProviderNamespace(ElfLibrary* library) {
  if (library == nullptr) return;
  if (library->native_owner != nullptr) {
    const int status = darwin_art_runtime_native_owner_destroy(library->native_owner);
    library->native_owner = nullptr;
    library->graph = nullptr;
    library->android_unwind_provider = nullptr;
    library->provider_namespace = nullptr;
    library->image_registry = nullptr;
    library->dso_lifecycle = nullptr;
    if (status != 0) std::abort();
    return;
  }
  // Provider installation is only reachable after the Rust native-owner slot
  // has been created. Reaching this branch with any graph resource would mean
  // that a C++ fallback has escaped the Rust ownership boundary.
  if (library->graph != nullptr || library->android_unwind_provider != nullptr ||
      library->provider_namespace != nullptr ||
      library->image_registry != nullptr || library->dso_lifecycle != nullptr) {
    std::abort();
  }
}

bool LookupOptionalElfSymbol(ElfLibrary* library,
                             const char* name,
                             uintptr_t* address,
                             std::string* error) {
  if (library == nullptr || library->graph == nullptr || name == nullptr ||
      address == nullptr || error == nullptr) {
    return false;
  }
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer error_buffer{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status =
      darwin_art_elf_graph_lookup_root(library->graph, name, address, &error_buffer);
  if (status == DARWIN_ART_ELF_OK) return true;
  if (status == DARWIN_ART_ELF_SYMBOL_NOT_FOUND) {
    *address = 0;
    return true;
  }
  *error = ElfError(status, error_buffer);
  return false;
}

}  // namespace android
