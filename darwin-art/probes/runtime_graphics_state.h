#pragma once

#include <jni.h>

#include <chrono>
#include <cstdint>
#include <memory>

struct DarwinArtSurface;

#if defined(DARWIN_ART_REAL_GRAPHICS)
namespace android::uirenderer {
class AnimationContext;
namespace renderthread {
class TimeLord;
}  // namespace renderthread
}  // namespace android::uirenderer
#endif

namespace darwin_art_graphics {

// Graphics state is process-local and is deliberately kept behind this ABI
// boundary.  The presentation, input, and shutdown translation units only
// borrow these slots; this TU owns their storage and cleanup order.
jclass& probe_canvas_class_for_state();
jobject& interactive_root_for_state();
jint& interactive_width_for_state();
jint& interactive_height_for_state();
jobject& gpu_render_node_for_state();
bool& gpu_render_node_recorded_for_state();
bool& gpu_ripple_overlay_active_for_state();
jfloat& gpu_ripple_overlay_x_for_state();
jfloat& gpu_ripple_overlay_y_for_state();
std::chrono::steady_clock::time_point& gpu_ripple_overlay_started_for_state();
jobject& pressed_view_for_state();
uint32_t& pending_pressed_action_for_state();
jfloat& pending_pressed_x_for_state();
jfloat& pending_pressed_y_for_state();
DarwinArtSurface*& gpu_surface_for_state();

void set_probe_canvas_class(JNIEnv* env, jclass canvas_class);
bool retain_interactive_root(JNIEnv* env, jobject root, jint width, jint height);
void shutdown(JNIEnv* env);

#if defined(DARWIN_ART_REAL_GRAPHICS)
::android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_state();
::android::uirenderer::AnimationContext* hwui_animation_context_for_state();
std::unique_ptr<::android::uirenderer::renderthread::TimeLord>&
hwui_time_lord_owner_for_state();
std::unique_ptr<::android::uirenderer::AnimationContext>&
hwui_animation_context_owner_for_state();
#endif

// Compatibility accessors used by the input TU.  They remain thin aliases;
// the backing storage and cleanup live only in runtime_graphics_state.cc.
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
#if defined(DARWIN_ART_REAL_GRAPHICS)
jobject gpu_render_node_for_input();
::android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_input();
::android::uirenderer::AnimationContext* hwui_animation_context_for_input();
#endif

}  // namespace darwin_art_graphics
