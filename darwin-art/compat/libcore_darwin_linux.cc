#include "libcore_darwin_linux.h"

#include "AsynchronousCloseMonitor.h"
#include "darwin_os_constants.h"

#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <nativehelper/ScopedUtfChars.h>

namespace darwin_art::libcore_darwin {
namespace {


void ThrowErrno(JNIEnv* env, const char* operation, int error) {
  int android_error = error;
  os_constants::AndroidErrnoFromDarwin(error, &android_error);
  jniThrowErrnoException(env, operation, android_error);
}

jobject MakeTimespec(JNIEnv* env, const struct timespec& value) {
  jclass klass = env->FindClass("android/system/StructTimespec");
  if (klass == nullptr) {
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(klass, "<init>", "(JJ)V");
  jobject result = constructor == nullptr
                       ? nullptr
                       : env->NewObject(klass, constructor,
                                        static_cast<jlong>(value.tv_sec),
                                        static_cast<jlong>(value.tv_nsec));
  env->DeleteLocalRef(klass);
  return result;
}

jobject MakeStructStat(JNIEnv* env, const struct stat& status) {
  jobject access_time = MakeTimespec(env, status.st_atimespec);
  jobject modification_time = MakeTimespec(env, status.st_mtimespec);
  jobject change_time = MakeTimespec(env, status.st_ctimespec);
  if (access_time == nullptr || modification_time == nullptr ||
      change_time == nullptr) {
    env->DeleteLocalRef(access_time);
    env->DeleteLocalRef(modification_time);
    env->DeleteLocalRef(change_time);
    return nullptr;
  }
  jclass klass = env->FindClass("android/system/StructStat");
  jmethodID constructor =
      klass == nullptr
          ? nullptr
          : env->GetMethodID(
                klass, "<init>",
                "(JJIJIIJJLandroid/system/StructTimespec;"
                "Landroid/system/StructTimespec;Landroid/system/StructTimespec;JJ)V");
  jobject result =
      constructor == nullptr
          ? nullptr
          : env->NewObject(
                klass, constructor, static_cast<jlong>(status.st_dev),
                static_cast<jlong>(status.st_ino),
                static_cast<jint>(status.st_mode),
                static_cast<jlong>(status.st_nlink),
                static_cast<jint>(status.st_uid),
                static_cast<jint>(status.st_gid),
                static_cast<jlong>(status.st_rdev),
                static_cast<jlong>(status.st_size), access_time,
                modification_time, change_time,
                static_cast<jlong>(status.st_blksize),
                static_cast<jlong>(status.st_blocks));
  env->DeleteLocalRef(klass);
  env->DeleteLocalRef(access_time);
  env->DeleteLocalRef(modification_time);
  env->DeleteLocalRef(change_time);
  return result;
}

jobject MakeStructPasswd(JNIEnv* env, const struct passwd& password) {
  jclass klass = env->FindClass("android/system/StructPasswd");
  jmethodID constructor =
      klass == nullptr
          ? nullptr
          : env->GetMethodID(
                klass, "<init>",
                "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(klass);
    return nullptr;
  }
  jstring name = env->NewStringUTF(password.pw_name);
  jstring directory = env->NewStringUTF(password.pw_dir);
  jstring shell = env->NewStringUTF(password.pw_shell);
  jobject result =
      name == nullptr || directory == nullptr || shell == nullptr
          ? nullptr
          : env->NewObject(klass, constructor, name,
                           static_cast<jint>(password.pw_uid),
                           static_cast<jint>(password.pw_gid), directory, shell);
  env->DeleteLocalRef(name);
  env->DeleteLocalRef(directory);
  env->DeleteLocalRef(shell);
  env->DeleteLocalRef(klass);
  return result;
}

jobject MakeStructUtsname(JNIEnv* env, const struct utsname& host) {
  jclass klass = env->FindClass("android/system/StructUtsname");
  jmethodID constructor =
      klass == nullptr
          ? nullptr
          : env->GetMethodID(
                klass, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;Ljava/lang/String;)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(klass);
    return nullptr;
  }
  // The compatibility environment exposes Android's kernel/ABI identity.
  // Host release/version remain available for diagnostics without making
  // framework ABI selection believe it is running on Darwin/x86_64.
  jstring sysname = env->NewStringUTF("Linux");
  jstring nodename = env->NewStringUTF(host.nodename);
  jstring release = env->NewStringUTF(host.release);
  jstring version = env->NewStringUTF(host.version);
  jstring machine = env->NewStringUTF("aarch64");
  jobject result =
      sysname == nullptr || nodename == nullptr || release == nullptr ||
              version == nullptr || machine == nullptr
          ? nullptr
          : env->NewObject(klass, constructor, sysname, nodename, release,
                           version, machine);
  env->DeleteLocalRef(sysname);
  env->DeleteLocalRef(nodename);
  env->DeleteLocalRef(release);
  env->DeleteLocalRef(version);
  env->DeleteLocalRef(klass);
  return result;
}

void DarwinUnsupported(JNIEnv* env, const char* operation) {
  ThrowErrno(env, operation, ENOTSUP);
}

template <typename Operation>
ssize_t RunInterruptibleIo(int fd, Operation operation, bool* was_signaled) {
  for (;;) {
    AsynchronousCloseMonitor monitor(fd);
    const ssize_t result = operation();
    const int saved_errno = errno;
    if (monitor.wasSignaled()) {
      *was_signaled = true;
      errno = EINTR;
      return -1;
    }
    if (result != -1 || saved_errno != EINTR) {
      *was_signaled = false;
      errno = saved_errno;
      return result;
    }
  }
}

jlong DarwinLinuxSysconf(JNIEnv* env, jobject, jint name) {
  // POSIX permits -1 with errno left at zero for an indeterminate value.
  errno = 0;
  const long result = Sysconf(name);
  if (result == -1 && errno == EINVAL) {
    ThrowErrno(env, "sysconf", errno);
  }
  return static_cast<jlong>(result);
}

jstring DarwinLinuxGetenv(JNIEnv* env, jobject, jstring java_name) {
  ScopedUtfChars name(env, java_name);
  if (name.c_str() == nullptr) {
    return nullptr;
  }
  const char* value = std::getenv(name.c_str());
  return value == nullptr ? nullptr : env->NewStringUTF(value);
}

jobject DarwinLinuxGetpwuid(JNIEnv* env, jobject, jint uid) {
  const long configured_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  const size_t buffer_size = configured_size > 0
                                 ? static_cast<size_t>(configured_size)
                                 : 1024u;
  std::vector<char> buffer(buffer_size);
  struct passwd password {};
  struct passwd* result = nullptr;
  const int error = getpwuid_r(static_cast<uid_t>(uid), &password,
                               buffer.data(), buffer.size(), &result);
  if (result == nullptr) {
    ThrowErrno(env, "getpwuid_r", error);
    return nullptr;
  }
  return MakeStructPasswd(env, password);
}

jobject DarwinLinuxStat(JNIEnv* env, jobject, jstring java_path) {
  ScopedUtfChars path(env, java_path);
  if (path.c_str() == nullptr) {
    return nullptr;
  }
  struct stat status {};
  if (Stat(path.c_str(), &status) == -1) {
    ThrowErrno(env, "stat", errno);
    return nullptr;
  }
  return MakeStructStat(env, status);
}

jobject DarwinLinuxUname(JNIEnv* env, jobject) {
  struct utsname host {};
  int result;
  do {
    result = uname(&host);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    // Matches upstream: uname failure is treated as impossible and returns
    // null without manufacturing an ErrnoException.
    return nullptr;
  }
  return MakeStructUtsname(env, host);
}

jstring DarwinLinuxStrerror(JNIEnv* env, jobject, jint error_number) {
  // Darwin exposes the XSI/POSIX int-returning strerror_r, unlike the
  // pointer-returning GNU/Bionic interface used by the upstream default path.
  // A private buffer preserves strerror's thread-safety across NewStringUTF.
  std::array<char, BUFSIZ> buffer {};
  const int result = strerror_r(error_number, buffer.data(), buffer.size());
  if (result != 0 && buffer[0] == '\0') {
    // Match upstream's POSIX strerror_r fallback: strerror never converts an
    // unknown errno into an exception or a null Java result.
    std::snprintf(buffer.data(), buffer.size(), "Unknown error %d",
                  error_number);
  }
  return env->NewStringUTF(buffer.data());
}

jstring DarwinLinuxStrsignal(JNIEnv* env, jobject, jint signal_number) {
  std::array<char, BUFSIZ> buffer {};
  if (strsignal_r(signal_number, buffer.data(), buffer.size()) == 0) {
    return env->NewStringUTF(buffer.data());
  }
  // On Darwin strsignal storage is unique to the calling thread. This keeps
  // the upstream libc result for invalid signals without inventing a message.
  const char* message = strsignal(signal_number);
  return message == nullptr ? nullptr : env->NewStringUTF(message);
}

void DarwinLinuxFdsanExchangeOwnerTag(JNIEnv*, jclass, jobject, jlong,
                                      jlong) {
  // Match upstream's complete non-Bionic branch. Java still updates
  // FileDescriptor.ownerId; Darwin libc has no fdsan kernel/libc side table.
}

jlong DarwinLinuxFdsanGetOwnerTag(JNIEnv*, jclass, jobject) { return 0; }

jstring DarwinLinuxFdsanGetTagType(JNIEnv* env, jclass, jlong) {
  return env->NewStringUTF("unknown");
}

jlong DarwinLinuxFdsanGetTagValue(JNIEnv*, jclass, jlong) { return 0; }

jint DarwinLinuxWriteBytes(JNIEnv* env, jobject, jobject java_fd,
                           jobject java_bytes, jint byte_offset,
                           jint byte_count) {
  if (java_bytes == nullptr) {
    jniThrowNullPointerException(env, "null byte storage");
    return -1;
  }
  if (byte_offset < 0 || byte_count < 0) {
    jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException",
                      "negative offset or byte count");
    return -1;
  }
  const void* bytes = nullptr;
  jbyteArray array = nullptr;
  jbyte* array_elements = nullptr;
  jclass byte_array_class = env->FindClass("[B");
  if (byte_array_class != nullptr &&
      env->IsInstanceOf(java_bytes, byte_array_class)) {
    array = static_cast<jbyteArray>(java_bytes);
    const jsize length = env->GetArrayLength(array);
    if (byte_offset > length || byte_count > length - byte_offset) {
      env->DeleteLocalRef(byte_array_class);
      jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException",
                        "byte range exceeds array");
      return -1;
    }
    array_elements = env->GetByteArrayElements(array, nullptr);
    bytes = array_elements == nullptr
                ? nullptr
                : static_cast<const void*>(array_elements + byte_offset);
  } else {
    const jlong capacity = env->GetDirectBufferCapacity(java_bytes);
    const void* direct = env->GetDirectBufferAddress(java_bytes);
    if (direct == nullptr || byte_offset > capacity ||
        byte_count > capacity - byte_offset) {
      env->DeleteLocalRef(byte_array_class);
      jniThrowException(env, "java/lang/IllegalArgumentException",
                        "storage is neither byte[] nor a valid direct buffer range");
      return -1;
    }
    bytes = static_cast<const char*>(direct) + byte_offset;
  }
  env->DeleteLocalRef(byte_array_class);
  if (bytes == nullptr) {
    return -1;
  }
  const int fd = jniGetFDFromFileDescriptor(env, java_fd);
  bool was_signaled = false;
  const ssize_t result = RunInterruptibleIo(
      fd,
      [&] { return write(fd, bytes, static_cast<size_t>(byte_count)); },
      &was_signaled);
  const int saved_errno = errno;
  if (array_elements != nullptr) {
    env->ReleaseByteArrayElements(array, array_elements, JNI_ABORT);
  }
  if (was_signaled) {
    jniThrowException(env, "java/io/InterruptedIOException",
                      "write interrupted by close() on another thread");
    return -1;
  }
  if (result == -1) {
    ThrowErrno(env, "write", saved_errno);
    return -1;
  }
  return static_cast<jint>(result);
}

