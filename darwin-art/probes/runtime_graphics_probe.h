#pragma once

#include <jni.h>
#include <cstdint>

namespace darwin_art_graphics {
struct GraphicsState;
void set_probe_canvas_class(GraphicsState* state, JNIEnv* env,
                            jclass canvas_class);
bool retain_interactive_root(GraphicsState* state, JNIEnv* env, jobject root,
                             jint width, jint height);
jboolean present_content(GraphicsState* state, JNIEnv* env, jclass unused,
                         jobject view, jint width, jint height);
void shutdown(GraphicsState* state, JNIEnv* env);
int32_t dispatch_pointer(GraphicsState* state, uint32_t action, float x,
                         float y);
int32_t pump_frame(GraphicsState* state, jlong frame_time_nanos);
}  // namespace darwin_art_graphics

extern "C" int32_t darwin_art_dispatch_pointer(uint32_t action, float x,
                                                 float y);
extern "C" int32_t darwin_art_pump_framework_frame(jlong frame_time_nanos);
