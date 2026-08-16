#include "darwin_art_bionic_dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);

namespace {

constexpr int kAndroidAfUnspec = 0;
constexpr int kAndroidAfInet = 2;
constexpr int kAndroidAfInet6 = 10;
constexpr int kAndroidSockStream = 1;
constexpr int kAndroidSockDgram = 2;
constexpr int kAndroidAiPassive = 0x1;
constexpr int kAndroidAiCanonname = 0x2;
constexpr int kAndroidAiNumericHost = 0x4;
constexpr int kAndroidAiNumericServ = 0x8;
constexpr int kAndroidNiNumericHost = 0x2;
constexpr int kAndroidNiNumericServ = 0x8;
constexpr int kAndroidNiDgram = 0x10;
constexpr size_t kMaxResults = 256;
constexpr size_t kMaxNodesPerResult = 64;

struct AndroidSockaddrIn {
  uint16_t family;
  uint16_t port;
  uint32_t address;
  uint8_t zero[8];
};

struct AndroidSockaddrIn6 {
  uint16_t family;
  uint16_t port;
  uint32_t flowinfo;
  uint8_t address[16];
  uint32_t scope_id;
};

struct ResultSlot {
  DarwinArtAndroidAddrinfo* head = nullptr;
  bool retired = false;
};

pthread_mutex_t g_results_lock = PTHREAD_MUTEX_INITIALIZER;
std::array<ResultSlot, kMaxResults> g_results{};

class PreserveErrno {
 public:
  PreserveErrno() : saved_(errno) {}
  ~PreserveErrno() { errno = saved_; }

 private:
  int saved_;
};

int32_t AndroidErrno(int error) {
  switch (error) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case EINTR: return 4;
    case EIO: return 5;
    case EBADF: return 9;
    case EAGAIN: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EINVAL: return 22;
    case EMFILE: return 24;
    case ENOSPC: return 28;
    case EPIPE: return 32;
    case ERANGE: return 34;
    case ENOSYS: return 38;
    case EOVERFLOW: return 75;
    default: return 5;
  }
}

int AndroidEai(int host_error) {
  switch (host_error) {
    case 0: return 0;
    case EAI_ADDRFAMILY: return 1;
    case EAI_AGAIN: return 2;
    case EAI_BADFLAGS: return 3;
    case EAI_FAIL: return 4;
    case EAI_FAMILY: return 5;
    case EAI_MEMORY: return 6;
    case EAI_NODATA: return 7;
    case EAI_NONAME: return 8;
    case EAI_SERVICE: return 9;
    case EAI_SOCKTYPE: return 10;
    case EAI_SYSTEM: return 11;
#ifdef EAI_BADHINTS
    case EAI_BADHINTS: return 12;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL: return 13;
#endif
    case EAI_OVERFLOW: return 14;
    default: return 4;
  }
}

bool EqualsAsciiIgnoreCase(const char* value, const char* expected) {
  if (value == nullptr) return false;
  for (;;) {
    const unsigned char left = static_cast<unsigned char>(*value++);
    const unsigned char right = static_cast<unsigned char>(*expected++);
    const unsigned char folded_left =
        left >= 'A' && left <= 'Z' ? static_cast<unsigned char>(left + 32) : left;
    const unsigned char folded_right =
        right >= 'A' && right <= 'Z' ? static_cast<unsigned char>(right + 32) : right;
    if (folded_left != folded_right) return false;
    if (left == 0) return true;
  }
}

bool IsNumericNode(const char* node) {
  in_addr address4{};
  in6_addr address6{};
  return inet_pton(AF_INET, node, &address4) == 1 ||
         inet_pton(AF_INET6, node, &address6) == 1;
}

bool IsAllowedNode(const char* node) {
  return node == nullptr || IsNumericNode(node) ||
         EqualsAsciiIgnoreCase(node, "localhost") ||
         EqualsAsciiIgnoreCase(node, "localhost.");
}