#if defined(DARWIN_LIBCORE_LINUX_MANAGED_ABI_SMOKE)
void AbiSmokeVoid(JNIEnv* env, jobject, jstring, jint) {
  DarwinUnsupported(env, "abi.void");
}
jint AbiSmokeInt(JNIEnv* env, jobject, jobject, jint, jint) {
  DarwinUnsupported(env, "abi.int");
  return 0;
}
jlong AbiSmokeLong(JNIEnv* env, jobject, jobject) {
  DarwinUnsupported(env, "abi.long");
  return 0;
}
jobject AbiSmokeObject(JNIEnv* env, jobject, jstring) {
  DarwinUnsupported(env, "abi.object");
  return nullptr;
}
jboolean AbiSmokeBoolean(JNIEnv* env, jobject, jstring, jint) {
  DarwinUnsupported(env, "abi.boolean");
  return JNI_FALSE;
}
jlong AbiSmokeAvailableProcessors(JNIEnv* env, jobject receiver) {
  constexpr jint kAndroidScNprocessorsConf = 96;
  return DarwinLinuxSysconf(env, receiver, kAndroidScNprocessorsConf);
}
jstring AbiSmokeStrerror(JNIEnv* env, jobject receiver, jint error_number) {
  return DarwinLinuxStrerror(env, receiver, error_number);
}
jstring AbiSmokeStrsignal(JNIEnv* env, jobject receiver, jint signal_number) {
  return DarwinLinuxStrsignal(env, receiver, signal_number);
}
jint AbiSmokeWriteFile(JNIEnv* env, jobject, jstring java_path,
                       jbyteArray java_bytes) {
  constexpr int kAndroidOCreat = 64;
  constexpr int kAndroidOTrunc = 512;
  ScopedUtfChars path(env, java_path);
  if (path.c_str() == nullptr || java_bytes == nullptr) {
    return -1;
  }
  jbyte* bytes = env->GetByteArrayElements(java_bytes, nullptr);
  if (bytes == nullptr) {
    return -1;
  }
  const jsize size = env->GetArrayLength(java_bytes);
  const int fd = Open(path.c_str(), kAndroidOTrunc | kAndroidOCreat | 1, 0600);
  const ssize_t written =
      fd == -1 ? -1 : Write(fd, bytes, static_cast<size_t>(size));
  struct stat status {};
  const int stat_result = fd == -1 ? -1 : Fstat(fd, &status);
  const int saved_errno = errno;
  if (fd != -1) {
    Close(fd);
  }
  env->ReleaseByteArrayElements(java_bytes, bytes, JNI_ABORT);
  if (written == -1 || stat_result == -1 || status.st_size != written) {
    ThrowErrno(env, "write", saved_errno == 0 ? EIO : saved_errno);
    return -1;
  }
  return static_cast<jint>(written);
}
#endif

