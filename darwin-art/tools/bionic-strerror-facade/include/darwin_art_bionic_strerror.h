#ifndef DARWIN_ART_BIONIC_STRERROR_H_
#define DARWIN_ART_BIONIC_STRERROR_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicStrerrorFunction)(void);

int darwin_art_bionic_strerror_r(int android_errno, char* buffer, size_t size);
char* darwin_art_bionic___gnu_strerror_r(int android_errno, char* buffer,
                                        size_t size);
char* darwin_art_bionic_strerror(int android_errno);
DarwinArtBionicStrerrorFunction darwin_art_bionic_strerror_resolve(
    const char* import_name);

/* Pure implementation boundary called by the host-errno-preserving shim. */
int darwin_art_bionic_strerror_r_core(int android_errno, char* buffer,
                                     size_t size);

#ifdef __cplusplus
}
#endif

#endif
