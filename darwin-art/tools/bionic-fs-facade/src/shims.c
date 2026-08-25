#include "darwin_art_bionic_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <sys/statvfs.h>
#include <unistd.h>

extern void darwin_art_bionic_errno_store(int32_t android_errno);
extern int32_t darwin_art_bionic_errno_load(void);

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
_Static_assert(sizeof(DarwinArtAndroidTimespec) == 16,
               "Android arm64 timespec size drift");
_Static_assert(sizeof(DarwinArtAndroidStatvfs) == 112,
               "Android arm64 statvfs size drift");
_Static_assert(offsetof(DarwinArtAndroidStatvfs, f_flag) == 72,
               "Android arm64 statvfs flag offset drift");
_Static_assert(sizeof(DarwinArtHostStatvfs) == 88,
               "portable host statvfs size drift");
_Static_assert(DT_UNKNOWN == 0 && DT_FIFO == 1 && DT_CHR == 2 && DT_DIR == 4 &&
                   DT_BLK == 6 && DT_REG == 8 && DT_LNK == 10 &&
                   DT_SOCK == 12 && DT_WHT == 14,
               "Darwin directory type values drifted from explicit translator");

int darwin_art_bionic_open(const char* path, int flags, uint32_t mode) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_open_core(path, flags, mode);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_openat(int directory_fd, const char* path, int flags,
                             uint32_t mode) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_fs_openat_core(directory_fd, path, flags, mode);
  errno = saved_host_errno;
  return result;
}

intptr_t darwin_art_bionic_read(int fd, void* buffer, size_t count) {
  const int saved_host_errno = errno;
  const intptr_t result = darwin_art_bionic_fs_read_core(fd, buffer, count);
  errno = saved_host_errno;
  return result;
}

intptr_t darwin_art_bionic_pread(int fd, void* buffer, size_t count,
                                 int64_t offset) {
  const int saved_host_errno = errno;
  const intptr_t result =
      darwin_art_bionic_fs_pread_core(fd, buffer, count, offset);
  errno = saved_host_errno;
  return result;
}

intptr_t darwin_art_bionic_write(int fd, const void* buffer, size_t count) {
  const int saved_host_errno = errno;
  const intptr_t result = darwin_art_bionic_fs_write_core(fd, buffer, count);
  errno = saved_host_errno;
  return result;
}

