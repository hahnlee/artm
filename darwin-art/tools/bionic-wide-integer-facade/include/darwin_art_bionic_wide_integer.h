#ifndef DARWIN_ART_BIONIC_WIDE_INTEGER_H_
#define DARWIN_ART_BIONIC_WIDE_INTEGER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DarwinArtAndroidWchar;

long darwin_art_bionic_wcstol(const DarwinArtAndroidWchar* input,
                              DarwinArtAndroidWchar** end_pointer,
                              int base);
long long darwin_art_bionic_wcstoll(const DarwinArtAndroidWchar* input,
                                    DarwinArtAndroidWchar** end_pointer,
                                    int base);
unsigned long darwin_art_bionic_wcstoul(const DarwinArtAndroidWchar* input,
                                        DarwinArtAndroidWchar** end_pointer,
                                        int base);
unsigned long long darwin_art_bionic_wcstoull(
    const DarwinArtAndroidWchar* input,
    DarwinArtAndroidWchar** end_pointer,
    int base);

void* darwin_art_bionic_wide_integer_resolve(const char* soname,
                                              const char* symbol,
                                              const char* version);
int darwin_art_bionic_wide_integer_capability(const char* capability);
void darwin_art_bionic_wide_integer_test_prepare_host_state(void);
int darwin_art_bionic_wide_integer_test_host_state_is_preserved(void);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_WIDE_INTEGER_H_
