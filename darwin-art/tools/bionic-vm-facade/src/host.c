#include "darwin_art_bionic_vm.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(PROT_NONE == 0 && PROT_READ == 1 && PROT_WRITE == 2 &&
                   PROT_EXEC == 4,
               "Darwin protection constants drift");
_Static_assert(MAP_PRIVATE == 2 && MAP_FIXED == 0x10 && MAP_ANON == 0x1000,
               "Darwin mapping constants drift");
_Static_assert(MAP_NORESERVE == 0x40, "Darwin MAP_NORESERVE drift");
_Static_assert(MAP_JIT == 0x800, "Darwin MAP_JIT drift");
_Static_assert(MADV_NORMAL == 0 && MADV_RANDOM == 1 && MADV_SEQUENTIAL == 2 &&
                   MADV_WILLNEED == 3 && MADV_DONTNEED == 4 &&
                   MADV_FREE == 5 && MADV_ZERO == 11,
               "Darwin advice constants drift");

size_t darwin_art_host_vm_page_size(void) {
  const int value = getpagesize();
  return value > 0 ? (size_t)value : 0;
}

void* darwin_art_host_vm_map(void* address, size_t length, int protection,
                             int android_flags, int fd, int64_t offset,
                             int* host_error) {
  const int saved = errno;
  errno = 0;
  /* Without MAP_FIXED, both Linux and Darwin interpret a non-null address as
   * a placement hint. PartitionAlloc relies on this for its second cage. */
  int flags = (android_flags & 1) != 0 ? MAP_SHARED : MAP_PRIVATE;
  if ((android_flags & 0x20) != 0) flags |= MAP_ANON;
  if ((android_flags & 0x10) != 0) flags |= MAP_FIXED;
  if ((android_flags & 0x4000) != 0) flags |= MAP_NORESERVE;
  /* V8 reserves its executable CodeRange as inert anonymous memory and later
   * commits it RWX. Apple silicon requires the original reservation to carry
   * MAP_JIT; the Rust side table limits RWX to these exact reservations. */
  if (protection == PROT_NONE && flags == (MAP_PRIVATE | MAP_ANON)) {
    flags |= MAP_JIT;
  }
  void* result = mmap(address, length, protection, flags, fd, offset);
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

void darwin_art_host_vm_jit_write_protect(int enabled) {
  if (pthread_jit_write_protect_supported_np()) {
    pthread_jit_write_protect_np(enabled != 0);
  }
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

void* darwin_art_host_vm_remap_zero(void* address, size_t length,
                                    int protection, int* host_error) {
  const int saved = errno;
  errno = 0;
  void* result = mmap(address, length, protection,
                      MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  *host_error = result == MAP_FAILED ? errno : 0;
  errno = saved;
  return result;
}

void darwin_art_host_vm_invalidate_icache(void* address, size_t length) {
  sys_icache_invalidate(address, length);
}

int darwin_art_host_vm_close_fd(int fd, int* host_error) {
  const int saved = errno;
  errno = 0;
  const int result = close(fd);
  *host_error = result == -1 ? errno : 0;
  errno = saved;
  return result;
}
