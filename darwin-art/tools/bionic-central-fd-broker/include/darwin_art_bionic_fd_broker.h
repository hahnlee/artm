#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtFdBroker DarwinArtFdBroker;
typedef uint64_t DarwinArtFdOwnerHandle;

typedef enum DarwinArtFdKind {
  DARWIN_ART_FD_FS_FILE = 1,
  DARWIN_ART_FD_FS_RANDOM = 2,
  DARWIN_ART_FD_STDIO = 3,
  DARWIN_ART_FD_SOCKET = 4,
} DarwinArtFdKind;

typedef enum DarwinArtFdBrokerStatus {
  DARWIN_ART_FD_BROKER_OK = 0,
  DARWIN_ART_FD_BROKER_INVALID_ARGUMENT = 1,
  DARWIN_ART_FD_BROKER_STALE = 2,
  DARWIN_ART_FD_BROKER_WRONG_OWNER = 3,
  DARWIN_ART_FD_BROKER_WRONG_KIND = 4,
  DARWIN_ART_FD_BROKER_BUSY = 5,
  DARWIN_ART_FD_BROKER_DRAINING = 6,
  DARWIN_ART_FD_BROKER_EXHAUSTED = 7,
} DarwinArtFdBrokerStatus;

typedef struct DarwinArtFdIoResult {
  intptr_t value;
  int android_errno;
} DarwinArtFdIoResult;

typedef struct DarwinArtFdPollEntry {
  int fd;
  int16_t events;
  int16_t revents;
} DarwinArtFdPollEntry;

typedef struct DarwinArtFdOwnerV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void *context;
  intptr_t (*read)(void *context, uint64_t object, void *bytes, size_t count,
                   int *android_errno);
  intptr_t (*write)(void *context, uint64_t object, const void *bytes,
                    size_t count, int *android_errno);
  int (*poll)(void *context, uint64_t object, int16_t events, int16_t *revents,
              int *android_errno);
  int (*ioctl)(void *context, uint64_t object, uint64_t request, void *argument,
               int *android_errno);
  int (*close)(void *context, uint64_t object, int *android_errno);
} DarwinArtFdOwnerV1;

enum { DARWIN_ART_FD_OWNER_ABI_V1 = 1 };

DarwinArtFdBroker *darwin_art_fd_broker_create(void);
DarwinArtFdBrokerStatus darwin_art_fd_broker_destroy(DarwinArtFdBroker *broker);

DarwinArtFdBrokerStatus darwin_art_fd_broker_install_owner(
    DarwinArtFdBroker *broker, DarwinArtFdKind kind,
    const DarwinArtFdOwnerV1 *callbacks, DarwinArtFdOwnerHandle *owner);
DarwinArtFdBrokerStatus
darwin_art_fd_broker_uninstall_owner(DarwinArtFdBroker *broker,
                                     DarwinArtFdOwnerHandle owner);

DarwinArtFdBrokerStatus
darwin_art_fd_broker_publish(DarwinArtFdBroker *broker,
                             DarwinArtFdOwnerHandle owner, uint64_t object,
                             int *guest_fd);
DarwinArtFdBrokerStatus darwin_art_fd_broker_close(DarwinArtFdBroker *broker,
                                                   int guest_fd,
                                                   DarwinArtFdIoResult *result);
DarwinArtFdBrokerStatus
darwin_art_fd_broker_close_owned(DarwinArtFdBroker *broker,
                                 DarwinArtFdOwnerHandle owner, int guest_fd,
                                 DarwinArtFdIoResult *result);

DarwinArtFdBrokerStatus darwin_art_fd_broker_read(DarwinArtFdBroker *broker,
                                                  int guest_fd, void *bytes,
                                                  size_t count,
                                                  DarwinArtFdIoResult *result);
DarwinArtFdBrokerStatus darwin_art_fd_broker_write(DarwinArtFdBroker *broker,
                                                   int guest_fd,
                                                   const void *bytes,
                                                   size_t count,
                                                   DarwinArtFdIoResult *result);
DarwinArtFdBrokerStatus
darwin_art_fd_broker_ioctl(DarwinArtFdBroker *broker, int guest_fd,
                           DarwinArtFdKind required_kind, uint64_t request,
                           void *argument, DarwinArtFdIoResult *result);
DarwinArtFdBrokerStatus darwin_art_fd_broker_poll(DarwinArtFdBroker *broker,
                                                  DarwinArtFdPollEntry *entries,
                                                  size_t count,
                                                  DarwinArtFdIoResult *result);
DarwinArtFdBrokerStatus
darwin_art_fd_broker_sendfile(DarwinArtFdBroker *broker, int output_fd,
                              int input_fd, size_t count,
                              DarwinArtFdIoResult *result);

#ifdef __cplusplus
}
#endif
