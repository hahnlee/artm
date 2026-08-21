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

void TeardownProviderNamespace(ElfLibrary* library) {
  if (library == nullptr) return;
  if (library->provider_namespace != nullptr) {
    const DarwinArtBionicNamespaceStatus status =
        darwin_art_bionic_namespace_teardown(library->provider_namespace);
    if (status != DARWIN_ART_BIONIC_NAMESPACE_OK) std::abort();
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(5, std::memory_order_relaxed);
    }
    darwin_art_bionic_namespace_destroy(library->provider_namespace);
    library->provider_namespace = nullptr;
  }
  if (library->image_registry != nullptr) {
    darwin_art_image_registry::Destroy(library->image_registry);
    library->image_registry = nullptr;
  }
  if (library->dso_lifecycle != nullptr) {
    darwin_art_bionic_dso_lifecycle_owner_destroy(library->dso_lifecycle);
    library->dso_lifecycle = nullptr;
  }
  if (library->network_owner) {
    darwin_art::providers::release_network();
    library->network_owner = false;
  }
  if (library->strftime_owner) {
    darwin_art::providers::release_strftime();
    library->strftime_owner = false;
  }
  if (library->sendfile_owner) {
    darwin_art::providers::release_sendfile();
    library->sendfile_owner = false;
  }
  if (library->ioctl_owner) {
    darwin_art::providers::release_ioctl();
    library->ioctl_owner = false;
  }
  if (library->stdio_owner) {
    darwin_art::providers::release_stdio();
    library->stdio_owner = false;
  }
  if (library->filesystem_owner) {
    darwin_art::providers::release_filesystem();
    library->filesystem_owner = false;
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
