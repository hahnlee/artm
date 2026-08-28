#ifndef DARWIN_ART_BIONIC_STRFTIME_H_
#define DARWIN_ART_BIONIC_STRFTIME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtAndroidTm {
  int32_t tm_sec;
  int32_t tm_min;
  int32_t tm_hour;
  int32_t tm_mday;
  int32_t tm_mon;
  int32_t tm_year;
  int32_t tm_wday;
  int32_t tm_yday;
  int32_t tm_isdst;
  int64_t tm_gmtoff;
  const char* tm_zone;
} DarwinArtAndroidTm;

typedef enum DarwinArtBionicStrftimeLifecycleStatus {
  DARWIN_ART_BIONIC_STRFTIME_OK = 0,
  DARWIN_ART_BIONIC_STRFTIME_INVALID_ARGUMENT = 1,
  DARWIN_ART_BIONIC_STRFTIME_ALREADY_ACTIVE = 2,
  DARWIN_ART_BIONIC_STRFTIME_NOT_ACTIVE = 3,
} DarwinArtBionicStrftimeLifecycleStatus;

/* Installs an immutable fixed-offset timezone model. Offsets are seconds east
 * of UTC. Names are owned copies and must be nonempty portable TZ abbreviations.
 * This seam intentionally does not claim IANA transition-rule support. */
DarwinArtBionicStrftimeLifecycleStatus darwin_art_bionic_strftime_activate(
    const char* standard_name, int32_t standard_offset,
    const char* daylight_name, int32_t daylight_offset);
/* Stops new calls and waits for every in-flight formatter before returning. */
DarwinArtBionicStrftimeLifecycleStatus
darwin_art_bionic_strftime_deactivate(void);

size_t darwin_art_bionic_strftime_l(char* destination, size_t capacity,
                                    const char* format,
                                    const DarwinArtAndroidTm* broken_down,
                                    void* locale);
size_t darwin_art_bionic_strftime(char* destination, size_t capacity,
                                  const char* format,
                                  const DarwinArtAndroidTm* broken_down);
char* darwin_art_bionic_strptime(const char* input, const char* format,
                                 DarwinArtAndroidTm* broken_down);
void darwin_art_bionic_tzset(void);

typedef void (*DarwinArtBionicStrftimeFunction)(void);
DarwinArtBionicStrftimeFunction darwin_art_bionic_strftime_resolve(
    const char* soname, const char* symbol, const char* version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_STRFTIME_H_
