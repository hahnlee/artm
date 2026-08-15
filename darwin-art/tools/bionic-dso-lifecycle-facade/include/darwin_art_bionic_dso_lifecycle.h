#ifndef DARWIN_ART_BIONIC_DSO_LIFECYCLE_H_
#define DARWIN_ART_BIONIC_DSO_LIFECYCLE_H_

#include <stddef.h>

typedef void (*DarwinArtBionicDsoDestructor)(void*);
typedef void (*DarwinArtBionicDsoFunction)(void);

int darwin_art_bionic___cxa_atexit(DarwinArtBionicDsoDestructor function,
                                   void* argument, void* dso);
void darwin_art_bionic___cxa_finalize(void* dso);
DarwinArtBionicDsoFunction darwin_art_bionic_dso_lifecycle_resolve(
    const char* name);

int darwin_art_bionic_dso_cxa_atexit_core(
    DarwinArtBionicDsoDestructor function, void* argument, void* dso);
void darwin_art_bionic_dso_cxa_finalize_core(void* dso);

#endif
