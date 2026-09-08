#pragma once

#include <string>
#include <cstdint>

#include "jni.h"

namespace android {

JNIEnv* CurrentArtEnv();
// Returns the tagged Generic-JNI SaveRefsAndArgs frame for the current ART
// thread when a native callback is entered through that AOSP transition.
bool CurrentGenericJniFrame(uint64_t* managed_sp);
bool DescriptorToShorty(const char* descriptor, std::string* shorty);

}  // namespace android
