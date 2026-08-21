#pragma once

#include <jni.h>

namespace darwin_art_graphics {

struct GraphicsState;

// Records the Android View display list and submits the retained RenderNode
// through the GPU surface. The presentation TU owns only the flavor dispatch;
// this TU owns the HWUI/Skia-heavy recording implementation.
jboolean record_gpu_content(GraphicsState* state, JNIEnv* env, jobject view,
                            jint width, jint height);

}  // namespace darwin_art_graphics
