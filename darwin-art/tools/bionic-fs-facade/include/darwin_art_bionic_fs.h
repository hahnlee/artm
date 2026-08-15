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

typedef void (*DarwinArtBionicFsFunction)(void);

int darwin_art_bionic_open(const char* path, int flags, ...);
int darwin_art_bionic_openat(int directory_fd, const char* path, int flags, ...);
intptr_t darwin_art_bionic_read(int fd, void* buffer, size_t count);
int darwin_art_bionic_close(int fd);
int darwin_art_bionic_fstat(int fd, DarwinArtAndroidStat* status);

DarwinArtBionicFsFunction darwin_art_bionic_fs_resolve(const char* import_name);

/* Rust implementation boundary called only by the errno-preserving shims. */
int darwin_art_bionic_fs_open_core(const char* path, int flags);
int darwin_art_bionic_fs_openat_core(int directory_fd, const char* path,
                                     int flags);
intptr_t darwin_art_bionic_fs_read_core(int fd, void* buffer, size_t count);
int darwin_art_bionic_fs_close_core(int fd);
int darwin_art_bionic_fs_fstat_core(int fd, DarwinArtAndroidStat* status);

#ifdef __cplusplus
}
#endif

#endif
