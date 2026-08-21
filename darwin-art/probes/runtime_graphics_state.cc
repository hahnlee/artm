#include "runtime_graphics_state.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#define private public
#define protected public
#include "AnimationContext.h"
#include "renderthread/TimeLord.h"
#undef protected
#undef private
#endif

namespace darwin_art_graphics {

GraphicsState::GraphicsState() = default;
GraphicsState::~GraphicsState() = default;

#if defined(DARWIN_ART_REAL_GRAPHICS)
android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_state(
    GraphicsState* state) {
  return state == nullptr ? nullptr : state->hwui_time_lord.get();
}

android::uirenderer::AnimationContext* hwui_animation_context_for_state(
    GraphicsState* state) {
  return state == nullptr ? nullptr : state->hwui_animation_context.get();
}
#endif

void set_probe_canvas_class(GraphicsState* state, JNIEnv* env,
                            jclass canvas_class) {
  if (state == nullptr || env == nullptr) return;
  if (state->probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(state->probe_canvas_class);
    state->probe_canvas_class = nullptr;
  }
  if (canvas_class != nullptr) {
    state->probe_canvas_class =
        static_cast<jclass>(env->NewGlobalRef(canvas_class));
  }
}

bool retain_interactive_root(GraphicsState* state, JNIEnv* env, jobject root,
                             jint width, jint height) {
  if (state == nullptr || env == nullptr || root == nullptr ||
      env->ExceptionCheck()) {
    return false;
  }
  if (state->interactive_root != nullptr) {
    env->DeleteGlobalRef(state->interactive_root);
    state->interactive_root = nullptr;
  }
  state->interactive_root = env->NewGlobalRef(root);
  state->interactive_width = width;
  state->interactive_height = height;
  return state->interactive_root != nullptr && !env->ExceptionCheck();
}

void shutdown(GraphicsState* state, JNIEnv* env) {
  if (state == nullptr || env == nullptr) return;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (state->hwui_animation_context != nullptr) {
    state->hwui_animation_context->destroy();
    state->hwui_animation_context.reset();
  }
  state->hwui_time_lord.reset();
#endif
  if (state->gpu_render_node != nullptr) {
    env->DeleteGlobalRef(state->gpu_render_node);
    state->gpu_render_node = nullptr;
  }
  if (state->pressed_view != nullptr) {
    env->DeleteGlobalRef(state->pressed_view);
    state->pressed_view = nullptr;
  }
  if (state->interactive_root != nullptr) {
    env->DeleteGlobalRef(state->interactive_root);
    state->interactive_root = nullptr;
  }
  if (state->probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(state->probe_canvas_class);
    state->probe_canvas_class = nullptr;
  }
  state->gpu_render_node_recorded = false;
  state->gpu_ripple_overlay_active = false;
  state->gpu_ripple_overlay_x = 0.0f;
  state->gpu_ripple_overlay_y = 0.0f;
  state->interactive_width = 0;
  state->interactive_height = 0;
  state->pending_pressed_action = 0;
  state->pending_pressed_x = 0.0f;
  state->pending_pressed_y = 0.0f;
  // SurfaceSession owns this handle. GraphicsState only borrows it.
  state->gpu_surface = nullptr;
}

}  // namespace darwin_art_graphics
