#ifndef DARWIN_ART_BIONIC_FORMAT_H_
#define DARWIN_ART_BIONIC_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicFormatFunction)(void);

int darwin_art_bionic_snprintf(char* dst, size_t size, const char* format, ...);
int darwin_art_bionic_sprintf(char* dst, const char* format, ...);
int darwin_art_bionic_asprintf(char** output, const char* format, ...);
int darwin_art_bionic_vsnprintf(char* dst, size_t size, const char* format,
                                const void* android_va_list);
int darwin_art_bionic_vasprintf(char** output, const char* format,
                                const void* android_va_list);
DarwinArtBionicFormatFunction darwin_art_bionic_format_resolve(const char* symbol);
const char* darwin_art_bionic_format_capability(const char* symbol);

#ifdef __cplusplus
}
#endif
#endif
