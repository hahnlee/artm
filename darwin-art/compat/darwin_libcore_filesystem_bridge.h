#ifndef DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_
#define DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_

#include <sys/stat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtAndroidStat DarwinArtAndroidStat;

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
int darwin_art_libcore_mkdir(const char* path, mode_t mode);
int darwin_art_libcore_chmod(const char* path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_LIBCORE_FILESYSTEM_BRIDGE_H_