jobject DarwinLinuxOpen(JNIEnv* env, jobject, jstring java_path, jint flags,
                        jint mode) {
  ScopedUtfChars path(env, java_path);
  if (path.c_str() == nullptr) {
    return nullptr;
  }
  const int fd = Open(path.c_str(), flags, static_cast<mode_t>(mode));
  if (fd == -1) {
    ThrowErrno(env, "open", errno);
    return nullptr;
  }
  jobject result = jniCreateFileDescriptor(env, fd);
  if (result == nullptr) {
    Close(fd);
  }
  return result;
}

void DarwinLinuxClose(JNIEnv* env, jobject, jobject java_fd) {
  if (java_fd == nullptr) {
    jniThrowNullPointerException(env, "null fd");
    return;
  }
  const int fd = jniGetFDFromFileDescriptor(env, java_fd);
  jniSetFileDescriptorOfFD(env, java_fd, -1);
  if (Close(fd) == -1) {
    ThrowErrno(env, "close", errno);
  }
}

jobject DarwinLinuxFstat(JNIEnv* env, jobject, jobject java_fd) {
  struct stat status {};
  if (Fstat(jniGetFDFromFileDescriptor(env, java_fd), &status) == -1) {
    ThrowErrno(env, "fstat", errno);
    return nullptr;
  }
  return MakeStructStat(env, status);
}

