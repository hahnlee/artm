#include "../compat/darwin_libcore_filesystem_bridge.h"
#include "bionic-fs-facade/include/darwin_art_bionic_fs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int32_t guest_errno;
static int32_t load_errno(void) { return guest_errno; }

// The production directory bridge imports the facade weakly so the libcore
// archive remains usable by host-only tools. Keep this statfs-only smoke
// self-contained by supplying the unavailable managed directory ABI.
void* darwin_art_bionic_opendir(const char* path) {
  (void)path;
  return NULL;
}
DarwinArtAndroidDirent* darwin_art_bionic_readdir(void* directory) {
  (void)directory;
  return NULL;
}
int darwin_art_bionic_closedir(void* directory) {
  (void)directory;
  return 0;
}
int32_t darwin_art_bionic_errno_load(void) { return guest_errno; }
void darwin_art_bionic_errno_store(int32_t value) { guest_errno = value; }

static int unused_stat(const char* path, DarwinArtAndroidStat* status) {
  (void)path; (void)status; return -1;
}
int darwin_art_bionic_fs_statvfs_core(const char* path, DarwinArtAndroidStatvfs* out) {
  if (strcmp(path, "/storage/emulated/0/Android/data/test/files") != 0) {
    guest_errno = ENOENT;
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->f_bsize = 16384;
  out->f_frsize = 4096;
  out->f_blocks = 1000;
  out->f_bfree = 400;
  out->f_bavail = 300;
  return 0;
}
int main(void) {
  struct statfs status;
  assert(darwin_art_libcore_statfs("/tmp", &status) == 0);
  assert(status.f_blocks > 0);
  darwin_art_libcore_install_filesystem_provider(unused_stat, NULL, NULL, load_errno);
  assert(darwin_art_libcore_statfs(
      "/storage/emulated/0/Android/data/test/files", &status) == 0);
  assert(status.f_bsize == 4096);
  assert(status.f_blocks == 1000 && status.f_bfree == 400 && status.f_bavail == 300);
  assert(darwin_art_libcore_statfs("/missing-guest", &status) == -1);
  assert(errno == ENOENT);
  puts("libcore-space: host/guest units and missing-path error PASS");
}
