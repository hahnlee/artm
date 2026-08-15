#ifndef DARWIN_ART_BIONIC_TIME_H_
#define DARWIN_ART_BIONIC_TIME_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtAndroidTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
} DarwinArtAndroidTimespec;

typedef void (*DarwinArtBionicTimeFunction)(void);

int darwin_art_bionic_clock_gettime(int android_clock,
                                    DarwinArtAndroidTimespec* result);
int darwin_art_bionic_nanosleep(const DarwinArtAndroidTimespec* request,
                                DarwinArtAndroidTimespec* remaining);
long darwin_art_bionic_sysconf(int android_name);
DarwinArtBionicTimeFunction darwin_art_bionic_time_resolve(
    const char* import_name);
int darwin_art_bionic_time_capability_failed(void);

/* Test-harness-only signal controls; hidden from dynamic and guest resolution. */
__attribute__((visibility("hidden"))) int
darwin_art_bionic_time_test_arm_alarm(uint32_t microseconds);
__attribute__((visibility("hidden"))) void
darwin_art_bionic_time_test_finish_alarm(void);
__attribute__((visibility("hidden"))) int
darwin_art_bionic_time_test_force_boottime_overflow(void);

#ifdef __cplusplus
}
#endif

#endif
