#include "darwin_art_bionic_socket_broker.h"

#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fd_broker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);
extern "C" int darwin_art_bionic_fs_close_core(int fd);

namespace {

constexpr int kAndroidAfInet = 2;
constexpr int kAndroidAfInet6 = 10;
constexpr int kAndroidSockStream = 1;
constexpr int kAndroidSockDgram = 2;
constexpr int kAndroidSockNonblock = 00004000;
constexpr int kAndroidSockCloexec = 02000000;
constexpr int kAndroidMsgOob = 0x1;
constexpr int kAndroidMsgPeek = 0x2;
constexpr int kAndroidMsgDontRoute = 0x4;
constexpr int kAndroidMsgDontWait = 0x40;
constexpr int kAndroidMsgEor = 0x80;
constexpr int kAndroidMsgWaitAll = 0x100;
constexpr int kAndroidMsgNoSignal = 0x4000;
constexpr uint32_t kCentralBrokerTokenMarker = UINT32_C(0x40000000);
constexpr uint32_t kCentralBrokerTokenTopMask = UINT32_C(0xc0000000);

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

struct SocketObject {
  int fd = -1;
};

struct Process {
  DarwinArtFdBroker *broker = nullptr;
  DarwinArtFdOwnerHandle owner = 0;
  std::mutex mutex;
  std::condition_variable changed;
  size_t active = 0;
  bool draining = false;
  std::atomic<size_t> objects{0};
  std::atomic<size_t> dns_results{0};
};

std::mutex g_process_mutex;
Process *g_process = nullptr;

class PreserveErrno {
public:
  PreserveErrno() : saved_(errno) {}
  ~PreserveErrno() { errno = saved_; }

private:
  int saved_;
};

int32_t AndroidErrno(int error) {
  switch (error) {
  case 0:
    return 0;
  case EPERM:
    return 1;
  case ENOENT:
    return 2;
  case EINTR:
    return 4;
  case EIO:
    return 5;
  case EBADF:
    return 9;
  case EAGAIN:
    return 11;
  case ENOMEM:
    return 12;
  case EACCES:
    return 13;
  case EFAULT:
    return 14;
  case EBUSY:
    return 16;
  case EEXIST:
    return 17;
  case EINVAL:
    return 22;
  case EMFILE:
    return 24;
  case ENOTTY:
    return 25;
  case ENOSPC:
    return 28;
  case EPIPE:
    return 32;
  case EDOM:
    return 33;
  case ERANGE:
    return 34;
  case ENOSYS:
    return 38;
  case EOVERFLOW:
    return 75;
  case ENOTSOCK:
    return 88;
  case EDESTADDRREQ:
    return 89;
  case EMSGSIZE:
    return 90;
  case EPROTOTYPE:
    return 91;
  case ENOPROTOOPT:
    return 92;
  case EPROTONOSUPPORT:
    return 93;
  case ESOCKTNOSUPPORT:
    return 94;
  case EOPNOTSUPP:
    return 95;
  case EAFNOSUPPORT:
    return 97;
  case EADDRINUSE:
    return 98;
  case EADDRNOTAVAIL:
    return 99;
  case ENETDOWN:
    return 100;
  case ENETUNREACH:
    return 101;
  case ENETRESET:
    return 102;
  case ECONNABORTED:
    return 103;
  case ECONNRESET:
    return 104;
  case ENOBUFS:
    return 105;
  case EISCONN:
    return 106;
  case ENOTCONN:
    return 107;
  case ETIMEDOUT:
    return 110;
  case ECONNREFUSED:
    return 111;
  case EHOSTUNREACH:
    return 113;
  case EALREADY:
    return 114;
  case EINPROGRESS:
    return 115;
  default:
    return 5;
  }
}

template <typename T> T Fail(int error, T value) {
  darwin_art_bionic_errno_store(error);
  return value;
}

Process *AcquireProcess() {
  std::lock_guard global(g_process_mutex);
  Process *process = g_process;
  if (process == nullptr)
    return nullptr;
  std::lock_guard local(process->mutex);
  if (process->draining)
    return nullptr;
  ++process->active;
  return process;
}

void ReleaseProcess(Process *process) {
  std::lock_guard lock(process->mutex);
  if (process->active == 0)
    std::abort();
  --process->active;
  if (process->active == 0)
    process->changed.notify_all();
}

class ProcessLease {
public:
  ProcessLease() : process_(AcquireProcess()) {}
  ~ProcessLease() {
    if (process_ != nullptr)
      ReleaseProcess(process_);
  }
  Process *get() const { return process_; }

private:
  Process *process_;
};

bool TranslateDomain(int android, int *host) {
  if (android == kAndroidAfInet) {
    *host = AF_INET;
    return true;
  }
  if (android == kAndroidAfInet6) {
    *host = AF_INET6;
    return true;
  }
  return false;
}

bool TranslateType(int android, int *host, bool *nonblocking) {
  constexpr int kKnown = kAndroidSockNonblock | kAndroidSockCloexec;
  if ((android & ~(kKnown | 0xf)) != 0)
    return false;
  if ((android & 0xf) == kAndroidSockStream)
    *host = SOCK_STREAM;
  else if ((android & 0xf) == kAndroidSockDgram)
    *host = SOCK_DGRAM;
  else
    return false;
  *nonblocking = (android & kAndroidSockNonblock) != 0;
  return true;
}

bool TranslateProtocol(int android, int *host) {
  if (android == 0) {
    *host = 0;
    return true;
  }
  if (android == 6) {
    *host = IPPROTO_TCP;
    return true;
  }
  if (android == 17) {
    *host = IPPROTO_UDP;
    return true;
  }
  return false;
}

bool TranslateFlags(int android, int *host) {
  constexpr int kKnown = kAndroidMsgOob | kAndroidMsgPeek |
                         kAndroidMsgDontRoute | kAndroidMsgDontWait |
                         kAndroidMsgEor | kAndroidMsgWaitAll |
                         kAndroidMsgNoSignal;
  if ((android & ~kKnown) != 0)
    return false;
  int value = 0;
  if ((android & kAndroidMsgOob) != 0)
    value |= MSG_OOB;
  if ((android & kAndroidMsgPeek) != 0)
    value |= MSG_PEEK;
  if ((android & kAndroidMsgDontRoute) != 0)
    value |= MSG_DONTROUTE;
  if ((android & kAndroidMsgDontWait) != 0)
    value |= MSG_DONTWAIT;
  if ((android & kAndroidMsgEor) != 0)
    value |= MSG_EOR;
  if ((android & kAndroidMsgWaitAll) != 0)
    value |= MSG_WAITALL;
  *host = value;
  return true;
}

bool ToHostAddress(const void *address, uint32_t length,
                   sockaddr_storage *storage, socklen_t *host_length) {
  if (address == nullptr || length < sizeof(uint16_t))
    return false;
  uint16_t family = 0;
  std::memcpy(&family, address, sizeof(family));
  std::memset(storage, 0, sizeof(*storage));
  if (family == kAndroidAfInet && length >= sizeof(AndroidSockaddrIn)) {
    AndroidSockaddrIn android{};
    std::memcpy(&android, address, sizeof(android));
    sockaddr_in host{};
    host.sin_len = sizeof(host);
    host.sin_family = AF_INET;
    host.sin_port = android.port;
    host.sin_addr.s_addr = android.address;
    std::memcpy(storage, &host, sizeof(host));
    *host_length = sizeof(host);
    return true;
  }
  if (family == kAndroidAfInet6 && length >= sizeof(AndroidSockaddrIn6)) {
    AndroidSockaddrIn6 android{};
    std::memcpy(&android, address, sizeof(android));
    sockaddr_in6 host{};
    host.sin6_len = sizeof(host);
    host.sin6_family = AF_INET6;
    host.sin6_port = android.port;
    host.sin6_flowinfo = android.flowinfo;
    std::memcpy(&host.sin6_addr, android.address, sizeof(android.address));
    host.sin6_scope_id = android.scope_id;
    std::memcpy(storage, &host, sizeof(host));
    *host_length = sizeof(host);
    return true;
  }
  return false;
}

intptr_t OwnerRead(void *, uint64_t object, void *bytes, size_t count,
                   int *android_errno) {
  auto *socket = reinterpret_cast<SocketObject *>(object);
  const ssize_t result = recv(socket->fd, bytes, count, 0);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

intptr_t OwnerWrite(void *, uint64_t object, const void *bytes, size_t count,
                    int *android_errno) {
  auto *socket = reinterpret_cast<SocketObject *>(object);
  const ssize_t result = send(socket->fd, bytes, count, 0);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

int OwnerPoll(void *, uint64_t object, int16_t events, int16_t *revents,
              int *android_errno) {
  auto *socket = reinterpret_cast<SocketObject *>(object);
  pollfd descriptor{socket->fd, events, 0};
  const int result = poll(&descriptor, 1, 0);
  if (result < 0) {
    *android_errno = AndroidErrno(errno);
    return -1;
  }
  *revents = descriptor.revents;
  *android_errno = 0;
  return result;
}

int OwnerIoctl(void *, uint64_t, uint64_t, void *, int *android_errno) {
  *android_errno = 25;
  return -1;
}

int OwnerClose(void *context, uint64_t object, int *android_errno) {
  auto *process = static_cast<Process *>(context);
  auto *socket = reinterpret_cast<SocketObject *>(object);
  const int result = close(socket->fd);
  const int saved = errno;
  delete socket;
  if (process->objects.fetch_sub(1, std::memory_order_acq_rel) == 0)
    std::abort();
  *android_errno = result < 0 ? AndroidErrno(saved) : 0;
  return result;
}

intptr_t OwnerSocketOperation(void *, uint64_t object,
                              const DarwinArtFdSocketRequestV1 *request,
                              DarwinArtFdSocketAcceptResultV1 *,
                              int *android_errno) {
  auto *socket = reinterpret_cast<SocketObject *>(object);
  if (request->operation == DARWIN_ART_FD_SOCKET_CONNECT) {
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (!ToHostAddress(request->address, request->address_length, &storage,
                       &length)) {
      *android_errno = 22;
      return -1;
    }
    const int result =
        connect(socket->fd, reinterpret_cast<sockaddr *>(&storage), length);
    *android_errno = result < 0 ? AndroidErrno(errno) : 0;
    return result;
  }
  int flags = 0;
  if ((request->operation == DARWIN_ART_FD_SOCKET_SEND ||
       request->operation == DARWIN_ART_FD_SOCKET_RECV) &&
      !TranslateFlags(request->flags, &flags)) {
    *android_errno = 22;
    return -1;
  }
  if (request->operation == DARWIN_ART_FD_SOCKET_SEND) {
    const ssize_t result =
        send(socket->fd, request->input_bytes, request->byte_count, flags);
    *android_errno = result < 0 ? AndroidErrno(errno) : 0;
    return result;
  }
  if (request->operation == DARWIN_ART_FD_SOCKET_RECV) {
    const ssize_t result =
        recv(socket->fd, request->output_bytes, request->byte_count, flags);
    *android_errno = result < 0 ? AndroidErrno(errno) : 0;
    return result;
  }
  *android_errno = 38;
  return -1;
}

int BrokerFailure(DarwinArtFdBrokerStatus status) {
  if (status == DARWIN_ART_FD_BROKER_STALE ||
      status == DARWIN_ART_FD_BROKER_WRONG_OWNER ||
      status == DARWIN_ART_FD_BROKER_WRONG_KIND)
    return 9;
  if (status == DARWIN_ART_FD_BROKER_EXHAUSTED)
    return 24;
  if (status == DARWIN_ART_FD_BROKER_UNSUPPORTED)
    return 38;
  return 22;
}

DarwinArtFdSocketRequestV1 Request(uint32_t operation) {
  DarwinArtFdSocketRequestV1 request{};
  request.abi_version = DARWIN_ART_FD_SOCKET_REQUEST_ABI_V1;
  request.struct_size = sizeof(request);
  request.operation = operation;
  return request;
}

} // namespace

extern "C" int darwin_art_bionic_socket_broker_activate() {
  PreserveErrno preserve;
  std::lock_guard global(g_process_mutex);
  if (g_process != nullptr)
    return -1;
  auto *process = new (std::nothrow) Process();
  if (process == nullptr)
    return -1;
  process->broker = darwin_art_fd_broker_create();
  if (process->broker == nullptr) {
    delete process;
    return -1;
  }
  DarwinArtFdOwnerV1 callbacks{DARWIN_ART_FD_OWNER_ABI_V3,
                               sizeof(DarwinArtFdOwnerV1),
                               process,
                               &OwnerRead,
                               &OwnerWrite,
                               &OwnerPoll,
                               &OwnerIoctl,
                               &OwnerClose,
                               nullptr,
                               nullptr,
                               &OwnerSocketOperation};
  if (darwin_art_fd_broker_install_owner(process->broker, DARWIN_ART_FD_SOCKET,
                                         &callbacks, &process->owner) !=
      DARWIN_ART_FD_BROKER_OK) {
    (void)darwin_art_fd_broker_destroy(process->broker);
    delete process;
    return -1;
  }
  g_process = process;
  return 0;
}

extern "C" int darwin_art_bionic_socket_broker_deactivate() {
  PreserveErrno preserve;
  Process *process = nullptr;
  {
    std::lock_guard global(g_process_mutex);
    process = g_process;
    if (process == nullptr)
      return -1;
    std::unique_lock local(process->mutex);
    process->draining = true;
    g_process = nullptr;
    process->changed.wait(local, [&] { return process->active == 0; });
    if (process->objects.load(std::memory_order_acquire) != 0 ||
        process->dns_results.load(std::memory_order_acquire) != 0) {
      process->draining = false;
      g_process = process;
      return -1;
    }
  }
  darwin_art_bionic_dns_reset_for_test();
  if (darwin_art_fd_broker_uninstall_owner(process->broker, process->owner) !=
          DARWIN_ART_FD_BROKER_OK ||
      darwin_art_fd_broker_destroy(process->broker) !=
          DARWIN_ART_FD_BROKER_OK) {
    std::abort();
  }
  delete process;
  return 0;
}

extern "C" int darwin_art_bionic_socket_broker_socket(int domain, int type,
                                                      int protocol) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  int host_domain = 0;
  int host_type = 0;
  int host_protocol = 0;
  bool nonblocking = false;
  if (!TranslateDomain(domain, &host_domain))
    return Fail(97, -1);
  if (!TranslateType(type, &host_type, &nonblocking))
    return Fail(94, -1);
  if (!TranslateProtocol(protocol, &host_protocol))
    return Fail(93, -1);
  const int fd = socket(host_domain, host_type, host_protocol);
  if (fd < 0)
    return Fail(AndroidErrno(errno), -1);
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      (nonblocking &&
       (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0))) {
    const int error = AndroidErrno(errno);
    (void)close(fd);
    return Fail(error, -1);
  }
#ifdef SO_NOSIGPIPE
  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
    const int error = AndroidErrno(errno);
    (void)close(fd);
    return Fail(error, -1);
  }
