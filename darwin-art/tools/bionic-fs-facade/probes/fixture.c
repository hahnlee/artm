#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int BytesEqual(const char* left, const char* right, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (left[index] != right[index]) return 0;
  }
  return 1;
}

static int NameEqual(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return *left == *right;
}

__attribute__((visibility("default"))) int
bionic_fs_fixture_pc_2_symlinks(void) {
  long value = pathconf("/system/etc/payload.txt", _PC_2_SYMLINKS);
  return value >= -1 && value <= INT32_MAX ? (int)value : INT32_MIN;
}

__attribute__((visibility("default"))) int bionic_fs_fixture_statvfs_bsize(void) {
  struct statvfs status;
  return statvfs("/system/etc/payload.txt", &status) == 0 &&
                 status.f_bsize <= INT32_MAX
             ? (int)status.f_bsize
             : -1;
}

__attribute__((visibility("default"))) int bionic_fs_fixture_statvfs_flags(void) {
  struct statvfs status;
  return statvfs("/system/etc/payload.txt", &status) == 0 &&
                 status.f_flag <= INT32_MAX
             ? (int)status.f_flag
             : -1;
}

__attribute__((visibility("default"))) int bionic_fs_fixture_run(void) {
  static const char expected[] = "brokered-data";
  char buffer[sizeof(expected)] = {0};
  int fd = open("/system/etc/payload.txt", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 1;
  if (fd < 10000) return 14;
  struct stat status;
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size != (off_t)(sizeof(expected) - 1)) return 2;
  ssize_t count = read(fd, buffer, sizeof(expected) - 1);
  if (count != (ssize_t)(sizeof(expected) - 1) ||
      !BytesEqual(buffer, expected, sizeof(expected) - 1)) return 3;
  if (close(fd) != 0) return 4;

  fd = openat(AT_FDCWD, "etc/payload.txt", O_RDONLY);
  if (fd < 0 || close(fd) != 0) return 5;

  errno = 0;
  if (open("/system/etc/missing", O_RDONLY) != -1 || errno != ENOENT) return 6;
  errno = 0;
  if (open("/system/etc/payload.txt", O_RDONLY | O_DIRECT) != -1 ||
      errno != EOPNOTSUPP) return 7;
  errno = 0;
  if (open("/system/etc/payload.txt", O_WRONLY) != -1 || errno != EROFS) return 8;
  errno = 0;
  if (open("/system/../secret", O_RDONLY) != -1 || errno != EACCES) return 9;
  errno = 0;
  if (open("/system/etc/outside-link", O_RDONLY) != -1 || errno != ELOOP) return 10;

  int directory = open("/system/etc", O_RDONLY | O_DIRECTORY);
  if (directory < 0) return 11;
  errno = 0;
  if (openat(directory, "payload.txt", O_RDONLY) != -1 ||
      errno != EOPNOTSUPP) return 12;
  if (close(directory) != 0) return 13;

  if (stat("/system/etc/payload.txt", &status) != 0 ||
      !S_ISREG(status.st_mode) ||
      status.st_size != (off_t)(sizeof(expected) - 1)) return 15;
  if (lstat("/system/etc/payload.txt", &status) != 0 ||
      !S_ISREG(status.st_mode)) return 16;
  errno = 0;
  if (stat("/system/etc/outside-link", &status) != -1 || errno != ELOOP)
    return 17;
  errno = 0;
  if (lstat("/system/etc/outside-link", &status) != -1 ||
      errno != EOPNOTSUPP) return 18;
  errno = 0;
  if (readlink("/system/etc/outside-link", buffer, sizeof(buffer)) != -1 ||
      errno != EOPNOTSUPP) return 19;

  char cwd[64];
  if (getcwd(cwd, sizeof(cwd)) != cwd || !NameEqual(cwd, "/system")) return 20;
  errno = 0;
  if (getcwd(cwd, 4) != NULL || errno != ERANGE) return 21;
  errno = 0;
  if (getcwd(NULL, 0) != NULL || errno != EOPNOTSUPP) return 22;
  if (chdir("etc") != 0 || getcwd(cwd, sizeof(cwd)) != cwd ||
      !NameEqual(cwd, "/system/etc")) return 23;
  if (stat("payload.txt", &status) != 0 || !S_ISREG(status.st_mode)) return 24;
  errno = 0;
  if (chdir("payload.txt") != -1 || errno != ENOTDIR) return 25;
  if (chdir("..") != 0 || getcwd(cwd, sizeof(cwd)) != cwd ||
      !NameEqual(cwd, "/system")) return 26;

  DIR* stream = opendir("etc");
  if (stream == NULL) return 27;
  int saw_payload = 0;
  int saw_link = 0;
  errno = 777;
  for (;;) {
    struct dirent* entry = readdir(stream);
    if (entry == NULL) break;
    if (entry->d_reclen < 24 || entry->d_reclen > sizeof(struct dirent)) return 28;
    if (NameEqual(entry->d_name, "payload.txt")) {
      if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_REG) return 29;
      saw_payload = 1;
    }
    if (NameEqual(entry->d_name, "outside-link")) {
      if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK) return 30;
      saw_link = 1;
    }
  }
  if (errno != 777 || !saw_payload || !saw_link) return 31;
  if (closedir(stream) != 0) return 32;
  errno = 0;
  if (opendir("etc/payload.txt") != NULL || errno != ENOTDIR) return 33;
  errno = 0;
  if (opendir("etc/outside-link") != NULL || errno != ELOOP) return 34;
  errno = 0;
  if (readlink("etc/payload.txt", buffer, sizeof(buffer)) != -1 ||
      errno != EINVAL) return 35;
  errno = 0;
  if (readlink("etc/outside-dir-link/secret", buffer, sizeof(buffer)) != -1 ||
      errno != ENOTDIR) return 36;

  fd = open("/system/etc/payload.txt", O_RDONLY);
  if (fd < 0) return 37;
  errno = 0;
  if (isatty(fd) != 0 || errno != ENOTTY) return 38;
  errno = 0;
  if (isatty(-7) != 0 || errno != EBADF) return 39;

  errno = 991;
  if (pathconf("/system/etc/payload.txt", _PC_NAME_MAX) <= 0 || errno != 991)
    return 40;
  errno = 0;
  if (pathconf("/system/etc/payload.txt", 999) != -1 || errno != EINVAL)
    return 41;

  struct statvfs filesystem;
  if (statvfs("/system/etc/payload.txt", &filesystem) != 0 ||
      filesystem.f_bsize == 0 || filesystem.f_namemax == 0 ||
      (filesystem.f_flag & ST_RDONLY) == 0)
    return 43;
  for (size_t index = 0; index < 6; ++index) {
    if (filesystem.__f_reserved[index] != 0) return 44;
  }

  char resolved[4096];
  if (realpath("/system/etc/../etc/payload.txt", resolved) != resolved ||
      !NameEqual(resolved, "/system/etc/payload.txt"))
    return 45;
  errno = 0;
  if (realpath("/system/etc/payload.txt", NULL) != NULL ||
      errno != EOPNOTSUPP)
    return 46;

  errno = 0;
  if (fchmod(fd, 0600) != -1 || errno != EROFS) return 47;
  errno = 0;
  if (fchmod(-7, 0600) != -1 || errno != EBADF) return 48;
  errno = 0;
  if (ftruncate(fd, 0) != -1 || errno != EROFS) return 49;
  errno = 0;
  if (fchmodat(AT_FDCWD, "/system/etc/payload.txt", 0600, 0) != -1 ||
      errno != EROFS)
    return 50;
  errno = 0;
  if (fchmodat(AT_FDCWD, "/system/etc/payload.txt", 0600, 0x40000000) !=
          -1 ||
      errno != EINVAL)
    return 51;
  errno = 0;
  if (link("/system/etc/payload.txt", "/system/etc/hard-link") != -1 ||
      errno != EROFS)
    return 52;
  errno = 0;
  if (mkdir("/system/etc/new-directory", 0700) != -1 || errno != EROFS)
    return 53;
  errno = 0;
  if (remove("/system/etc/payload.txt") != -1 || errno != EROFS) return 54;
  errno = 0;
  if (rename("/system/etc/payload.txt", "/system/etc/renamed") != -1 ||
      errno != EROFS)
    return 55;
  errno = 0;
  if (symlink("../../outside", "/system/etc/new-link") != -1 ||
      errno != EROFS)
    return 56;
  errno = 0;
  if (truncate("/system/etc/payload.txt", 0) != -1 || errno != EROFS)
    return 57;
  errno = 0;
  if (unlinkat(AT_FDCWD, "/system/etc/payload.txt", 0) != -1 ||
      errno != EROFS)
    return 58;
  errno = 0;
  if (utimensat(AT_FDCWD, "/system/etc/payload.txt", NULL,
                AT_SYMLINK_NOFOLLOW) != -1 ||
      errno != EROFS)
    return 59;
  errno = 0;
  if (utimensat(fd, NULL, NULL, 0) != -1 || errno != EROFS) return 60;
  errno = 0;
  if (mkdir("/outside/new-directory", 0700) != -1 || errno != EACCES)
    return 61;
  if (close(fd) != 0) return 62;

  fd = open("/system/etc/payload.txt", O_RDONLY);
  if (fd < 0 || fstat(fd, &status) != 0 ||
      status.st_size != (off_t)(sizeof(expected) - 1) || close(fd) != 0)
    return 63;
  return 42;
}
