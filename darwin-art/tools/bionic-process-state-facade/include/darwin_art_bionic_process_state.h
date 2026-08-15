#ifndef DARWIN_ART_BIONIC_PROCESS_STATE_H_
#define DARWIN_ART_BIONIC_PROCESS_STATE_H_

#include <stdint.h>

typedef void (*DarwinArtBionicProcessFunction)(void);

char* darwin_art_bionic_getenv(const char* name);
int darwin_art_bionic___system_property_get(const char* name, char* value);
unsigned long darwin_art_bionic_getauxval(unsigned long type);
DarwinArtBionicProcessFunction darwin_art_bionic_process_state_resolve(
    const char* name);

char* darwin_art_bionic_process_getenv_core(const char* name);
int darwin_art_bionic_process_property_get_core(const char* name, char* value);
unsigned long darwin_art_bionic_process_getauxval_core(unsigned long type);

#endif
