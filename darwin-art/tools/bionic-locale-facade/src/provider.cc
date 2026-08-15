#include "darwin_art_bionic_locale.h"

#include <errno.h>
#include <fenv.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_map>

extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);

namespace {

static_assert(sizeof(DarwinArtAndroidMbState) == 8);
static_assert(alignof(DarwinArtAndroidMbState) == 1);
static_assert(sizeof(uint32_t) == 4);
static_assert(sizeof(DarwinArtAndroidLconv) == 96);

constexpr int kAndroidEnoent = 2;
constexpr int kAndroidEnomem = 12;
constexpr int kAndroidEinval = 22;
constexpr int kAndroidEilseq = 84;
constexpr int kAndroidLcCtype = 0;
constexpr int kAndroidLcIdentification = 12;
constexpr int kAndroidLcAllMask = 0x1fbf;
constexpr size_t kIllegal = static_cast<size_t>(-1);
constexpr size_t kIncomplete = static_cast<size_t>(-2);
constexpr uint32_t kAndroidWeof = UINT32_MAX;
constexpr int kEof = -1;

struct HostStateGuard {
  int saved_errno{errno};
  fenv_t saved_environment{};
  HostStateGuard() { fegetenv(&saved_environment); }
  ~HostStateGuard() {
    fesetenv(&saved_environment);
    errno = saved_errno;
  }
};

enum class LocaleMode { kC, kUtf8 };

struct LocaleEntry {
  explicit LocaleEntry(LocaleMode mode_value) : mode(mode_value) {}
  LocaleMode mode;
};

struct ProviderState {
  std::mutex mutex;
  std::unordered_map<DarwinArtAndroidLocale, std::unique_ptr<LocaleEntry>>
      locales;
  std::atomic<bool> global_utf8{true};
};

ProviderState& State() {
  static ProviderState state;
  return state;
}

struct ThreadLocaleState {
  DarwinArtAndroidLocale handle{};
  LocaleMode mode{LocaleMode::kUtf8};
};

thread_local ThreadLocaleState g_thread_locale;
thread_local DarwinArtAndroidMbState g_mbrtowc_state{};
thread_local DarwinArtAndroidMbState g_mbrlen_state{};
thread_local DarwinArtAndroidMbState g_mbsnrtowcs_state{};
thread_local DarwinArtAndroidMbState g_mbtowc_state{};
thread_local DarwinArtAndroidMbState g_wcrtomb_state{};
thread_local DarwinArtAndroidMbState g_wcsnrtombs_state{};

DarwinArtAndroidLocale GlobalLocale() {
  return reinterpret_cast<DarwinArtAndroidLocale>(UINTPTR_MAX);
}

void SetAndroidErrno(int value) { darwin_art_bionic_errno_store(value); }

bool IsSupportedLocale(const char* name) {
  return std::strcmp(name, "") == 0 || std::strcmp(name, "C") == 0 ||
         std::strcmp(name, "C.UTF-8") == 0 ||
         std::strcmp(name, "en_US.UTF-8") == 0 ||
         std::strcmp(name, "POSIX") == 0;
}

LocaleMode ModeForName(const char* name) {
  return (*name == '\0' || std::strstr(name, "UTF-8") != nullptr)
             ? LocaleMode::kUtf8
             : LocaleMode::kC;
}

bool StateInitial(const DarwinArtAndroidMbState* state) {
  uint32_t value = 0;
  std::memcpy(&value, state->sequence, sizeof(value));
  return value == 0;
}

size_t StateBytes(const DarwinArtAndroidMbState* state) {
  return state->sequence[2] != 0 ? 3
                                : state->sequence[1] != 0
                                      ? 2
                                      : state->sequence[0] != 0 ? 1 : 0;
}

void ResetState(DarwinArtAndroidMbState* state) {
  std::memset(state->sequence, 0, sizeof(state->sequence));
}

size_t Illegal(int android_errno, DarwinArtAndroidMbState* state) {
  SetAndroidErrno(android_errno);
  ResetState(state);
  return kIllegal;
}

size_t Decode(uint32_t* output,
              const char* source,
              size_t length,
              DarwinArtAndroidMbState* state) {
  if (state->sequence[3] != 0) return Illegal(kAndroidEinval, state);
  static constexpr char kEmpty[] = "";
  if (source == nullptr) {
    source = kEmpty;
    length = 1;
    output = nullptr;
  }
  if (length == 0) return kIncomplete;

  uint8_t byte = static_cast<uint8_t>(*source);
  if (StateInitial(state) && byte < 0x80) {
    if (output != nullptr) *output = byte;
    return byte == 0 ? 0 : 1;
  }

  const size_t bytes_so_far = StateBytes(state);
  byte = bytes_so_far == 0 ? static_cast<uint8_t>(*source)
                           : state->sequence[0];
  size_t sequence_length = 0;
  uint32_t mask = 0;
  uint32_t lower_bound = 0;
  if ((byte & 0xe0) == 0xc0) {
    sequence_length = 2;
    mask = 0x1f;
    lower_bound = 0x80;
  } else if ((byte & 0xf0) == 0xe0) {
    sequence_length = 3;
    mask = 0x0f;
    lower_bound = 0x800;
  } else if ((byte & 0xf8) == 0xf0) {
    sequence_length = 4;
    mask = 0x07;
    lower_bound = 0x10000;
  } else {
    return Illegal(kAndroidEilseq, state);
  }

  const size_t wanted = sequence_length - bytes_so_far;
  size_t consumed = 0;
  for (; consumed < std::min(wanted, length); ++consumed) {
    const uint8_t current = static_cast<uint8_t>(*source++);
    if (!StateInitial(state) && (current & 0xc0) != 0x80) {
      return Illegal(kAndroidEilseq, state);
    }
    state->sequence[bytes_so_far + consumed] = current;
  }
  if (consumed < wanted) return kIncomplete;

  uint32_t code_point = state->sequence[0] & mask;
  for (size_t index = 1; index < sequence_length; ++index) {
    code_point = (code_point << 6) | (state->sequence[index] & 0x3f);
  }
  if (code_point < lower_bound ||
      (code_point >= 0xd800 && code_point <= 0xdfff) ||
      code_point > 0x10ffff) {
    return Illegal(kAndroidEilseq, state);
  }
  if (output != nullptr) *output = code_point;
  ResetState(state);
  return code_point == 0 ? 0 : wanted;
}

size_t Encode(char* destination,
              uint32_t code_point,
              DarwinArtAndroidMbState* state) {
  if (destination == nullptr) {
    ResetState(state);
    return 1;
  }
  if (code_point == 0) {
    *destination = '\0';
    ResetState(state);
    return 1;
  }
  if (!StateInitial(state)) return Illegal(kAndroidEilseq, state);
  if (code_point < 0x80) {
    *destination = static_cast<char>(code_point);
    return 1;
  }
  uint8_t lead = 0;
  size_t length = 0;
  if (code_point <= 0x7ff) {
    lead = 0xc0;
    length = 2;
  } else if (code_point <= 0xffff) {
    lead = 0xe0;
    length = 3;
  } else if (code_point <= 0x1fffff) {
    lead = 0xf0;
    length = 4;
  } else {
    SetAndroidErrno(kAndroidEilseq);
    return kIllegal;
  }
  for (size_t index = length - 1; index > 0; --index) {
    destination[index] = static_cast<char>((code_point & 0x3f) | 0x80);
    code_point >>= 6;
  }
  destination[0] = static_cast<char>((code_point & 0xff) | lead);
  return length;
}

size_t WideLength(const uint32_t* source) {
  const uint32_t* cursor = source;
  while (*cursor != 0) ++cursor;
  return static_cast<size_t>(cursor - source);
}

int WideCompare(const uint32_t* left, const uint32_t* right) {
  while (*left == *right && *left != 0) {
    ++left;
    ++right;
  }
  return *left < *right ? -1 : *left > *right ? 1 : 0;
}

char g_decimal_point[] = ".";
char g_empty[] = "";
char g_c_locale[] = "C";
char g_utf8_locale[] = "C.UTF-8";
constexpr char kAndroidCharMax = static_cast<char>(UINT8_MAX);
DarwinArtAndroidLconv g_localeconv = {
    g_decimal_point, g_empty, g_empty, g_empty, g_empty,
    g_empty,         g_empty, g_empty, g_empty, g_empty,
    kAndroidCharMax,  kAndroidCharMax, kAndroidCharMax, kAndroidCharMax,
    kAndroidCharMax,  kAndroidCharMax, kAndroidCharMax, kAndroidCharMax,
    kAndroidCharMax,  kAndroidCharMax, kAndroidCharMax, kAndroidCharMax,
    kAndroidCharMax,  kAndroidCharMax};

}  // namespace