#endif
  auto *object = new (std::nothrow) SocketObject{fd};
  if (object == nullptr) {
    (void)close(fd);
    return Fail(12, -1);
  }
  process->objects.fetch_add(1, std::memory_order_release);
  int guest_fd = -1;
  const int status_flags = nonblocking ? DARWIN_ART_FD_STATUS_NONBLOCK : 0;
  const int descriptor_flags =
      (type & kAndroidSockCloexec) != 0 ? DARWIN_ART_FD_CLOEXEC : 0;
  const DarwinArtFdBrokerStatus status =
      darwin_art_fd_broker_publish_with_flags(
          process->broker, process->owner, reinterpret_cast<uint64_t>(object),
          status_flags, descriptor_flags, &guest_fd);
  if (status != DARWIN_ART_FD_BROKER_OK) {
    int ignored = 0;
    (void)OwnerClose(process, reinterpret_cast<uint64_t>(object), &ignored);
    return Fail(BrokerFailure(status), -1);
  }
  return guest_fd;
}

extern "C" int darwin_art_bionic_socket_broker_connect(int fd,
                                                       const void *address,
                                                       uint32_t length) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_CONNECT);
  request.address = address;
  request.address_length = length;
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status = darwin_art_fd_broker_socket_operation(
      process->broker, process->owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  if (result.value < 0)
    return Fail(result.android_errno, -1);
  return static_cast<int>(result.value);
}

