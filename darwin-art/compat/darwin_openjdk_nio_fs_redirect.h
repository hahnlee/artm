#ifndef DARWIN_ART_OPENJDK_NIO_FS_REDIRECT_H_
#define DARWIN_ART_OPENJDK_NIO_FS_REDIRECT_H_

// FileDescriptor.fd contains a Darwin ART virtual descriptor, not a Darwin
// kernel descriptor. OpenJDK's upstream FileDispatcherImpl is compiled as
// host C, so route its descriptor operations back through the same Bionic
// filesystem owner used by java.io and Android native code.

#include "darwin_art_bionic_errno.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_socket_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static inline void darwin_art_openjdk_nio_publish_errno(intptr_t result) {
  if (result < 0) errno = darwin_art_bionic_errno_load();
}

static inline int darwin_art_openjdk_nio_is_virtual_fd(int fd) {
  return fd >= 0 &&
         ((((uint32_t)fd & UINT32_C(0x40000000)) != 0) ||
          darwin_art_bionic_fs_owns_fd_core(fd) != 0);
}

static inline int darwin_art_openjdk_nio_is_central_fd(int fd) {
  return fd >= 0 &&
         (((uint32_t)fd & UINT32_C(0x40000000)) != 0);
}

static inline ssize_t darwin_art_openjdk_nio_read(int fd, void* bytes,
                                                   size_t count) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd)) return read(fd, bytes, count);
  if (darwin_art_openjdk_nio_is_central_fd(fd)) {
    const intptr_t result =
        darwin_art_bionic_socket_broker_read(fd, bytes, count);
    darwin_art_openjdk_nio_publish_errno(result);
    return (ssize_t)result;
  }
  const intptr_t result = darwin_art_bionic_read(fd, bytes, count);
  darwin_art_openjdk_nio_publish_errno(result);
  return (ssize_t)result;
}

static inline ssize_t darwin_art_openjdk_nio_write(int fd, const void* bytes,
                                                    size_t count) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return write(fd, bytes, count);
  if (darwin_art_openjdk_nio_is_central_fd(fd)) {
    const intptr_t result =
        darwin_art_bionic_socket_broker_write(fd, bytes, count);
    darwin_art_openjdk_nio_publish_errno(result);
    return (ssize_t)result;
  }
  const intptr_t result = darwin_art_bionic_write(fd, bytes, count);
  darwin_art_openjdk_nio_publish_errno(result);
  return (ssize_t)result;
}

static inline ssize_t darwin_art_openjdk_nio_pread(int fd, void* bytes,
                                                    size_t count,
                                                    off_t offset) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return pread(fd, bytes, count, offset);
  const intptr_t result = darwin_art_bionic_pread(fd, bytes, count, offset);
  darwin_art_openjdk_nio_publish_errno(result);
  return (ssize_t)result;
}

static inline ssize_t darwin_art_openjdk_nio_pwrite(int fd,
                                                     const void* bytes,
                                                     size_t count,
                                                     off_t offset) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return pwrite(fd, bytes, count, offset);
  const intptr_t result = darwin_art_bionic_pwrite(fd, bytes, count, offset);
  darwin_art_openjdk_nio_publish_errno(result);
  return (ssize_t)result;
}

static inline ssize_t darwin_art_openjdk_nio_readv(int fd,
                                                    const struct iovec* iov,
                                                    int count) {
  ssize_t total = 0;
  for (int index = 0; index < count; ++index) {
    const ssize_t result = darwin_art_openjdk_nio_read(
        fd, iov[index].iov_base, iov[index].iov_len);
    if (result < 0) return total == 0 ? -1 : total;
    total += result;
    if ((size_t)result != iov[index].iov_len) break;
  }
  return total;
}

static inline ssize_t darwin_art_openjdk_nio_writev(
    int fd, const struct iovec* iov, int count) {
  ssize_t total = 0;
  for (int index = 0; index < count; ++index) {
    const ssize_t result = darwin_art_openjdk_nio_write(
        fd, iov[index].iov_base, iov[index].iov_len);
    if (result < 0) return total == 0 ? -1 : total;
    total += result;
    if ((size_t)result != iov[index].iov_len) break;
  }
  return total;
}

static inline int darwin_art_openjdk_nio_close(int fd) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd)) return close(fd);
  if (darwin_art_openjdk_nio_is_central_fd(fd)) {
    const int result = darwin_art_bionic_socket_broker_close(fd);
    darwin_art_openjdk_nio_publish_errno(result);
    return result;
  }
  const int result = darwin_art_bionic_close(fd);
  darwin_art_openjdk_nio_publish_errno(result);
  return result;
}

static inline int darwin_art_openjdk_nio_fsync(int fd) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd)) return fsync(fd);
  const int result = darwin_art_bionic_fsync(fd);
  darwin_art_openjdk_nio_publish_errno(result);
  return result;
}

static inline int darwin_art_openjdk_nio_fdatasync(int fd) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd)) return fsync(fd);
  const int result = darwin_art_bionic_fdatasync(fd);
  darwin_art_openjdk_nio_publish_errno(result);
  return result;
}

static inline int darwin_art_openjdk_nio_ftruncate(int fd, off_t length) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return ftruncate(fd, length);
  const int result = darwin_art_bionic_ftruncate(fd, length);
  darwin_art_openjdk_nio_publish_errno(result);
  return result;
}