jint DarwinLinuxReadBytes(JNIEnv* env, jobject, jobject java_fd,
                          jobject java_bytes, jint byte_offset,
                          jint byte_count) {
  if (java_bytes == nullptr) {
    jniThrowNullPointerException(env, "null byte storage");
    return -1;
  }
  if (byte_offset < 0 || byte_count < 0) {
    jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException",
                      "negative offset or byte count");
    return -1;
  }

  void* bytes = nullptr;
  jbyteArray array = nullptr;
  jclass byte_array_class = env->FindClass("[B");
  if (byte_array_class != nullptr &&
      env->IsInstanceOf(java_bytes, byte_array_class)) {
    array = static_cast<jbyteArray>(java_bytes);
    const jsize length = env->GetArrayLength(array);
    if (byte_offset > length || byte_count > length - byte_offset) {
      env->DeleteLocalRef(byte_array_class);
      jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException",
                        "byte range exceeds array");
      return -1;
    }
    bytes = env->GetByteArrayElements(array, nullptr);
  } else {
    const jlong capacity = env->GetDirectBufferCapacity(java_bytes);
    bytes = env->GetDirectBufferAddress(java_bytes);
    if (bytes == nullptr || byte_offset > capacity ||
        byte_count > capacity - byte_offset) {
      env->DeleteLocalRef(byte_array_class);
      jniThrowException(env, "java/lang/IllegalArgumentException",
                        "storage is neither byte[] nor a valid direct buffer range");
      return -1;
    }
  }
  env->DeleteLocalRef(byte_array_class);
  if (bytes == nullptr) {
    return -1;
  }

  const int fd = jniGetFDFromFileDescriptor(env, java_fd);
  bool was_signaled = false;
  const ssize_t result = RunInterruptibleIo(
      fd,
      [&] {
        return read(fd, static_cast<char*>(bytes) + byte_offset,
                    static_cast<size_t>(byte_count));
      },
      &was_signaled);
  const int saved_errno = errno;
  if (array != nullptr) {
    env->ReleaseByteArrayElements(array, static_cast<jbyte*>(bytes), 0);
  }
  if (was_signaled) {
    jniThrowException(env, "java/io/InterruptedIOException",
                      "read interrupted by close() on another thread");
    return -1;
  }
  if (result == -1) {
    ThrowErrno(env, "read", saved_errno);
  }
  if (result > std::numeric_limits<jint>::max()) {
    ThrowErrno(env, "read", EOVERFLOW);
    return -1;
  }
  return static_cast<jint>(result);
}

