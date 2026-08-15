#pragma once

#include <jni.h>

namespace darwin_art {

bool RegisterIcuCharsetNatives(JNIEnv* env);
void ShutdownIcuCharsetNatives();

}  // namespace darwin_art
