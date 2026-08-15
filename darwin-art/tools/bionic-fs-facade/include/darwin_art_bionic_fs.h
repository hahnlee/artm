#ifndef DARWIN_ART_BIONIC_FS_H_
#define DARWIN_ART_BIONIC_FS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  DARWIN_ART_ANDROID_AT_FDCWD = -100,
  DARWIN_ART_ANDROID_O_RDONLY = 0,
  DARWIN_ART_ANDROID_O_WRONLY = 1,
  DARWIN_ART_ANDROID_O_RDWR = 2,
  DARWIN_ART_ANDROID_O_NONBLOCK = 2048,
  DARWIN_ART_ANDROID_O_DIRECTORY = 16384,
  DARWIN_ART_ANDROID_O_NOFOLLOW = 32768,
  DARWIN_ART_ANDROID_O_LARGEFILE = 131072,
  DARWIN_ART_ANDROID_O_CLOEXEC = 524288,
  DARWIN_ART_ANDROID_AT_SYMLINK_NOFOLLOW = 0x100,
  DARWIN_ART_ANDROID_AT_REMOVEDIR = 0x200,
};

typedef struct DarwinArtAndroidTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
} DarwinArtAndroidTimespec;

typedef struct DarwinArtAndroidStat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  DarwinArtAndroidTimespec st_atim;
  DarwinArtAndroidTimespec st_mtim;
  DarwinArtAndroidTimespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
} DarwinArtAndroidStat;

typedef struct DarwinArtAndroidDirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} DarwinArtAndroidDirent;

typedef struct DarwinArtAndroidStatvfs {
  uint64_t f_bsize;
  uint64_t f_frsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_favail;
  uint64_t f_fsid;
  uint64_t f_flag;
  uint64_t f_namemax;
  uint32_t __f_reserved[6];
} DarwinArtAndroidStatvfs;

typedef struct DarwinArtHostDirent {
  uint64_t d_ino;
  uint16_t d_name_length;
  uint8_t d_type;
  uint8_t d_name[256];
} DarwinArtHostDirent;

typedef struct DarwinArtHostStatvfs {
  uint64_t f_bsize;
  uint64_t f_frsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_favail;
  uint64_t f_fsid;
  uint64_t f_flag;
  uint64_t f_namemax;
} DarwinArtHostStatvfs;

typedef void (*DarwinArtBionicFsFunction)(void);

int darwin_art_bionic_open(const char* path, int flags, ...);
int darwin_art_bionic_openat(int directory_fd, const char* path, int flags, ...);
intptr_t darwin_art_bionic_read(int fd, void* buffer, size_t count);
int darwin_art_bionic_close(int fd);
int darwin_art_bionic_fstat(int fd, DarwinArtAndroidStat* status);
int darwin_art_bionic_stat(const char* path, DarwinArtAndroidStat* status);
int darwin_art_bionic_lstat(const char* path, DarwinArtAndroidStat* status);
intptr_t darwin_art_bionic_readlink(const char* path, char* buffer,
                                    size_t size);
char* darwin_art_bionic_getcwd(char* buffer, size_t size);
int darwin_art_bionic_chdir(const char* path);
void* darwin_art_bionic_opendir(const char* path);
DarwinArtAndroidDirent* darwin_art_bionic_readdir(void* directory);
int darwin_art_bionic_closedir(void* directory);
int darwin_art_bionic_fchmod(int fd, uint32_t mode);
int darwin_art_bionic_fchmodat(int directory_fd, const char* path,
                               uint32_t mode, int flags);
int darwin_art_bionic_ftruncate(int fd, int64_t length);
int darwin_art_bionic_isatty(int fd);
int darwin_art_bionic_link(const char* old_path, const char* new_path);
int darwin_art_bionic_mkdir(const char* path, uint32_t mode);
int64_t darwin_art_bionic_pathconf(const char* path, int name);
char* darwin_art_bionic_realpath(const char* path, char* resolved);
int darwin_art_bionic_remove(const char* path);
int darwin_art_bionic_rename(const char* old_path, const char* new_path);
int darwin_art_bionic_statvfs(const char* path,
                              DarwinArtAndroidStatvfs* status);