jlong DarwinLinuxLseek(JNIEnv* env, jobject, jobject java_fd, jlong offset,
                       jint whence) {
  const int64_t result =
      Lseek(jniGetFDFromFileDescriptor(env, java_fd),
            static_cast<int64_t>(offset), whence);
  if (result == -1) {
    ThrowErrno(env, "lseek", errno);
    return -1;
  }
  return static_cast<jlong>(result);
}

jlong DarwinLinuxMmap(JNIEnv* env, jobject, jlong address, jlong byte_count,
                      jint prot, jint flags, jobject java_fd, jlong offset) {
  if (byte_count < 0 || offset < 0 ||
      static_cast<unsigned long long>(byte_count) >
          std::numeric_limits<size_t>::max()) {
    ThrowErrno(env, "mmap", EINVAL);
    return 0;
  }
  void* result = Mmap(
      reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
      static_cast<size_t>(byte_count), prot, flags,
      jniGetFDFromFileDescriptor(env, java_fd), static_cast<off_t>(offset));
  if (result == MAP_FAILED) {
    ThrowErrno(env, "mmap", errno);
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(result));
}

void DarwinLinuxMunmap(JNIEnv* env, jobject, jlong address, jlong byte_count) {
  if (byte_count < 0 || Munmap(
                            reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
                            static_cast<size_t>(byte_count)) == -1) {
    ThrowErrno(env, "munmap", byte_count < 0 ? EINVAL : errno);
  }
}

jint DarwinNativeGetegid() { return static_cast<jint>(getegid()); }
jint DarwinNativeGeteuid() { return static_cast<jint>(geteuid()); }
jint DarwinNativeGetgid() { return static_cast<jint>(getgid()); }
jint DarwinNativeGetpid() { return static_cast<jint>(getpid()); }
jint DarwinNativeGetppid() { return static_cast<jint>(getppid()); }
jint DarwinNativeGetuid() { return static_cast<jint>(getuid()); }
jint DarwinNativeGettid() {
  uint64_t thread_id = 0;
  if (pthread_threadid_np(nullptr, &thread_id) != 0 ||
      thread_id > static_cast<uint64_t>(std::numeric_limits<jint>::max())) {
    return -1;
  }
  return static_cast<jint>(thread_id);
}

// Generated from the checksum-locked Android 16 upstream gMethods table.
#include "darwin_linux_method_table.inc"

