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
  return 42;
}