extern "C" size_t darwin_art_bionic___ctype_get_mb_cur_max(void) {
  HostStateGuard guard;
  const LocaleMode mode =
      g_thread_locale.handle == nullptr
          ? (State().global_utf8.load(std::memory_order_acquire)
                 ? LocaleMode::kUtf8
                 : LocaleMode::kC)
          : g_thread_locale.mode;
  return mode == LocaleMode::kUtf8 ? 4 : 1;
}

extern "C" DarwinArtAndroidLocale darwin_art_bionic_newlocale(
    int category_mask,
    const char* locale_name,
    DarwinArtAndroidLocale /*base*/) {
  HostStateGuard guard;
  if ((category_mask & ~kAndroidLcAllMask) != 0 || locale_name == nullptr) {
    SetAndroidErrno(kAndroidEinval);
    return nullptr;
  }
  if (!IsSupportedLocale(locale_name)) {
    SetAndroidErrno(kAndroidEnoent);
    return nullptr;
  }
  std::unique_ptr<LocaleEntry> entry(
      new (std::nothrow) LocaleEntry(ModeForName(locale_name)));
  if (entry == nullptr) {
    SetAndroidErrno(kAndroidEnomem);
    return nullptr;
  }
  DarwinArtAndroidLocale handle = entry.get();
  try {
    ProviderState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.locales.emplace(handle, std::move(entry));
  } catch (...) {
    SetAndroidErrno(kAndroidEnomem);
    return nullptr;
  }
  return handle;
}

