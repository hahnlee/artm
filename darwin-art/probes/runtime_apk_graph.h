#ifndef DARWIN_ART_RUNTIME_APK_GRAPH_H_
#define DARWIN_ART_RUNTIME_APK_GRAPH_H_

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)

#include "darwin_art_elf_loader.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

DarwinArtElfStatus darwin_art_direct_discover_sibling_graph(
    int directory_fd, const uint8_t* root_component,
    size_t root_component_length, const char* const* provider_sonames,
    size_t provider_count, int* out_root_is_elf,
    DarwinArtElfDiscoveredGraph** out_graph, DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_direct_discovered_graph_root_soname(
    const DarwinArtElfDiscoveredGraph* graph, const char** out_soname,
    DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_direct_discovered_graph_sources(
    const DarwinArtElfDiscoveredGraph* graph,
    const DarwinArtElfGraphSource** out_sources, size_t* out_count,
    DarwinArtElfErrorBuffer* error);

void darwin_art_direct_discovered_graph_destroy(
    DarwinArtElfDiscoveredGraph** graph);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // defined(DARWIN_ART_DIRECT_APK_RUNTIME)

#endif  // DARWIN_ART_RUNTIME_APK_GRAPH_H_
