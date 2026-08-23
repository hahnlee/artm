#ifndef DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_
#define DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_

#include <jni.h>

namespace darwin_art {

bool RegisterLibcoreNatives(JNIEnv* env);
// Complete Android java.lang.Runtime + sun.nio.fs.UnixNativeDispatcher tables.
// The real-graphics owner installs these after Math and before app code.
bool RegisterManagedLoadNatives(JNIEnv* env);
bool RegisterLibcoreCharacterNatives(JNIEnv* env);
bool RegisterLibcoreIcuNatives(JNIEnv* env);
bool ShutdownLibcoreNatives();

}  // namespace darwin_art

#endif  // DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_
