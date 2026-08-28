#include "darwin_art_elf_loader.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct ErrorStorage {
  char bytes[512] = {};
  DarwinArtElfErrorBuffer value{bytes, sizeof(bytes), 0};
};

int Fail(const char* operation, DarwinArtElfStatus status,
         const ErrorStorage& error) {
  std::fprintf(stderr, "apk-native-discovery-smoke: %s: %s (%d): %s\n",
               operation, darwin_art_elf_status_name(status), status,
               error.bytes);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr,
                 "usage: apk-native-discovery-smoke DIRECTORY ROOT_SONAME\n");
    return 64;
  }
  const int directory =
      open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    std::perror("apk-native-discovery-smoke: open directory");
    return 1;
  }

  ErrorStorage error;
  DarwinArtElfDiscoveredGraph* discovered = nullptr;
  int root_is_elf = 0;
  DarwinArtElfStatus status = darwin_art_elf_discover_sibling_graph(
      directory, reinterpret_cast<const uint8_t*>(argv[2]),
      std::strlen(argv[2]), nullptr, 0, &root_is_elf, &discovered,
      &error.value);
  close(directory);
  if (status != DARWIN_ART_ELF_OK || discovered == nullptr || root_is_elf != 1) {
    darwin_art_elf_discovered_graph_destroy(&discovered);
    return Fail("discover", status, error);
  }

  const char* root = nullptr;
  const DarwinArtElfGraphSource* sources = nullptr;
  size_t count = 0;
  status = darwin_art_elf_discovered_graph_root_soname(discovered, &root,
                                                       &error.value);
  if (status != DARWIN_ART_ELF_OK || root == nullptr ||
      std::strcmp(root, argv[2]) != 0) {
    darwin_art_elf_discovered_graph_destroy(&discovered);
    return Fail("root-soname", status, error);
  }
  status = darwin_art_elf_discovered_graph_sources(discovered, &sources, &count,
                                                   &error.value);
  if (status != DARWIN_ART_ELF_OK || sources == nullptr || count != 3) {
    darwin_art_elf_discovered_graph_destroy(&discovered);
    return Fail("graph-sources", status, error);
  }

  DarwinArtElfGraphHandle* graph = nullptr;
  status = darwin_art_elf_graph_load(root, sources, count, nullptr, 0, nullptr,
                                     &graph, &error.value);
  darwin_art_elf_discovered_graph_destroy(&discovered);
  if (status != DARWIN_ART_ELF_OK || graph == nullptr) {
    darwin_art_elf_graph_unload(&graph, &error.value);
    return Fail("graph-load", status, error);
  }

  uintptr_t address = 0;
  status = darwin_art_elf_graph_lookup_root(graph, "JNI_OnLoad", &address,
                                            &error.value);
  if (status != DARWIN_ART_ELF_OK || address == 0) {
    darwin_art_elf_graph_unload(&graph, &error.value);
    return Fail("lookup JNI_OnLoad", status, error);
  }
  // This gate owns extraction, closed-namespace discovery, and symbol lookup.
  // The shared fixture's JNI_OnLoad intentionally dereferences a real
  // JavaVM/JNIEnv to test RegisterNatives, so invoking it with a sentinel VM
  // here would be undefined behavior. Managed-native-load owns that separate
  // execution contract.
  status = darwin_art_elf_graph_unload(&graph, &error.value);
  if (status != DARWIN_ART_ELF_OK || graph != nullptr) {
    return Fail("graph-unload", status, error);
  }
  std::puts(
      "apk-native-discovery-smoke: PASS extracted=APK graph=root+child+grandchild "
      "discovery=dirfd+no-follow load=closed-no-provider JNI_OnLoad=located "
      "fallback=none");
  return 0;
}
