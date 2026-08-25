#ifndef DARWIN_ART_BIONIC_FORMATTED_STDIO_H_
#define DARWIN_ART_BIONIC_FORMATTED_STDIO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "darwin_art_bionic_stdio.h"

typedef void (*DarwinArtBionicFormattedStdioFunction)(void);

int darwin_art_bionic_fprintf(DarwinArtAndroidFile* file, const char* format, ...);
int darwin_art_bionic_vfprintf(DarwinArtAndroidFile* file, const char* format,
                               const void* android_va_list);
int darwin_art_bionic_printf(const char* format, ...);
DarwinArtBionicFormattedStdioFunction darwin_art_bionic_formatted_stdio_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_formatted_stdio_capability(const char* name);

#ifdef __cplusplus
}
#endif
#endif
