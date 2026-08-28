#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_errno.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_art_elf_loader.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_filesystem_closes{0};

struct HttpFixtureResult {
  int32_t connected_family;
  int32_t send_calls;
  int32_t recv_calls;
  int32_t eintr_retries;
  int32_t last_android_errno;
  uint32_t response_length;
  char response[256];
};

struct PipeFixtureResult {
  int32_t empty_ready;
  int32_t readable_ready;
  int32_t read_revents;
  int32_t last_android_errno;
  intptr_t write_count;
  intptr_t read_count;
  uint8_t value;
};

struct AndroidEpollEvent {
  uint32_t events;
  uint64_t data;
};

static_assert(offsetof(AndroidEpollEvent, data) == 8);
static_assert(sizeof(AndroidEpollEvent) == 16);

void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "bionic-socket-broker-adapter: FAIL %s\n", message);
    std::abort();
  }
}

DarwinArtElfResolveStatus Resolve(void *,
                                  const DarwinArtElfSymbolRequest *request,
                                  uintptr_t *output,
                                  DarwinArtElfErrorBuffer *) {
  if (request == nullptr || output == nullptr || request->symbol == nullptr ||
      request->version_soname == nullptr || request->version_name == nullptr ||
      std::strcmp(request->version_soname, "libc.so") != 0 ||
      std::strcmp(request->version_name, "LIBC") != 0) {
    return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
  }
  if (std::strcmp(request->symbol, "__errno") == 0) {
    auto function = darwin_art_bionic_errno_resolve(request->symbol);
    if (function != nullptr) {
      *output = reinterpret_cast<uintptr_t>(function);
      return DARWIN_ART_ELF_RESOLVE_FOUND;
    }
  }
  auto dns = darwin_art_bionic_socket_broker_dns_resolve(
      request->version_soname, request->symbol, request->version_name);
  if (dns != nullptr) {
    *output = reinterpret_cast<uintptr_t>(dns);
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }
  auto socket = darwin_art_bionic_socket_broker_resolve(
      request->version_soname, request->symbol, request->version_name);
  if (socket != nullptr) {
    *output = reinterpret_cast<uintptr_t>(socket);
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }
  return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
}

void Serve(int listener) {
  const int client = accept(listener, nullptr, nullptr);
  Check(client >= 0, "accept loopback client");
  std::array<char, 512> request{};
  size_t used = 0;
  while (used < request.size()) {
    const ssize_t count =
        recv(client, request.data() + used, request.size() - used, 0);
    Check(count > 0, "read HTTP request");
    used += static_cast<size_t>(count);
    if (used >= 4 && request[used - 4] == '\r' && request[used - 3] == '\n' &&
        request[used - 2] == '\r' && request[used - 1] == '\n')
      break;
  }
  Check(std::strstr(request.data(), "GET /acceptance HTTP/1.0\r\n") ==
            request.data(),
        "HTTP request path");
  static constexpr char kResponse[] =
      "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nHELLO";
  size_t sent = 0;
  while (sent < sizeof(kResponse) - 1) {
    const ssize_t count =
        send(client, kResponse + sent, sizeof(kResponse) - 1 - sent, 0);
    Check(count > 0, "write HTTP response");
    sent += static_cast<size_t>(count);
  }
  (void)shutdown(client, SHUT_WR);
  (void)close(client);
  (void)close(listener);
}

} // namespace

extern "C" int darwin_art_bionic_fs_close_core(int) {
  g_filesystem_closes.fetch_add(1, std::memory_order_relaxed);
  darwin_art_bionic_errno_store(9);
  return -1;
}

extern "C" intptr_t darwin_art_bionic_fs_read_core(int, void *, size_t) {
  darwin_art_bionic_errno_store(9);
  return -1;
}

extern "C" intptr_t darwin_art_bionic_fs_write_core(int, const void *, size_t) {
  darwin_art_bionic_errno_store(9);
  return -1;
}

extern "C" int darwin_art_bionic_fs_fcntl_core(int, int, intptr_t) {
  darwin_art_bionic_errno_store(9);
  return -1;
}