#if defined(DARWIN_LIBCORE_LINUX_MANAGED_ABI_SMOKE)
JNINativeMethod kAbiSmokeMethods[] = {
    {const_cast<char*>("unsupportedVoid"),
     const_cast<char*>("(Ljava/lang/String;I)V"),
     reinterpret_cast<void*>(&AbiSmokeVoid)},
    {const_cast<char*>("unsupportedInt"),
     const_cast<char*>("(Ljava/io/FileDescriptor;II)I"),
     reinterpret_cast<void*>(&AbiSmokeInt)},
    {const_cast<char*>("unsupportedLong"),
     const_cast<char*>("(Ljava/io/FileDescriptor;)J"),
     reinterpret_cast<void*>(&AbiSmokeLong)},
    {const_cast<char*>("unsupportedObject"),
     const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
     reinterpret_cast<void*>(&AbiSmokeObject)},
    {const_cast<char*>("unsupportedBoolean"),
     const_cast<char*>("(Ljava/lang/String;I)Z"),
     reinterpret_cast<void*>(&AbiSmokeBoolean)},
    {const_cast<char*>("availableProcessors"), const_cast<char*>("()J"),
     reinterpret_cast<void*>(&AbiSmokeAvailableProcessors)},
    {const_cast<char*>("environment"),
     const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
     reinterpret_cast<void*>(&DarwinLinuxGetenv)},
    {const_cast<char*>("statPath"),
     const_cast<char*>("(Ljava/lang/String;)Landroid/system/StructStat;"),
     reinterpret_cast<void*>(&DarwinLinuxStat)},
    {const_cast<char*>("writeFile"), const_cast<char*>("(Ljava/lang/String;[B)I"),
     reinterpret_cast<void*>(&AbiSmokeWriteFile)},
    {const_cast<char*>("unameView"),
     const_cast<char*>("()Landroid/system/StructUtsname;"),
     reinterpret_cast<void*>(&DarwinLinuxUname)},
    {const_cast<char*>("errorMessage"),
     const_cast<char*>("(I)Ljava/lang/String;"),
     reinterpret_cast<void*>(&AbiSmokeStrerror)},
    {const_cast<char*>("signalMessage"),
     const_cast<char*>("(I)Ljava/lang/String;"),
     reinterpret_cast<void*>(&AbiSmokeStrsignal)},
    {const_cast<char*>("openFile"),
     const_cast<char*>("(Ljava/lang/String;II)Ljava/io/FileDescriptor;"),
     reinterpret_cast<void*>(&DarwinLinuxOpen)},
    {const_cast<char*>("seekFile"),
     const_cast<char*>("(Ljava/io/FileDescriptor;JI)J"),
     reinterpret_cast<void*>(&DarwinLinuxLseek)},
    {const_cast<char*>("readFile"),
     const_cast<char*>("(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I"),
     reinterpret_cast<void*>(&DarwinLinuxReadBytes)},
    {const_cast<char*>("closeFile"),
     const_cast<char*>("(Ljava/io/FileDescriptor;)V"),
     reinterpret_cast<void*>(&DarwinLinuxClose)},
    {const_cast<char*>("exchangeOwnerTag"),
     const_cast<char*>("(Ljava/io/FileDescriptor;JJ)V"),
     reinterpret_cast<void*>(&DarwinLinuxFdsanExchangeOwnerTag)},
    {const_cast<char*>("getOwnerTag"),
     const_cast<char*>("(Ljava/io/FileDescriptor;)J"),
     reinterpret_cast<void*>(&DarwinLinuxFdsanGetOwnerTag)},
    {const_cast<char*>("getTagType"),
     const_cast<char*>("(J)Ljava/lang/String;"),
     reinterpret_cast<void*>(&DarwinLinuxFdsanGetTagType)},
    {const_cast<char*>("getTagValue"), const_cast<char*>("(J)J"),
     reinterpret_cast<void*>(&DarwinLinuxFdsanGetTagValue)},
};
#endif

}  // namespace

#if defined(DARWIN_LIBCORE_LINUX_MANAGED_ABI_SMOKE)
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass klass =
      env->FindClass("dev/darwinart/probe/LibcoreDarwinAbiSmoke");
  if (klass == nullptr ||
      env->RegisterNatives(
          klass, kAbiSmokeMethods,
          static_cast<jint>(sizeof(kAbiSmokeMethods) /
                            sizeof(kAbiSmokeMethods[0]))) != JNI_OK) {
    env->DeleteLocalRef(klass);
    return JNI_ERR;
  }
  env->DeleteLocalRef(klass);
  return JNI_VERSION_1_6;
}
#endif

bool RegisterLinuxNatives(JNIEnv* env) {
  jclass klass = env->FindClass("libcore/io/Linux");
  if (klass == nullptr) {
    return false;
  }
  const jint method_count =
      static_cast<jint>(sizeof(kDarwinLinuxMethods) /
                        sizeof(kDarwinLinuxMethods[0]));
  const bool registered = method_count == 135 &&
                          env->RegisterNatives(klass, kDarwinLinuxMethods,
                                               method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}

}  // namespace darwin_art::libcore_darwin