bool IsNumericService(const char* service) {
  if (service == nullptr) return true;
  if (*service == 0) return false;
  uint32_t value = 0;
  for (const unsigned char* cursor =
           reinterpret_cast<const unsigned char*>(service);
       *cursor != 0; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    value = value * 10 + static_cast<uint32_t>(*cursor - '0');
    if (value > 65535) return false;
  }
  return true;
}

bool TranslateFamilyToHost(int android, int* host) {
  switch (android) {
    case kAndroidAfUnspec: *host = AF_UNSPEC; return true;
    case kAndroidAfInet: *host = AF_INET; return true;
    case kAndroidAfInet6: *host = AF_INET6; return true;
    default: return false;
  }
}

bool TranslateFamilyFromHost(int host, int* android) {
  switch (host) {
    case AF_INET: *android = kAndroidAfInet; return true;
    case AF_INET6: *android = kAndroidAfInet6; return true;
    default: return false;
  }
}

bool TranslateSocktypeToHost(int android, int* host) {
  switch (android) {
    case 0: *host = 0; return true;
    case kAndroidSockStream: *host = SOCK_STREAM; return true;
    case kAndroidSockDgram: *host = SOCK_DGRAM; return true;
    default: return false;
  }
}

bool TranslateSocktypeFromHost(int host, int* android) {
  switch (host) {
    case 0: *android = 0; return true;
    case SOCK_STREAM: *android = kAndroidSockStream; return true;
    case SOCK_DGRAM: *android = kAndroidSockDgram; return true;
    default: return false;
  }
}

bool TranslateProtocolToHost(int android, int* host) {
  switch (android) {
    case 0: *host = 0; return true;
    case 6: *host = IPPROTO_TCP; return true;
    case 17: *host = IPPROTO_UDP; return true;
    default: return false;
  }
}

bool TranslateProtocolFromHost(int host, int* android) {
  switch (host) {
    case 0: *android = 0; return true;
    case IPPROTO_TCP: *android = 6; return true;
    case IPPROTO_UDP: *android = 17; return true;
    default: return false;
  }
}

bool TranslateFlagsToHost(int android, int* host) {
  constexpr int kAllowed = kAndroidAiPassive | kAndroidAiCanonname |
                           kAndroidAiNumericHost | kAndroidAiNumericServ;
  if ((android & ~kAllowed) != 0) return false;
  int result = 0;
  if ((android & kAndroidAiPassive) != 0) result |= AI_PASSIVE;
  if ((android & kAndroidAiCanonname) != 0) result |= AI_CANONNAME;
  if ((android & kAndroidAiNumericHost) != 0) result |= AI_NUMERICHOST;
  if ((android & kAndroidAiNumericServ) != 0) result |= AI_NUMERICSERV;
  *host = result;
  return true;
}

int TranslateFlagsFromHost(int host) {
  int result = 0;
  if ((host & AI_PASSIVE) != 0) result |= kAndroidAiPassive;
  if ((host & AI_CANONNAME) != 0) result |= kAndroidAiCanonname;
  if ((host & AI_NUMERICHOST) != 0) result |= kAndroidAiNumericHost;
  if ((host & AI_NUMERICSERV) != 0) result |= kAndroidAiNumericServ;
  return result;
}

char* CopyString(const char* source) {
  if (source == nullptr) return nullptr;
  const size_t length = std::strlen(source);
  if (length == std::numeric_limits<size_t>::max()) return nullptr;
  char* result = new (std::nothrow) char[length + 1];
  if (result != nullptr) std::memcpy(result, source, length + 1);
  return result;
}

void DestroyList(DarwinArtAndroidAddrinfo* head) {
  while (head != nullptr) {
    DarwinArtAndroidAddrinfo* next = head->ai_next;
    delete[] reinterpret_cast<uint8_t*>(head->ai_addr);
    delete[] head->ai_canonname;
    delete head;
    head = next;
  }
}

