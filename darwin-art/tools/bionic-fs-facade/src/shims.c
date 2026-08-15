#include "darwin_art_bionic_fs.h"

#include <errno.h>
#include <stddef.h>

_Static_assert(sizeof(void*) == 8, "Android and Darwin arm64 pointer width drift");
_Static_assert(sizeof(size_t) == 8, "Android and Darwin arm64 size_t width drift");
_Static_assert(sizeof(intptr_t) == 8, "Android ssize_t/Darwin intptr_t width drift");
_Static_assert(sizeof(DarwinArtAndroidStat) == 128, "Android arm64 stat size drift");
_Static_assert(offsetof(DarwinArtAndroidStat, st_mode) == 16, "stat mode offset drift");
_Static_assert(offsetof(DarwinArtAndroidStat, st_size) == 48, "stat size offset drift");
_Static_assert(offsetof(DarwinArtAndroidStat, st_blocks) == 64,
               "stat blocks offset drift");
_Static_assert(offsetof(DarwinArtAndroidStat, st_atim) == 72,
               "stat timestamp offset drift");

int darwin_art_bionic_open(const char* path, int flags, ...) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_open_core(path, flags);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_openat(int directory_fd, const char* path, int flags, ...) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_openat_core(directory_fd, path, flags);
  errno = saved_host_errno;
  return result;
}

intptr_t darwin_art_bionic_read(int fd, void* buffer, size_t count) {
  const int saved_host_errno = errno;
  const intptr_t result = darwin_art_bionic_fs_read_core(fd, buffer, count);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_close(int fd) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_close_core(fd);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_fstat(int fd, DarwinArtAndroidStat* status) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_fstat_core(fd, status);
  errno = saved_host_errno;
  return result;
}

static int NameCompare(const char* left, const char* right) {
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
  DarwinArtBionicFsFunction address;
} Binding;

static const Binding kBindings[] = {
    {"close", (DarwinArtBionicFsFunction)darwin_art_bionic_close},
    {"fstat", (DarwinArtBionicFsFunction)darwin_art_bionic_fstat},
    {"open", (DarwinArtBionicFsFunction)darwin_art_bionic_open},
    {"openat", (DarwinArtBionicFsFunction)darwin_art_bionic_openat},
    {"read", (DarwinArtBionicFsFunction)darwin_art_bionic_read},
};

DarwinArtBionicFsFunction darwin_art_bionic_fs_resolve(const char* import_name) {
  if (import_name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = NameCompare(import_name, kBindings[middle].name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}
