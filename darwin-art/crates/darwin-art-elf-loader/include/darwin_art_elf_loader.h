#ifndef DARWIN_ART_ELF_LOADER_H_
#define DARWIN_ART_ELF_LOADER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DARWIN_ART_ELF_ABI_VERSION 1u

typedef enum DarwinArtElfStatus {
  DARWIN_ART_ELF_OK = 0,
  DARWIN_ART_ELF_INVALID_ARGUMENT = 1,
  DARWIN_ART_ELF_IO = 2,
  DARWIN_ART_ELF_FORMAT = 3,
  DARWIN_ART_ELF_BOUNDS = 4,
  DARWIN_ART_ELF_CAPABILITY = 5,
  DARWIN_ART_ELF_PROTECTION = 6,
  DARWIN_ART_ELF_RESOLVER = 7,
  DARWIN_ART_ELF_UNRESOLVED_SYMBOL = 8,
  DARWIN_ART_ELF_SYMBOL_NOT_FOUND = 9,
  DARWIN_ART_ELF_INVALID_SYMBOL = 10,
  DARWIN_ART_ELF_LIFECYCLE = 11,
  DARWIN_ART_ELF_SYSTEM = 12,
  DARWIN_ART_ELF_POISONED = 13,
  DARWIN_ART_ELF_PANIC = 14,
} DarwinArtElfStatus;

typedef enum DarwinArtElfResolveStatus {
  DARWIN_ART_ELF_RESOLVE_FOUND = 0,
  DARWIN_ART_ELF_RESOLVE_NOT_FOUND = 1,
  DARWIN_ART_ELF_RESOLVE_ERROR = 2,
} DarwinArtElfResolveStatus;

typedef struct DarwinArtElfErrorBuffer {
  char* data;
  size_t capacity;
  /* Required bytes including NUL. Zero means no error message. */
  size_t required;
} DarwinArtElfErrorBuffer;

typedef struct DarwinArtElfSymbolRequest {
  uint32_t abi_version;
  const char* symbol;
  /* Both are NULL for an unversioned symbol. */
  const char* version_soname;
  const char* version_name;
  uint16_t version_flags;
  uint8_t version_hidden;
  uint8_t reserved;
  const char* const* needed_libraries;
  size_t needed_library_count;
} DarwinArtElfSymbolRequest;

typedef DarwinArtElfResolveStatus (*DarwinArtElfResolverCallback)(
    void* context,
    const DarwinArtElfSymbolRequest* request,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error);

typedef struct DarwinArtElfLoadOptions {
  uint32_t abi_version;
  DarwinArtElfResolverCallback resolver;
  void* resolver_context;
} DarwinArtElfLoadOptions;

typedef struct DarwinArtElfHandle DarwinArtElfHandle;
typedef struct DarwinArtElfGraphHandle DarwinArtElfGraphHandle;

typedef struct DarwinArtElfGraphSource {
  const char* soname;
  const uint8_t* bytes;
  size_t length;
} DarwinArtElfGraphSource;

uint32_t darwin_art_elf_abi_version(void);
const char* darwin_art_elf_status_name(int32_t status);

DarwinArtElfStatus darwin_art_elf_load_bytes(
    const uint8_t* bytes,
    size_t length,
    const DarwinArtElfLoadOptions* options,
    DarwinArtElfHandle** out_handle,
    DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_elf_load_path(
    const char* path,
    const DarwinArtElfLoadOptions* options,
    DarwinArtElfHandle** out_handle,
    DarwinArtElfErrorBuffer* error);

/*
 * Atomically maps, eagerly relocates, and dependency-first initializes the
 * complete recursive graph rooted at root_soname. Every DT_NEEDED SONAME must
 * be present exactly once in sources or provider_sonames. The returned handle
 * is published only after the complete graph succeeds.
 */
DarwinArtElfStatus darwin_art_elf_graph_load(
    const char* root_soname,
    const DarwinArtElfGraphSource* sources,
    size_t source_count,
    const char* const* provider_sonames,
    size_t provider_count,
    const DarwinArtElfLoadOptions* options,
    DarwinArtElfGraphHandle** out_handle,
    DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_elf_graph_lookup_root(
    DarwinArtElfGraphHandle* handle,
    const char* name,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error);

/* Finalizes dependents before dependencies, then nulls the unique handle. */
DarwinArtElfStatus darwin_art_elf_graph_unload(
    DarwinArtElfGraphHandle** handle,
    DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_elf_run_initializers(
    DarwinArtElfHandle* handle,
    DarwinArtElfErrorBuffer* error);

DarwinArtElfStatus darwin_art_elf_lookup(
    DarwinArtElfHandle* handle,
    const char* name,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error);

/* Idempotent for *handle == NULL. On success, sets *handle to NULL. */
DarwinArtElfStatus darwin_art_elf_unload(
    DarwinArtElfHandle** handle,
    DarwinArtElfErrorBuffer* error);

/*
 * Ownership and lifetime contract:
 *
 * - A non-NULL handle returned by load is uniquely owned by the caller and must
 *   be released exactly once with darwin_art_elf_unload.
 * - Init and lookup may occur on threads different from load and are internally
 *   serialized. Reentrant operations on the same handle from an initializer are
 *   forbidden.
 * - Unload may run on any thread, but the caller must first establish quiescence:
 *   no concurrent ABI operation and no execution through a borrowed lookup or
 *   imported function address. The raw handle does not provide reference counting.
 * - Resolver request strings/arrays are borrowed and valid only during the
 *   synchronous callback. The callback/context itself is not retained because
 *   all supported relocations are eager and lazy PLT binding is rejected.
 * - Every FOUND provider address must remain ABI-compatible and valid until the
 *   loaded handle is unloaded. The loader never falls back to Darwin's global
 *   symbol namespace.
 * - A lookup address is borrowed from the handle and becomes invalid at unload.
 *   The caller owns the ABI cast and must run required init arrays before use.
 * - A NativeBridge open adapter must keep a newly loaded handle private, run
 *   initializers and any preflight atomically, and publish it only on success. On
 *   failure it must unload the still-private handle after quiescing callbacks.
 * - Resolver callbacks must not throw C++/Objective-C exceptions or unwind.
 *   Rust panics originating inside this library are contained and reported as
 *   DARWIN_ART_ELF_PANIC.
 * - Graph source/provider arrays and strings are borrowed only for graph_load.
 *   The graph copies source bytes and retains no resolver callback after eager
 *   relocation. Graph lookup addresses remain borrowed until graph_unload.
 */

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_ELF_LOADER_H_