bool CopyAddress(const sockaddr* source, socklen_t length, void** output,
                 uint32_t* output_length) {
  *output = nullptr;
  *output_length = 0;
  if (source->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
    const auto* host = reinterpret_cast<const sockaddr_in*>(source);
    auto* bytes = new (std::nothrow) uint8_t[sizeof(AndroidSockaddrIn)];
    if (bytes == nullptr) return false;
    AndroidSockaddrIn android{};
    android.family = kAndroidAfInet;
    android.port = host->sin_port;
    android.address = host->sin_addr.s_addr;
    std::memcpy(bytes, &android, sizeof(android));
    *output = bytes;
    *output_length = sizeof(android);
    return true;
  }
  if (source->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto* host = reinterpret_cast<const sockaddr_in6*>(source);
    auto* bytes = new (std::nothrow) uint8_t[sizeof(AndroidSockaddrIn6)];
    if (bytes == nullptr) return false;
    AndroidSockaddrIn6 android{};
    android.family = kAndroidAfInet6;
    android.port = host->sin6_port;
    android.flowinfo = host->sin6_flowinfo;
    std::memcpy(android.address, &host->sin6_addr, sizeof(android.address));
    android.scope_id = host->sin6_scope_id;
    std::memcpy(bytes, &android, sizeof(android));
    *output = bytes;
    *output_length = sizeof(android);
    return true;
  }
  return false;
}

bool ToHostAddress(const void* source, uint32_t length,
                   sockaddr_storage* storage, socklen_t* output_length) {
  if (source == nullptr || length < sizeof(uint16_t)) return false;
  uint16_t family = 0;
  std::memcpy(&family, source, sizeof(family));
  std::memset(storage, 0, sizeof(*storage));
  if (family == kAndroidAfInet && length >= sizeof(AndroidSockaddrIn)) {
    AndroidSockaddrIn android{};
    std::memcpy(&android, source, sizeof(android));
    sockaddr_in host{};
    host.sin_len = sizeof(host);
    host.sin_family = AF_INET;
    host.sin_port = android.port;
    host.sin_addr.s_addr = android.address;
    std::memcpy(storage, &host, sizeof(host));
    *output_length = sizeof(host);
    return true;
  }
  if (family == kAndroidAfInet6 && length >= sizeof(AndroidSockaddrIn6)) {
    AndroidSockaddrIn6 android{};
    std::memcpy(&android, source, sizeof(android));
    sockaddr_in6 host{};
    host.sin6_len = sizeof(host);
    host.sin6_family = AF_INET6;
    host.sin6_port = android.port;
    host.sin6_flowinfo = android.flowinfo;
    std::memcpy(&host.sin6_addr, android.address, sizeof(android.address));
    host.sin6_scope_id = android.scope_id;
    std::memcpy(storage, &host, sizeof(host));
    *output_length = sizeof(host);
    return true;
  }
  return false;
}

bool RegisterResult(DarwinArtAndroidAddrinfo* head) {
  (void)pthread_mutex_lock(&g_results_lock);
  for (ResultSlot& slot : g_results) {
    if (slot.head != nullptr) continue;
    slot = ResultSlot{head, false};
    (void)pthread_mutex_unlock(&g_results_lock);
    return true;
  }
  (void)pthread_mutex_unlock(&g_results_lock);
  return false;
}

}  // namespace

