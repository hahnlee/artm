#include "jni.h"
#include "jni_util.h"
#include "jlong.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "sun_nio_fs_UnixCopyFile.h"

#define RESTARTABLE(command, result) \
  do {                                 \
    do {                               \
      (result) = (command);            \
    } while ((result) == -1 && errno == EINTR); \
  } while (0)

static void throw_unix_exception(JNIEnv* env, int error_number) {
  jobject exception = JNU_NewObjectByName(
      env, "sun/nio/fs/UnixException", "(I)V", error_number);
  if (exception != NULL) (*env)->Throw(env, exception);
}

// Keep the upstream UnixCopyFile native contract. The read/write calls are
// redirected by darwin_openjdk_nio_fs_redirect.h, so Android virtual FDs are
// copied through the Bionic descriptor table without a host-side alias.
JNIEXPORT void JNICALL Java_sun_nio_fs_UnixCopyFile_transfer(
    JNIEnv* env, jclass unused, jint destination, jint source,
    jlong cancel_address) {
  char buffer[8192];
  volatile jint* cancel = (volatile jint*)jlong_to_ptr(cancel_address);
  for (;;) {
    ssize_t count;
    RESTARTABLE(read((int)source, buffer, sizeof(buffer)), count);
    if (count <= 0) {
      if (count < 0) throw_unix_exception(env, errno);
      return;
    }
    if (cancel != NULL && *cancel != 0) {
      throw_unix_exception(env, ECANCELED);
      return;
    }
    ssize_t offset = 0;
    while (offset < count) {
      ssize_t written;
      RESTARTABLE(write((int)destination, buffer + offset,
                        (size_t)(count - offset)), written);
      if (written <= 0) {
        if (written < 0) throw_unix_exception(env, errno);
        return;
      }
      offset += written;
    }
  }
}

#undef RESTARTABLE
