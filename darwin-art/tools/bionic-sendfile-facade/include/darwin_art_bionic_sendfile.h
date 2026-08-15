#ifndef DARWIN_ART_BIONIC_SENDFILE_H_
#define DARWIN_ART_BIONIC_SENDFILE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DARWIN_ART_BIONIC_SENDFILE_ABI_VERSION = 1 };

typedef struct DarwinArtBionicSendfileRequest {
  uint32_t abi_version;
  int32_t output_fd;
  int32_t input_fd;
  uint32_t has_explicit_offset;
  int64_t offset;
  size_t count;
} DarwinArtBionicSendfileRequest;

typedef struct DarwinArtBionicSendfileResult {
  uint32_t abi_version;
  int32_t android_errno;
  intptr_t transferred;
  int64_t next_offset;
} DarwinArtBionicSendfileResult;

typedef enum DarwinArtBionicSendfileTransferStatus {
  DARWIN_ART_BIONIC_SENDFILE_TRANSFER_OK = 0,
  DARWIN_ART_BIONIC_SENDFILE_TRANSFER_BAD_FD = 1,
  DARWIN_ART_BIONIC_SENDFILE_TRANSFER_UNAVAILABLE = 2,
} DarwinArtBionicSendfileTransferStatus;

/* The owner performs one atomic virtual-fd transfer. A successful partial
 * result is not an error. With an explicit offset it must leave the input
 * descriptor position unchanged and return offset+transferred. */
typedef DarwinArtBionicSendfileTransferStatus (*DarwinArtBionicSendfileTransfer)(
    void* context, const DarwinArtBionicSendfileRequest* request,
    DarwinArtBionicSendfileResult* result);

typedef enum DarwinArtBionicSendfileLifecycleStatus {
  DARWIN_ART_BIONIC_SENDFILE_LIFECYCLE_OK = 0,
  DARWIN_ART_BIONIC_SENDFILE_LIFECYCLE_INVALID_ARGUMENT = 1,
  DARWIN_ART_BIONIC_SENDFILE_LIFECYCLE_ALREADY_ACTIVE = 2,
} DarwinArtBionicSendfileLifecycleStatus;

typedef void (*DarwinArtBionicSendfileFunction)(void);

DarwinArtBionicSendfileLifecycleStatus darwin_art_bionic_sendfile_activate(
    DarwinArtBionicSendfileTransfer transfer, void* context);
/* Stops admission and waits for every in-flight transfer callback. */
DarwinArtBionicSendfileLifecycleStatus darwin_art_bionic_sendfile_deactivate(void);

intptr_t darwin_art_bionic_sendfile(int output_fd, int input_fd,
                                    int64_t* offset, size_t count);
DarwinArtBionicSendfileFunction darwin_art_bionic_sendfile_resolve(
    const char* soname, const char* symbol, const char* version);

#ifdef __cplusplus
}
#endif

#endif
