#include "runtime_graphics_state.h"
// State TU compile flags intentionally mirror the production HWUI probe.

#include <memory>

#include "darwin_surface_bridge.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#define private public
#define protected public
#include "AnimationContext.h"
#include "renderthread/TimeLord.h"
#undef protected
#undef private
#endif

namespace darwin_art_graphics {
namespace {

jclass g_probe_canvas_class = nullptr;
jobject g_interactive_root = nullptr;
jobject g_gpu_render_node = nullptr;
bool g_gpu_render_node_recorded = false;
bool g_gpu_ripple_overlay_active = false;
jfloat g_gpu_ripple_overlay_x = 0.0f;
jfloat g_gpu_ripple_overlay_y = 0.0f;
std::chrono::steady_clock::time_point g_gpu_ripple_overlay_started;
jobject g_pressed_view = nullptr;
uint32_t g_pending_pressed_action = 0;
jfloat g_pending_pressed_x = 0.0f;
jfloat g_pending_pressed_y = 0.0f;
jint g_interactive_width = 0;
jint g_interactive_height = 0;
DarwinArtSurface* g_gpu_surface = nullptr;
#if defined(DARWIN_ART_REAL_GRAPHICS)
std::unique_ptr<android::uirenderer::renderthread::TimeLord> g_hwui_time_lord;
std::unique_ptr<android::uirenderer::AnimationContext> g_hwui_animation_context;
#endif

}  // namespace

jclass& probe_canvas_class_for_state() { return g_probe_canvas_class; }
jobject& interactive_root_for_state() { return g_interactive_root; }
jint& interactive_width_for_state() { return g_interactive_width; }
jint& interactive_height_for_state() { return g_interactive_height; }
jobject& gpu_render_node_for_state() { return g_gpu_render_node; }
bool& gpu_render_node_recorded_for_state() {
  return g_gpu_render_node_recorded;
}
bool& gpu_ripple_overlay_active_for_state() {
  return g_gpu_ripple_overlay_active;
}
jfloat& gpu_ripple_overlay_x_for_state() { return g_gpu_ripple_overlay_x; }
jfloat& gpu_ripple_overlay_y_for_state() { return g_gpu_ripple_overlay_y; }
std::chrono::steady_clock::time_point& gpu_ripple_overlay_started_for_state() {
  return g_gpu_ripple_overlay_started;
}
jobject& pressed_view_for_state() { return g_pressed_view; }
uint32_t& pending_pressed_action_for_state() {
  return g_pending_pressed_action;
}
jfloat& pending_pressed_x_for_state() { return g_pending_pressed_x; }
jfloat& pending_pressed_y_for_state() { return g_pending_pressed_y; }
DarwinArtSurface*& gpu_surface_for_state() { return g_gpu_surface; }

#if defined(DARWIN_ART_REAL_GRAPHICS)
android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_state() {
  return g_hwui_time_lord.get();
}
android::uirenderer::AnimationContext* hwui_animation_context_for_state() {
  return g_hwui_animation_context.get();
}
std::unique_ptr<android::uirenderer::renderthread::TimeLord>&
hwui_time_lord_owner_for_state() {
  return g_hwui_time_lord;
}
std::unique_ptr<android::uirenderer::AnimationContext>&
hwui_animation_context_owner_for_state() {
  return g_hwui_animation_context;
}
#endif

jobject interactive_root_for_input() { return interactive_root_for_state(); }
jint interactive_width_for_input() { return interactive_width_for_state(); }
jint interactive_height_for_input() { return interactive_height_for_state(); }
jobject& pressed_view_for_input() { return pressed_view_for_state(); }
uint32_t& pending_pressed_action_for_input() {
  return pending_pressed_action_for_state();
}
jfloat& pending_pressed_x_for_input() { return pending_pressed_x_for_state(); }
jfloat& pending_pressed_y_for_input() { return pending_pressed_y_for_state(); }
bool& gpu_ripple_overlay_active_for_input() {
  return gpu_ripple_overlay_active_for_state();
}
jfloat& gpu_ripple_overlay_x_for_input() {
  return gpu_ripple_overlay_x_for_state();
}
jfloat& gpu_ripple_overlay_y_for_input() {
  return gpu_ripple_overlay_y_for_state();
}
std::chrono::steady_clock::time_point& gpu_ripple_overlay_started_for_input() {
  return gpu_ripple_overlay_started_for_state();
}
#if defined(DARWIN_ART_REAL_GRAPHICS)
jobject gpu_render_node_for_input() { return gpu_render_node_for_state(); }
android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_input() {
  return hwui_time_lord_for_state();
}
android::uirenderer::AnimationContext* hwui_animation_context_for_input() {
  return hwui_animation_context_for_state();
}
#endif

void set_probe_canvas_class(JNIEnv* env, jclass canvas_class) {
  if (env != nullptr && g_probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(g_probe_canvas_class);
    g_probe_canvas_class = nullptr;
  }
  if (env != nullptr && canvas_class != nullptr) {
    g_probe_canvas_class =
        static_cast<jclass>(env->NewGlobalRef(canvas_class));
  }
}

bool retain_interactive_root(JNIEnv* env, jobject root, jint width, jint height) {
  if (env == nullptr || root == nullptr || env->ExceptionCheck()) {
    return false;
  }
  if (g_interactive_root != nullptr) {
    env->DeleteGlobalRef(g_interactive_root);
    g_interactive_root = nullptr;
  }
  g_interactive_root = env->NewGlobalRef(root);
  g_interactive_width = width;
  g_interactive_height = height;
  return g_interactive_root != nullptr && !env->ExceptionCheck();
}

void shutdown(JNIEnv* env) {
  if (env == nullptr) return;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (g_hwui_animation_context != nullptr) {
    g_hwui_animation_context->destroy();
    g_hwui_animation_context.reset();
  }
  g_hwui_time_lord.reset();
#endif
  if (g_gpu_render_node != nullptr) {
    env->DeleteGlobalRef(g_gpu_render_node);
    g_gpu_render_node = nullptr;
  }
  if (g_pressed_view != nullptr) {
    env->DeleteGlobalRef(g_pressed_view);
    g_pressed_view = nullptr;
  }
  if (g_interactive_root != nullptr) {
    env->DeleteGlobalRef(g_interactive_root);
    g_interactive_root = nullptr;
  }
  if (g_probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(g_probe_canvas_class);
    g_probe_canvas_class = nullptr;
  }
  g_gpu_render_node_recorded = false;
  g_gpu_ripple_overlay_active = false;
  g_gpu_ripple_overlay_x = 0.0f;
  g_gpu_ripple_overlay_y = 0.0f;
  g_interactive_width = 0;
  g_interactive_height = 0;
  g_pending_pressed_action = 0;
  g_pending_pressed_x = 0.0f;
  g_pending_pressed_y = 0.0f;
  g_gpu_surface = nullptr;
}

}  // namespace darwin_art_graphics
