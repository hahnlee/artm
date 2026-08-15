#include "darwin_art_libm_facade.h"

#include <bit>
#include <cstring>

extern "C" double darwin_art_bionic_fabs(double value) {
  return std::bit_cast<double>(std::bit_cast<uint64_t>(value) & 0x7fffffffffffffffULL);
}

extern "C" float darwin_art_bionic_fabsf(float value) {
  return std::bit_cast<float>(std::bit_cast<uint32_t>(value) & 0x7fffffffU);
}

extern "C" double darwin_art_bionic_copysign(double magnitude, double sign) {
  uint64_t magnitude_bits = std::bit_cast<uint64_t>(magnitude) & 0x7fffffffffffffffULL;
  uint64_t sign_bits = std::bit_cast<uint64_t>(sign) & 0x8000000000000000ULL;
  return std::bit_cast<double>(magnitude_bits | sign_bits);
}

extern "C" float darwin_art_bionic_copysignf(float magnitude, float sign) {
  uint32_t magnitude_bits = std::bit_cast<uint32_t>(magnitude) & 0x7fffffffU;
  uint32_t sign_bits = std::bit_cast<uint32_t>(sign) & 0x80000000U;
  return std::bit_cast<float>(magnitude_bits | sign_bits);
}

struct Entry {
  const char* name;
  uintptr_t address;
};

static const Entry kSafeEntries[] = {
    {"copysign", reinterpret_cast<uintptr_t>(&darwin_art_bionic_copysign)},
    {"copysignf", reinterpret_cast<uintptr_t>(&darwin_art_bionic_copysignf)},
    {"fabs", reinterpret_cast<uintptr_t>(&darwin_art_bionic_fabs)},
    {"fabsf", reinterpret_cast<uintptr_t>(&darwin_art_bionic_fabsf)},
};

extern "C" uintptr_t darwin_art_libm_resolve(const char* symbol, const char* version) {
  if (symbol == nullptr ||
      (version != nullptr && version[0] != '\0' && std::strcmp(version, "LIBC") != 0)) {
    return 0;
  }
  for (const Entry& entry : kSafeEntries) {
    if (std::strcmp(entry.name, symbol) == 0) return entry.address;
  }
  return 0;
}

static bool IsOneOf(const char* symbol, const char* const* values, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (std::strcmp(symbol, values[index]) == 0) return true;
  }
  return false;
}

extern "C" enum DarwinArtLibmCapability darwin_art_libm_capability(const char* symbol) {
  if (symbol == nullptr) return DARWIN_ART_LIBM_UNKNOWN;
  for (const Entry& entry : kSafeEntries) {
    if (std::strcmp(entry.name, symbol) == 0) return DARWIN_ART_LIBM_BIT_EXACT;
  }
  static const char* const kRounding[] = {
      "ceil", "ceilf", "floor", "floorf", "round", "roundf", "trunc", "truncf"};
  if (IsOneOf(symbol, kRounding, sizeof(kRounding) / sizeof(kRounding[0]))) {
    return DARWIN_ART_LIBM_RESULT_ONLY_FENV_UNPROVEN;
  }
  static const char* const kSensitive[] = {
      "acos", "acosf", "asin", "asinf", "atan", "atan2", "atan2f", "atanf",
      "cos", "cosf", "exp", "expf", "fmod", "fmodf", "log", "logf", "pow",
      "powf", "remainder", "remainderf", "sin", "sinf", "sqrt", "sqrtf", "tan",
      "tanf"};
  if (IsOneOf(symbol, kSensitive, sizeof(kSensitive) / sizeof(kSensitive[0]))) {
    return DARWIN_ART_LIBM_ERRNO_OR_FENV_SENSITIVE;
  }
  static const char* const kComplexBases[] = {
      "cabs", "cacos", "cacosh", "carg", "casin", "casinh", "catan", "catanh",
      "ccos", "ccosh", "cexp", "cimag", "clog", "conj", "cproj", "creal", "csin",
      "csinh", "csqrt", "ctan", "ctanh"};
  for (const char* base : kComplexBases) {
    size_t base_length = std::strlen(base);
    if (std::strncmp(symbol, base, base_length) == 0 &&
        (symbol[base_length] == '\0' ||
         ((symbol[base_length] == 'f' || symbol[base_length] == 'l') &&
          symbol[base_length + 1] == '\0'))) {
      return DARWIN_ART_LIBM_UNSUPPORTED_ABI;
    }
  }
  size_t length = std::strlen(symbol);
  if ((length > 0 && symbol[length - 1] == 'l') ||
      std::strcmp(symbol, "nexttoward") == 0 || std::strcmp(symbol, "nexttowardf") == 0) {
    return DARWIN_ART_LIBM_UNSUPPORTED_ABI;
  }
  return DARWIN_ART_LIBM_UNKNOWN;
}
