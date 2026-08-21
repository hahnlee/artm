#include <array>
#include <atomic>
#include <cstdlib>
#include <sstream>
#include <string>

#include "darwin_provider_owners.h"
#include "darwin_runtime_adapters_internal.h"

namespace android {
namespace {

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
    library->provider_namespace = nullptr;
    library->image_registry = nullptr;
    library->dso_lifecycle = nullptr;
    if (status != 0) std::abort();
    return;
  }
  // Provider installation is only reachable after the Rust native-owner slot
  // has been created. Reaching this branch with any graph resource would mean
  // that a C++ fallback has escaped the Rust ownership boundary.
  if (library->graph != nullptr || library->provider_namespace != nullptr ||
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