static inline int darwin_art_openjdk_nio_fstat(int fd, struct stat* status) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd)) return fstat(fd, status);
  DarwinArtAndroidStat android_status;
  const int result = darwin_art_bionic_fstat(fd, &android_status);
  darwin_art_openjdk_nio_publish_errno(result);
  if (result != 0) return result;
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
  return 0;
}

static inline off_t darwin_art_openjdk_nio_lseek(int fd, off_t offset,
                                                  int whence) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return lseek(fd, offset, whence);
  if (!darwin_art_openjdk_nio_is_central_fd(fd)) {
    const int64_t result = darwin_art_bionic_lseek(fd, offset, whence);
    darwin_art_openjdk_nio_publish_errno(result);
    return (off_t)result;
  }

  // Central-broker descriptors do not belong to the filesystem facade. A
  // duplicate shares the underlying open-file description (and therefore its
  // position), while keeping the host descriptor number out of guest state.
  const int host_fd = darwin_art_bionic_fd_export_for_scm(fd);
  if (host_fd < 0) {
    darwin_art_openjdk_nio_publish_errno(-1);
    return (off_t)-1;
  }
  const off_t result = lseek(host_fd, offset, whence);
  const int saved_errno = errno;
  (void)close(host_fd);
  errno = saved_errno;
  return result;
}

static inline void* darwin_art_openjdk_nio_mmap(void* address, size_t length,
                                                 int protection, int flags,
                                                 int fd, off_t offset) {
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return mmap(address, length, protection, flags, fd, offset);

  // mmap retains the vnode after this duplicate is closed, so this is a
  // zero-copy bridge from the Android virtual descriptor to Darwin VM.
  const int host_fd = darwin_art_bionic_fd_export_for_scm(fd);
  if (host_fd < 0) {
    darwin_art_openjdk_nio_publish_errno(-1);
    return MAP_FAILED;
  }
  void* const result =
      mmap(address, length, protection, flags, host_fd, offset);
  const int saved_errno = errno;
  (void)close(host_fd);
  errno = saved_errno;
  return result;
}

static inline int darwin_art_openjdk_nio_open(const char* path, int flags,
                                               ...) {
  uint32_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list arguments;
    va_start(arguments, flags);
    mode = (uint32_t)va_arg(arguments, int);
    va_end(arguments);
  }
  return open(path, flags, mode);
}

static inline int darwin_art_openjdk_nio_dup2(int old_fd, int new_fd) {
  // preClose0 uses dup2(/dev/null, fd) solely to wake blocking file I/O.
  // Darwin ART interrupts blocking channels through NativeThread signals;
  // preserving the virtual descriptor here lets close0 perform the one
  // authoritative close without exposing a virtual fd to the host kernel.
  if (!darwin_art_openjdk_nio_is_virtual_fd(old_fd) &&
      !darwin_art_openjdk_nio_is_virtual_fd(new_fd))
    return dup2(old_fd, new_fd);
  return new_fd;
}

static inline int darwin_art_openjdk_nio_fcntl(int fd, int command, ...) {
  struct DarwinArtOpenJdkAndroidFlock {
    int16_t type;
    int16_t whence;
    int32_t padding0;
    int64_t start;
    int64_t length;
    int32_t pid;
    int32_t padding1;
  } android_lock = {0};
  va_list arguments;
  va_start(arguments, command);
  struct flock* host_lock = va_arg(arguments, struct flock*);
  va_end(arguments);
  if (host_lock == NULL) {
    errno = EFAULT;
    return -1;
  }
  if (!darwin_art_openjdk_nio_is_virtual_fd(fd))
    return fcntl(fd, command, host_lock);
  android_lock.type = host_lock->l_type == F_RDLCK
                          ? 0
                          : host_lock->l_type == F_WRLCK ? 1 : 2;
  android_lock.whence = host_lock->l_whence;
  android_lock.start = host_lock->l_start;
  android_lock.length = host_lock->l_len;
  const int android_command = command == F_SETLK ? 6 : 7;
  const int result = darwin_art_bionic_fs_fcntl_core(
      fd, android_command, (intptr_t)&android_lock);
  darwin_art_openjdk_nio_publish_errno(result);
  return result;
}

#define read darwin_art_openjdk_nio_read
#define write darwin_art_openjdk_nio_write
#define pread darwin_art_openjdk_nio_pread
#define pwrite darwin_art_openjdk_nio_pwrite
#define readv darwin_art_openjdk_nio_readv
#define writev darwin_art_openjdk_nio_writev
#define close darwin_art_openjdk_nio_close
#define fsync darwin_art_openjdk_nio_fsync
#define ftruncate darwin_art_openjdk_nio_ftruncate
#define fstat darwin_art_openjdk_nio_fstat
#define lseek darwin_art_openjdk_nio_lseek
#define mmap darwin_art_openjdk_nio_mmap
#define open darwin_art_openjdk_nio_open
#define dup2 darwin_art_openjdk_nio_dup2
#define fcntl darwin_art_openjdk_nio_fcntl

#endif  // DARWIN_ART_OPENJDK_NIO_FS_REDIRECT_H_
