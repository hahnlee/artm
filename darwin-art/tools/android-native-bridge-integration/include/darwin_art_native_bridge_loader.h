#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DarwinArtJniCallType {
  DARWIN_ART_JNI_CALL_REGULAR = 1,
  DARWIN_ART_JNI_CALL_CRITICAL = 2,
};

typedef uint64_t DarwinArtLoaderNamespace;
typedef void* DarwinArtLoaderHandle;

/*
 * Atomic C ABI expected by the Darwin libnativeloader/nativebridge adapter.
 * The adapter owns Java ClassLoader weak-global identity and maps it to one
 * persistent loader namespace. The Rust ELF loader owns all returned handles.
 */
typedef struct DarwinArtElfLoaderV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* context;

  DarwinArtLoaderNamespace (*create_namespace)(
      void* context, DarwinArtLoaderNamespace parent, const char* search_path,
      const char* permitted_path, char* error, size_t error_capacity);
  DarwinArtLoaderHandle (*open)(
      void* context, DarwinArtLoaderNamespace name_space, const char* path, int flags,
      char* error, size_t error_capacity);
  int (*close)(void* context, DarwinArtLoaderHandle handle, char* error, size_t error_capacity);
  void* (*get_trampoline)(
      void* context, DarwinArtLoaderHandle handle, const char* symbol,
      const char* shorty, uint32_t shorty_length, enum DarwinArtJniCallType call_type,
      char* error, size_t error_capacity);
  int (*owns_function_pointer)(void* context, const void* address);
} DarwinArtElfLoaderV1;

enum { DARWIN_ART_ELF_LOADER_ABI_V1 = 1 };

#ifdef __cplusplus
}
#endif