extern "C" int darwin_art_bionic_dns_getaddrinfo(
    const char* node, const char* service,
    const DarwinArtAndroidAddrinfo* hints,
    DarwinArtAndroidAddrinfo** result) {
  PreserveErrno preserve;
  if (result == nullptr) return 4;
  *result = nullptr;
  if (node == nullptr && service == nullptr) return 8;
  if (!IsAllowedNode(node)) return 8;
  if (!IsNumericService(service)) return 9;

  addrinfo host_hints{};
  if (hints != nullptr) {
    if (!TranslateFlagsToHost(hints->ai_flags, &host_hints.ai_flags)) return 3;
    if (!TranslateFamilyToHost(hints->ai_family, &host_hints.ai_family)) return 5;
    if (!TranslateSocktypeToHost(hints->ai_socktype,
                                 &host_hints.ai_socktype)) return 10;
    if (!TranslateProtocolToHost(hints->ai_protocol,
                                 &host_hints.ai_protocol)) return 13;
    if (hints->ai_addrlen != 0 || hints->ai_addr != nullptr ||
        hints->ai_canonname != nullptr || hints->ai_next != nullptr) {
      return 12;
    }
  }

  addrinfo* host_result = nullptr;
  const int host_status = getaddrinfo(node, service,
                                      hints == nullptr ? nullptr : &host_hints,
                                      &host_result);
  const int host_errno = errno;
  if (host_status != 0) {
    if (host_status == EAI_SYSTEM) {
      darwin_art_bionic_errno_store(AndroidErrno(host_errno));
    }
    return AndroidEai(host_status);
  }

  DarwinArtAndroidAddrinfo* head = nullptr;
  DarwinArtAndroidAddrinfo** tail = &head;
  size_t count = 0;
  for (const addrinfo* current = host_result; current != nullptr;
       current = current->ai_next) {
    int android_family = 0;
    int android_socktype = 0;
    int android_protocol = 0;
    if (!TranslateFamilyFromHost(current->ai_family, &android_family) ||
        !TranslateSocktypeFromHost(current->ai_socktype, &android_socktype) ||
        !TranslateProtocolFromHost(current->ai_protocol, &android_protocol)) {
      continue;
    }
    if (++count > kMaxNodesPerResult) {
      freeaddrinfo(host_result);
      DestroyList(head);
      return 6;
    }
    auto* copied = new (std::nothrow) DarwinArtAndroidAddrinfo{};
    if (copied == nullptr ||
        !CopyAddress(current->ai_addr, current->ai_addrlen, &copied->ai_addr,
                     &copied->ai_addrlen)) {
      delete copied;
      freeaddrinfo(host_result);
      DestroyList(head);
      return 6;
    }
    if (current->ai_canonname != nullptr) {
      copied->ai_canonname = CopyString(current->ai_canonname);
      if (copied->ai_canonname == nullptr) {
        delete[] reinterpret_cast<uint8_t*>(copied->ai_addr);
        delete copied;
        freeaddrinfo(host_result);
        DestroyList(head);
        return 6;
      }
    }
    copied->ai_flags = TranslateFlagsFromHost(current->ai_flags);
    copied->ai_family = android_family;
    copied->ai_socktype = android_socktype;
    copied->ai_protocol = android_protocol;
    *tail = copied;
    tail = &copied->ai_next;
  }
  freeaddrinfo(host_result);
  if (head == nullptr) return 7;
  if (!RegisterResult(head)) {
    DestroyList(head);
    return 6;
  }
  *result = head;
  return 0;
}

extern "C" void darwin_art_bionic_dns_freeaddrinfo(
    DarwinArtAndroidAddrinfo* result) {
  PreserveErrno preserve;
  if (result == nullptr) return;
  (void)pthread_mutex_lock(&g_results_lock);
  for (ResultSlot& slot : g_results) {
    if (slot.head == result) {
      slot.retired = true;
      break;
    }
  }
  (void)pthread_mutex_unlock(&g_results_lock);
}

extern "C" const char* darwin_art_bionic_dns_gai_strerror(int error) {
  PreserveErrno preserve;
  switch (error) {
    case 0: return "Success";
    case 1: return "Address family for hostname not supported";
    case 2: return "Temporary failure in name resolution";
    case 3: return "Invalid value for ai_flags";
    case 4: return "Non-recoverable failure in name resolution";
    case 5: return "Address family not supported";
    case 6: return "Memory allocation failure";
    case 7: return "No address associated with hostname";
    case 8: return "Name or service not known";
    case 9: return "Servname not supported for ai_socktype";
    case 10: return "Socket type not supported";
    case 11: return "System error";
    case 12: return "Invalid value for hints";
    case 13: return "Resolved protocol is unknown";
    case 14: return "Argument buffer overflow";
    default: return "Unknown error";
  }
}

