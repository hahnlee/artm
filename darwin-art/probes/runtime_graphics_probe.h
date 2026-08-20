#pragma once

#include <jni.h>
#include <cstdint>

namespace darwin_art_graphics {
void set_probe_canvas_class(JNIEnv* env, jclass canvas_class);
bool retain_interactive_root(JNIEnv* env, jobject root, jint width, jint height);
jboolean present_content(JNIEnv* env, jclass unused, jobject view, jint width,
                         jint height);
void shutdown(JNIEnv* env);
}  // namespace darwin_art_graphics

extern "C" int32_t darwin_art_dispatch_pointer(uint32_t action, float x,
                                                 float y);
extern "C" int32_t darwin_art_pump_framework_frame(jlong frame_time_nanos);

