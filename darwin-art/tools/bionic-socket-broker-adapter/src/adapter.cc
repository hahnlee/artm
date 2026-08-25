#include "darwin_art_bionic_socket_broker.h"

#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fd_broker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#include <vector>

extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);
extern "C" int darwin_art_bionic_fs_close_core(int fd);
extern "C" intptr_t darwin_art_bionic_fs_read_core(int fd, void *buffer,
                                                   size_t count);
extern "C" intptr_t darwin_art_bionic_fs_write_core(int fd, const void *buffer,
                                                    size_t count);

namespace {

constexpr int kAndroidAfInet = 2;
constexpr int kAndroidAfInet6 = 10;
constexpr int kAndroidSockStream = 1;
constexpr int kAndroidSockDgram = 2;
constexpr int kAndroidSockNonblock = 00004000;
constexpr int kAndroidSockCloexec = 02000000;
constexpr int kAndroidSolSocket = 1;
constexpr int kAndroidSoReuseaddr = 2;
constexpr int kAndroidSoType = 3;
constexpr int kAndroidSoError = 4;
constexpr int kAndroidSoSndbuf = 7;
constexpr int kAndroidSoRcvbuf = 8;
constexpr int kAndroidSoKeepalive = 9;
constexpr int kAndroidMsgOob = 0x1;
constexpr int kAndroidMsgPeek = 0x2;
constexpr int kAndroidMsgDontRoute = 0x4;
constexpr int kAndroidMsgDontWait = 0x40;
constexpr int kAndroidMsgEor = 0x80;
constexpr int kAndroidMsgWaitAll = 0x100;
constexpr int kAndroidMsgNoSignal = 0x4000;
constexpr uint32_t kCentralBrokerTokenMarker = UINT32_C(0x40000000);
constexpr uint32_t kCentralBrokerTokenTopMask = UINT32_C(0xc0000000);
constexpr int kAndroidFDupfd = 0;
constexpr int kAndroidFGetfd = 1;
constexpr int kAndroidFSetfd = 2;
constexpr int kAndroidFGetfl = 3;
constexpr int kAndroidFSetfl = 4;
constexpr int kAndroidFDupfdCloexec = 1030;
constexpr int kAndroidFdCloexec = 1;
constexpr int kAndroidOAppend = 1024;
constexpr int kAndroidONonblock = 2048;

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

struct HostFdObject {
  int fd = -1;
};

struct Process {
  DarwinArtFdBroker *broker = nullptr;
  DarwinArtFdOwnerHandle socket_owner = 0;
  DarwinArtFdOwnerHandle pipe_owner = 0;
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

bool FromHostAddress(const sockaddr *host, socklen_t host_length,
                     void *android_address, uint32_t capacity,
                     uint32_t *android_length) {
  if (android_length == nullptr)
    return false;
  if (host->sa_family == AF_INET && host_length >= sizeof(sockaddr_in)) {
    const auto *source = reinterpret_cast<const sockaddr_in *>(host);
    AndroidSockaddrIn result{};
    result.family = kAndroidAfInet;
    result.port = source->sin_port;
    result.address = source->sin_addr.s_addr;
    *android_length = sizeof(result);
    if (android_address != nullptr && capacity != 0) {
      std::memcpy(android_address, &result,
                  capacity < sizeof(result) ? capacity : sizeof(result));
    }
    return true;
  }
  if (host->sa_family == AF_INET6 && host_length >= sizeof(sockaddr_in6)) {
    const auto *source = reinterpret_cast<const sockaddr_in6 *>(host);
    AndroidSockaddrIn6 result{};
    result.family = kAndroidAfInet6;
    result.port = source->sin6_port;
    result.flowinfo = source->sin6_flowinfo;
    std::memcpy(result.address, &source->sin6_addr, sizeof(result.address));
    result.scope_id = source->sin6_scope_id;
    *android_length = sizeof(result);
    if (android_address != nullptr && capacity != 0) {
      std::memcpy(android_address, &result,
                  capacity < sizeof(result) ? capacity : sizeof(result));
    }
    return true;
  }
  return false;
}

bool TranslateOption(int android_level, int android_option, int *host_level,
                     int *host_option) {
  if (android_level == kAndroidSolSocket) {
    *host_level = SOL_SOCKET;
    switch (android_option) {
    case kAndroidSoReuseaddr:
      *host_option = SO_REUSEADDR;
      return true;
    case kAndroidSoType:
      *host_option = SO_TYPE;
      return true;
    case kAndroidSoError:
      *host_option = SO_ERROR;
      return true;
    case kAndroidSoSndbuf:
      *host_option = SO_SNDBUF;
      return true;
    case kAndroidSoRcvbuf:
      *host_option = SO_RCVBUF;
      return true;
    case kAndroidSoKeepalive:
      *host_option = SO_KEEPALIVE;
      return true;
    default:
      return false;
    }
  }
  if (android_level == IPPROTO_TCP && android_option == 1) {
    *host_level = IPPROTO_TCP;
    *host_option = TCP_NODELAY;
    return true;
  }
  return false;
}

intptr_t OwnerRead(void *, uint64_t object, void *bytes, size_t count,
                   int *android_errno) {
  auto *socket = reinterpret_cast<HostFdObject *>(object);
  const ssize_t result = recv(socket->fd, bytes, count, 0);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

intptr_t OwnerWrite(void *, uint64_t object, const void *bytes, size_t count,
                    int *android_errno) {
  auto *socket = reinterpret_cast<HostFdObject *>(object);
  const ssize_t result = send(socket->fd, bytes, count, 0);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

int OwnerPoll(void *, uint64_t object, int16_t events, int16_t *revents,
              int *android_errno) {
  auto *socket = reinterpret_cast<HostFdObject *>(object);
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

int OwnerPollMany(void *, const uint64_t *objects, const int16_t *events,
                  int16_t *revents, size_t count, int timeout_ms,
                  int *android_errno) {
  static_assert(POLLIN == 0x0001 && POLLPRI == 0x0002 && POLLOUT == 0x0004 &&
                POLLERR == 0x0008 && POLLHUP == 0x0010 && POLLNVAL == 0x0020);
  if ((count != 0 &&
       (objects == nullptr || events == nullptr || revents == nullptr)) ||
      timeout_ms < -1) {
    *android_errno = 22;
    return -1;
  }
  std::vector<pollfd> descriptors(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *object = reinterpret_cast<const HostFdObject *>(objects[index]);
    descriptors[index] = pollfd{object->fd, events[index], 0};
  }
  const int result =
      poll(descriptors.data(), static_cast<nfds_t>(count), timeout_ms);
  const int saved = errno;
  if (result >= 0) {
    for (size_t index = 0; index < count; ++index)
      revents[index] = descriptors[index].revents;
  }
  *android_errno = result < 0 ? AndroidErrno(saved) : 0;
  return result;
}

intptr_t PipeOwnerRead(void *, uint64_t object, void *bytes, size_t count,
                       int *android_errno) {
  auto *pipe = reinterpret_cast<HostFdObject *>(object);
  const ssize_t result = read(pipe->fd, bytes, count);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

intptr_t PipeOwnerWrite(void *, uint64_t object, const void *bytes,
                        size_t count, int *android_errno) {
  auto *pipe = reinterpret_cast<HostFdObject *>(object);
  const ssize_t result = write(pipe->fd, bytes, count);
  *android_errno = result < 0 ? AndroidErrno(errno) : 0;
  return result;
}

int OwnerIoctl(void *, uint64_t, uint64_t, void *, int *android_errno) {
  *android_errno = 25;
  return -1;
}

int OwnerClose(void *context, uint64_t object, int *android_errno) {
  auto *process = static_cast<Process *>(context);
  auto *socket = reinterpret_cast<HostFdObject *>(object);
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
  auto *socket = reinterpret_cast<HostFdObject *>(object);
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
  if (request->operation == DARWIN_ART_FD_SOCKET_SHUTDOWN) {
    if (request->argument < 0 || request->argument > 2) {
      *android_errno = 22;
      return -1;
    }
    const int result = shutdown(socket->fd, request->argument);
    *android_errno = result < 0 ? AndroidErrno(errno) : 0;
    return result;
  }
  if (request->operation == DARWIN_ART_FD_SOCKET_SENDTO) {
    if (!TranslateFlags(request->flags, &flags)) {
      *android_errno = 22;
      return -1;
    }
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (!ToHostAddress(request->address, request->address_length, &storage,
                       &length)) {
      *android_errno = 22;
      return -1;
    }
    const ssize_t result =
        sendto(socket->fd, request->input_bytes, request->byte_count, flags,
               reinterpret_cast<const sockaddr *>(&storage), length);
    *android_errno = result < 0 ? AndroidErrno(errno) : 0;
    return result;
  }
  if (request->operation == DARWIN_ART_FD_SOCKET_RECVFROM) {
    if (!TranslateFlags(request->flags, &flags)) {
      *android_errno = 22;
      return -1;
    }
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    const ssize_t result =
        recvfrom(socket->fd, request->output_bytes, request->byte_count, flags,
                 reinterpret_cast<sockaddr *>(&storage), &length);
    if (result < 0) {
      *android_errno = AndroidErrno(errno);
      return -1;
    }
    if (request->output_address != nullptr &&
        !FromHostAddress(reinterpret_cast<const sockaddr *>(&storage), length,
                         request->output_address,
                         request->output_address_capacity,
                         request->output_address_length)) {
      *android_errno = 97;
      return -1;
    }
    *android_errno = 0;
    return result;
  }
  if (request->operation == DARWIN_ART_FD_SOCKET_GETSOCKOPT ||
      request->operation == DARWIN_ART_FD_SOCKET_SETSOCKOPT) {
    int host_level = 0;
    int host_option = 0;
    if (!TranslateOption(request->level, request->option, &host_level,
                         &host_option)) {
      *android_errno = 92;
      return -1;
    }
    if (request->operation == DARWIN_ART_FD_SOCKET_SETSOCKOPT) {
      const int result =
          setsockopt(socket->fd, host_level, host_option, request->option_input,
                     request->option_input_length);
      *android_errno = result < 0 ? AndroidErrno(errno) : 0;
      return result;
    }
    socklen_t length = request->option_output_capacity;
    int result = 0;
    if (request->level == kAndroidSolSocket &&
        request->option == kAndroidSoError) {
      int host_error = 0;
      length = sizeof(host_error);
      result =
          getsockopt(socket->fd, host_level, host_option, &host_error, &length);
      if (result == 0) {
        const int32_t android_error = AndroidErrno(host_error);
        if (request->option_output_capacity < sizeof(android_error)) {
          *android_errno = 22;
          return -1;
        }
        std::memcpy(request->option_output, &android_error,
                    sizeof(android_error));
        length = sizeof(android_error);
      }
    } else if (request->level == kAndroidSolSocket &&
               (request->option == kAndroidSoReuseaddr ||
                request->option == kAndroidSoKeepalive)) {
      int host_value = 0;
      length = sizeof(host_value);
      result =
          getsockopt(socket->fd, host_level, host_option, &host_value, &length);
      if (result == 0) {
        const int32_t android_value = host_value == 0 ? 0 : 1;
        if (request->option_output_capacity < sizeof(android_value)) {
          *android_errno = 22;
          return -1;
        }
        std::memcpy(request->option_output, &android_value,
                    sizeof(android_value));
        length = sizeof(android_value);
      }
    } else {
      result = getsockopt(socket->fd, host_level, host_option,
                          request->option_output, &length);
    }
    if (result == 0)
      *request->option_output_length = length;
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
  DarwinArtFdOwnerV1 callbacks{DARWIN_ART_FD_OWNER_ABI_V4,
                               sizeof(DarwinArtFdOwnerV1),
                               process,
                               &OwnerRead,
                               &OwnerWrite,
                               &OwnerPoll,
                               &OwnerIoctl,
                               &OwnerClose,
                               nullptr,
                               nullptr,
                               &OwnerSocketOperation,
                               &OwnerPollMany};
  if (darwin_art_fd_broker_install_owner(process->broker, DARWIN_ART_FD_SOCKET,
                                         &callbacks, &process->socket_owner) !=
      DARWIN_ART_FD_BROKER_OK) {
    (void)darwin_art_fd_broker_destroy(process->broker);
    delete process;
    return -1;
  }
  DarwinArtFdOwnerV1 pipe_callbacks{DARWIN_ART_FD_OWNER_ABI_V4,
                                    sizeof(DarwinArtFdOwnerV1),
                                    process,
                                    &PipeOwnerRead,
                                    &PipeOwnerWrite,
                                    &OwnerPoll,
                                    &OwnerIoctl,
                                    &OwnerClose,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    &OwnerPollMany};
  if (darwin_art_fd_broker_install_owner(
          process->broker, DARWIN_ART_FD_PIPE, &pipe_callbacks,
          &process->pipe_owner) != DARWIN_ART_FD_BROKER_OK) {
    (void)darwin_art_fd_broker_uninstall_owner(process->broker,
                                               process->socket_owner);
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
  if (darwin_art_fd_broker_uninstall_owner(
          process->broker, process->pipe_owner) != DARWIN_ART_FD_BROKER_OK ||
      darwin_art_fd_broker_uninstall_owner(
          process->broker, process->socket_owner) != DARWIN_ART_FD_BROKER_OK ||
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
  auto *object = new (std::nothrow) HostFdObject{fd};
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
          process->broker, process->socket_owner,
          reinterpret_cast<uint64_t>(object), status_flags, descriptor_flags,
          &guest_fd);
  if (status != DARWIN_ART_FD_BROKER_OK) {
    int ignored = 0;
    (void)OwnerClose(process, reinterpret_cast<uint64_t>(object), &ignored);
    return Fail(BrokerFailure(status), -1);
  }
  return guest_fd;
}

extern "C" int darwin_art_bionic_socket_broker_pipe(int32_t descriptors[2]) {
  PreserveErrno preserve;
  if (descriptors == nullptr)
    return Fail(14, -1);
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);

  int host[2] = {-1, -1};
  if (pipe(host) != 0)
    return Fail(AndroidErrno(errno), -1);
  if (fcntl(host[0], F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(host[1], F_SETFD, FD_CLOEXEC) != 0) {
    const int error = AndroidErrno(errno);
    (void)close(host[0]);
    (void)close(host[1]);
    return Fail(error, -1);
  }

  auto *read_end = new (std::nothrow) HostFdObject{host[0]};
  auto *write_end = new (std::nothrow) HostFdObject{host[1]};
  if (read_end == nullptr || write_end == nullptr) {
    delete read_end;
    delete write_end;
    (void)close(host[0]);
    (void)close(host[1]);
    return Fail(12, -1);
  }
  process->objects.fetch_add(2, std::memory_order_release);

  int guest[2] = {-1, -1};
  DarwinArtFdBrokerStatus status = darwin_art_fd_broker_publish(
      process->broker, process->pipe_owner,
      reinterpret_cast<uint64_t>(read_end), &guest[0]);
  if (status != DARWIN_ART_FD_BROKER_OK) {
    int ignored = 0;
    (void)OwnerClose(process, reinterpret_cast<uint64_t>(read_end), &ignored);
    (void)OwnerClose(process, reinterpret_cast<uint64_t>(write_end), &ignored);
    return Fail(BrokerFailure(status), -1);
  }
  status = darwin_art_fd_broker_publish(process->broker, process->pipe_owner,
                                        reinterpret_cast<uint64_t>(write_end),
                                        &guest[1]);
  if (status != DARWIN_ART_FD_BROKER_OK) {
    DarwinArtFdIoResult ignored_result{};
    (void)darwin_art_fd_broker_close_owned(process->broker, process->pipe_owner,
                                           guest[0], &ignored_result);
    int ignored_error = 0;
    (void)OwnerClose(process, reinterpret_cast<uint64_t>(write_end),
                     &ignored_error);
    return Fail(BrokerFailure(status), -1);
  }
  descriptors[0] = guest[0];
  descriptors[1] = guest[1];
  return 0;
}

extern "C" intptr_t darwin_art_bionic_socket_broker_read(int fd, void *bytes,
                                                         size_t count) {
  if ((static_cast<uint32_t>(fd) & kCentralBrokerTokenTopMask) !=
      kCentralBrokerTokenMarker)
    return darwin_art_bionic_fs_read_core(fd, bytes, count);
  PreserveErrno preserve;
  if (bytes == nullptr && count != 0)
    return Fail(14, intptr_t{-1});
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(9, intptr_t{-1});
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status =
      darwin_art_fd_broker_read(process->broker, fd, bytes, count, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  return result.value < 0 ? Fail(result.android_errno, intptr_t{-1})
                          : result.value;
}

extern "C" intptr_t
darwin_art_bionic_socket_broker_write(int fd, const void *bytes, size_t count) {
  if ((static_cast<uint32_t>(fd) & kCentralBrokerTokenTopMask) !=
      kCentralBrokerTokenMarker)
    return darwin_art_bionic_fs_write_core(fd, bytes, count);
  PreserveErrno preserve;
  if (bytes == nullptr && count != 0)
    return Fail(14, intptr_t{-1});
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(9, intptr_t{-1});
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status =
      darwin_art_fd_broker_write(process->broker, fd, bytes, count, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  return result.value < 0 ? Fail(result.android_errno, intptr_t{-1})
                          : result.value;
}

extern "C" int
darwin_art_bionic_socket_broker_poll(DarwinArtBionicPollFd *descriptors,
                                     size_t count, int timeout_ms) {
  static_assert(sizeof(DarwinArtBionicPollFd) == sizeof(DarwinArtFdPollEntry));
  static_assert(alignof(DarwinArtBionicPollFd) ==
                alignof(DarwinArtFdPollEntry));
  PreserveErrno preserve;
  if (timeout_ms < -1)
    return Fail(22, -1);
  if (count != 0 && descriptors == nullptr)
    return Fail(14, -1);
  if (count > 65536)
    return Fail(22, -1);
  if (count == 0) {
    const int value = poll(nullptr, 0, timeout_ms);
    return value < 0 ? Fail(AndroidErrno(errno), -1) : value;
  }
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  std::vector<DarwinArtFdPollEntry> entries(count);
  for (size_t index = 0; index < count; ++index) {
    entries[index] = DarwinArtFdPollEntry{descriptors[index].fd,
                                          descriptors[index].events, 0};
  }
  DarwinArtFdIoResult result{};
  const DarwinArtFdBrokerStatus status = darwin_art_fd_broker_poll_wait(
      process->broker, entries.data(), count, timeout_ms, &result);
  for (size_t index = 0; index < count; ++index)
    descriptors[index].revents = entries[index].revents;
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  return result.value < 0 ? Fail(result.android_errno, -1)
                          : static_cast<int>(result.value);
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
      process->broker, process->socket_owner, fd, &request, &result);
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
      process->broker, process->socket_owner, fd, &request, &result);
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
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  if (result.value < 0)
    return Fail(result.android_errno, intptr_t{-1});
  return result.value;
}

extern "C" intptr_t
darwin_art_bionic_socket_broker_sendto(int fd, const void *bytes, size_t count,
                                       int flags, const void *address,
                                       uint32_t address_length) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, intptr_t{-1});
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_SENDTO);
  request.flags = flags;
  request.input_bytes = bytes;
  request.byte_count = count;
  request.address = address;
  request.address_length = address_length;
  DarwinArtFdIoResult result{};
  const auto status = darwin_art_fd_broker_socket_operation(
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  return result.value < 0 ? Fail(result.android_errno, intptr_t{-1})
                          : result.value;
}

extern "C" intptr_t
darwin_art_bionic_socket_broker_recvfrom(int fd, void *bytes, size_t count,
                                         int flags, void *address,
                                         uint32_t *address_length) {
  PreserveErrno preserve;
  if (address_length == nullptr)
    return Fail(14, intptr_t{-1});
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, intptr_t{-1});
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_RECVFROM);
  request.flags = flags;
  request.output_bytes = bytes;
  request.byte_count = count;
  request.output_address = address;
  request.output_address_capacity = *address_length;
  request.output_address_length = address_length;
  DarwinArtFdIoResult result{};
  const auto status = darwin_art_fd_broker_socket_operation(
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), intptr_t{-1});
  return result.value < 0 ? Fail(result.android_errno, intptr_t{-1})
                          : result.value;
}

extern "C" int darwin_art_bionic_socket_broker_getsockopt(int fd, int level,
                                                          int option,
                                                          void *value,
                                                          uint32_t *length) {
  PreserveErrno preserve;
  if (value == nullptr || length == nullptr)
    return Fail(14, -1);
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_GETSOCKOPT);
  request.level = level;
  request.option = option;
  request.option_output = value;
  request.option_output_capacity = *length;
  request.option_output_length = length;
  DarwinArtFdIoResult result{};
  const auto status = darwin_art_fd_broker_socket_operation(
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  return result.value < 0 ? Fail(result.android_errno, -1)
                          : static_cast<int>(result.value);
}

extern "C" int darwin_art_bionic_socket_broker_setsockopt(int fd, int level,
                                                          int option,
                                                          const void *value,
                                                          uint32_t length) {
  PreserveErrno preserve;
  if (value == nullptr)
    return Fail(14, -1);
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_SETSOCKOPT);
  request.level = level;
  request.option = option;
  request.option_input = value;
  request.option_input_length = length;
  DarwinArtFdIoResult result{};
  const auto status = darwin_art_fd_broker_socket_operation(
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  return result.value < 0 ? Fail(result.android_errno, -1)
                          : static_cast<int>(result.value);
}

extern "C" int darwin_art_bionic_socket_broker_shutdown(int fd, int how) {
  PreserveErrno preserve;
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  DarwinArtFdSocketRequestV1 request = Request(DARWIN_ART_FD_SOCKET_SHUTDOWN);
  request.argument = how;
  DarwinArtFdIoResult result{};
  const auto status = darwin_art_fd_broker_socket_operation(
      process->broker, process->socket_owner, fd, &request, &result);
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  return result.value < 0 ? Fail(result.android_errno, -1)
                          : static_cast<int>(result.value);
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

extern "C" int darwin_art_bionic_socket_broker_fcntl(int fd, int command,
                                                     intptr_t argument) {
  PreserveErrno preserve;
  if ((static_cast<uint32_t>(fd) & kCentralBrokerTokenTopMask) !=
      kCentralBrokerTokenMarker) {
    return Fail(9, -1);
  }
  ProcessLease lease;
  Process *process = lease.get();
  if (process == nullptr)
    return Fail(38, -1);
  int value = 0;
  DarwinArtFdBrokerStatus status = DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  switch (command) {
  case kAndroidFDupfd:
    status = darwin_art_fd_broker_dup(process->broker, fd, &value);
    break;
  case kAndroidFDupfdCloexec:
    status = darwin_art_fd_broker_fcntl_dupfd_cloexec(
        process->broker, fd, static_cast<int>(argument), &value);
    break;
  case kAndroidFGetfd:
    status =
        darwin_art_fd_broker_get_descriptor_flags(process->broker, fd, &value);
    break;
  case kAndroidFSetfd:
    if ((argument & ~kAndroidFdCloexec) != 0)
      return Fail(22, -1);
    status = darwin_art_fd_broker_set_descriptor_flags(
        process->broker, fd, static_cast<int>(argument));
    break;
  case kAndroidFGetfl:
    status = darwin_art_fd_broker_get_status_flags(process->broker, fd, &value);
    if (status == DARWIN_ART_FD_BROKER_OK)
      value |= 2;
    break;
  case kAndroidFSetfl:
    status = darwin_art_fd_broker_set_status_flags(
        process->broker, fd,
        static_cast<int>(argument) & (kAndroidOAppend | kAndroidONonblock));
    break;
  default:
    return Fail(22, -1);
  }
  if (status != DARWIN_ART_FD_BROKER_OK)
    return Fail(BrokerFailure(status), -1);
  return command == kAndroidFSetfd || command == kAndroidFSetfl ? 0 : value;
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
  if (std::strcmp(symbol, "pipe") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_pipe);
  if (std::strcmp(symbol, "read") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_read);
  if (std::strcmp(symbol, "write") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_write);
  if (std::strcmp(symbol, "poll") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_poll);
  if (std::strcmp(symbol, "connect") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_connect);
  if (std::strcmp(symbol, "send") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_send);
  if (std::strcmp(symbol, "recv") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_recv);
  if (std::strcmp(symbol, "sendto") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_sendto);
  if (std::strcmp(symbol, "recvfrom") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_recvfrom);
  if (std::strcmp(symbol, "getsockopt") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_getsockopt);
  if (std::strcmp(symbol, "setsockopt") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_setsockopt);
  if (std::strcmp(symbol, "shutdown") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_shutdown);
  if (std::strcmp(symbol, "close") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_close);
  if (std::strcmp(symbol, "fcntl") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_socket_broker_fcntl);
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
  if (std::strcmp(symbol, "gai_strerror") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_dns_gai_strerror);
  if (std::strcmp(symbol, "inet_ntop") == 0)
    return reinterpret_cast<DarwinArtBionicSocketBrokerFunction>(
        &darwin_art_bionic_dns_inet_ntop);
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
