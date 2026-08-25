#include "darwin_art_bionic_builtin_adapters.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using SymbolFunction = void (*)(void);

struct Expected {
  const char *soname;
  const char *symbol;
  const char *version;
  DarwinArtBionicProviderId owner;
};

constexpr Expected kExpected[] = {
#include "ownership.inc"
};

std::array<size_t, DARWIN_ART_BIONIC_PROVIDER_COUNT> calls{};

void Stub() {}

SymbolFunction OneArg(DarwinArtBionicProviderId owner, const char *symbol) {
  if (symbol == nullptr || symbol[0] == '\0')
    std::abort();
  ++calls[static_cast<size_t>(owner)];
  return Stub;
}

void *Triple(DarwinArtBionicProviderId owner, const char *expected_soname,
             const char *soname, const char *symbol, const char *version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, expected_soname) != 0 ||
      std::strcmp(version, "LIBC") != 0)
    std::abort();
  ++calls[static_cast<size_t>(owner)];
  return reinterpret_cast<void *>(&Stub);
}

} // namespace

extern "C" SymbolFunction darwin_art_bionic_libc_leaf_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_LEAF, s);
}
extern "C" SymbolFunction darwin_art_bionic_allocator_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_ALLOCATOR, s);
}
extern "C" SymbolFunction darwin_art_bionic_errno_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_ERRNO, s);
}
extern "C" SymbolFunction darwin_art_bionic_fs_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_FILESYSTEM, s);
}
extern "C" SymbolFunction darwin_art_bionic_time_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_TIME, s);
}
extern "C" void *darwin_art_bionic_pthread_resolve(const char *soname,
                                                   const char *symbol,
                                                   const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_PTHREAD, "libc.so", soname, symbol,
                version);
}
extern "C" SymbolFunction
darwin_art_bionic_process_state_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_PROCESS_STATE, s);
}
extern "C" void *darwin_art_dl_phdr_resolve(const char *soname,
                                            const char *symbol,
                                            const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_PHDR, "libdl.so", soname, symbol,
                version);
}
extern "C" SymbolFunction darwin_art_bionic_stdio_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_STDIO, s);
}
extern "C" void *darwin_art_bionic_wide_stdio_resolve(const char *soname,
                                                      const char *symbol,
                                                      const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_WIDE_STDIO, "libc.so", soname,
                symbol, version);
}
extern "C" void *darwin_art_bionic_scanf_resolve(const char *soname,
                                                 const char *symbol,
                                                 const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_SCANF, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_swprintf_resolve(const char *soname,
                                                    const char *symbol,
                                                    const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_SWPRINTF, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_ioctl_resolve(const char *soname,
                                                 const char *symbol,
                                                 const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_IOCTL, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_strftime_resolve(const char *soname,
                                                    const char *symbol,
                                                    const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_STRFTIME, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_sendfile_resolve(const char *soname,
                                                    const char *symbol,
                                                    const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_SENDFILE, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_socket_broker_resolve(const char *soname,
                                                         const char *symbol,
                                                         const char *version) {
  const bool central =
      std::strcmp(symbol, "close") == 0 || std::strcmp(symbol, "fcntl") == 0 ||
      std::strcmp(symbol, "pipe") == 0 || std::strcmp(symbol, "poll") == 0 ||
      std::strcmp(symbol, "read") == 0 || std::strcmp(symbol, "write") == 0;
  const auto owner = central ? DARWIN_ART_BIONIC_PROVIDER_CENTRAL_FD_BROKER
                             : DARWIN_ART_BIONIC_PROVIDER_SOCKET;
  return Triple(owner, "libc.so", soname, symbol, version);
}
extern "C" void *darwin_art_bionic_socket_broker_dns_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_DNS, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_locale_resolve(const char *soname,
                                                  const char *symbol,
                                                  const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_LOCALE, "libc.so", soname, symbol,
                version);
}
extern "C" void *darwin_art_bionic_math_resolve(const char *soname,
                                                const char *symbol,
                                                const char *version) {
  if (std::strcmp(soname, "libc.so") == 0 &&
      std::strcmp(symbol, "ldexp") == 0) {
    if (std::strcmp(version, "LIBC") != 0)
      std::abort();
    ++calls[DARWIN_ART_BIONIC_PROVIDER_MATH];
    return reinterpret_cast<void *>(&Stub);
  }
  return Triple(DARWIN_ART_BIONIC_PROVIDER_MATH, "libm.so", soname, symbol,
                version);
}
extern "C" SymbolFunction darwin_art_bionic_vm_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_VM, s);
}
extern "C" SymbolFunction darwin_art_bionic_numeric_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_NUMERIC, s);
}
extern "C" void *darwin_art_bionic_float_conversion_resolve(
    const char *soname, const char *symbol, const char *version) {
  const char *expected_version = std::strcmp(symbol, "strtod_l") == 0 ||
                                         std::strcmp(symbol, "strtof_l") == 0
                                     ? "LIBC_O"
                                     : "LIBC";
  if (soname == nullptr || version == nullptr ||
      std::strcmp(soname, "libc.so") != 0 ||
      std::strcmp(version, expected_version) != 0)
    std::abort();
  ++calls[DARWIN_ART_BIONIC_PROVIDER_FLOAT_CONVERSION];
  return reinterpret_cast<void *>(&Stub);
}
extern "C" SymbolFunction darwin_art_bionic_format_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_FORMAT, s);
}
extern "C" SymbolFunction darwin_art_bionic_strerror_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_STRERROR, s);
}
extern "C" void *darwin_art_bionic_wide_integer_resolve(const char *soname,
                                                        const char *symbol,
                                                        const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_WIDE_INTEGER, "libc.so", soname,
                symbol, version);
}
extern "C" void *darwin_art_bionic_wide_float_resolve(const char *soname,
                                                      const char *symbol,
                                                      const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_WIDE_FLOAT, "libc.so", soname,
                symbol, version);
}
extern "C" void *darwin_art_bionic_binary128_conversion_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_BINARY128_CONVERSION, "libc.so",
                soname, symbol, version);
}
extern "C" void *darwin_art_bionic_abort_resolve(const char *soname,
                                                 const char *symbol,
                                                 const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_ABORT, "libc.so", soname, symbol,
                version);
}
extern "C" uintptr_t darwin_art_liblog_provider_resolve(const char *symbol,
                                                        const char *version) {
  if (symbol == nullptr || (version != nullptr && version[0] != '\0' &&
                            std::strcmp(version, "LIBLOG") != 0))
    std::abort();
  ++calls[DARWIN_ART_BIONIC_PROVIDER_LIBLOG];
  return reinterpret_cast<uintptr_t>(&Stub);
}
extern "C" SymbolFunction
darwin_art_bionic_dso_lifecycle_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_DSO_LIFECYCLE, s);
}
extern "C" SymbolFunction
darwin_art_bionic_syslog_resolve(const char *soname, const char *symbol,
                                 const char *version) {
  return reinterpret_cast<SymbolFunction>(Triple(
      DARWIN_ART_BIONIC_PROVIDER_SYSLOG, "libc.so", soname, symbol, version));
}
extern "C" SymbolFunction darwin_art_bionic_formatted_stdio_resolve(
    const char *soname, const char *symbol, const char *version) {
  return reinterpret_cast<SymbolFunction>(
      Triple(DARWIN_ART_BIONIC_PROVIDER_FORMATTED_STDIO, "libc.so", soname,
             symbol, version));
}
extern "C" SymbolFunction
darwin_art_bionic_syscall_resolve(const char *soname, const char *symbol,
                                  const char *version) {
  return reinterpret_cast<SymbolFunction>(Triple(
      DARWIN_ART_BIONIC_PROVIDER_SYSCALL, "libc.so", soname, symbol, version));
}

