#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DarwinArtLibmCapability {
  DARWIN_ART_LIBM_UNKNOWN = 0,
  DARWIN_ART_LIBM_BIT_EXACT = 1,
  DARWIN_ART_LIBM_RESULT_ONLY_FENV_UNPROVEN = 2,
  DARWIN_ART_LIBM_ERRNO_OR_FENV_SENSITIVE = 3,
  DARWIN_ART_LIBM_UNSUPPORTED_ABI = 4,
};

double darwin_art_bionic_fabs(double value);
float darwin_art_bionic_fabsf(float value);
double darwin_art_bionic_copysign(double magnitude, double sign);
float darwin_art_bionic_copysignf(float magnitude, float sign);

uintptr_t darwin_art_libm_resolve(const char* symbol, const char* version);
enum DarwinArtLibmCapability darwin_art_libm_capability(const char* symbol);

#ifdef __cplusplus
}
#endif