extern "C" void darwin_art_bionic_freelocale(
    DarwinArtAndroidLocale locale) {
  HostStateGuard guard;
  if (locale == nullptr || locale == GlobalLocale()) return;
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.locales.erase(locale);
}

extern "C" DarwinArtAndroidLocale darwin_art_bionic_uselocale(
    DarwinArtAndroidLocale locale) {
  HostStateGuard guard;
  DarwinArtAndroidLocale old =
      g_thread_locale.handle == nullptr ? GlobalLocale()
                                        : g_thread_locale.handle;
  if (locale == nullptr) return old;
  if (locale == GlobalLocale()) {
    g_thread_locale.handle = nullptr;
    return old;
  }
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.locales.find(locale);
  if (found == state.locales.end()) {
    SetAndroidErrno(kAndroidEinval);
    return nullptr;
  }
  g_thread_locale.handle = locale;
  g_thread_locale.mode = found->second->mode;
  return old;
}

extern "C" char* darwin_art_bionic_setlocale(int category,
                                               const char* locale_name) {
  HostStateGuard guard;
  if (category < kAndroidLcCtype || category > kAndroidLcIdentification) {
    SetAndroidErrno(kAndroidEinval);
    return nullptr;
  }
  ProviderState& state = State();
  if (locale_name != nullptr) {
    if (!IsSupportedLocale(locale_name)) {
      SetAndroidErrno(kAndroidEnoent);
      return nullptr;
    }
    state.global_utf8.store(ModeForName(locale_name) == LocaleMode::kUtf8,
                            std::memory_order_release);
  }
  return state.global_utf8.load(std::memory_order_acquire) ? g_utf8_locale
                                                           : g_c_locale;
}

extern "C" DarwinArtAndroidLconv* darwin_art_bionic_localeconv(void) {
  HostStateGuard guard;
  return &g_localeconv;
}

extern "C" size_t darwin_art_bionic_mbrtowc(
    uint32_t* output,
    const char* source,
    size_t length,
    DarwinArtAndroidMbState* state) {
  HostStateGuard guard;
  return Decode(output, source, length,
                state == nullptr ? &g_mbrtowc_state : state);
}

extern "C" size_t darwin_art_bionic_mbrlen(
    const char* source,
    size_t length,
    DarwinArtAndroidMbState* state) {
  HostStateGuard guard;
  return Decode(nullptr, source, length,
                state == nullptr ? &g_mbrlen_state : state);
}