int darwin_art_bionic_symlink(const char* target, const char* link_path);
int darwin_art_bionic_truncate(const char* path, int64_t length);
int darwin_art_bionic_unlinkat(int directory_fd, const char* path, int flags);
int darwin_art_bionic_utimensat(int directory_fd, const char* path,
                                const DarwinArtAndroidTimespec times[2],
                                int flags);

DarwinArtBionicFsFunction darwin_art_bionic_fs_resolve(const char* import_name);

/* Rust implementation boundary called only by the errno-preserving shims. */
int darwin_art_bionic_fs_open_core(const char* path, int flags);
int darwin_art_bionic_fs_openat_core(int directory_fd, const char* path,
                                     int flags);
intptr_t darwin_art_bionic_fs_read_core(int fd, void* buffer, size_t count);
int darwin_art_bionic_fs_close_core(int fd);
int darwin_art_bionic_fs_fstat_core(int fd, DarwinArtAndroidStat* status);
int darwin_art_bionic_fs_stat_core(const char* path,
                                   DarwinArtAndroidStat* status);
int darwin_art_bionic_fs_lstat_core(const char* path,
                                    DarwinArtAndroidStat* status);
intptr_t darwin_art_bionic_fs_readlink_core(const char* path, char* buffer,
                                            size_t size);
char* darwin_art_bionic_fs_getcwd_core(char* buffer, size_t size);
int darwin_art_bionic_fs_chdir_core(const char* path);
void* darwin_art_bionic_fs_opendir_core(const char* path);
DarwinArtAndroidDirent* darwin_art_bionic_fs_readdir_core(void* directory);
int darwin_art_bionic_fs_closedir_core(void* directory);
int darwin_art_bionic_fs_fchmod_core(int fd, uint32_t mode);
int darwin_art_bionic_fs_fchmodat_core(int directory_fd, const char* path,
                                      uint32_t mode, int flags);
int darwin_art_bionic_fs_ftruncate_core(int fd, int64_t length);
int darwin_art_bionic_fs_isatty_core(int fd);
int darwin_art_bionic_fs_link_core(const char* old_path,
                                   const char* new_path);
int darwin_art_bionic_fs_mkdir_core(const char* path, uint32_t mode);
int64_t darwin_art_bionic_fs_pathconf_core(const char* path, int name);
char* darwin_art_bionic_fs_realpath_core(const char* path, char* resolved);
int darwin_art_bionic_fs_remove_core(const char* path);
int darwin_art_bionic_fs_rename_core(const char* old_path,
                                     const char* new_path);
int darwin_art_bionic_fs_statvfs_core(const char* path,
                                      DarwinArtAndroidStatvfs* status);
int darwin_art_bionic_fs_symlink_core(const char* target,
                                      const char* link_path);
int darwin_art_bionic_fs_truncate_core(const char* path, int64_t length);
int darwin_art_bionic_fs_unlinkat_core(int directory_fd, const char* path,
                                      int flags);
int darwin_art_bionic_fs_utimensat_core(
    int directory_fd, const char* path,
    const DarwinArtAndroidTimespec times[2], int flags);

/* Darwin-only helpers. Their opaque stream never crosses the guest facade. */
__attribute__((visibility("hidden"))) void*
darwin_art_bionic_fs_host_fdopendir(int fd, int* host_errno);
__attribute__((visibility("hidden"))) int darwin_art_bionic_fs_host_readdir(
    void* directory, DarwinArtHostDirent* entry, int* host_errno);
__attribute__((visibility("hidden"))) int darwin_art_bionic_fs_host_closedir(
    void* directory, int* host_errno);
__attribute__((visibility("hidden"))) int darwin_art_bionic_fs_host_fpathconf(
    int fd, int semantic_name, int64_t* value, int* host_errno);
__attribute__((visibility("hidden"))) int darwin_art_bionic_fs_host_fstatvfs(
    int fd, DarwinArtHostStatvfs* status, int* host_errno);

#ifdef __cplusplus
}
#endif

#endif
