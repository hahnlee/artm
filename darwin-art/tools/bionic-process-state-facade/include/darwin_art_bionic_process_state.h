#ifndef DARWIN_ART_BIONIC_PROCESS_STATE_H_
#define DARWIN_ART_BIONIC_PROCESS_STATE_H_

#include <stdint.h>
#include <stddef.h>

typedef void (*DarwinArtBionicProcessFunction)(void);

char* darwin_art_bionic_getenv(const char* name);
int darwin_art_bionic___system_property_get(const char* name, char* value);
unsigned long darwin_art_bionic_getauxval(unsigned long type);
int darwin_art_bionic_rand(void);
void darwin_art_bionic_srand(unsigned seed);
long darwin_art_bionic_random(void);
void darwin_art_bionic_srandom(unsigned seed);
int darwin_art_bionic_rand_r(unsigned* seed);
double darwin_art_bionic_erand48(unsigned short state[3]);
long darwin_art_bionic_nrand48(unsigned short state[3]);
long darwin_art_bionic_jrand48(unsigned short state[3]);
uint32_t darwin_art_bionic_arc4random(void);
int darwin_art_bionic_getpid(void);
unsigned darwin_art_bionic_geteuid(void);
int darwin_art_bionic_getpagesize(void);
int darwin_art_bionic_daemon(int nochdir, int noclose);
int darwin_art_bionic_setjmp(void* environment);
void darwin_art_bionic_longjmp(void* environment, int value);
DarwinArtBionicProcessFunction darwin_art_bionic_process_state_resolve(
    const char* name);

char* darwin_art_bionic_process_getenv_core(const char* name);
int darwin_art_bionic_process_property_get_core(const char* name, char* value);
unsigned long darwin_art_bionic_process_getauxval_core(unsigned long type);

#endif