int main() {
  DarwinArtBionicNamespace *instance = darwin_art_bionic_namespace_create();
  if (instance == nullptr ||
      darwin_art_bionic_namespace_bind_builtins(instance, nullptr) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK ||
      darwin_art_bionic_namespace_seal(instance) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK)
    return 10;
  for (const Expected &expected : kExpected) {
    const auto result = darwin_art_bionic_namespace_resolve(
        instance, expected.soname, expected.symbol,
        expected.version[0] == '\0' ? nullptr : expected.version);
    if (result.status != DARWIN_ART_BIONIC_NAMESPACE_OK ||
        result.owner != expected.owner || result.address == 0)
      return 11;
  }
  constexpr size_t kExpectedCalls[] = {
      62, 12, 1, 49, 17, 52, 60, 1, 45, 40, 10, 5, 8, 3, 4, 4,  20,
      10, 2,  4, 3,  1,  3,  3,  3, 1,  1,  2, 1, 13, 21, 8, 60, 5};
  for (size_t index = 0; index < calls.size(); ++index) {
    if (calls[index] != kExpectedCalls[index]) {
      std::fprintf(stderr,
                   "bionic-provider-builtin-adapters: call-count mismatch "
                   "provider=%zu expected=%zu actual=%zu\n",
                   index, kExpectedCalls[index], calls[index]);
      return 12;
    }
  }
  if (darwin_art_bionic_namespace_teardown(instance) !=
      DARWIN_ART_BIONIC_NAMESPACE_OK)
    return 13;
  darwin_art_bionic_namespace_destroy(instance);
  std::fprintf(stderr, "bionic-provider-builtin-adapters: PASS providers=34 "
                       "routes=534 version-aliases=exact\n");
  return 0;
}
