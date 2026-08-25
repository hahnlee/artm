#ifndef DARWIN_ART_BIONIC_VM_H_
#define DARWIN_ART_BIONIC_VM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicVmFunction)(void);

void* darwin_art_bionic_mmap(void*, size_t, int, int, int, int64_t);
void* darwin_art_bionic_mmap64(void*, size_t, int, int, int, int64_t);
int darwin_art_bionic_munmap(void*, size_t);
int darwin_art_bionic_mprotect(void*, size_t, int);
int darwin_art_bionic_madvise(void*, size_t, int);
DarwinArtBionicVmFunction darwin_art_bionic_vm_resolve(const char*);
int darwin_art_bionic_vm_process_install(void);
int darwin_art_bionic_vm_process_uninstall(void);

void* darwin_art_bionic_vm_mmap_core(void*, size_t, int, int, int, int64_t);
int darwin_art_bionic_vm_munmap_core(void*, size_t);
int darwin_art_bionic_vm_mprotect_core(void*, size_t, int);
int darwin_art_bionic_vm_madvise_core(void*, size_t, int);

size_t darwin_art_host_vm_page_size(void);
void* darwin_art_host_vm_map(size_t, int, int*);
int darwin_art_host_vm_unmap(void*, size_t, int*);
int darwin_art_host_vm_protect(void*, size_t, int, int*);
int darwin_art_host_vm_advise(void*, size_t, int, int*);
void darwin_art_host_vm_invalidate_icache(void*, size_t);

#ifdef __cplusplus
}
#endif

#endif