extern "C" int darwin_art_bionic_mbtowc(uint32_t* output,
                                         const char* source,
                                         size_t length) {
  HostStateGuard guard;
  if (source == nullptr) {
    ResetState(&g_mbtowc_state);
    return 0;
  }
  const size_t result = Decode(output, source, length, &g_mbtowc_state);
  if (result == kIncomplete) SetAndroidErrno(kAndroidEilseq);
  return result == kIllegal || result == kIncomplete
             ? -1
             : static_cast<int>(result);
}

extern "C" uint32_t darwin_art_bionic_btowc(int byte) {
  HostStateGuard guard;
  if (byte == kEof) return kAndroidWeof;
  DarwinArtAndroidMbState state{};
  const char character = static_cast<char>(byte);
  uint32_t output = 0;
  const size_t result = Decode(&output, &character, 1, &state);
  return result > 1 ? kAndroidWeof : output;
}

extern "C" size_t darwin_art_bionic_mbsnrtowcs(
    uint32_t* destination,
    const char** source,
    size_t source_length,
    size_t destination_length,
    DarwinArtAndroidMbState* state_pointer) {
  HostStateGuard guard;
  DarwinArtAndroidMbState* state =
      state_pointer == nullptr ? &g_mbsnrtowcs_state : state_pointer;
  if (source == nullptr || *source == nullptr) return 0;
  if (source_length > 0 && StateBytes(state) > 0 &&
      static_cast<uint8_t>((*source)[0]) < 0x80) {
    return Illegal(kAndroidEilseq, state);
  }
  size_t input = 0;
  size_t output = 0;
  if (destination == nullptr) {
    while (input < source_length) {
      size_t consumed = 1;
      if (static_cast<uint8_t>((*source)[input]) < 0x80) {
        if ((*source)[input] == '\0') {
          ResetState(state);
          return output;
        }
      } else {
        consumed = Decode(nullptr, *source + input, source_length - input,
                          state);
        if (consumed == kIllegal || consumed == kIncomplete) {
          return Illegal(kAndroidEilseq, state);
        }
        if (consumed == 0) {
          ResetState(state);
          return output;
        }
      }
      input += consumed;
      ++output;
    }
    ResetState(state);
    return output;
  }
  while (input < source_length && output < destination_length) {
    size_t consumed = 1;
    if (static_cast<uint8_t>((*source)[input]) < 0x80) {
      destination[output] = static_cast<uint8_t>((*source)[input]);
      if ((*source)[input] == '\0') {
        *source = nullptr;
        ResetState(state);
        return output;
      }
    } else {
      consumed = Decode(destination + output, *source + input,
                        source_length - input, state);
      if (consumed == kIllegal) {
        *source += input;
        return Illegal(kAndroidEilseq, state);
      }
      if (consumed == kIncomplete) {
        *source += source_length;
        return Illegal(kAndroidEilseq, state);
      }
      if (consumed == 0) {
        *source = nullptr;
        ResetState(state);
        return output;
      }
    }
    input += consumed;
    ++output;
  }
  *source += input;
  ResetState(state);
  return output;
}

extern "C" size_t darwin_art_bionic_mbsrtowcs(
    uint32_t* destination,
    const char** source,
    size_t destination_length,
    DarwinArtAndroidMbState* state) {
  return darwin_art_bionic_mbsnrtowcs(destination, source, SIZE_MAX,
                                      destination_length, state);
}

extern "C" size_t darwin_art_bionic_wcrtomb(
    char* destination,
    uint32_t code_point,
    DarwinArtAndroidMbState* state) {
  HostStateGuard guard;
  return Encode(destination, code_point,
                state == nullptr ? &g_wcrtomb_state : state);
}

extern "C" size_t darwin_art_bionic_wcsnrtombs(
    char* destination,
    const uint32_t** source,
    size_t source_length,
    size_t destination_length,
    DarwinArtAndroidMbState* state_pointer) {
  HostStateGuard guard;
  DarwinArtAndroidMbState* state =
      state_pointer == nullptr ? &g_wcsnrtombs_state : state_pointer;
  if (source == nullptr || *source == nullptr) return 0;
  if (!StateInitial(state)) return Illegal(kAndroidEilseq, state);
  char buffer[4];
  size_t input = 0;
  size_t output = 0;
  if (destination == nullptr) {
    while (input < source_length) {
      const uint32_t code_point = (*source)[input++];
      if (code_point == 0) return output;
      const size_t encoded = Encode(buffer, code_point, state);
      if (encoded == kIllegal) return encoded;
      output += encoded;
    }
    return output;
  }
  while (input < source_length && output < destination_length) {
    const uint32_t code_point = (*source)[input];
    if (code_point == 0) {
      destination[output] = '\0';
      *source = nullptr;
      return output;
    }
    const size_t encoded = Encode(buffer, code_point, state);
    if (encoded == kIllegal) {
      *source += input;
      return encoded;
    }
    if (encoded > destination_length - output) break;
    std::memcpy(destination + output, buffer, encoded);
    output += encoded;
    ++input;
  }
  *source += input;
  return output;
}

