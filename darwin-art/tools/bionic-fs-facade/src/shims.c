#include "darwin_art_bionic_fs.h"

#include <dirent.h>
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
_Static_assert(sizeof(DarwinArtAndroidDirent) == 280,
               "Android arm64 dirent size drift");
_Static_assert(offsetof(DarwinArtAndroidDirent, d_name) == 19,
               "Android arm64 dirent name offset drift");
_Static_assert(DT_UNKNOWN == 0 && DT_FIFO == 1 && DT_CHR == 2 && DT_DIR == 4 &&
                   DT_BLK == 6 && DT_REG == 8 && DT_LNK == 10 &&
                   DT_SOCK == 12 && DT_WHT == 14,
               "Darwin directory type values drifted from explicit translator");

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

int darwin_art_bionic_stat(const char* path, DarwinArtAndroidStat* status) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_stat_core(path, status);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_lstat(const char* path, DarwinArtAndroidStat* status) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_lstat_core(path, status);
  errno = saved_host_errno;
  return result;
}

intptr_t darwin_art_bionic_readlink(const char* path, char* buffer,
                                    size_t size) {
  const int saved_host_errno = errno;
  const intptr_t result = darwin_art_bionic_fs_readlink_core(path, buffer, size);
  errno = saved_host_errno;
  return result;
}

char* darwin_art_bionic_getcwd(char* buffer, size_t size) {
  const int saved_host_errno = errno;
  char* result = darwin_art_bionic_fs_getcwd_core(buffer, size);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_chdir(const char* path) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_chdir_core(path);
  errno = saved_host_errno;
  return result;
}

void* darwin_art_bionic_opendir(const char* path) {
  const int saved_host_errno = errno;
  void* result = darwin_art_bionic_fs_opendir_core(path);
  errno = saved_host_errno;
  return result;
}

DarwinArtAndroidDirent* darwin_art_bionic_readdir(void* directory) {
  const int saved_host_errno = errno;
  DarwinArtAndroidDirent* result = darwin_art_bionic_fs_readdir_core(directory);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_closedir(void* directory) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_closedir_core(directory);
  errno = saved_host_errno;
  return result;
}

void* darwin_art_bionic_fs_host_fdopendir(int fd, int* host_errno) {
  errno = 0;
  DIR* result = fdopendir(fd);
  if (host_errno != NULL) *host_errno = result == NULL ? errno : 0;
  return result;
}

int darwin_art_bionic_fs_host_readdir(void* directory,
                                      DarwinArtHostDirent* entry,
                                      int* host_errno) {
  if (directory == NULL || entry == NULL || host_errno == NULL) return -1;
  errno = 0;
  struct dirent* source = readdir((DIR*)directory);
  if (source == NULL) {
    *host_errno = errno;
    return errno == 0 ? 0 : -1;
  }
  entry->d_ino = source->d_ino;
  entry->d_type = source->d_type;
  entry->d_name_length = source->d_namlen;
  if (entry->d_name_length >= sizeof(entry->d_name)) {
    *host_errno = EIO;
    return -1;
  }
  for (size_t index = 0; index <= entry->d_name_length; ++index) {
    entry->d_name[index] = (uint8_t)source->d_name[index];
  }
  *host_errno = 0;
  return 1;
}

int darwin_art_bionic_fs_host_closedir(void* directory, int* host_errno) {
  if (directory == NULL || host_errno == NULL) return -1;
  errno = 0;
  const int result = closedir((DIR*)directory);
  *host_errno = result == 0 ? 0 : errno;
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
    {"chdir", (DarwinArtBionicFsFunction)darwin_art_bionic_chdir},
    {"close", (DarwinArtBionicFsFunction)darwin_art_bionic_close},
    {"closedir", (DarwinArtBionicFsFunction)darwin_art_bionic_closedir},
    {"fstat", (DarwinArtBionicFsFunction)darwin_art_bionic_fstat},
    {"getcwd", (DarwinArtBionicFsFunction)darwin_art_bionic_getcwd},
    {"lstat", (DarwinArtBionicFsFunction)darwin_art_bionic_lstat},
    {"open", (DarwinArtBionicFsFunction)darwin_art_bionic_open},
    {"openat", (DarwinArtBionicFsFunction)darwin_art_bionic_openat},
    {"opendir", (DarwinArtBionicFsFunction)darwin_art_bionic_opendir},
    {"read", (DarwinArtBionicFsFunction)darwin_art_bionic_read},
    {"readdir", (DarwinArtBionicFsFunction)darwin_art_bionic_readdir},
    {"readlink", (DarwinArtBionicFsFunction)darwin_art_bionic_readlink},
    {"stat", (DarwinArtBionicFsFunction)darwin_art_bionic_stat},
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
