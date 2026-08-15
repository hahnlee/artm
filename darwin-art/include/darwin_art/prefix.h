#ifndef DARWIN_ART_PREFIX_H_
#define DARWIN_ART_PREFIX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtPrefix DarwinArtPrefix;

typedef enum DarwinArtPrefixResult {
  DARWIN_ART_PREFIX_OK = 0,
  DARWIN_ART_PREFIX_INVALID_ARGUMENT = 1,
  DARWIN_ART_PREFIX_INVALID_PATH = 2,
  DARWIN_ART_PREFIX_DUPLICATE_MOUNT = 3,
  DARWIN_ART_PREFIX_TABLE_SEALED = 4,
  DARWIN_ART_PREFIX_TABLE_NOT_SEALED = 5,
  DARWIN_ART_PREFIX_NO_MOUNT = 6,
  DARWIN_ART_PREFIX_BUFFER_TOO_SMALL = 7,
} DarwinArtPrefixResult;

typedef enum DarwinArtPrefixMountKind {
  DARWIN_ART_PREFIX_IMMUTABLE = 1,
  DARWIN_ART_PREFIX_PRIVATE = 2,
  DARWIN_ART_PREFIX_SHARED = 3,
  DARWIN_ART_PREFIX_SYNTHETIC = 4,
} DarwinArtPrefixMountKind;

typedef struct DarwinArtPrefixResolution {
  uint32_t mount_id;
  uint32_t mount_kind;
  bool writable;
  bool requires_directory;
  size_t normalized_path_length;
  size_t relative_path_length;
} DarwinArtPrefixResolution;

DarwinArtPrefix* darwin_art_prefix_create(void);
void darwin_art_prefix_destroy(DarwinArtPrefix* prefix);

DarwinArtPrefixResult darwin_art_prefix_add_mount(
    DarwinArtPrefix* prefix,
    uint32_t mount_id,
    DarwinArtPrefixMountKind kind,
    bool writable,
    const char* guest_prefix);

DarwinArtPrefixResult darwin_art_prefix_seal(DarwinArtPrefix* prefix);

// Resolves an Android byte pathname without touching the host filesystem.
// Construction is single-threaded. After seal, resolve calls may run
// concurrently, but add/seal/destroy require all calls to be quiescent.
//
// This is only a namespace routing result, not a filesystem authorization.
// The returned relative path must be walked component-by-component beneath the
// pre-authorized directory FD for mount_id. Callers must preserve
// requires_directory, apply the final-component symlink policy, and must not
// concatenate this value with an unchecked host path.
DarwinArtPrefixResult darwin_art_prefix_resolve(
    const DarwinArtPrefix* prefix,
    const char* cwd,
    const char* path,
    DarwinArtPrefixResolution* resolution,
    char* normalized_path,
    size_t normalized_path_capacity,
    char* relative_path,
    size_t relative_path_capacity);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_PREFIX_H_
