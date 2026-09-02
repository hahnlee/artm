#include "darwin_art_bionic_dns.h"
#include "darwin_art_elf_loader.h"

#include <errno.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "bionic-dns-facade: FAIL %s\n", message);
    std::abort();
  }
}

DarwinArtElfResolveStatus Resolve(void*,
                                  const DarwinArtElfSymbolRequest* request,
                                  uintptr_t* output,
                                  DarwinArtElfErrorBuffer*) {
  if (request == nullptr || output == nullptr) {
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  const auto function = darwin_art_bionic_dns_resolve(
      request->version_soname, request->symbol, request->version_name);
  if (function == nullptr) return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
  *output = reinterpret_cast<uintptr_t>(function);
  return DARWIN_ART_ELF_RESOLVE_FOUND;
}

template <typename Function>
Function Lookup(DarwinArtElfHandle* image, const char* name) {
  uintptr_t address = 0;
  char message[256]{};
  DarwinArtElfErrorBuffer error{message, sizeof(message), 0};
  Check(darwin_art_elf_lookup(image, name, &address, &error) ==
            DARWIN_ART_ELF_OK &&
            address != 0,
        name);
  return reinterpret_cast<Function>(address);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 10;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  if (bytes.empty() || input.bad()) return 11;

  const char* symbols[] = {
      "freeaddrinfo", "gai_strerror", "getaddrinfo", "getnameinfo",
      "inet_ntop"};
  for (const char* symbol : symbols) {
    Check(darwin_art_bionic_dns_resolve("libc.so", symbol, "LIBC") != nullptr,
          "exact resolver entry");
  }
  Check(darwin_art_bionic_dns_resolve("libSystem.B.dylib", "getaddrinfo",
                                      "LIBC") == nullptr &&
            darwin_art_bionic_dns_resolve("libc.so", "getaddrinfo", nullptr) ==
                nullptr &&
            darwin_art_bionic_dns_resolve("libc.so", "gethostbyname", "LIBC") ==
                nullptr,
        "closed resolver");

  DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION, Resolve, nullptr};
  DarwinArtElfHandle* image = nullptr;
  char message[512]{};
  DarwinArtElfErrorBuffer error{message, sizeof(message), 0};
  Check(darwin_art_elf_load_bytes(bytes.data(), bytes.size(), &options, &image,
                                  &error) == DARWIN_ART_ELF_OK,
        "load actual Android ELF");
  Check(darwin_art_elf_run_initializers(image, &error) == DARWIN_ART_ELF_OK,
        "run initializers");

  using LookupAddress = int (*)(const char*, const char*, int, int,
                                DarwinArtAndroidAddrinfo**);
  using LookupPassive = int (*)(const char*, int,
                                DarwinArtAndroidAddrinfo**);
  using Count = int (*)(const DarwinArtAndroidAddrinfo*, int);
  using Reverse = int (*)(const DarwinArtAndroidAddrinfo*, char*, size_t,
                          char*, size_t);
  using ReverseRejected = int (*)(const DarwinArtAndroidAddrinfo*, char*,
                                  size_t);
  using Free = void (*)(DarwinArtAndroidAddrinfo*);
  using ErrorString = const char* (*)(int);
  using Ntop = const char* (*)(int, const void*, char*, uint32_t);
  LookupAddress lookup = Lookup<LookupAddress>(image, "DnsFixtureLookup");
  LookupPassive passive = Lookup<LookupPassive>(image, "DnsFixtureLookupPassive");
  Count count = Lookup<Count>(image, "DnsFixtureCount");
  Reverse reverse = Lookup<Reverse>(image, "DnsFixtureReverseNumeric");
  ReverseRejected reverse_rejected =
      Lookup<ReverseRejected>(image, "DnsFixtureReversePolicyRejected");
  Free free_result = Lookup<Free>(image, "DnsFixtureFree");
  ErrorString error_string = Lookup<ErrorString>(image, "DnsFixtureErrorString");
  Ntop ntop = Lookup<Ntop>(image, "DnsFixtureNtop");

  constexpr int kAfUnspec = 0;
  constexpr int kAfInet = 2;
  constexpr int kAfInet6 = 10;
  constexpr int kAiCanonname = 0x2;
  constexpr int kAiNumericHost = 0x4;
  constexpr int kAiNumericServ = 0x8;
  errno = EDOM;

  const uint8_t loopback4[] = {127, 0, 0, 1};
  const uint8_t loopback6[] = {0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 1};
  char presentation[64]{};
  Check(ntop(kAfInet, loopback4, presentation, sizeof(presentation)) ==
                presentation &&
            std::strcmp(presentation, "127.0.0.1") == 0,
        "inet_ntop Android IPv4 family");
  std::memset(presentation, 0, sizeof(presentation));
  Check(ntop(kAfInet6, loopback6, presentation, sizeof(presentation)) ==
                presentation &&
            std::strcmp(presentation, "::1") == 0,
        "inet_ntop Android IPv6 family");

  DarwinArtAndroidAddrinfo* ipv4 = nullptr;
  Check(lookup("127.0.0.1", "443", kAfInet,
               kAiNumericHost | kAiNumericServ, &ipv4) == 0 &&
            ipv4 != nullptr && count(ipv4, kAfInet) >= 1,
        "numeric IPv4 lookup and Android addrinfo");
  char host[128]{};
  char service[32]{};
  Check(reverse(ipv4, host, sizeof(host), service, sizeof(service)) == 0 &&
            std::strcmp(host, "127.0.0.1") == 0 &&
            std::strcmp(service, "443") == 0,
        "numeric IPv4 reverse");
  Check(reverse_rejected(ipv4, host, sizeof(host)) == 8,
        "reverse name policy closed");

  DarwinArtAndroidAddrinfo* ipv6 = nullptr;
  Check(lookup("::1", "53", kAfInet6,
               kAiNumericHost | kAiNumericServ, &ipv6) == 0 &&
            ipv6 != nullptr && count(ipv6, kAfInet6) >= 1,
        "numeric IPv6 lookup and Android sockaddr");
  std::memset(host, 0, sizeof(host));
  std::memset(service, 0, sizeof(service));
  Check(reverse(ipv6, host, sizeof(host), service, sizeof(service)) == 0 &&
            std::strcmp(host, "::1") == 0 && std::strcmp(service, "53") == 0,
        "numeric IPv6 reverse");

  DarwinArtAndroidAddrinfo* localhost = nullptr;
  Check(lookup("localhost", "80", kAfUnspec,
               kAiCanonname | kAiNumericServ, &localhost) == 0 &&
            localhost != nullptr && count(localhost, kAfUnspec) >= 1,
        "bounded localhost lookup");
  DarwinArtAndroidAddrinfo* wildcard = nullptr;
  Check(passive("9000", kAfInet, &wildcard) == 0 && wildcard != nullptr &&
            count(wildcard, kAfInet) >= 1,
        "passive numeric service");

  DarwinArtAndroidAddrinfo* rejected = nullptr;
  Check(lookup("unqualified", "443", kAfInet, kAiNumericServ,
               &rejected) == 8 && rejected == nullptr &&
            lookup("printer.local", "443", kAfInet, kAiNumericServ,
                   &rejected) == 8 && rejected == nullptr &&
            lookup("printer.LOCAL.", "443", kAfInet, kAiNumericServ,
                   &rejected) == 8 && rejected == nullptr &&
            lookup("bad/name.example", "443", kAfInet, kAiNumericServ,
                   &rejected) == 8 && rejected == nullptr,
        "search mDNS and malformed name policy closed");
  Check(lookup("127.0.0.1", "https", kAfInet, kAiNumericHost,
               &rejected) == 9 && rejected == nullptr,
        "named service policy closed");
  DarwinArtAndroidAddrinfo* configured = nullptr;
  Check(lookup("127.0.0.1", "443", kAfInet,
               kAiNumericHost | kAiNumericServ | 0x400,
               &configured) == 0 && configured != nullptr,
        "AI_ADDRCONFIG translated");
  Check(std::strcmp(error_string(8), "Name or service not known") == 0 &&
            std::strcmp(error_string(999), "Unknown error") == 0,
        "Android gai_strerror table");
  Check(errno == EDOM, "host errno preserved");

  DarwinArtAndroidAddrinfo* concurrent = nullptr;
  Check(lookup("127.0.0.1", "1234", kAfInet,
               kAiNumericHost | kAiNumericServ, &concurrent) == 0,
        "concurrent result setup");
  std::atomic<bool> reader_started{false};
  std::atomic<bool> stop_reader{false};
  std::atomic<bool> reader_ok{true};
  std::thread reader([&] {
    errno = EDOM;
    reader_started.store(true, std::memory_order_release);
    while (!stop_reader.load(std::memory_order_acquire)) {
      char local_host[64]{};
      char local_service[16]{};
      if (count(concurrent, kAfInet) < 1 ||
          reverse(concurrent, local_host, sizeof(local_host), local_service,
                  sizeof(local_service)) != 0 ||
          std::strcmp(local_host, "127.0.0.1") != 0 || errno != EDOM) {
        reader_ok.store(false, std::memory_order_release);
        break;
      }
    }
  });
  while (!reader_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::thread first_free([&] { free_result(concurrent); });
  std::thread duplicate_free([&] { free_result(concurrent); });
  first_free.join();
  duplicate_free.join();
  Check(darwin_art_bionic_dns_retired_results_for_test() >= 1,
        "concurrent free atomically retires");
  stop_reader.store(true, std::memory_order_release);
  reader.join();
  Check(reader_ok.load(std::memory_order_acquire),
        "reader remains memory-safe after logical free");

  free_result(ipv4);
  free_result(ipv6);
  free_result(localhost);
  free_result(wildcard);
  free_result(configured);
  Check(darwin_art_bionic_dns_live_results_for_test() == 0 &&
            darwin_art_bionic_dns_retired_results_for_test() == 6,
        "all allocations retired");
  darwin_art_bionic_dns_reset_for_test();
  Check(darwin_art_bionic_dns_live_results_for_test() == 0 &&
            darwin_art_bionic_dns_retired_results_for_test() == 0,
        "quiescent reset reclaims all results");

  Check(darwin_art_elf_unload(&image, &error) == DARWIN_ART_ELF_OK &&
            image == nullptr,
        "unload Android ELF");
  std::fprintf(stderr,
               "bionic-dns-facade: PASS Android-ELF=yes localhost+IPv4+IPv6="
               "yes reverse=numeric allocation=retire+quiescent-reset "
               "policy=absolute-host-dns errno=preserved\n");
  return 0;
}