extern "C" int darwin_art_bionic_fs_dup_host_fd_core(int, int *) { return 0; }

extern "C" int darwin_art_bionic_fs_adopt_host_fd_core(int host_fd) {
  (void)close(host_fd);
  darwin_art_bionic_errno_store(9);
  return -1;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 10;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  Check(!bytes.empty() && !input.bad(), "read Android network fixture");
  Check(darwin_art_bionic_socket_broker_close(10000) == -1 &&
            g_filesystem_closes.load(std::memory_order_relaxed) == 1,
        "filesystem close works before network activation");
  Check(darwin_art_bionic_socket_broker_close(0x40000001) == -1 &&
            g_filesystem_closes.load(std::memory_order_relaxed) == 1,
        "stale central token cannot fall through before activation");
  Check(darwin_art_bionic_socket_broker_activate() == 0,
        "activate socket broker owner");

  int32_t nonblocking_pair[2]{-1, -1};
  int nonblocking = 1;
  int ioctl_handled = 0;
  int ioctl_result = -1;
  int ioctl_errno = 0;
  uint8_t empty_byte = 0;
  Check(darwin_art_bionic_socket_broker_socketpair(1, 1, 0, nonblocking_pair) ==
                0 &&
            darwin_art_bionic_socket_broker_ioctl_dispatch(
                nonblocking_pair[0], 0x5421, &nonblocking, &ioctl_handled,
                &ioctl_result, &ioctl_errno) == 0 &&
            ioctl_handled == 1 && ioctl_result == 0 && ioctl_errno == 0 &&
            (darwin_art_bionic_socket_broker_fcntl(nonblocking_pair[0], 3, 0) &
             2048) != 0 &&
            darwin_art_bionic_socket_broker_read(nonblocking_pair[0],
                                                 &empty_byte, 1) == -1 &&
            darwin_art_bionic_errno_load() == 11,
        "FIONBIO makes empty broker socket return EAGAIN");
  nonblocking = 0;
  ioctl_handled = 0;
  ioctl_result = -1;
  ioctl_errno = 0;
  Check(darwin_art_bionic_socket_broker_ioctl_dispatch(
            nonblocking_pair[0], 0x5421, &nonblocking, &ioctl_handled,
            &ioctl_result, &ioctl_errno) == 0 &&
            ioctl_handled == 1 && ioctl_result == 0 && ioctl_errno == 0 &&
            (darwin_art_bionic_socket_broker_fcntl(nonblocking_pair[0], 3, 0) &
             2048) == 0 &&
            darwin_art_bionic_socket_broker_close(nonblocking_pair[0]) == 0 &&
            darwin_art_bionic_socket_broker_close(nonblocking_pair[1]) == 0,
        "FIONBIO clears shared broker socket status");

  int32_t readable_pair[2]{-1, -1};
  const uint32_t readable_value = 0x12345678;
  int available = -1;
  ioctl_handled = 0;
  ioctl_result = -1;
  ioctl_errno = 0;
  Check(darwin_art_bionic_socket_broker_socketpair(1, 1, 0, readable_pair) ==
                0 &&
            darwin_art_bionic_socket_broker_write(
                readable_pair[0], &readable_value, sizeof(readable_value)) ==
                static_cast<intptr_t>(sizeof(readable_value)) &&
            darwin_art_bionic_socket_broker_ioctl_dispatch(
                readable_pair[1], 0x541b, &available, &ioctl_handled,
                &ioctl_result, &ioctl_errno) == 0 &&
            ioctl_handled == 1 && ioctl_result == 0 && ioctl_errno == 0 &&
            available == static_cast<int>(sizeof(readable_value)) &&
            darwin_art_bionic_socket_broker_close(readable_pair[0]) == 0 &&
            darwin_art_bionic_socket_broker_close(readable_pair[1]) == 0,
        "FIONREAD reports bytes queued on broker socket");

  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  Check(listener >= 0, "create host listener");
  sockaddr_in address{};
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Check(bind(listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) == 0 &&
            listen(listener, 1) == 0,
        "bind host loopback listener");
  socklen_t address_length = sizeof(address);
  Check(getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_length) == 0,
        "get host listener port");
  char port[16]{};
  std::snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
  std::thread server(Serve, listener);

  DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION, &Resolve,
                                  nullptr};
  DarwinArtElfHandle *image = nullptr;
  char message[512]{};
  DarwinArtElfErrorBuffer error{message, sizeof(message), 0};
  Check(darwin_art_elf_load_bytes(bytes.data(), bytes.size(), &options, &image,
                                  &error) == DARWIN_ART_ELF_OK,
        "load Android HTTP fixture");
  Check(darwin_art_elf_run_initializers(image, &error) == DARWIN_ART_ELF_OK,
        "initialize Android HTTP fixture");
  uintptr_t address_value = 0;
  Check(darwin_art_elf_lookup(image, "NetworkFixtureHttp", &address_value,
                              &error) == DARWIN_ART_ELF_OK &&
            address_value != 0,
        "lookup Android HTTP function");
  using Http =
      int (*)(const char *, const char *, int, int, HttpFixtureResult *);
  auto http = reinterpret_cast<Http>(address_value);
  HttpFixtureResult result{};
  errno = EDOM;
  Check(http("127.0.0.1", port, 2, 1, &result) == 0 && errno == EDOM &&
            result.connected_family == 2 && result.last_android_errno == 0 &&
            std::strstr(result.response, "\r\n\r\nHELLO") != nullptr,
        "Android ELF loopback HTTP through broker");
  server.join();
  address_value = 0;
  Check(darwin_art_elf_lookup(image, "PipeFixtureRoundTrip", &address_value,
                              &error) == DARWIN_ART_ELF_OK &&
            address_value != 0,
        "lookup Android pipe/poll function");
  using PipeRoundTrip = int (*)(PipeFixtureResult *);
  PipeFixtureResult pipe_result{};
  auto pipe_round_trip = reinterpret_cast<PipeRoundTrip>(address_value);
  Check(pipe_round_trip(&pipe_result) == 0 && pipe_result.empty_ready == 0 &&
            pipe_result.readable_ready == 1 &&
            (pipe_result.read_revents & POLLIN) != 0 &&
            pipe_result.write_count == 1 && pipe_result.read_count == 1 &&
            pipe_result.value == 0xa5,
        "Android ELF pipe/read/write/blocking-poll round trip");
  const int event_fd = darwin_art_bionic_socket_broker_eventfd(0, 0x800);
  const int epoll_fd = darwin_art_bionic_socket_broker_epoll_create1(0x80000);
  AndroidEpollEvent registration{1, UINT64_C(0x1122334455667788)};
  AndroidEpollEvent ready{};
  uint64_t event_value = 1;
  Check(event_fd >= 0 && epoll_fd >= 0 &&
            darwin_art_bionic_socket_broker_epoll_ctl(epoll_fd, 1, event_fd,
                                                      &registration) == 0 &&
            darwin_art_bionic_socket_broker_epoll_wait(epoll_fd, &ready, 1,
                                                       0) == 0 &&
            darwin_art_bionic_socket_broker_write(event_fd, &event_value,
                                                  sizeof(event_value)) ==
                sizeof(event_value) &&
            darwin_art_bionic_socket_broker_epoll_wait(epoll_fd, &ready, 1,
                                                       0) == 1 &&
            ready.events == 1 && ready.data == registration.data &&
            darwin_art_bionic_socket_broker_read(event_fd, &event_value,
                                                 sizeof(event_value)) ==
                sizeof(event_value) &&
            darwin_art_bionic_socket_broker_close(event_fd) == 0 &&
            darwin_art_bionic_socket_broker_close(epoll_fd) == 0,
        "eventfd readiness through epoll");
  Check(darwin_art_bionic_socket_broker_live_objects() == 0,
        "all central socket objects closed");
  Check(darwin_art_bionic_socket_broker_close(123) == -1,
        "generic close fell back to filesystem owner");
  Check(g_filesystem_closes.load(std::memory_order_relaxed) == 2,
        "filesystem close delegation count");
  Check(darwin_art_bionic_socket_broker_close(0x40000001) == -1 &&
            g_filesystem_closes.load(std::memory_order_relaxed) == 2,
        "stale broker-shaped close cannot alias filesystem token");
  Check(darwin_art_bionic_dns_live_results_for_test() == 0 &&
            darwin_art_bionic_dns_retired_results_for_test() == 1,
        "DNS result retired");
  darwin_art_bionic_dns_reset_for_test();
  Check(darwin_art_elf_unload(&image, &error) == DARWIN_ART_ELF_OK,
        "unload Android HTTP fixture");
  Check(darwin_art_bionic_socket_broker_deactivate() == 0,
        "deactivate quiescent socket owner");
  Check(darwin_art_bionic_socket_broker_close(10001) == -1 &&
            g_filesystem_closes.load(std::memory_order_relaxed) == 3,
        "filesystem close works after network deactivation");
  for (int iteration = 0; iteration < 100; ++iteration) {
    Check(darwin_art_bionic_socket_broker_activate() == 0,
          "reactivate race owner");
    std::atomic<bool> start{false};
    std::thread creator([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      const int descriptor = darwin_art_bionic_socket_broker_socket(2, 1, 0);
      if (descriptor >= 0)
        Check(darwin_art_bionic_socket_broker_close(descriptor) == 0,
              "close racing created socket");
    });
    start.store(true, std::memory_order_release);
    const int first_deactivate = darwin_art_bionic_socket_broker_deactivate();
    creator.join();
    if (first_deactivate != 0)
      Check(darwin_art_bionic_socket_broker_deactivate() == 0,
            "retry deactivate after admitted socket");
    Check(darwin_art_bionic_socket_broker_live_objects() == 0,
          "deactivate race leaked object");
  }
  Check(darwin_art_bionic_socket_broker_activate() == 0,
        "activate DNS lifetime owner");
  DarwinArtAndroidAddrinfo hints{};
  hints.ai_flags = 0x4 | 0x8;
  hints.ai_family = 2;
  hints.ai_socktype = 1;
  DarwinArtAndroidAddrinfo *dns_result = nullptr;
  using GetAddrInfo =
      int (*)(const char *, const char *, const DarwinArtAndroidAddrinfo *,
              DarwinArtAndroidAddrinfo **);
  using FreeAddrInfo = void (*)(DarwinArtAndroidAddrinfo *);
  auto getaddrinfo =
      reinterpret_cast<GetAddrInfo>(darwin_art_bionic_socket_broker_dns_resolve(
          "libc.so", "getaddrinfo", "LIBC"));
  auto freeaddrinfo = reinterpret_cast<FreeAddrInfo>(
      darwin_art_bionic_socket_broker_dns_resolve("libc.so", "freeaddrinfo",
                                                  "LIBC"));
  Check(getaddrinfo != nullptr && freeaddrinfo != nullptr &&
            getaddrinfo("127.0.0.1", "80", &hints, &dns_result) == 0 &&
            dns_result != nullptr,
        "lease DNS result");
  Check(darwin_art_bionic_socket_broker_deactivate() == -1,
        "DNS result blocks reset and deactivate");
  freeaddrinfo(dns_result);
  Check(darwin_art_bionic_socket_broker_deactivate() == 0 &&
            darwin_art_bionic_dns_live_results_for_test() == 0 &&
            darwin_art_bionic_dns_retired_results_for_test() == 0,
        "DNS free drains before reset and deactivate");
  std::fprintf(stderr, "bionic-socket-broker-adapter: PASS Android-ELF=yes "
                       "HTTP=127.0.0.1 pipe-poll=blocking eventfd-epoll=yes "
                       "central-token=yes owner=v6 "
                       "close=generic "
                       "DNS=retired deactivate-race=100 host-errno=preserved "
                       "Internet=no\n");
  return 0;
}
