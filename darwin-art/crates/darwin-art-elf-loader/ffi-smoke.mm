#include "darwin_art_elf_loader.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {

static_assert(sizeof(DarwinArtElfStatus) == sizeof(int32_t));
static_assert(sizeof(DarwinArtElfErrorBuffer) == 24);
static_assert(sizeof(DarwinArtElfLoadOptions) == 24);
static_assert(sizeof(DarwinArtElfSymbolRequest) == 56);
static_assert(offsetof(DarwinArtElfSymbolRequest, needed_libraries) == 40);
static_assert(sizeof(DarwinArtElfGraphSource) == 24);

int g_provider_data = 11;

extern "C" int provider_value() { return 77; }

struct ResolverState {
  int requests = 0;
};

void SetCallbackError(DarwinArtElfErrorBuffer* error, const char* message) {
  if (error == nullptr) return;
  error->required = std::strlen(message) + 1;
  if (error->capacity == 0 || error->data == nullptr) return;
  const size_t count = error->required <= error->capacity
                           ? error->required - 1
                           : error->capacity - 1;
  std::memcpy(error->data, message, count);
  error->data[count] = '\0';
}

DarwinArtElfResolveStatus Resolve(void* context,
                                 const DarwinArtElfSymbolRequest* request,
                                 uintptr_t* out_address,
                                 DarwinArtElfErrorBuffer* error) {
  auto* state = static_cast<ResolverState*>(context);
  if (state == nullptr || request == nullptr || out_address == nullptr ||
      request->abi_version != DARWIN_ART_ELF_ABI_VERSION) {
    SetCallbackError(error, "bad resolver arguments");
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  ++state->requests;
  if (request->needed_library_count != 1 ||
      std::strcmp(request->needed_libraries[0],
                  "libdarwin_art_provider.so") != 0 ||
      request->version_soname == nullptr || request->version_name == nullptr ||
      std::strcmp(request->version_soname,
                  "libdarwin_art_provider.so") != 0 ||
      std::strcmp(request->version_name, "DARWIN_ART_1") != 0 ||
      request->version_flags != 0 || request->version_hidden != 0) {
    SetCallbackError(error, "dependency/version contract mismatch");
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  if (std::strcmp(request->symbol, "provider_value") == 0) {
    *out_address = reinterpret_cast<uintptr_t>(&provider_value);
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }
  if (std::strcmp(request->symbol, "provider_data") == 0) {
    *out_address = reinterpret_cast<uintptr_t>(&g_provider_data);
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }
  return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
}

struct ErrorStorage {
  char bytes[512] = {};
  DarwinArtElfErrorBuffer value{bytes, sizeof(bytes), 0};

  void Reset() {
    bytes[0] = '\0';
    value.required = 0;
  }
};

int Fail(const char* operation, DarwinArtElfStatus status,
         const ErrorStorage& error) {
  std::fprintf(stderr, "%s: %s (%d): %s [required=%zu]\n", operation,
               darwin_art_elf_status_name(status), status, error.bytes,
               error.value.required);
  return 1;
}

std::vector<uint8_t> Read(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6 || darwin_art_elf_abi_version() != DARWIN_ART_ELF_ABI_VERSION) {
    std::fprintf(stderr,
                 "usage: ffi-smoke POSITIVE.so IMPORT.so PARENT.so DEP_A.so DEP_B.so\n");
    return 2;
  }

  ErrorStorage error;
  ResolverState resolver_state;
  DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION, Resolve,
                                  &resolver_state};
  DarwinArtElfHandle* imported = nullptr;
  DarwinArtElfStatus status =
      darwin_art_elf_load_path(argv[2], &options, &imported, &error.value);
  if (status != DARWIN_ART_ELF_OK) return Fail("load_path(import)", status, error);
  if (resolver_state.requests != 2) {
    std::fprintf(stderr, "resolver call/cache count mismatch\n");
    return 1;
  }
  status = darwin_art_elf_run_initializers(imported, &error.value);
  if (status != DARWIN_ART_ELF_OK) return Fail("init(import)", status, error);
  uintptr_t imported_address = 0;
  status = darwin_art_elf_lookup(imported, "imported_value", &imported_address,
                                 &error.value);
  if (status != DARWIN_ART_ELF_OK)
    return Fail("lookup(imported_value)", status, error);
  auto imported_value = reinterpret_cast<int (*)(void)>(imported_address);
  if (imported_value() != 165) {
    std::fprintf(stderr, "versioned provider execution mismatch\n");
    return 1;
  }

  DarwinArtElfStatus other_thread_status = DARWIN_ART_ELF_OK;
  std::thread wrong_thread([&] {
    uintptr_t ignored = 0;
    ErrorStorage thread_error;
    other_thread_status = darwin_art_elf_lookup(
        imported, "imported_value", &ignored, &thread_error.value);
  });
  wrong_thread.join();
  if (other_thread_status != DARWIN_ART_ELF_OK) {
    std::fprintf(stderr, "cross-thread lookup failed\n");
    return 1;
  }
  DarwinArtElfStatus cross_thread_unload = DARWIN_ART_ELF_OK;
  std::thread unloading_thread([&] {
    ErrorStorage thread_error;
    cross_thread_unload = darwin_art_elf_unload(&imported, &thread_error.value);
  });
  unloading_thread.join();
  if (cross_thread_unload != DARWIN_ART_ELF_OK || imported != nullptr) {
    std::fprintf(stderr, "cross-thread unload failed\n");
    return 1;
  }
  status = darwin_art_elf_unload(&imported, &error.value);
  if (status != DARWIN_ART_ELF_OK)
    return Fail("unload(import,idempotent)", status, error);

  const std::vector<uint8_t> positive = Read(argv[1]);
  if (positive.empty()) {
    std::fprintf(stderr, "failed to read positive fixture\n");
    return 1;
  }
  DarwinArtElfHandle* local = nullptr;
  status = darwin_art_elf_load_bytes(positive.data(), positive.size(), nullptr,
                                     &local, &error.value);
  if (status != DARWIN_ART_ELF_OK) return Fail("load_bytes", status, error);
  status = darwin_art_elf_run_initializers(local, &error.value);
  if (status != DARWIN_ART_ELF_OK) return Fail("init(local)", status, error);
  error.Reset();
  status = darwin_art_elf_run_initializers(local, &error.value);
  if (status != DARWIN_ART_ELF_LIFECYCLE || error.value.required == 0) {
    return Fail("init(local,twice)", status, error);
  }
  uintptr_t local_address = 0;
  status = darwin_art_elf_lookup(local, "fixture_value", &local_address,
                                 &error.value);
  if (status != DARWIN_ART_ELF_OK)
    return Fail("lookup(fixture_value)", status, error);
  auto fixture_value = reinterpret_cast<int (*)(void)>(local_address);
  if (fixture_value() != 42) {
    std::fprintf(stderr, "bytes-loaded fixture mismatch\n");
    return 1;
  }
  status = darwin_art_elf_unload(&local, &error.value);
  if (status != DARWIN_ART_ELF_OK) return Fail("unload(local)", status, error);

  DarwinArtElfHandle* unresolved = nullptr;
  error.Reset();
  status = darwin_art_elf_load_path(argv[2], nullptr, &unresolved, &error.value);
  if (status != DARWIN_ART_ELF_UNRESOLVED_SYMBOL || unresolved != nullptr ||
      error.value.required == 0) {
    return Fail("closed-default import rejection", status, error);
  }

  const std::vector<uint8_t> parent = Read(argv[3]);
  const std::vector<uint8_t> dep_a = Read(argv[4]);
  const std::vector<uint8_t> dep_b = Read(argv[5]);
  const DarwinArtElfGraphSource graph_sources[] = {
      {"libgraph_parent.so", parent.data(), parent.size()},
      {"libgraph_dep_a.so", dep_a.data(), dep_a.size()},
      {"libgraph_dep_b.so", dep_b.data(), dep_b.size()},
  };
  DarwinArtElfGraphHandle* graph = nullptr;
  error.Reset();
  status = darwin_art_elf_graph_load(
      "libgraph_parent.so", graph_sources, 1, nullptr, 0, nullptr, &graph,
      &error.value);
  if (status != DARWIN_ART_ELF_UNRESOLVED_SYMBOL || graph != nullptr ||
      error.value.required == 0) {
    return Fail("graph_load(missing-dependency)", status, error);
  }
  error.Reset();
  status = darwin_art_elf_graph_load(
      "libgraph_parent.so", graph_sources, std::size(graph_sources), nullptr,
      0, nullptr, &graph, &error.value);
  if (status != DARWIN_ART_ELF_OK || graph == nullptr) {
    return Fail("graph_load", status, error);
  }
  uintptr_t graph_address = 0;
  status = darwin_art_elf_graph_lookup_root(
      graph, "graph_value", &graph_address, &error.value);
  if (status != DARWIN_ART_ELF_OK ||
      reinterpret_cast<int (*)(void)>(graph_address)() != 62) {
    return Fail("graph_lookup_root", status, error);
  }
  status = darwin_art_elf_graph_unload(&graph, &error.value);
  if (status != DARWIN_ART_ELF_OK || graph != nullptr) {
    return Fail("graph_unload", status, error);
  }
  status = darwin_art_elf_graph_unload(&graph, &error.value);
  if (status != DARWIN_ART_ELF_OK) {
    return Fail("graph_unload(idempotent)", status, error);
  }

  std::puts(
      "elf-loader-ffi-smoke: bytes=pass path=pass resolver=versioned "
      "lookup=pass init=lifecycle graph=atomic+recursive "
      "unload=idempotent thread=cross-thread+quiescent");
  return 0;
}
