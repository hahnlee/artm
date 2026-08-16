#ifndef DARWIN_ART_BIONIC_SOCKET_H_
#define DARWIN_ART_BIONIC_SOCKET_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DarwinArtAndroidSocklen;
typedef void (*DarwinArtBionicSocketFunction)(void);

typedef struct DarwinArtAndroidPollfd {
  int32_t fd;
  int16_t events;
  int16_t revents;
} DarwinArtAndroidPollfd;

typedef struct DarwinArtAndroidTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
} DarwinArtAndroidTimespec;

typedef struct DarwinArtAndroidIovec {
  void* iov_base;
  uint64_t iov_len;
} DarwinArtAndroidIovec;

typedef struct DarwinArtAndroidMsghdr {
  void* msg_name;
  uint32_t msg_namelen;
  uint32_t reserved_name_padding;
  DarwinArtAndroidIovec* msg_iov;
  uint64_t msg_iovlen;
  void* msg_control;
  uint64_t msg_controllen;
  int32_t msg_flags;
  uint32_t reserved_flags_padding;
} DarwinArtAndroidMsghdr;

int darwin_art_bionic_socket_socket(int domain, int type, int protocol);
int darwin_art_bionic_socket_socketpair(int domain, int type, int protocol,
                                        int32_t sockets[2]);
int darwin_art_bionic_socket_close(int fd);
int darwin_art_bionic_socket_bind(int fd, const void* address,
                                  DarwinArtAndroidSocklen length);
int darwin_art_bionic_socket_connect(int fd, const void* address,
                                     DarwinArtAndroidSocklen length);
int darwin_art_bionic_socket_listen(int fd, int backlog);
int darwin_art_bionic_socket_accept4(int fd, void* address,
                                     DarwinArtAndroidSocklen* length,
                                     int flags);
int darwin_art_bionic_socket_shutdown(int fd, int how);
int darwin_art_bionic_socket_getsockname(int fd, void* address,
                                         DarwinArtAndroidSocklen* length);
int darwin_art_bionic_socket_getpeername(int fd, void* address,
                                         DarwinArtAndroidSocklen* length);
int darwin_art_bionic_socket_getsockopt(int fd, int level, int option,
                                        void* value,
                                        DarwinArtAndroidSocklen* length);
int darwin_art_bionic_socket_setsockopt(int fd, int level, int option,
                                        const void* value,
                                        DarwinArtAndroidSocklen length);
intptr_t darwin_art_bionic_socket_send(int fd, const void* buffer,
                                       size_t length, int flags);
intptr_t darwin_art_bionic_socket_recv(int fd, void* buffer, size_t length,
                                       int flags);
intptr_t darwin_art_bionic_socket_sendto(int fd, const void* buffer,
                                         size_t length, int flags,
                                         const void* address,
                                         DarwinArtAndroidSocklen address_length);
intptr_t darwin_art_bionic_socket_recvfrom(
    int fd, void* buffer, size_t length, int flags, void* address,
    DarwinArtAndroidSocklen* address_length);
int darwin_art_bionic_socket_poll(DarwinArtAndroidPollfd* descriptors,
                                  uint32_t count, int timeout_ms);
int darwin_art_bionic_socket_ppoll(
    DarwinArtAndroidPollfd* descriptors, uint32_t count,
    const DarwinArtAndroidTimespec* timeout, const void* android_sigset);
intptr_t darwin_art_bionic_socket_sendmsg(
    int fd, const DarwinArtAndroidMsghdr* message, int flags);
intptr_t darwin_art_bionic_socket_recvmsg(
    int fd, DarwinArtAndroidMsghdr* message, int flags);

DarwinArtBionicSocketFunction darwin_art_bionic_socket_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_socket_capability(const char* capability);

/* Standalone-owner lifecycle. Reset closes every token and waits only for the
 * table lock; operations borrow a duplicate, so an already-admitted call may
 * finish safely after reset or close. */
void darwin_art_bionic_socket_reset_for_test(void);
size_t darwin_art_bionic_socket_live_tokens_for_test(void);
size_t darwin_art_bionic_socket_borrowed_descriptors_for_test(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_SOCKET_H_
