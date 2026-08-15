#ifndef DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_
#define DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_

#include <jni.h>

namespace darwin_art {

bool RegisterLibcoreNatives(JNIEnv* env);

}  // namespace darwin_art

#endif  // DARWIN_ART_COMPAT_DARWIN_LIBCORE_NATIVES_H_
