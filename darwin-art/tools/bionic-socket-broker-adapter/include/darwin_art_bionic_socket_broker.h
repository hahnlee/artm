#ifndef DARWIN_ART_BIONIC_SOCKET_BROKER_H_
#define DARWIN_ART_BIONIC_SOCKET_BROKER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicSocketBrokerFunction)(void);

int darwin_art_bionic_socket_broker_activate(void);
int darwin_art_bionic_socket_broker_deactivate(void);

int darwin_art_bionic_socket_broker_socket(int domain, int type, int protocol);
int darwin_art_bionic_socket_broker_connect(int fd, const void *address,
                                            uint32_t length);
intptr_t darwin_art_bionic_socket_broker_send(int fd, const void *bytes,
                                              size_t count, int flags);
intptr_t darwin_art_bionic_socket_broker_recv(int fd, void *bytes, size_t count,
                                              int flags);
int darwin_art_bionic_socket_broker_close(int fd);

DarwinArtBionicSocketBrokerFunction
darwin_art_bionic_socket_broker_resolve(const char *soname, const char *symbol,
                                        const char *version);
DarwinArtBionicSocketBrokerFunction darwin_art_bionic_socket_broker_dns_resolve(
    const char *soname, const char *symbol, const char *version);
size_t darwin_art_bionic_socket_broker_live_objects(void);
int darwin_art_bionic_socket_broker_is_active(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DARWIN_ART_BIONIC_SOCKET_BROKER_H_
