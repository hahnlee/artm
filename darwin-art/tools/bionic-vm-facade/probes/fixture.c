#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

static void* gRaceMapping;

__attribute__((visibility("default"))) int bionic_vm_fixture_basic(void) {
  const size_t length = 4096;
  void* mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 1;
  if ((uintptr_t)mapping % 4096 != 0 ||
      ((volatile unsigned char*)mapping)[0] != 0)
    return 23;

  volatile uint32_t* instructions = (volatile uint32_t*)mapping;
  instructions[0] = 0x52800540u; /* mov w0, #42 */
  instructions[1] = 0xd65f03c0u; /* ret */
  if (madvise(mapping, length, MADV_NORMAL) != 0) return 2;
  if (mprotect(mapping, length, PROT_READ | PROT_EXEC) != 0) return 3;

  union {
    void* object;
    int (*function)(void);
  } code = {.object = mapping};
  if (code.function() != 42) return 4;

  errno = 0;
  if (mprotect(mapping, length, PROT_READ | PROT_WRITE | PROT_EXEC) != -1 ||
      errno != EACCES)
    return 5;
  if (mprotect(mapping, length, PROT_READ | PROT_WRITE) != 0) return 6;
  ((volatile unsigned char*)mapping)[0] = 0xaa;
  if (munmap(mapping, length) != 0) return 7;

  mapping = mmap64(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
  if (mapping == MAP_FAILED || munmap(mapping, length) != 0) return 8;

  errno = 0;
  if (mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
          MAP_FAILED ||
      errno != EINVAL)
    return 9;
  errno = 0;
  if (mmap(NULL, SIZE_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
          MAP_FAILED ||
      errno != EOVERFLOW)
    return 10;
  errno = 0;
  if (mmap(NULL, length, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) !=
          MAP_FAILED ||
      errno != EOPNOTSUPP)
    return 11;
  errno = 0;
  if (mmap((void*)4096, length, PROT_READ,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED ||
      errno != EOPNOTSUPP)
    return 12;
  errno = 0;
  if (mmap(NULL, length, PROT_READ, MAP_PRIVATE, 3, 0) != MAP_FAILED ||
      errno != EOPNOTSUPP)
    return 13;
  errno = 0;
  if (mmap(NULL, length, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
      errno != EACCES)
    return 25;
  errno = 0;
  if (mmap(NULL, length, 0x8, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
          MAP_FAILED ||
      errno != EINVAL)
    return 26;
  errno = 0;
  if (mmap64(NULL, length, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1,
             16384) != MAP_FAILED ||
      errno != EOPNOTSUPP)
    return 14;
  errno = 0;
  if (mmap64(NULL, length, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 1) !=
          MAP_FAILED ||
      errno != EINVAL)
    return 21;

  mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, 12345, 0);
  if (mapping == MAP_FAILED) return 15;
  errno = 0;
  if (mprotect(mapping, 0, PROT_READ) != -1 || errno != EINVAL) return 27;
  errno = 0;
  if (madvise(mapping, 0, MADV_NORMAL) != -1 || errno != EINVAL) return 28;
  errno = 0;
  if (munmap(mapping, 0) != -1 || errno != EINVAL) return 29;
  errno = 0;
  if (mprotect((unsigned char*)mapping + 1, length, PROT_READ) != -1 ||
      errno != EINVAL)
    return 16;
  errno = 0;
  if (mprotect(mapping, length / 2, PROT_READ) != -1 ||
      errno != EOPNOTSUPP)
    return 17;
  errno = 0;
  if (munmap(mapping, length / 2) != -1 || errno != EOPNOTSUPP) return 34;
  errno = 0;
  if (mprotect((void*)16384, 16384, PROT_READ) != -1 || errno != ENOMEM)
    return 35;
  errno = 0;
  if (madvise((void*)16384, 16384, MADV_NORMAL) != -1 || errno != ENOMEM)
    return 36;
  ((volatile unsigned char*)mapping)[0] = 0x7b;
  if (madvise(mapping, length, MADV_DONTNEED) != 0 ||
      ((volatile unsigned char*)mapping)[0] != 0)
    return 18;
  if (madvise(mapping, length, MADV_FREE) != 0) return 22;
  errno = 0;
  if (madvise(mapping, length, 999) != -1 || errno != EINVAL) return 19;
  if (munmap(mapping, length) != 0) return 20;
  return 42;
}

__attribute__((visibility("default"))) int bionic_vm_fixture_race_setup(void) {
  gRaceMapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return gRaceMapping == MAP_FAILED ? 30 : 42;
}

__attribute__((visibility("default"))) int bionic_vm_fixture_race_protect(void) {
  if (mprotect(gRaceMapping, 4096, PROT_READ) == -1) {
    return errno == ENOMEM ? 42 : 31;
  }
  if (mprotect(gRaceMapping, 4096, PROT_READ | PROT_WRITE) == -1) {
    return errno == ENOMEM ? 42 : 32;
  }
  return 42;
}

__attribute__((visibility("default"))) int bionic_vm_fixture_race_unmap(void) {
  return munmap(gRaceMapping, 4096) == 0 ? 42 : 33;
}