extern "C" intptr_t darwin_art_bionic_socket_broker_send(int fd,
                                                         const void *bytes,
                                                         size_t count,
                                                         int flags) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, intptr_t{-1});
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_SEND);
  request.flags = flags;
  request.input_bytes = bytes;
  request.byte_count = count;
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status = darwin_art_fd_broker_socket_operation(
      process->broker, process->owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  if (result.value < 0)
    return Fail(result.android_errno, intptr_t{-1});
  return result.value;
}

extern "C" intptr_t darwin_art_bionic_socket_broker_recv(int fd, void *bytes,
                                                         size_t count,
                                                         int flags) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, intptr_t{-1});
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_RECV);
  request.flags = flags;
  request.output_bytes = bytes;
  request.byte_count = count;
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status = darwin_art_fd_broker_socket_operation(
      process->broker, process->owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  if (result.value < 0)
    return Fail(result.android_errno, intptr_t{-1});
  return result.value;
}

extern "C" int darwin_art_bionic_socket_broker_close(int fd) {
  const uint32_t token = static_cast<uint32_t>(fd);
  if ((token & kCentralBrokerTokenTopMask) != kCentralBrokerTokenMarker)
    return darwin_art_bionic_fs_close_core(fd);
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(9, -1);
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status =
      darwin_art_fd_broker_close(process->broker, fd, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  if (result.value < 0)
    return Fail(result.android_errno, -1);
  return static_cast<int>(result.value);
}

extern "C" int darwin_art_bionic_socket_broker_getaddrinfo(
    const char *node, const char *service,
    const DarwinArtAndroidAddrinfo *hints, DarwinArtAndroidAddrinfo **result) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr) {
    if (result != nullptr)
      *result = nullptr;
    return 11;
  }
  const int status =
      darwin_art_bionic_dns_getaddrinfo(node, service, hints, result);
  if (status == 0 && result != nullptr && *result != nullptr)
    process->dns_results.fetch_add(1, std::memory_order_acq_rel);
  return status;
}

