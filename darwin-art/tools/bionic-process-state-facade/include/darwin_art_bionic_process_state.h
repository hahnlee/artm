#ifndef DARWIN_ART_BIONIC_PROCESS_STATE_H_
#define DARWIN_ART_BIONIC_PROCESS_STATE_H_

#include <stdint.h>
#include <stddef.h>

typedef void (*DarwinArtBionicProcessFunction)(void);
typedef int (*DarwinArtBionicJitFaultRecovery)(uintptr_t fault_address,
                                                int execution_fault);
typedef int (*DarwinArtBionicSigchainOwnsSignal)(int signal_number);
typedef void (*DarwinArtBionicEnsureFrontOfChain)(int signal_number);

char* darwin_art_bionic_getenv(const char* name);
int darwin_art_bionic_process_state_process_install(void);
int darwin_art_bionic_process_state_process_uninstall(void);
void darwin_art_bionic_process_state_bind_jit_fault_recovery(
    DarwinArtBionicJitFaultRecovery recovery);
void darwin_art_bionic_process_state_bind_sigchain(
    DarwinArtBionicSigchainOwnsSignal owns_signal,
    DarwinArtBionicEnsureFrontOfChain ensure_front);
int darwin_art_bionic___system_property_get(const char* name, char* value);
const void* darwin_art_bionic___system_property_find(const char* name);
int darwin_art_bionic___system_property_read(const void* property, char* name,
                                             char* value);
void darwin_art_bionic___system_property_read_callback(
    const void* property,
    void (*callback)(void*, const char*, const char*, uint32_t), void* cookie);
unsigned long darwin_art_bionic_getauxval(unsigned long type);
int darwin_art_bionic_rand(void);
void darwin_art_bionic_srand(unsigned seed);
long darwin_art_bionic_random(void);
void darwin_art_bionic_srandom(unsigned seed);
int darwin_art_bionic_rand_r(unsigned* seed);
double darwin_art_bionic_erand48(unsigned short state[3]);
long darwin_art_bionic_nrand48(unsigned short state[3]);
long darwin_art_bionic_jrand48(unsigned short state[3]);
void darwin_art_bionic_srand48(long seed);
double darwin_art_bionic_drand48(void);
long darwin_art_bionic_lrand48(void);
long darwin_art_bionic_mrand48(void);
uint32_t darwin_art_bionic_arc4random(void);
int darwin_art_bionic_getpid(void);
unsigned darwin_art_bionic_geteuid(void);
int darwin_art_bionic_getpagesize(void);
int darwin_art_bionic_daemon(int nochdir, int noclose);
int darwin_art_bionic_posix_spawn(int* process_id, const char* path,
                                  const void* file_actions,
                                  const void* attributes, char* const argv[],
                                  char* const environment[]);
int darwin_art_bionic_setjmp(void* environment);
void darwin_art_bionic_longjmp(void* environment, int value);
DarwinArtBionicProcessFunction darwin_art_bionic_process_state_resolve(
    const char* name);
uintptr_t darwin_art_bionic_process_state_data_resolve(const char* name);

char* darwin_art_bionic_process_getenv_core(const char* name);
int darwin_art_bionic_process_property_get_core(const char* name, char* value);
const void* darwin_art_bionic_process_property_find_core(const char* name);
void darwin_art_bionic_process_property_read_callback_core(
    const void* property,
    void (*callback)(void*, const char*, const char*, uint32_t), void* cookie);
unsigned long darwin_art_bionic_process_getauxval_core(unsigned long type);

#endif
