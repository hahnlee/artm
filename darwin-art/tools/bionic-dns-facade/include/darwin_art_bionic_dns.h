#ifndef DARWIN_ART_BIONIC_DNS_H_
#define DARWIN_ART_BIONIC_DNS_H_

#include <stddef.h>
#include <stdint.h>
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DarwinArtAndroidSocklen;

typedef struct DarwinArtAndroidAddrinfo {
  int32_t ai_flags;
  int32_t ai_family;
  int32_t ai_socktype;
  int32_t ai_protocol;
  DarwinArtAndroidSocklen ai_addrlen;
  uint32_t reserved_padding;
  char* ai_canonname;
  void* ai_addr;
  struct DarwinArtAndroidAddrinfo* ai_next;
} DarwinArtAndroidAddrinfo;

typedef void (*DarwinArtBionicDnsFunction)(void);

int darwin_art_bionic_dns_getaddrinfo(
    const char* node, const char* service,
    const DarwinArtAndroidAddrinfo* hints,
    DarwinArtAndroidAddrinfo** result);
int darwin_art_bionic_dns_android_getaddrinfofornet(
    const char* node, const char* service, const struct addrinfo* hints,
    unsigned netid, unsigned mark, struct addrinfo** result);
void darwin_art_bionic_dns_freeaddrinfo(DarwinArtAndroidAddrinfo* result);
const char* darwin_art_bionic_dns_gai_strerror(int error);
int darwin_art_bionic_dns_getnameinfo(
    const void* address, DarwinArtAndroidSocklen address_length,
    char* host, size_t host_length, char* service, size_t service_length,
    int flags);
const char* darwin_art_bionic_dns_inet_ntop(
    int family, const void* address, char* output,
    DarwinArtAndroidSocklen output_length);
int darwin_art_bionic_dns_inet_pton(int family, const char* source,
                                    void* destination);
uint32_t darwin_art_bionic_dns_inet_addr(const char* source);
char* darwin_art_bionic_dns_inet_ntoa(uint32_t address);
int* darwin_art_bionic_dns_get_h_errno(void);
struct hostent* darwin_art_bionic_dns_gethostbyname(const char* name);
struct servent* darwin_art_bionic_dns_getservbyname(const char* name,
                                                    const char* proto);

DarwinArtBionicDnsFunction darwin_art_bionic_dns_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_dns_capability(const char* capability);

/* freeaddrinfo retires ownership without reclaiming storage. reset is the
 * quiescent lifecycle boundary and must not race guest readers. */
void darwin_art_bionic_dns_reset_for_test(void);
size_t darwin_art_bionic_dns_live_results_for_test(void);
size_t darwin_art_bionic_dns_retired_results_for_test(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_DNS_H_
