#include "darwin_art_bionic_float_conversion.h"

#include <errno.h>
#include <fenv.h>

#include <cstdint>
#include <cstring>
#include <cfloat>
#include <string_view>

extern "C" double darwin_art_aosp_strtod(const char*, char**);
extern "C" float darwin_art_aosp_strtof(const char*, char**);
extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);

namespace {

constexpr int kAndroidErange = 34;

static_assert(sizeof(long double) == sizeof(double));
static_assert(alignof(long double) == alignof(double));
static_assert(LDBL_MANT_DIG == DBL_MANT_DIG);

struct HostStateSnapshot {
  int saved_errno{errno};
  fenv_t saved_environment{};

  HostStateSnapshot() { fegetenv(&saved_environment); }

  void Restore() const {
    fesetenv(&saved_environment);
    errno = saved_errno;
  }
};

template <typename Result>
Result ParseWithAosp(Result (*parser)(const char*, char**),
                     const char* input,
                     char** end_pointer) {
  HostStateSnapshot snapshot;
  errno = 0;
  Result result = parser(input, end_pointer);
  const int parser_errno = errno;
  snapshot.Restore();
  if (parser_errno == ERANGE) {
    darwin_art_bionic_errno_store(kAndroidErange);
  }
  return result;
}

}  // namespace

extern "C" double darwin_art_bionic_strtod(const char* input,
                                             char** end_pointer) {
  return ParseWithAosp(darwin_art_aosp_strtod, input, end_pointer);
}

extern "C" float darwin_art_bionic_strtof(const char* input,
                                            char** end_pointer) {
  return ParseWithAosp(darwin_art_aosp_strtof, input, end_pointer);
}

extern "C" void* darwin_art_bionic_float_conversion_resolve(
    const char* soname,
    const char* symbol,
    const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::string_view(soname) != "libc.so" ||
      std::string_view(version) != "LIBC") {
    return nullptr;
  }
  if (std::string_view(symbol) == "strtod") {
    return reinterpret_cast<void*>(&darwin_art_bionic_strtod);
  }
  if (std::string_view(symbol) == "strtof") {
    return reinterpret_cast<void*>(&darwin_art_bionic_strtof);
  }
  return nullptr;
}

extern "C" int darwin_art_bionic_float_conversion_capability(
    const char* capability) {
  if (capability == nullptr) return 0;
  const std::string_view name(capability);
  return name == "strtod-binary64" || name == "strtof-binary32" ||
         name == "AOSP-gdtoa" || name == "C-locale-only";
}

extern "C" void darwin_art_bionic_float_conversion_test_prepare_host_state(
    void) {
  errno = 31991;
  fesetround(FE_DOWNWARD);
  feraiseexcept(FE_DIVBYZERO);
}

extern "C" int
darwin_art_bionic_float_conversion_test_host_state_is_preserved(void) {
  return errno == 31991 && fegetround() == FE_DOWNWARD &&
         (fetestexcept(FE_DIVBYZERO) != 0);
}
