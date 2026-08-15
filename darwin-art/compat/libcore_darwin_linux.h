#ifndef DARWIN_ART_LIBCORE_DARWIN_LINUX_H_
#define DARWIN_ART_LIBCORE_DARWIN_LINUX_H_

#include <jni.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>

namespace darwin_art::libcore_darwin {

// These functions are the syscall boundary shared by JNI and the standalone
// acceptance probe. Inputs use Android/Linux flag values where documented.
int Open(const char* path, int linux_flags, mode_t mode);
int Close(int fd);
ssize_t Read(int fd, void* bytes, size_t byte_count);
int Fstat(int fd, struct stat* status);
void* Mmap(void* address, size_t byte_count, int linux_prot,
           int linux_flags, int fd, off_t offset);
int Munmap(void* address, size_t byte_count);

// Owns the complete Android 16 libcore.io.Linux 135-entry registration table.
// No other registrar may register a subset of this class after this succeeds.
bool RegisterLinuxNatives(JNIEnv* env);

}  // namespace darwin_art::libcore_darwin

#endif  // DARWIN_ART_LIBCORE_DARWIN_LINUX_H_
