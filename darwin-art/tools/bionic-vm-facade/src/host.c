#include "darwin_art_bionic_vm.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(PROT_NONE == 0 && PROT_READ == 1 && PROT_WRITE == 2 &&
                   PROT_EXEC == 4,
               "Darwin protection constants drift");
_Static_assert(MAP_PRIVATE == 2 && MAP_ANON == 0x1000,
               "Darwin mapping constants drift");
_Static_assert(MADV_NORMAL == 0 && MADV_RANDOM == 1 && MADV_SEQUENTIAL == 2 &&
                   MADV_WILLNEED == 3 && MADV_DONTNEED == 4 &&
                   MADV_FREE == 5 && MADV_ZERO == 11,
               "Darwin advice constants drift");

size_t darwin_art_host_vm_page_size(void) {
  const int value = getpagesize();
  return value > 0 ? (size_t)value : 0;
}

void* darwin_art_host_vm_map(size_t length, int protection, int* host_error) {
  const int saved = errno;
  errno = 0;
  void* result = mmap(NULL, length, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
  *host_error = result == MAP_FAILED ? errno : 0;
  errno = saved;
  return result;
}

int darwin_art_host_vm_unmap(void* address, size_t length, int* host_error) {
  const int saved = errno;
  errno = 0;
  const int result = munmap(address, length);
  *host_error = result == -1 ? errno : 0;
  errno = saved;
  return result;
}

int darwin_art_host_vm_protect(void* address, size_t length, int protection,
                               int* host_error) {
  const int saved = errno;
  errno = 0;
  const int result = mprotect(address, length, protection);
  *host_error = result == -1 ? errno : 0;
  errno = saved;
  return result;
}

int darwin_art_host_vm_advise(void* address, size_t length, int advice,
                              int* host_error) {
  const int saved = errno;
  errno = 0;
  const int result = madvise(address, length, advice);
  *host_error = result == -1 ? errno : 0;
  errno = saved;
  return result;
}

void darwin_art_host_vm_invalidate_icache(void* address, size_t length) {
  sys_icache_invalidate(address, length);
}
