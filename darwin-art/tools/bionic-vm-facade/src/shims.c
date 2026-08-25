#include "darwin_art_bionic_vm.h"

#include <errno.h>

extern void darwin_art_bionic_errno_store(int32_t);
#include <stddef.h>

#define WRAP(saved, call)                       \
  do {                                          \
    const int saved = errno;                    \
    const __typeof__(call) result = (call);      \
    errno = saved;                              \
    return result;                              \
  } while (0)

void* darwin_art_bionic_mmap(void* address, size_t length, int protection,
                             int flags, int fd, int64_t offset) {
  WRAP(saved, darwin_art_bionic_vm_mmap_core(address, length, protection, flags,
                                             fd, offset));
}

void* darwin_art_bionic_mmap64(void* address, size_t length, int protection,
                               int flags, int fd, int64_t offset) {
  WRAP(saved, darwin_art_bionic_vm_mmap_core(address, length, protection, flags,
                                             fd, offset));
}

int darwin_art_bionic_munmap(void* address, size_t length) {
  WRAP(saved, darwin_art_bionic_vm_munmap_core(address, length));
}

int darwin_art_bionic_mprotect(void* address, size_t length, int protection) {
  WRAP(saved,
       darwin_art_bionic_vm_mprotect_core(address, length, protection));
}

int darwin_art_bionic_madvise(void* address, size_t length, int advice) {
  WRAP(saved, darwin_art_bionic_vm_madvise_core(address, length, advice));
}

int darwin_art_bionic_mlock_unsupported(const void* address, size_t length) {
  (void)address;
  (void)length;
  darwin_art_bionic_errno_store(38);
  return -1;
}

static int Compare(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return (unsigned char)*left < (unsigned char)*right
             ? -1
             : ((unsigned char)*left != (unsigned char)*right);
}

typedef struct Binding {
  const char* name;
  DarwinArtBionicVmFunction address;
} Binding;

static const Binding kBindings[] = {
    {"madvise", (DarwinArtBionicVmFunction)darwin_art_bionic_madvise},
    {"mlock", (DarwinArtBionicVmFunction)darwin_art_bionic_mlock_unsupported},
    {"mmap", (DarwinArtBionicVmFunction)darwin_art_bionic_mmap},
    {"mmap64", (DarwinArtBionicVmFunction)darwin_art_bionic_mmap64},
    {"mprotect", (DarwinArtBionicVmFunction)darwin_art_bionic_mprotect},
    {"munmap", (DarwinArtBionicVmFunction)darwin_art_bionic_munmap},
};

DarwinArtBionicVmFunction darwin_art_bionic_vm_resolve(
    const char* import_name) {
  if (import_name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = Compare(import_name, kBindings[middle].name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}
