#pragma once

#include <string>

#include "jni.h"

namespace android {

JNIEnv* CurrentArtEnv();
bool DescriptorToShorty(const char* descriptor, std::string* shorty);

}  // namespace android