extern "C" int darwin_art_bionic_wctob(uint32_t code_point) {
  HostStateGuard guard;
  if (code_point == kAndroidWeof) return kEof;
  DarwinArtAndroidMbState state{};
  char buffer[4];
  const size_t result = Encode(buffer, code_point, &state);
  return result == 1 ? static_cast<unsigned char>(buffer[0]) : kEof;
}

extern "C" int darwin_art_bionic_strcoll_l(
    const char* left,
    const char* right,
    DarwinArtAndroidLocale /*locale*/) {
  HostStateGuard guard;
  return std::strcmp(left, right);
}

extern "C" size_t darwin_art_bionic_strxfrm_l(
    char* destination,
    const char* source,
    size_t length,
    DarwinArtAndroidLocale /*locale*/) {
  HostStateGuard guard;
  const size_t source_length = std::strlen(source);
  if (length != 0) {
    const size_t copy = std::min(source_length, length - 1);
    std::memcpy(destination, source, copy);
    destination[copy] = '\0';
  }
  return source_length;
}

extern "C" int darwin_art_bionic_wcscoll_l(
    const uint32_t* left,
    const uint32_t* right,
    DarwinArtAndroidLocale /*locale*/) {
  HostStateGuard guard;
  return WideCompare(left, right);
}

extern "C" size_t darwin_art_bionic_wcsxfrm_l(
    uint32_t* destination,
    const uint32_t* source,
    size_t length,
    DarwinArtAndroidLocale /*locale*/) {
  HostStateGuard guard;
  const size_t source_length = WideLength(source);
  if (length != 0) {
    const size_t copy = std::min(source_length, length - 1);
    std::memcpy(destination, source, copy * sizeof(uint32_t));
    destination[copy] = 0;
  }
  return source_length;
}

extern "C" void* darwin_art_bionic_locale_resolve(const char* soname,
                                                    const char* symbol,
                                                    const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::string_view(soname) != "libc.so" ||
      std::string_view(version) != "LIBC") {
    return nullptr;
  }
#define RESOLVE(name)                                                        \
  if (std::string_view(symbol) == #name)                                     \
    return reinterpret_cast<void*>(&darwin_art_bionic_##name)
  RESOLVE(__ctype_get_mb_cur_max);
  RESOLVE(btowc);
  RESOLVE(freelocale);
  RESOLVE(localeconv);
  RESOLVE(mbrlen);
  RESOLVE(mbrtowc);
  RESOLVE(mbsnrtowcs);
  RESOLVE(mbsrtowcs);
  RESOLVE(mbtowc);
  RESOLVE(newlocale);
  RESOLVE(setlocale);
  RESOLVE(strcoll_l);
  RESOLVE(strxfrm_l);
  RESOLVE(uselocale);
  RESOLVE(wcrtomb);
  RESOLVE(wcscoll_l);
  RESOLVE(wcsnrtombs);
  RESOLVE(wcsxfrm_l);
  RESOLVE(wctob);
#undef RESOLVE
  return nullptr;
}

extern "C" int darwin_art_bionic_locale_capability(
    const char* capability) {
  if (capability == nullptr) return 0;
  const std::string_view name(capability);
  return name == "C" || name == "POSIX" || name == "C.UTF-8" ||
         name == "en_US.UTF-8" || name == "utf8-mbstate8" ||
         name == "C-collation";
}

extern "C" size_t darwin_art_bionic_locale_live_handle_count(void) {
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.locales.size();
}

extern "C" void darwin_art_bionic_locale_test_prepare_host_state(void) {
  errno = 33101;
  fesetround(FE_DOWNWARD);
}

extern "C" int darwin_art_bionic_locale_test_host_state_is_preserved(void) {
  return errno == 33101 && fegetround() == FE_DOWNWARD;
}
