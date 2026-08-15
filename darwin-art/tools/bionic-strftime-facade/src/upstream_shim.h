#ifndef DARWIN_ART_BIONIC_STRFTIME_UPSTREAM_SHIM_H_
#define DARWIN_ART_BIONIC_STRFTIME_UPSTREAM_SHIM_H_

#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char* darwin_art_bionic_strftime_tzname[2];
void darwin_art_bionic_strftime_tzset(void);
time_t darwin_art_bionic_strftime_mktime(struct tm* value);
int darwin_art_bionic_strftime_ascii_tolower(int value);
int darwin_art_bionic_strftime_ascii_toupper(int value);
int darwin_art_bionic_strftime_ascii_islower(int value);
int darwin_art_bionic_strftime_ascii_isupper(int value);
int darwin_art_bionic_strftime_decimal(char* destination,
                                       const char* format, ...);
size_t darwin_art_bionic_strftime_upstream(
    char* restrict destination, size_t capacity,
    const char* restrict format, const struct tm* restrict broken_down);
size_t darwin_art_bionic_strftime_upstream_l(
    char* restrict destination, size_t capacity,
    const char* restrict format, const struct tm* restrict broken_down,
    locale_t locale);

#ifdef __cplusplus
}
#endif

/* Apply the namespace substitutions after Darwin's headers have installed
 * their fortified/inline macros. The exact AOSP source includes these headers
 * again, but their include guards leave this closed provider namespace intact. */
#undef islower
#undef isupper
#undef sprintf
#undef strftime
#undef strftime_l
#undef tolower
#undef toupper
#undef tzname
#define islower darwin_art_bionic_strftime_ascii_islower
#define isupper darwin_art_bionic_strftime_ascii_isupper
#define sprintf darwin_art_bionic_strftime_decimal
#define strftime darwin_art_bionic_strftime_upstream
#define strftime_l darwin_art_bionic_strftime_upstream_l
#define tolower darwin_art_bionic_strftime_ascii_tolower
#define toupper darwin_art_bionic_strftime_ascii_toupper
#define tzname darwin_art_bionic_strftime_tzname
#define tzset darwin_art_bionic_strftime_tzset
#define mktime darwin_art_bionic_strftime_mktime

#endif
