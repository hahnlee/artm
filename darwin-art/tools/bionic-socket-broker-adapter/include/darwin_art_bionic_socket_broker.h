#ifndef DARWIN_ART_BIONIC_SOCKET_BROKER_H_
#define DARWIN_ART_BIONIC_SOCKET_BROKER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicSocketBrokerFunction)(void);

typedef struct DarwinArtBionicPollFd {
  int32_t fd;
  int16_t events;
  int16_t revents;
} DarwinArtBionicPollFd;

int darwin_art_bionic_socket_broker_activate(void);
int darwin_art_bionic_socket_broker_deactivate(void);

int darwin_art_bionic_socket_broker_socket(int domain, int type, int protocol);
int darwin_art_bionic_socket_broker_pipe(int32_t descriptors[2]);
int darwin_art_bionic_socket_broker_pipe2(int32_t descriptors[2], int flags);
int darwin_art_bionic_socket_broker_eventfd(uint32_t initial_value, int flags);
int darwin_art_bionic_socket_broker_timerfd_create(int clock_id, int flags);
int darwin_art_bionic_socket_broker_epoll_create(int size);
int darwin_art_bionic_socket_broker_epoll_create1(int flags);
int darwin_art_bionic_socket_broker_epoll_ctl(int epoll_fd, int operation,
                                               int target_fd,
                                               const void* event);
int darwin_art_bionic_socket_broker_epoll_wait(int epoll_fd, void* events,
                                                int capacity, int timeout_ms);
intptr_t darwin_art_bionic_socket_broker_readv(int fd, const void *vectors,
                                               int count);
intptr_t darwin_art_bionic_socket_broker_writev(int fd, const void *vectors,
                                                int count);
intptr_t darwin_art_bionic_socket_broker_read(int fd, void *bytes,
                                              size_t count);
intptr_t darwin_art_bionic_socket_broker_write(int fd, const void *bytes,
                                               size_t count);
int darwin_art_bionic_socket_broker_poll(DarwinArtBionicPollFd *descriptors,
                                         size_t count, int timeout_ms);
int darwin_art_bionic_socket_broker_connect(int fd, const void *address,
                                            uint32_t length);
int darwin_art_bionic_socket_broker_bind(int fd, const void *address,
                                         uint32_t length);
int darwin_art_bionic_socket_broker_listen(int fd, int backlog);
int darwin_art_bionic_socket_broker_accept4(int fd, void *address,
                                            uint32_t *length, int flags);
int darwin_art_bionic_socket_broker_accept(int fd, void *address,
                                           uint32_t *length);
int darwin_art_bionic_socket_broker_getsockname(int fd, void *address,
                                                uint32_t *length);
int darwin_art_bionic_socket_broker_getpeername(int fd, void *address,
                                                uint32_t *length);
int darwin_art_bionic_socket_broker_socketpair(int domain, int type,
                                               int protocol,
                                               int32_t descriptors[2]);
intptr_t darwin_art_bionic_socket_broker_send(int fd, const void *bytes,
                                              size_t count, int flags);
intptr_t darwin_art_bionic_socket_broker_recv(int fd, void *bytes, size_t count,
                                              int flags);
intptr_t darwin_art_bionic_socket_broker_sendto(int fd, const void *bytes,
                                                size_t count, int flags,
                                                const void *address,
                                                uint32_t address_length);
intptr_t darwin_art_bionic_socket_broker_recvfrom(int fd, void *bytes,
                                                  size_t count, int flags,
                                                  void *address,
                                                  uint32_t *address_length);
int darwin_art_bionic_socket_broker_getsockopt(int fd, int level, int option,
                                               void *value, uint32_t *length);
int darwin_art_bionic_socket_broker_setsockopt(int fd, int level, int option,
                                               const void *value,
                                               uint32_t length);
int darwin_art_bionic_socket_broker_shutdown(int fd, int how);
int darwin_art_bionic_socket_broker_dup(int fd);
int darwin_art_bionic_socket_broker_close(int fd);
int darwin_art_bionic_socket_broker_fcntl(int fd, int command,
                                          intptr_t argument);
int darwin_art_bionic_fd_export_for_scm(int guest_fd);
int darwin_art_bionic_fd_import_from_scm(int host_fd);
/* Returns zero after reporting whether a central-broker descriptor handled the
 * ioctl. This is the device-owner seam used by the Bionic ioctl facade. */
int darwin_art_bionic_socket_broker_ioctl_dispatch(
    int fd, uint32_t request, void* argument, int* handled, int* result,
    int* android_errno);

DarwinArtBionicSocketBrokerFunction
darwin_art_bionic_socket_broker_resolve(const char *soname, const char *symbol,
                                        const char *version);
uintptr_t darwin_art_bionic_socket_broker_data_resolve(
    const char *soname, const char *symbol, const char *version);
DarwinArtBionicSocketBrokerFunction darwin_art_bionic_socket_broker_dns_resolve(
    const char *soname, const char *symbol, const char *version);
size_t darwin_art_bionic_socket_broker_live_objects(void);
int darwin_art_bionic_socket_broker_is_active(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DARWIN_ART_BIONIC_SOCKET_BROKER_H_
