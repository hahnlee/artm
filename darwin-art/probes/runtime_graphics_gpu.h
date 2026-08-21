#pragma once

#include <jni.h>

namespace darwin_art_graphics {

struct GraphicsState;

jboolean present_gpu_content(GraphicsState* state, JNIEnv* env, jobject view,
                             jint width, jint height);

}  // namespace darwin_art_graphics
