#ifndef DARWIN_ART_BIONIC_SYSCALL_H_
#define DARWIN_ART_BIONIC_SYSCALL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicSyscallFunction)(void);

long darwin_art_bionic_syscall(long number, ...);
int darwin_art_bionic_gettid(void);
DarwinArtBionicSyscallFunction darwin_art_bionic_syscall_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_syscall_capability(const char* capability);

/* Diagnostic state used by deterministic contention gates. */
size_t darwin_art_bionic_syscall_waiter_count(const int32_t* address);
void darwin_art_bionic_syscall_spurious_wake(const int32_t* address);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_SYSCALL_H_
