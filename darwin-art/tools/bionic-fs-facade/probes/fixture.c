#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
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
  return 42;
}
