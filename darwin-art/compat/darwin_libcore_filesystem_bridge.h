#ifndef DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_
#define DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_

#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include "../tools/bionic-fs-facade/include/darwin_art_bionic_stat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*DarwinArtLibcoreStatProvider)(const char*,
                                            DarwinArtAndroidStat*);
typedef int (*DarwinArtLibcoreModeProvider)(const char*, uint32_t);
typedef int32_t (*DarwinArtLibcoreErrnoProvider)(void);

void darwin_art_libcore_install_filesystem_provider(
    DarwinArtLibcoreStatProvider stat_provider,
    DarwinArtLibcoreModeProvider mkdir_provider,
    DarwinArtLibcoreModeProvider chmod_provider,
    DarwinArtLibcoreErrnoProvider errno_provider);

int darwin_art_libcore_stat(const char* path, struct stat* status);
int darwin_art_libcore_fstat(int fd, struct stat* status);
int darwin_art_libcore_open(const char* path, int flags, ...);
int darwin_art_libcore_close(int fd);
int darwin_art_libcore_mkdir(const char* path, mode_t mode);
int darwin_art_libcore_chmod(const char* path, mode_t mode);
int darwin_art_libcore_statfs(const char* path, struct statfs* status);
DIR* darwin_art_libcore_opendir(const char* path);
struct dirent* darwin_art_libcore_readdir(DIR* directory);
int darwin_art_libcore_closedir(DIR* directory);
char* darwin_art_libcore_realpath(const char* path, char* resolved);

#ifdef __cplusplus
}
#endif

// Install redirects only after the Darwin declarations have been parsed.
// Command-line -Dstatfs rewrites the declaration but retains its asm("statfs")
// symbol alias, silently binding the supposed bridge back to the host syscall.
#ifdef DARWIN_ART_LIBCORE_REDIRECT_FILESYSTEM
#define stat(path, status) darwin_art_libcore_stat(path, status)
#define fstat(fd, status) darwin_art_libcore_fstat(fd, status)
#define open(path, flags, ...) \
  darwin_art_libcore_open(path, flags, ##__VA_ARGS__)
#define close(fd) darwin_art_libcore_close(fd)
#define statfs(path, status) darwin_art_libcore_statfs(path, status)
#define mkdir(path, mode) darwin_art_libcore_mkdir(path, mode)
#define chmod(path, mode) darwin_art_libcore_chmod(path, mode)
#define opendir(path) darwin_art_libcore_opendir(path)
// UnixFileSystem_md.c aliases readdir64 to readdir on Darwin. Redirect the
// underlying spelling so that alias remains source-compatible without a
// conflicting readdir64 macro definition.
#define readdir(directory) darwin_art_libcore_readdir(directory)
#define closedir(directory) darwin_art_libcore_closedir(directory)
#define realpath(path, resolved) darwin_art_libcore_realpath(path, resolved)
#endif

#endif  // DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_
