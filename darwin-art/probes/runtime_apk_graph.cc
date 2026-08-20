#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)

#define main darwin_art_direct_apk_standalone_main
#include "../tools/android-apk-native-direct-load/direct_load.cc"
#undef main

#include "runtime_apk_graph.h"

#include <fcntl.h>
#include <limits.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct DirectApkDiscoveredGraph {
  Mapping mapping;
  std::vector<Entry> entries;
  std::vector<std::string> names;
  std::vector<DarwinArtElfGraphSource> sources;
  std::string root;
};

std::mutex g_direct_apk_graphs_mutex;
std::unordered_set<DarwinArtElfDiscoveredGraph*> g_direct_apk_graphs;

void SetDirectApkError(DarwinArtElfErrorBuffer* error,
                       const std::string& message) {
  if (error == nullptr) return;
  error->required = message.size() + 1;
  if (error->data == nullptr || error->capacity == 0) return;
  const size_t count = std::min(message.size(), error->capacity - 1);
  std::memcpy(error->data, message.data(), count);
  error->data[count] = '\0';
}

bool IsDirectApkGraph(DarwinArtElfDiscoveredGraph* graph) {
  std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
  return g_direct_apk_graphs.contains(graph);
}

DarwinArtElfStatus BuildDirectApkGraph(
    const char* apk_path, const char* root,
    const char* const* provider_sonames, size_t provider_count,
    DarwinArtElfDiscoveredGraph** out_graph, DarwinArtElfErrorBuffer* error) {
  auto graph = std::make_unique<DirectApkDiscoveredGraph>();
  std::string failure;
  if (!graph->mapping.Open(apk_path, &failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  size_t mutation_offset = std::numeric_limits<size_t>::max();
  if (!Parse(graph->mapping, &graph->entries, &mutation_offset, &failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_FORMAT;
  }
  std::unordered_map<std::string, const Entry*> native;
  for (const Entry& entry : graph->entries) {
    if (entry.native) native.emplace(entry.leaf, &entry);
  }
  if (!native.contains(root)) {
    SetDirectApkError(error, "direct APK root entry is absent");
    return DARWIN_ART_ELF_FORMAT;
  }
  std::unordered_set<std::string> providers;
  for (size_t index = 0; index < provider_count; ++index) {
    if (provider_sonames[index] != nullptr) {
      providers.emplace(provider_sonames[index]);
    }
  }
  std::deque<std::string> queue{root};
  std::unordered_set<std::string> queued{root};
  while (!queue.empty()) {
    std::string name = std::move(queue.front());
    queue.pop_front();
    const Entry& entry = *native.at(name);
    char storage[512] = {};
    DarwinArtElfErrorBuffer inspect_error{storage, sizeof(storage), 0};
    DarwinArtElfInspection* inspection = nullptr;
    DarwinArtElfStatus status = darwin_art_elf_inspect_bytes(
        graph->mapping.data() + entry.data_offset, entry.uncompressed_size,
        &inspection, &inspect_error);
    if (status != DARWIN_ART_ELF_OK || inspection == nullptr) {
      SetDirectApkError(error, std::string("direct APK ELF inspection failed: ") + storage);
      return status;
    }
    const char* soname = nullptr;
    size_t needed_count = 0;
    status = darwin_art_elf_inspection_soname(inspection, &soname, &inspect_error);
    if (status != DARWIN_ART_ELF_OK || soname == nullptr || name != soname) {
      darwin_art_elf_inspection_destroy(&inspection);
      SetDirectApkError(error, "direct APK leaf and DT_SONAME differ");
      return DARWIN_ART_ELF_FORMAT;
    }
    status = darwin_art_elf_inspection_needed_count(
        inspection, &needed_count, &inspect_error);
    if (status != DARWIN_ART_ELF_OK) {
      darwin_art_elf_inspection_destroy(&inspection);
      SetDirectApkError(error, "direct APK DT_NEEDED count failed");
      return status;
    }
    for (size_t index = 0; index < needed_count; ++index) {
      const char* needed = nullptr;
      status = darwin_art_elf_inspection_needed_at(
          inspection, index, &needed, &inspect_error);
      if (status != DARWIN_ART_ELF_OK || needed == nullptr) {
        darwin_art_elf_inspection_destroy(&inspection);
        SetDirectApkError(error, "direct APK DT_NEEDED inspection failed");
        return status;
      }
      if (providers.contains(needed)) continue;
      if (!native.contains(needed)) {
        darwin_art_elf_inspection_destroy(&inspection);
        SetDirectApkError(error, "direct APK DT_NEEDED escaped the closed namespace");
        return DARWIN_ART_ELF_UNRESOLVED_SYMBOL;
      }
      if (queued.insert(needed).second) queue.emplace_back(needed);
    }
    darwin_art_elf_inspection_destroy(&inspection);
    graph->names.push_back(std::move(name));
  }
  graph->root = root;
  graph->sources.reserve(graph->names.size());
  for (const std::string& name : graph->names) {
    const Entry& entry = *native.at(name);
    graph->sources.push_back(DarwinArtElfGraphSource{
        name.c_str(), graph->mapping.data() + entry.data_offset,
        entry.uncompressed_size});
  }
  if (!graph->mapping.Unchanged(&failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  auto* published = reinterpret_cast<DarwinArtElfDiscoveredGraph*>(graph.release());
  {
    std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
    g_direct_apk_graphs.insert(published);
  }
  *out_graph = published;
  return DARWIN_ART_ELF_OK;
}

}  // namespace

extern "C" DarwinArtElfStatus darwin_art_direct_discover_sibling_graph(
    int directory_fd, const uint8_t* root_component,
    size_t root_component_length, const char* const* provider_sonames,
    size_t provider_count, int* out_root_is_elf,
    DarwinArtElfDiscoveredGraph** out_graph, DarwinArtElfErrorBuffer* error) {
  const char* expected_apk = std::getenv("DARWIN_ART_DIRECT_APK_FIXTURE");
  const char* root = std::getenv("DARWIN_ART_DIRECT_APK_ROOT");
  if (expected_apk == nullptr || root == nullptr || root_component == nullptr ||
      root_component_length == 0 || out_graph == nullptr) {
    return darwin_art_elf_discover_sibling_graph(
        directory_fd, root_component, root_component_length, provider_sonames,
        provider_count, out_root_is_elf, out_graph, error);
  }
  char directory_path[PATH_MAX] = {};
  if (fcntl(directory_fd, F_GETPATH, directory_path) != 0) {
    SetDirectApkError(error, "direct APK directory F_GETPATH failed");
    return DARWIN_ART_ELF_IO;
  }
  std::string apk_path(directory_path);
  if (!apk_path.ends_with('/')) apk_path.push_back('/');
  apk_path.append(reinterpret_cast<const char*>(root_component),
                  root_component_length);
  if (apk_path != expected_apk) {
    return darwin_art_elf_discover_sibling_graph(
        directory_fd, root_component, root_component_length, provider_sonames,
        provider_count, out_root_is_elf, out_graph, error);
  }
  if (out_root_is_elf != nullptr) *out_root_is_elf = 1;
  return BuildDirectApkGraph(apk_path.c_str(), root, provider_sonames,
                             provider_count, out_graph, error);
}

extern "C" DarwinArtElfStatus darwin_art_direct_discovered_graph_root_soname(
    const DarwinArtElfDiscoveredGraph* graph, const char** out_soname,
    DarwinArtElfErrorBuffer* error) {
  auto* mutable_graph = const_cast<DarwinArtElfDiscoveredGraph*>(graph);
  if (!IsDirectApkGraph(mutable_graph)) {
    return darwin_art_elf_discovered_graph_root_soname(graph, out_soname, error);
  }
  if (out_soname == nullptr) return DARWIN_ART_ELF_INVALID_ARGUMENT;
  *out_soname = reinterpret_cast<const DirectApkDiscoveredGraph*>(graph)->root.c_str();
  return DARWIN_ART_ELF_OK;
}

extern "C" DarwinArtElfStatus darwin_art_direct_discovered_graph_sources(
    const DarwinArtElfDiscoveredGraph* graph,
    const DarwinArtElfGraphSource** out_sources, size_t* out_count,
    DarwinArtElfErrorBuffer* error) {
  auto* mutable_graph = const_cast<DarwinArtElfDiscoveredGraph*>(graph);
  if (!IsDirectApkGraph(mutable_graph)) {
    return darwin_art_elf_discovered_graph_sources(graph, out_sources, out_count,
                                                   error);
  }
  if (out_sources == nullptr || out_count == nullptr) {
    return DARWIN_ART_ELF_INVALID_ARGUMENT;
  }
  const auto* direct = reinterpret_cast<const DirectApkDiscoveredGraph*>(graph);
  std::string failure;
  if (!direct->mapping.Unchanged(&failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  *out_sources = direct->sources.data();
  *out_count = direct->sources.size();
  return DARWIN_ART_ELF_OK;
}

extern "C" void darwin_art_direct_discovered_graph_destroy(
    DarwinArtElfDiscoveredGraph** graph) {
  if (graph == nullptr || *graph == nullptr) return;
  bool direct = false;
  {
    std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
    direct = g_direct_apk_graphs.erase(*graph) == 1;
  }
  if (direct) {
    delete reinterpret_cast<DirectApkDiscoveredGraph*>(*graph);
    *graph = nullptr;
  } else {
    darwin_art_elf_discovered_graph_destroy(graph);
  }
}

#endif  // defined(DARWIN_ART_DIRECT_APK_RUNTIME)