extern "C" int darwin_art_bionic_dns_getnameinfo(
    const void* address, uint32_t address_length, char* host,
    size_t host_length, char* service, size_t service_length, int flags) {
  PreserveErrno preserve;
  if (address == nullptr || (host == nullptr && service == nullptr)) return 4;
  constexpr int kAllowed = kAndroidNiNumericHost | kAndroidNiNumericServ |
                           kAndroidNiDgram;
  if ((flags & ~kAllowed) != 0) return 3;
  if (host != nullptr && (flags & kAndroidNiNumericHost) == 0) return 8;
  if (service != nullptr && (flags & kAndroidNiNumericServ) == 0) return 8;
  sockaddr_storage storage{};
  socklen_t host_address_length = 0;
  if (!ToHostAddress(address, address_length, &storage,
                     &host_address_length)) return 5;
  int host_flags = 0;
  if ((flags & kAndroidNiNumericHost) != 0) host_flags |= NI_NUMERICHOST;
  if ((flags & kAndroidNiNumericServ) != 0) host_flags |= NI_NUMERICSERV;
  if ((flags & kAndroidNiDgram) != 0) host_flags |= NI_DGRAM;
  const int status = getnameinfo(
      reinterpret_cast<const sockaddr*>(&storage), host_address_length,
      host, host_length, service, service_length, host_flags);
  const int host_errno = errno;
  if (status == EAI_SYSTEM) {
    darwin_art_bionic_errno_store(AndroidErrno(host_errno));
  }
  return AndroidEai(status);
}

extern "C" DarwinArtBionicDnsFunction darwin_art_bionic_dns_resolve(
    const char* soname, const char* symbol, const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, "libc.so") != 0 ||
      std::strcmp(version, "LIBC") != 0) {
    return nullptr;
  }
#define DNS_SYMBOL(name)                                                       \
  if (std::strcmp(symbol, #name) == 0)                                         \
    return reinterpret_cast<DarwinArtBionicDnsFunction>(                       \
        darwin_art_bionic_dns_##name)
  DNS_SYMBOL(getaddrinfo);
  DNS_SYMBOL(freeaddrinfo);
  DNS_SYMBOL(gai_strerror);
  DNS_SYMBOL(getnameinfo);
#undef DNS_SYMBOL
  return nullptr;
}

extern "C" const char* darwin_art_bionic_dns_capability(
    const char* capability) {
  if (capability == nullptr) return "invalid-capability";
  if (std::strcmp(capability, "localhost-numeric-policy") == 0 ||
      std::strcmp(capability, "android-addrinfo-deep-copy") == 0 ||
      std::strcmp(capability, "numeric-reverse") == 0 ||
      std::strcmp(capability, "retire-then-quiescent-reclaim") == 0) {
    return "supported";
  }
  return "unsupported";
}

extern "C" void darwin_art_bionic_dns_reset_for_test() {
  PreserveErrno preserve;
  std::array<DarwinArtAndroidAddrinfo*, kMaxResults> heads{};
  (void)pthread_mutex_lock(&g_results_lock);
  for (size_t index = 0; index < g_results.size(); ++index) {
    heads[index] = g_results[index].head;
    g_results[index] = ResultSlot{};
  }
  (void)pthread_mutex_unlock(&g_results_lock);
  for (DarwinArtAndroidAddrinfo* head : heads) DestroyList(head);
}

extern "C" size_t darwin_art_bionic_dns_live_results_for_test() {
  PreserveErrno preserve;
  size_t count = 0;
  (void)pthread_mutex_lock(&g_results_lock);
  for (const ResultSlot& slot : g_results) {
    count += slot.head != nullptr && !slot.retired ? 1 : 0;
  }
  (void)pthread_mutex_unlock(&g_results_lock);
  return count;
}

extern "C" size_t darwin_art_bionic_dns_retired_results_for_test() {
  PreserveErrno preserve;
  size_t count = 0;
  (void)pthread_mutex_lock(&g_results_lock);
  for (const ResultSlot& slot : g_results) {
    count += slot.head != nullptr && slot.retired ? 1 : 0;
  }
  (void)pthread_mutex_unlock(&g_results_lock);
  return count;
}
