#pragma once

#include <jni.h>

#include <chrono>
#include <cstdint>

namespace darwin_art_graphics {

// Private state accessors shared by the graphics and pointer-input
// translation units. The owning storage remains in runtime_graphics_probe.cc
// so shutdown and the GPU path retain their existing lifetime semantics.
jobject interactive_root_for_input();
jint interactive_width_for_input();
jint interactive_height_for_input();
jobject& pressed_view_for_input();
uint32_t& pending_pressed_action_for_input();
jfloat& pending_pressed_x_for_input();
jfloat& pending_pressed_y_for_input();
bool& gpu_ripple_overlay_active_for_input();
jfloat& gpu_ripple_overlay_x_for_input();
jfloat& gpu_ripple_overlay_y_for_input();
std::chrono::steady_clock::time_point& gpu_ripple_overlay_started_for_input();

jobject find_clickable_view_at(JNIEnv* env, jobject view, jfloat x, jfloat y);

}  // namespace darwin_art_graphics
