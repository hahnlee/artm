#ifndef DARWIN_ART_BIONIC_SWPRINTF_H_
#define DARWIN_ART_BIONIC_SWPRINTF_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DarwinArtAndroidWchar;
typedef void (*DarwinArtBionicSwprintfFunction)(void);

int darwin_art_bionic_swprintf(DarwinArtAndroidWchar* output, size_t capacity,
                               const DarwinArtAndroidWchar* format, ...);
int darwin_art_bionic_swprintf_captured(
    DarwinArtAndroidWchar* output, size_t capacity,
    const DarwinArtAndroidWchar* format, const uint8_t* fp_registers,
    uint8_t* stack);
DarwinArtBionicSwprintfFunction darwin_art_bionic_swprintf_resolve(
    const char* soname, const char* symbol, const char* version);

#ifdef __cplusplus
}
#endif

#endif
