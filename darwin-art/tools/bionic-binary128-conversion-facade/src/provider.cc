#include "darwin_art_bionic_binary128_conversion.h"

#include <errno.h>
#include <fenv.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include <androidicuinit/android_icu_init.h>
#include <unicode/uchar.h>
#include <unicode/uversion.h>

#include "darwin_art_bionic_allocator.h"
#include "darwin_art_bionic_errno.h"

extern "C" int darwin_art_aosp_strtorQ(const char*, char**, int, void*);
extern bool android_icu_is_registered();

namespace {

static_assert(sizeof(DarwinArtAndroidWchar) == 4);
static_assert(U_ICU_VERSION_MAJOR_NUM == 76);
static_assert(U_ICU_VERSION_MINOR_NUM == 1);

constexpr int32_t kAndroidEnomem = 12;
constexpr int32_t kAndroidErange = 34;
constexpr std::string_view kAllowedAscii =
    "-+0123456789.xXeEpP()nNaAiIfFtTyY";

struct HostStateGuard {
  int saved_errno{errno};
  fenv_t saved_environment{};
  HostStateGuard() { fegetenv(&saved_environment); }
  ~HostStateGuard() { Restore(); }
  void Restore() const {
    fesetenv(&saved_environment);
    errno = saved_errno;
  }
};

int GdtoaRoundingMode() {
  switch (fegetround()) {
    case FE_TOWARDZERO:
      return 0;
    case FE_TONEAREST:
      return 1;
    case FE_UPWARD:
      return 2;
    case FE_DOWNWARD:
      return 3;
    default:
      std::abort();
  }
}

void ParseAscii(const char* input, char** end_pointer, void* output) {
  HostStateGuard host_state;
  const int rounding = GdtoaRoundingMode();
  errno = 0;
  darwin_art_aosp_strtorQ(input, end_pointer, rounding, output);
  const int parser_errno = errno;
  host_state.Restore();
  if (parser_errno == ERANGE) darwin_art_bionic_errno_store(kAndroidErange);
}

void EnsureAndroidIcu76() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!android_icu_is_registered()) android_icu_init();
    if (!android_icu_is_registered()) std::abort();
    UVersionInfo version{};
    u_getVersion(version);
    if (version[0] != 76 || version[1] != 1) std::abort();
  });
}

bool IsIcuWhitespace(DarwinArtAndroidWchar code_point) {
  return u_hasBinaryProperty(static_cast<UChar32>(code_point),
                             UCHAR_WHITE_SPACE);
}

bool IsAllowedAscii(DarwinArtAndroidWchar code_point) {
  return code_point <= 0x7f &&
         kAllowedAscii.find(static_cast<char>(code_point)) !=
             std::string_view::npos;
}

}  // namespace

extern "C" void darwin_art_bionic_strtold_raw(const char* input,
                                                char** end_pointer,
                                                void* output) {
  ParseAscii(input, end_pointer, output);
}

extern "C" void darwin_art_bionic_strtold_l_raw(const char* input,
                                                  char** end_pointer,
                                                  const void* locale,
                                                  void* output) {
  (void)locale;  // Exact pinned Bionic locale wrapper: ignored, never read.
  ParseAscii(input, end_pointer, output);
}

extern "C" void darwin_art_bionic_wcstold_raw(
    const DarwinArtAndroidWchar* input,
    DarwinArtAndroidWchar** end_pointer,
    void* output) {
  HostStateGuard host_state;
  EnsureAndroidIcu76();
  host_state.Restore();

  const DarwinArtAndroidWchar* const original_input = input;
  while (IsIcuWhitespace(*input)) ++input;
  size_t maximum_length = 0;
  while (IsAllowedAscii(input[maximum_length])) ++maximum_length;
  if (maximum_length == SIZE_MAX) {
    std::memset(output, 0, 16);
    darwin_art_bionic_errno_store(kAndroidEnomem);
    return;
  }
  const DarwinArtBionicAllocationResult allocation =
      darwin_art_bionic_malloc_result(maximum_length + 1);
  if (allocation.pointer == nullptr) {
    std::memset(output, 0, 16);
    darwin_art_bionic_errno_store(allocation.bionic_errno == 0
                                      ? kAndroidEnomem
                                      : allocation.bionic_errno);
    return;
  }
  auto* ascii = static_cast<char*>(allocation.pointer);
  for (size_t index = 0; index < maximum_length; ++index) {
    ascii[index] = static_cast<char>(input[index]);
  }
  ascii[maximum_length] = '\0';

  host_state.Restore();
  char* ascii_end = nullptr;
  ParseAscii(ascii, &ascii_end, output);
  if (ascii_end < ascii ||
      static_cast<size_t>(ascii_end - ascii) > maximum_length) {
    std::abort();
  }
  const size_t consumed = static_cast<size_t>(ascii_end - ascii);
  darwin_art_bionic_free(ascii);
  if (end_pointer != nullptr) {
    *end_pointer = const_cast<DarwinArtAndroidWchar*>(
        consumed == 0 ? original_input : input + consumed);
  }
}

extern "C" void* darwin_art_bionic_binary128_conversion_resolve(
    const char* soname,
    const char* symbol,
    const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::string_view(soname) != "libc.so" ||
      std::string_view(version) != "LIBC") {
    return nullptr;
  }
  const std::string_view name(symbol);
  if (name == "strtold") return (void*)darwin_art_bionic_strtold;
  if (name == "strtold_l") return (void*)darwin_art_bionic_strtold_l;
  if (name == "wcstold") return (void*)darwin_art_bionic_wcstold;
  return nullptr;
}

extern "C" int darwin_art_bionic_binary128_conversion_capability(
    const char* capability) {
  if (capability == nullptr) return 0;
  const std::string_view name(capability);
  return name == "Android-AAPCS64-binary128-q0" ||
         name == "AOSP-gdtoa-strtorQ" ||
         name == "strtold_l-locale-ignored" ||
         name == "Android-unsigned-wchar32" || name == "ICU76-iswspace" ||
         name == "conversion-only-no-binary128-arithmetic";
}

extern "C" void darwin_art_bionic_binary128_conversion_test_prepare_host_state(
    void) {
  errno = 31997;
  fesetround(FE_DOWNWARD);
  feraiseexcept(FE_DIVBYZERO);
}

extern "C" int
darwin_art_bionic_binary128_conversion_test_host_state_is_preserved(void) {
  return errno == 31997 && fegetround() == FE_DOWNWARD &&
         (fetestexcept(FE_DIVBYZERO) != 0);
}
