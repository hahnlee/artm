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
extern "C" void *darwin_art_bionic_locale_resolve(const char *soname,
                                                  const char *symbol,
                                                  const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_LOCALE, "libc.so", soname, symbol,
                version);
}
extern "C" SymbolFunction darwin_art_bionic_numeric_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_NUMERIC, s);
}
extern "C" void *darwin_art_bionic_float_conversion_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_FLOAT_CONVERSION, "libc.so", soname,
                symbol, version);
}
extern "C" SymbolFunction darwin_art_bionic_format_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_FORMAT, s);
}
extern "C" SymbolFunction darwin_art_bionic_strerror_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_STRERROR, s);
}
extern "C" void *darwin_art_bionic_wide_integer_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_WIDE_INTEGER, "libc.so", soname,
                symbol, version);
}
extern "C" void *darwin_art_bionic_wide_float_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_WIDE_FLOAT, "libc.so", soname,
                symbol, version);
}
extern "C" void *darwin_art_bionic_abort_resolve(
    const char *soname, const char *symbol, const char *version) {
  return Triple(DARWIN_ART_BIONIC_PROVIDER_ABORT, "libc.so", soname, symbol,
                version);
}
extern "C" uintptr_t darwin_art_liblog_provider_resolve(const char *symbol,
                                                        const char *version) {
  if (symbol == nullptr || (version != nullptr && version[0] != '\0'))
    std::abort();
  ++calls[DARWIN_ART_BIONIC_PROVIDER_LIBLOG];
  return reinterpret_cast<uintptr_t>(&Stub);
}
extern "C" SymbolFunction
darwin_art_bionic_dso_lifecycle_resolve(const char *s) {
  return OneArg(DARWIN_ART_BIONIC_PROVIDER_DSO_LIFECYCLE, s);
}
extern "C" SymbolFunction darwin_art_bionic_syslog_resolve(
    const char *soname, const char *symbol, const char *version) {
  return reinterpret_cast<SymbolFunction>(
      Triple(DARWIN_ART_BIONIC_PROVIDER_SYSLOG, "libc.so", soname, symbol,
             version));
}
extern "C" SymbolFunction darwin_art_bionic_formatted_stdio_resolve(
    const char *soname, const char *symbol, const char *version) {
  return reinterpret_cast<SymbolFunction>(
      Triple(DARWIN_ART_BIONIC_PROVIDER_FORMATTED_STDIO, "libc.so", soname,
             symbol, version));
}
extern "C" SymbolFunction darwin_art_bionic_syscall_resolve(
    const char *soname, const char *symbol, const char *version) {
  return reinterpret_cast<SymbolFunction>(
      Triple(DARWIN_ART_BIONIC_PROVIDER_SYSCALL, "libc.so", soname, symbol,
             version));
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
  constexpr size_t kExpectedCalls[] = {11, 4,  1,  29, 3, 24, 3, 1, 13, 31, 6,
                                       2,  3,  1,  4,  2, 18, 2, 2, 3,  2, 1};
  for (size_t index = 0; index < calls.size(); ++index) {
    if (calls[index] != kExpectedCalls[index])
      return 12;
  }
  if (darwin_art_bionic_namespace_teardown(instance) !=
      DARWIN_ART_BIONIC_NAMESPACE_OK)
    return 13;
  darwin_art_bionic_namespace_destroy(instance);
  std::fprintf(stderr, "bionic-provider-builtin-adapters: PASS providers=22 "
                       "routes=166 libdl-soname=exact\n");
  return 0;
}
