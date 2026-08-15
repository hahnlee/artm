#ifndef DARWIN_ART_DARWIN_OS_CONSTANTS_H_
#define DARWIN_ART_DARWIN_OS_CONSTANTS_H_

#include <jni.h>

namespace darwin_art::os_constants {

// Translate Java-visible Android/Linux ABI values at the Darwin syscall edge.
bool DarwinOpenFlagsFromAndroid(int android_flags, int* darwin_flags);
bool DarwinSysconfNameFromAndroid(int android_name, int* darwin_name);
bool AndroidErrnoFromDarwin(int darwin_errno, int* android_errno);

}  // namespace darwin_art::os_constants

// Exact Android 16 android.system.OsConstants registrar owner.
void register_android_system_OsConstants(JNIEnv* env);

#endif  // DARWIN_ART_DARWIN_OS_CONSTANTS_H_
