#include "darwin_art_bionic_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <sys/statvfs.h>
#include <unistd.h>

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
    {"chdir", (DarwinArtBionicFsFunction)darwin_art_bionic_chdir},
    {"close", (DarwinArtBionicFsFunction)darwin_art_bionic_close},
    {"closedir", (DarwinArtBionicFsFunction)darwin_art_bionic_closedir},
    {"fchmod", (DarwinArtBionicFsFunction)darwin_art_bionic_fchmod},
    {"fchmodat", (DarwinArtBionicFsFunction)darwin_art_bionic_fchmodat},
    {"fdopendir", (DarwinArtBionicFsFunction)darwin_art_bionic_fdopendir},
    {"fstat", (DarwinArtBionicFsFunction)darwin_art_bionic_fstat},
    {"ftruncate", (DarwinArtBionicFsFunction)darwin_art_bionic_ftruncate},
    {"getcwd", (DarwinArtBionicFsFunction)darwin_art_bionic_getcwd},
    {"isatty", (DarwinArtBionicFsFunction)darwin_art_bionic_isatty},
    {"link", (DarwinArtBionicFsFunction)darwin_art_bionic_link},
    {"lstat", (DarwinArtBionicFsFunction)darwin_art_bionic_lstat},
    {"mkdir", (DarwinArtBionicFsFunction)darwin_art_bionic_mkdir},
    {"open", (DarwinArtBionicFsFunction)darwin_art_bionic_open},
    {"openat", (DarwinArtBionicFsFunction)darwin_art_bionic_openat},
    {"opendir", (DarwinArtBionicFsFunction)darwin_art_bionic_opendir},
    {"pathconf", (DarwinArtBionicFsFunction)darwin_art_bionic_pathconf},
    {"read", (DarwinArtBionicFsFunction)darwin_art_bionic_read},
    {"readdir", (DarwinArtBionicFsFunction)darwin_art_bionic_readdir},
    {"readlink", (DarwinArtBionicFsFunction)darwin_art_bionic_readlink},
    {"realpath", (DarwinArtBionicFsFunction)darwin_art_bionic_realpath},
    {"remove", (DarwinArtBionicFsFunction)darwin_art_bionic_remove},
    {"rename", (DarwinArtBionicFsFunction)darwin_art_bionic_rename},
    {"stat", (DarwinArtBionicFsFunction)darwin_art_bionic_stat},
    {"statvfs", (DarwinArtBionicFsFunction)darwin_art_bionic_statvfs},
    {"symlink", (DarwinArtBionicFsFunction)darwin_art_bionic_symlink},
    {"truncate", (DarwinArtBionicFsFunction)darwin_art_bionic_truncate},
    {"unlinkat", (DarwinArtBionicFsFunction)darwin_art_bionic_unlinkat},
    {"utimensat", (DarwinArtBionicFsFunction)darwin_art_bionic_utimensat},
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
