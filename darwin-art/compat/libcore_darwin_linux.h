#ifndef DARWIN_ART_LIBCORE_DARWIN_LINUX_H_
#define DARWIN_ART_LIBCORE_DARWIN_LINUX_H_

#include <jni.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>

namespace darwin_art::libcore_darwin {

// These functions are the syscall boundary shared by JNI and the standalone
// acceptance probe. Inputs use Android/Linux flag values where documented.
int Open(const char* path, int linux_flags, mode_t mode);
int Close(int fd);
ssize_t Read(int fd, void* bytes, size_t byte_count);
ssize_t Write(int fd, const void* bytes, size_t byte_count);
int Fstat(int fd, struct stat* status);
int Stat(const char* path, struct stat* status);
int64_t Lseek(int fd, int64_t offset, int android_whence);
void* Mmap(void* address, size_t byte_count, int linux_prot,
           int linux_flags, int fd, off_t offset);
int Munmap(void* address, size_t byte_count);
long Sysconf(int name);

// System/metadata JNI phase. Keeping these definitions out of the generated
// registrar TU lets edits to environment, identity, and diagnostic policy
// rebuild one small object instead of the complete 135-entry table.
void ThrowErrno(JNIEnv* env, const char* operation, int error);
jlong DarwinLinuxSysconf(JNIEnv* env, jobject receiver, jint name);
jstring DarwinLinuxGetenv(JNIEnv* env, jobject receiver, jstring java_name);
jobject DarwinLinuxGetpwuid(JNIEnv* env, jobject receiver, jint uid);
jobject DarwinLinuxUname(JNIEnv* env, jobject receiver);
jstring DarwinLinuxStrerror(JNIEnv* env, jobject receiver, jint error_number);
jstring DarwinLinuxStrsignal(JNIEnv* env, jobject receiver, jint signal_number);

// Owns the complete Android 16 libcore.io.Linux 135-entry registration table.
// No other registrar may register a subset of this class after this succeeds.
bool RegisterLinuxNatives(JNIEnv* env);

}  // namespace darwin_art::libcore_darwin

#endif  // DARWIN_ART_LIBCORE_DARWIN_LINUX_H_