int64_t darwin_art_bionic_lseek(int fd, int64_t offset, int whence) {
  const int saved_host_errno = errno;
  const int64_t result = darwin_art_bionic_fs_lseek_core(fd, offset, whence);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_access(const char* path, int mode) {
  if ((mode & ~7) != 0) return -1;
  DarwinArtAndroidStat status;
  return darwin_art_bionic_stat(path, &status);
}

int darwin_art_bionic_fdatasync(int fd) {
  DarwinArtAndroidStat status;
  return darwin_art_bionic_fstat(fd, &status);
}

int darwin_art_bionic_fsync(int fd) {
  return darwin_art_bionic_fdatasync(fd);
}

int darwin_art_bionic_unlink(const char* path) {
  return darwin_art_bionic_unlinkat(DARWIN_ART_ANDROID_AT_FDCWD, path, 0);
}

int darwin_art_bionic_rmdir(const char* path) {
  return darwin_art_bionic_unlinkat(DARWIN_ART_ANDROID_AT_FDCWD, path,
                                    DARWIN_ART_ANDROID_AT_REMOVEDIR);
}

int darwin_art_bionic___open_2(const char* path, int flags) {
  return darwin_art_bionic_open(path, flags, 0);
}

intptr_t darwin_art_bionic___read_chk(int fd, void* buffer, size_t count,
                                      size_t buffer_size) {
  if (count > buffer_size) return -1;
  return darwin_art_bionic_read(fd, buffer, count);
}

intptr_t darwin_art_bionic___write_chk(int fd, const void* buffer,
                                       size_t count, size_t buffer_size) {
  if (count > buffer_size) return -1;
  return darwin_art_bionic_write(fd, buffer, count);
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

void* darwin_art_bionic_fdopendir(int fd) {
  const int saved_host_errno = errno;
  void* result = darwin_art_bionic_fs_fdopendir_core(fd);
  errno = saved_host_errno;
  return result;
}

DarwinArtAndroidDirent* darwin_art_bionic_readdir(void* directory) {
  const int saved_host_errno = errno;
  DarwinArtAndroidDirent* result = darwin_art_bionic_fs_readdir_core(directory);
  errno = saved_host_errno;
  return result;
}

void darwin_art_bionic_rewinddir(void* directory) {
  const int saved_host_errno = errno;
  darwin_art_bionic_fs_rewinddir_core(directory);
  errno = saved_host_errno;
}

int64_t darwin_art_bionic_lseek64(int fd, int64_t offset, int whence) {
  return darwin_art_bionic_lseek(fd, offset, whence);
}

int darwin_art_bionic_fstat64(int fd, DarwinArtAndroidStat* status) {
  return darwin_art_bionic_fstat(fd, status);
}

int darwin_art_bionic_lstat64(const char* path, DarwinArtAndroidStat* status) {
  return darwin_art_bionic_lstat(path, status);
}

int darwin_art_bionic_posix_fadvise(int fd, int64_t offset, int64_t length,
                                    int advice) {
  (void)fd;
  (void)offset;
  (void)length;
  (void)advice;
  return 0;
}

int darwin_art_bionic_mkstemp(char* path_template) {
  static _Atomic uint32_t sequence = UINT32_C(0x13579bdf);
  if (path_template == NULL) {
    darwin_art_bionic_errno_store(14);
    return -1;
  }
  size_t length = 0;
  while (path_template[length] != '\0') ++length;
  if (length < 6) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  for (size_t index = length - 6; index < length; ++index) {
    if (path_template[index] != 'X') {
      darwin_art_bionic_errno_store(22);
      return -1;
    }
  }
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (uint32_t attempt = 0; attempt < 256; ++attempt) {
    uint32_t value = atomic_fetch_add_explicit(
        &sequence, UINT32_C(0x9e3779b9), memory_order_relaxed);
    value ^= (uint32_t)(uintptr_t)path_template;
    for (size_t index = 0; index < 6; ++index) {
      value = value * UINT32_C(1664525) + UINT32_C(1013904223);
      path_template[length - 6 + index] = alphabet[value % 62];
    }
    const int fd = darwin_art_bionic_open(path_template, 2 | 64 | 128, 0600);
    if (fd >= 0) return fd;
  }
  for (size_t index = length - 6; index < length; ++index)
    path_template[index] = 'X';
  return -1;
}

int darwin_art_bionic_mkstemp64(char* path_template) {
  return darwin_art_bionic_mkstemp(path_template);
}

int darwin_art_bionic_flock_unsupported(int fd, int operation) {
  (void)fd;
  (void)operation;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_fstatfs_unsupported(int fd, void* status) {
  (void)fd;
  (void)status;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_dirfd_unsupported(void* directory) {
  (void)directory;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_fstatat_unsupported(int directory_fd, const char* path,
                                         DarwinArtAndroidStat* status,
                                         int flags) {
  (void)directory_fd;
  (void)path;
  (void)status;
  (void)flags;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_readdir_r(void* directory, DarwinArtAndroidDirent* entry,
                                DarwinArtAndroidDirent** result) {
  if (entry == NULL || result == NULL) return 22;
  DarwinArtAndroidDirent* found = darwin_art_bionic_readdir(directory);
  if (found == NULL) {
    *result = NULL;
    const int error = darwin_art_bionic_errno_load();
    return error == 0 ? 0 : error;
  }
  *entry = *found;
  *result = entry;
  return 0;
}

int darwin_art_bionic_closedir(void* directory) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_closedir_core(directory);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_fchmod(int fd, uint32_t mode) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_fchmod_core(fd, mode);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_fchmodat(int directory_fd, const char* path,
                               uint32_t mode, int flags) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_fs_fchmodat_core(directory_fd, path, mode, flags);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_ftruncate(int fd, int64_t length) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_ftruncate_core(fd, length);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_isatty(int fd) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_isatty_core(fd);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_link(const char* old_path, const char* new_path) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_link_core(old_path, new_path);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_mkdir(const char* path, uint32_t mode) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_mkdir_core(path, mode);
  errno = saved_host_errno;
  return result;
}

int64_t darwin_art_bionic_pathconf(const char* path, int name) {
  const int saved_host_errno = errno;
  const int64_t result = darwin_art_bionic_fs_pathconf_core(path, name);
  errno = saved_host_errno;
  return result;
}

char* darwin_art_bionic_realpath(const char* path, char* resolved) {
  const int saved_host_errno = errno;
  char* result = darwin_art_bionic_fs_realpath_core(path, resolved);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_remove(const char* path) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_remove_core(path);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_rename(const char* old_path, const char* new_path) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_rename_core(old_path, new_path);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_statvfs(const char* path,
                              DarwinArtAndroidStatvfs* status) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_statvfs_core(path, status);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_symlink(const char* target, const char* link_path) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_symlink_core(target, link_path);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_truncate(const char* path, int64_t length) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_fs_truncate_core(path, length);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_unlinkat(int directory_fd, const char* path, int flags) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_fs_unlinkat_core(directory_fd, path, flags);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic_utimensat(int directory_fd, const char* path,
                                const DarwinArtAndroidTimespec times[2],
                                int flags) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_fs_utimensat_core(directory_fd, path, times, flags);
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

void darwin_art_bionic_fs_host_rewinddir(void* directory) {
  if (directory != NULL) rewinddir((DIR*)directory);
}

int darwin_art_bionic_fs_host_closedir(void* directory, int* host_errno) {
  if (directory == NULL || host_errno == NULL) return -1;
  errno = 0;
  const int result = closedir((DIR*)directory);
  *host_errno = result == 0 ? 0 : errno;
  return result;
}

int darwin_art_bionic_fs_host_fpathconf(int fd, int semantic_name,
                                        int64_t* value, int* host_errno) {
  if (value == NULL || host_errno == NULL) return -1;
  int host_name;
  switch (semantic_name) {
    case 0: host_name = _PC_FILESIZEBITS; break;
    case 1: host_name = _PC_LINK_MAX; break;
    case 2: host_name = _PC_MAX_CANON; break;
    case 3: host_name = _PC_MAX_INPUT; break;
    case 4: host_name = _PC_NAME_MAX; break;
    case 5: host_name = _PC_PATH_MAX; break;
    case 6: host_name = _PC_PIPE_BUF; break;
    case 7: host_name = _PC_2_SYMLINKS; break;
    case 8: host_name = _PC_ALLOC_SIZE_MIN; break;
    case 9: host_name = _PC_REC_INCR_XFER_SIZE; break;
    case 10: host_name = _PC_REC_MAX_XFER_SIZE; break;
    case 11: host_name = _PC_REC_MIN_XFER_SIZE; break;
    case 12: host_name = _PC_REC_XFER_ALIGN; break;
    case 13: host_name = _PC_SYMLINK_MAX; break;
    case 14: host_name = _PC_CHOWN_RESTRICTED; break;
    case 15: host_name = _PC_NO_TRUNC; break;
    case 16: host_name = _PC_VDISABLE; break;
    case 17: host_name = _PC_ASYNC_IO; break;
    case 18: host_name = _PC_PRIO_IO; break;
    case 19: host_name = _PC_SYNC_IO; break;
    default:
      *host_errno = EINVAL;
      return -1;
  }
  errno = 0;
  const long result = fpathconf(fd, host_name);
  *value = (int64_t)result;
  *host_errno = errno;
  if (result != -1) return 1;
  return errno == 0 ? 0 : -1;
}

int darwin_art_bionic_fs_host_fstatvfs(int fd,
                                       DarwinArtHostStatvfs* status,
                                       int* host_errno) {
  if (status == NULL || host_errno == NULL) return -1;
  struct statvfs host;
  errno = 0;
  if (fstatvfs(fd, &host) != 0) {
    *host_errno = errno;
    return -1;
  }
  status->f_bsize = host.f_bsize;
  status->f_frsize = host.f_frsize;
  status->f_blocks = host.f_blocks;
  status->f_bfree = host.f_bfree;
  status->f_bavail = host.f_bavail;
  status->f_files = host.f_files;
  status->f_ffree = host.f_ffree;
  status->f_favail = host.f_favail;
  status->f_fsid = host.f_fsid;
  status->f_flag = 0;
  if ((host.f_flag & ST_RDONLY) != 0) status->f_flag |= 0x0001;
  if ((host.f_flag & ST_NOSUID) != 0) status->f_flag |= 0x0002;
  status->f_namemax = host.f_namemax;
  *host_errno = 0;
  return 0;
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
    {"__open_2", (DarwinArtBionicFsFunction)darwin_art_bionic___open_2},
    {"__read_chk", (DarwinArtBionicFsFunction)darwin_art_bionic___read_chk},
    {"__write_chk", (DarwinArtBionicFsFunction)darwin_art_bionic___write_chk},
    {"access", (DarwinArtBionicFsFunction)darwin_art_bionic_access},
    {"chdir", (DarwinArtBionicFsFunction)darwin_art_bionic_chdir},
    {"close", (DarwinArtBionicFsFunction)darwin_art_bionic_close},
    {"closedir", (DarwinArtBionicFsFunction)darwin_art_bionic_closedir},
    {"dirfd", (DarwinArtBionicFsFunction)darwin_art_bionic_dirfd_unsupported},
    {"fchmod", (DarwinArtBionicFsFunction)darwin_art_bionic_fchmod},
    {"fchmodat", (DarwinArtBionicFsFunction)darwin_art_bionic_fchmodat},
    {"fdatasync", (DarwinArtBionicFsFunction)darwin_art_bionic_fdatasync},
    {"fdopendir", (DarwinArtBionicFsFunction)darwin_art_bionic_fdopendir},
    {"flock", (DarwinArtBionicFsFunction)darwin_art_bionic_flock_unsupported},
    {"fstat", (DarwinArtBionicFsFunction)darwin_art_bionic_fstat},
    {"fstat64", (DarwinArtBionicFsFunction)darwin_art_bionic_fstat64},
    {"fstatat", (DarwinArtBionicFsFunction)darwin_art_bionic_fstatat_unsupported},
    {"fstatfs", (DarwinArtBionicFsFunction)darwin_art_bionic_fstatfs_unsupported},
    {"fsync", (DarwinArtBionicFsFunction)darwin_art_bionic_fsync},
    {"ftruncate", (DarwinArtBionicFsFunction)darwin_art_bionic_ftruncate},
    {"getcwd", (DarwinArtBionicFsFunction)darwin_art_bionic_getcwd},
    {"isatty", (DarwinArtBionicFsFunction)darwin_art_bionic_isatty},
    {"link", (DarwinArtBionicFsFunction)darwin_art_bionic_link},
    {"lseek", (DarwinArtBionicFsFunction)darwin_art_bionic_lseek},
    {"lseek64", (DarwinArtBionicFsFunction)darwin_art_bionic_lseek64},
    {"lstat", (DarwinArtBionicFsFunction)darwin_art_bionic_lstat},
    {"lstat64", (DarwinArtBionicFsFunction)darwin_art_bionic_lstat64},
    {"mkdir", (DarwinArtBionicFsFunction)darwin_art_bionic_mkdir},
    {"mkstemp", (DarwinArtBionicFsFunction)darwin_art_bionic_mkstemp},
    {"mkstemp64", (DarwinArtBionicFsFunction)darwin_art_bionic_mkstemp64},
    {"open", (DarwinArtBionicFsFunction)darwin_art_bionic_open},
    {"openat", (DarwinArtBionicFsFunction)darwin_art_bionic_openat},
    {"opendir", (DarwinArtBionicFsFunction)darwin_art_bionic_opendir},
    {"pathconf", (DarwinArtBionicFsFunction)darwin_art_bionic_pathconf},
    {"posix_fadvise", (DarwinArtBionicFsFunction)darwin_art_bionic_posix_fadvise},
    {"pread", (DarwinArtBionicFsFunction)darwin_art_bionic_pread},
    {"read", (DarwinArtBionicFsFunction)darwin_art_bionic_read},
    {"readdir", (DarwinArtBionicFsFunction)darwin_art_bionic_readdir},
    {"readdir_r", (DarwinArtBionicFsFunction)darwin_art_bionic_readdir_r},
    {"readlink", (DarwinArtBionicFsFunction)darwin_art_bionic_readlink},
    {"realpath", (DarwinArtBionicFsFunction)darwin_art_bionic_realpath},
    {"remove", (DarwinArtBionicFsFunction)darwin_art_bionic_remove},
    {"rename", (DarwinArtBionicFsFunction)darwin_art_bionic_rename},
    {"rewinddir", (DarwinArtBionicFsFunction)darwin_art_bionic_rewinddir},
    {"rmdir", (DarwinArtBionicFsFunction)darwin_art_bionic_rmdir},
    {"stat", (DarwinArtBionicFsFunction)darwin_art_bionic_stat},
    {"statvfs", (DarwinArtBionicFsFunction)darwin_art_bionic_statvfs},
    {"symlink", (DarwinArtBionicFsFunction)darwin_art_bionic_symlink},
    {"truncate", (DarwinArtBionicFsFunction)darwin_art_bionic_truncate},
    {"unlink", (DarwinArtBionicFsFunction)darwin_art_bionic_unlink},
    {"unlinkat", (DarwinArtBionicFsFunction)darwin_art_bionic_unlinkat},
    {"utimensat", (DarwinArtBionicFsFunction)darwin_art_bionic_utimensat},
    {"write", (DarwinArtBionicFsFunction)darwin_art_bionic_write},
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
