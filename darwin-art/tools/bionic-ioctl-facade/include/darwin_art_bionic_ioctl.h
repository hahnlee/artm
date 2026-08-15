#ifndef DARWIN_ART_BIONIC_IOCTL_H_
#define DARWIN_ART_BIONIC_IOCTL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DARWIN_ART_BIONIC_IOCTL_FD_INFO_ABI_VERSION = 1 };

typedef enum DarwinArtBionicIoctlFdKind {
  DARWIN_ART_BIONIC_IOCTL_FD_OTHER = 0,
  DARWIN_ART_BIONIC_IOCTL_FD_RANDOM_DEVICE = 1,
} DarwinArtBionicIoctlFdKind;

typedef struct DarwinArtBionicIoctlFdInfo {
  uint32_t abi_version;
  DarwinArtBionicIoctlFdKind kind;
} DarwinArtBionicIoctlFdInfo;

typedef enum DarwinArtBionicIoctlFdLookupStatus {
  DARWIN_ART_BIONIC_IOCTL_FD_FOUND = 0,
  DARWIN_ART_BIONIC_IOCTL_FD_BAD = 1,
  DARWIN_ART_BIONIC_IOCTL_FD_CAPABILITY_UNAVAILABLE = 2,
} DarwinArtBionicIoctlFdLookupStatus;

typedef DarwinArtBionicIoctlFdLookupStatus (*DarwinArtBionicIoctlFdLookup)(
    void* context, int32_t fd, DarwinArtBionicIoctlFdInfo* info);
/* The callback and its context remain valid until deactivate returns. It must
 * not invoke the activation/deactivation lifecycle recursively. */

typedef enum DarwinArtBionicIoctlLifecycleStatus {
  DARWIN_ART_BIONIC_IOCTL_LIFECYCLE_OK = 0,
  DARWIN_ART_BIONIC_IOCTL_LIFECYCLE_INVALID_ARGUMENT = 1,
  DARWIN_ART_BIONIC_IOCTL_LIFECYCLE_ALREADY_ACTIVE = 2,
} DarwinArtBionicIoctlLifecycleStatus;

typedef void (*DarwinArtBionicIoctlFunction)(void);

DarwinArtBionicIoctlLifecycleStatus darwin_art_bionic_ioctl_activate(
    DarwinArtBionicIoctlFdLookup lookup, void* context);
/* Stops new calls and waits until every in-flight lookup has returned. */
DarwinArtBionicIoctlLifecycleStatus darwin_art_bionic_ioctl_deactivate(void);

int darwin_art_bionic_ioctl(int fd, int request, ...);
DarwinArtBionicIoctlFunction darwin_art_bionic_ioctl_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_ioctl_capability(const char* capability);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_IOCTL_H_
