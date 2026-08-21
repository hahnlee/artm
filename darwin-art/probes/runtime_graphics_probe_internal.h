#pragma once

#include <jni.h>

#include <chrono>
#include <cstdint>

#include "runtime_graphics_state.h"

namespace darwin_art_graphics {

jobject find_clickable_view_at(JNIEnv* env, jobject view, jfloat x, jfloat y);

}  // namespace darwin_art_graphics
