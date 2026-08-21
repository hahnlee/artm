#include "libcore_darwin_linux.h"

#include "darwin_os_constants.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace darwin_art::libcore_darwin {
namespace {

constexpr int kLinuxMapShared = 0x01;
constexpr int kLinuxMapPrivate = 0x02;
constexpr int kLinuxMapFixed = 0x10;
constexpr int kLinuxMapAnonymous = 0x20;
constexpr int kAndroidSeekSet = 0;
constexpr int kAndroidSeekCur = 1;
constexpr int kAndroidSeekEnd = 2;

bool DarwinWhenceFromAndroid(int android_whence, int* darwin_whence) {
  if (darwin_whence == nullptr) {
    return false;
  }
  switch (android_whence) {
    case kAndroidSeekSet:
      *darwin_whence = SEEK_SET;
      return true;
    case kAndroidSeekCur:
      *darwin_whence = SEEK_CUR;
      return true;
    case kAndroidSeekEnd:
      *darwin_whence = SEEK_END;
      return true;
    default:
      return false;
  }
}

int TranslateMmapFlags(int linux_flags) {
  int remaining = linux_flags;
  int darwin_flags = 0;
  if ((remaining & kLinuxMapShared) != 0) {
    darwin_flags |= MAP_SHARED;
    remaining &= ~kLinuxMapShared;
  }
  if ((remaining & kLinuxMapPrivate) != 0) {
    darwin_flags |= MAP_PRIVATE;
    remaining &= ~kLinuxMapPrivate;
  }
  if ((remaining & kLinuxMapFixed) != 0) {
    darwin_flags |= MAP_FIXED;
    remaining &= ~kLinuxMapFixed;
  }
  if ((remaining & kLinuxMapAnonymous) != 0) {
    darwin_flags |= MAP_ANON;
    remaining &= ~kLinuxMapAnonymous;
  }
  if (remaining != 0 ||
      ((darwin_flags & MAP_SHARED) != 0 &&
       (darwin_flags & MAP_PRIVATE) != 0)) {
    errno = ENOTSUP;
    return -1;
  }
  return darwin_flags;
}

}  // namespace

int Open(const char* path, int linux_flags, mode_t mode) {
  int flags = 0;
  if (!os_constants::DarwinOpenFlagsFromAndroid(linux_flags, &flags)) {
    errno = ENOTSUP;
    return -1;
  }
  // This is a probe-only sealed-root authority. Production callers never set
  // it and therefore cannot inherit a host fallback for arbitrary guest paths.
  const char* test_root = std::getenv("DARWIN_ART_ANDROID_SYSTEM_ROOT");
  if (test_root != nullptr && path != nullptr &&
      std::strncmp(path, "/system/", 8) == 0) {
    const char* relative = path + 8;
    if (*relative == '\0' || std::strstr(relative, "/../") != nullptr ||
        std::strncmp(relative, "../", 3) == 0 ||
        (std::strlen(relative) >= 3 &&
         std::strcmp(relative + std::strlen(relative) - 3, "/..") == 0) ||
        std::strcmp(relative, ".") == 0 || std::strstr(relative, "/./") != nullptr ||
        std::strncmp(relative, "./", 2) == 0 ||
        (std::strlen(relative) >= 2 &&
         std::strcmp(relative + std::strlen(relative) - 2, "/.") == 0)) {
      errno = EINVAL;
      return -1;
    }
    std::string redirected(test_root);
    redirected.push_back('/');
    redirected.append(relative);
    int result;
    do {
      result = open(redirected.c_str(), flags, mode);
    } while (result == -1 && errno == EINTR);
    return result;
  }
  int result;
  do {
    result = open(path, flags, mode);
  } while (result == -1 && errno == EINTR);
  return result;
}

int Close(int fd) { return close(fd); }

ssize_t Read(int fd, void* bytes, size_t byte_count) {
  ssize_t result;
  do {
    result = read(fd, bytes, byte_count);
  } while (result == -1 && errno == EINTR);
  return result;
}

ssize_t Write(int fd, const void* bytes, size_t byte_count) {
  ssize_t result;
  do {
    result = write(fd, bytes, byte_count);
  } while (result == -1 && errno == EINTR);
  return result;
}

int Fstat(int fd, struct stat* status) {
  int result;
  do {
    result = fstat(fd, status);
  } while (result == -1 && errno == EINTR);
  return result;
}

int Stat(const char* path, struct stat* status) {
  int result;
  do {
    result = stat(path, status);
  } while (result == -1 && errno == EINTR);
  return result;
}

int64_t Lseek(int fd, int64_t offset, int android_whence) {
  int darwin_whence = 0;
  if (!DarwinWhenceFromAndroid(android_whence, &darwin_whence)) {
    errno = EINVAL;
    return -1;
  }
  const off_t darwin_offset = static_cast<off_t>(offset);
  if (static_cast<int64_t>(darwin_offset) != offset) {
    errno = EOVERFLOW;
    return -1;
  }
  off_t result;
  do {
    result = lseek(fd, darwin_offset, darwin_whence);
  } while (result == static_cast<off_t>(-1) && errno == EINTR);
  if (result != static_cast<off_t>(-1) &&
      static_cast<off_t>(static_cast<int64_t>(result)) != result) {
    errno = EOVERFLOW;
    return -1;
  }
  return static_cast<int64_t>(result);
}

void* Mmap(void* address, size_t byte_count, int linux_prot,
           int linux_flags, int fd, off_t offset) {
  const int flags = TranslateMmapFlags(linux_flags);
  if (flags == -1) {
    return MAP_FAILED;
  }
  return mmap(address, byte_count, linux_prot, flags, fd, offset);
}

int Munmap(void* address, size_t byte_count) {
  return munmap(address, byte_count);
}

long Sysconf(int name) {
  int darwin_name = 0;
  if (!os_constants::DarwinSysconfNameFromAndroid(name, &darwin_name)) {
    errno = EINVAL;
    return -1;
  }
  return sysconf(darwin_name);
}

}  // namespace darwin_art::libcore_darwin