extern "C" void
darwin_art_bionic_socket_broker_freeaddrinfo(DarwinArtAndroidAddrinfo *result) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return;
  darwin_art_bionic_dns_freeaddrinfo(result);
  if (result != nullptr) {
    const size_t previous =
        process->dns_results.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0)
      std::abort();
  }
}

extern "C" DarwinArtBionicSocketBrokerFunction
darwin_art_bionic_socket_broker_resolve(const char *soname, const char *symbol,
                                        const char *version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, "libc.so") != 0 || std::strcmp(version, "LIBC") != 0)
    return nullptr;
  if (std::strcmp(symbol, "socket") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_socket);
  if (std::strcmp(symbol, "connect") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_connect);
  if (std::strcmp(symbol, "send") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_send);
  if (std::strcmp(symbol, "recv") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_recv);
  if (std::strcmp(symbol, "close") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_close);
  return nullptr;
}

extern "C" DarwinArtBionicSocketBrokerFunction
darwin_art_bionic_socket_broker_dns_resolve(const char *soname,
                                            const char *symbol,
                                            const char *version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, "libc.so") != 0 || std::strcmp(version, "LIBC") != 0)
    return nullptr;
  if (std::strcmp(symbol, "getaddrinfo") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_getaddrinfo);
  if (std::strcmp(symbol, "freeaddrinfo") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_freeaddrinfo);
  return nullptr;
}

extern "C" size_t darwin_art_bionic_socket_broker_live_objects() {
  std::lock_guard global(g_process_mutex);
  return g_process == nullptr
             ? 0
             : g_process->objects.load(std::memory_order_acquire);
}

extern "C" int darwin_art_bionic_socket_broker_is_active() {
  std::lock_guard global(g_process_mutex);
  return g_process == nullptr ? 0 : 1;
}
