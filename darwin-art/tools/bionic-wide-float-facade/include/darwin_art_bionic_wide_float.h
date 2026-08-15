#ifndef DARWIN_ART_BIONIC_WIDE_FLOAT_H_
#define DARWIN_ART_BIONIC_WIDE_FLOAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DarwinArtAndroidWchar;

double darwin_art_bionic_wcstod(const DarwinArtAndroidWchar* input,
                                DarwinArtAndroidWchar** end_pointer);
float darwin_art_bionic_wcstof(const DarwinArtAndroidWchar* input,
                               DarwinArtAndroidWchar** end_pointer);

void* darwin_art_bionic_wide_float_resolve(const char* soname,
                                            const char* symbol,
                                            const char* version);
int darwin_art_bionic_wide_float_capability(const char* capability);

void darwin_art_bionic_wide_float_test_prepare_host_state(void);
int darwin_art_bionic_wide_float_test_host_state_is_preserved(void);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_WIDE_FLOAT_H_
