#include "darwin_libcore_filesystem_bridge.h"

#include "darwin_art_bionic_errno.h"
#include "darwin_art_bionic_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static DarwinArtLibcoreStatProvider g_stat_provider;
static DarwinArtLibcoreModeProvider g_mkdir_provider;
static DarwinArtLibcoreModeProvider g_chmod_provider;
static DarwinArtLibcoreErrnoProvider g_errno_provider;
static unsigned g_debug_stat_count;

void darwin_art_libcore_install_filesystem_provider(
    DarwinArtLibcoreStatProvider stat_provider,
    DarwinArtLibcoreModeProvider mkdir_provider,
    DarwinArtLibcoreModeProvider chmod_provider,
    DarwinArtLibcoreErrnoProvider errno_provider) {
  g_stat_provider = stat_provider;
  g_mkdir_provider = mkdir_provider;
  g_chmod_provider = chmod_provider;
  g_errno_provider = errno_provider;
  if (getenv("DARWIN_ART_DEBUG_FS_BRIDGE") != NULL) {
    fprintf(stderr, "DARWIN libcore FS: provider installed stat=%p\n",
            (void*)stat_provider);
  }
}

static void publish_android_errno(void) {
  if (g_errno_provider != NULL) errno = g_errno_provider();
}

int darwin_art_libcore_stat(const char* path, struct stat* status) {
  if (g_stat_provider == NULL) return stat(path, status);
  DarwinArtAndroidStat android_status;
  const int provider_result = g_stat_provider(path, &android_status);
  if (getenv("DARWIN_ART_DEBUG_FS_BRIDGE") != NULL &&
      (g_debug_stat_count++ < 64 ||
       (path != NULL && strstr(path, "local") != NULL))) {
    fprintf(stderr, "DARWIN libcore FS: stat path=%s result=%d errno=%d\n",
            path, provider_result,
            g_errno_provider == NULL ? -1 : g_errno_provider());
  }
  if (provider_result != 0) {
    publish_android_errno();
    return -1;
  }
  memset(status, 0, sizeof(*status));
  status->st_dev = (dev_t)android_status.st_dev;
  status->st_ino = (ino_t)android_status.st_ino;
  status->st_mode = (mode_t)android_status.st_mode;
  status->st_nlink = (nlink_t)android_status.st_nlink;
  status->st_uid = (uid_t)android_status.st_uid;
  status->st_gid = (gid_t)android_status.st_gid;
  status->st_rdev = (dev_t)android_status.st_rdev;
  status->st_size = (off_t)android_status.st_size;
  status->st_blksize = (blksize_t)android_status.st_blksize;
  status->st_blocks = (blkcnt_t)android_status.st_blocks;
  status->st_atimespec.tv_sec = (time_t)android_status.st_atim.tv_sec;
  status->st_atimespec.tv_nsec = android_status.st_atim.tv_nsec;
  status->st_mtimespec.tv_sec = (time_t)android_status.st_mtim.tv_sec;
  status->st_mtimespec.tv_nsec = android_status.st_mtim.tv_nsec;
  status->st_ctimespec.tv_sec = (time_t)android_status.st_ctim.tv_sec;
  status->st_ctimespec.tv_nsec = android_status.st_ctim.tv_nsec;
  return 0;
}

int darwin_art_libcore_mkdir(const char* path, mode_t mode) {
  if (g_mkdir_provider == NULL) return mkdir(path, mode);
  const int result = g_mkdir_provider(path, (uint32_t)mode);
  if (result != 0) publish_android_errno();
  return result;
}

int darwin_art_libcore_chmod(const char* path, mode_t mode) {
  if (g_chmod_provider == NULL) return chmod(path, mode);
  const int result = g_chmod_provider(path, (uint32_t)mode);
  if (result != 0) publish_android_errno();
  return result;
}
