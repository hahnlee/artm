#include "runtime_graphics_state.h"

#include "darwin_framework_natives.h"

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

bool retain_service_bridge_class(GraphicsState* state, JNIEnv* env,
                                 jclass bridge_class) {
  if (state == nullptr || env == nullptr || bridge_class == nullptr ||
      env->ExceptionCheck()) {
    return false;
  }
  if (state->service_bridge_class != nullptr) {
    env->DeleteGlobalRef(state->service_bridge_class);
    state->service_bridge_class = nullptr;
  }
  state->service_bridge_class =
      static_cast<jclass>(env->NewGlobalRef(bridge_class));
  return state->service_bridge_class != nullptr && !env->ExceptionCheck();
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

bool retain_interactive_view_root(GraphicsState* state, JNIEnv* env,
                                  jobject view_root) {
  if (state == nullptr || env == nullptr || view_root == nullptr) return false;
  if (state->interactive_view_root != nullptr) {
    env->DeleteGlobalRef(state->interactive_view_root);
    state->interactive_view_root = nullptr;
  }
  state->interactive_view_root = env->NewGlobalRef(view_root);
  if (state->interactive_view_root == nullptr || env->ExceptionCheck()) {
    return false;
  }
  return darwin_art::FocusFrameworkViewRoot(env, view_root);
}

bool retain_hardware_context(GraphicsState* state, JNIEnv* env, jobject context) {
  if (state == nullptr || env == nullptr || context == nullptr ||
      env->ExceptionCheck()) {
    return false;
  }
  if (state->hardware_context != nullptr) {
    env->DeleteGlobalRef(state->hardware_context);
    state->hardware_context = nullptr;
  }
  state->hardware_context = env->NewGlobalRef(context);
  return state->hardware_context != nullptr && !env->ExceptionCheck();
}

void begin_activity_transition(GraphicsState* state, JNIEnv* env) {
  if (state == nullptr || env == nullptr) return;
  auto clear = [env](jobject* reference) {
    if (*reference != nullptr) {
      env->DeleteGlobalRef(*reference);
      *reference = nullptr;
    }
  };
  clear(&state->gpu_render_node);
  clear(&state->pressed_view);
  clear(&state->pointer_dispatch_root);
  clear(&state->pointer_dispatch_view_root);
  state->gpu_render_node_recorded = false;
  state->gpu_last_traversal_barrier = -1;
  state->gpu_ripple_overlay_active = false;
  state->pointer_stream_active = false;
  state->pointer_click_candidate = false;
  state->pointer_dispatch_is_window = false;
  state->pointer_dispatch_outside_only = false;
}

void shutdown(GraphicsState* state, JNIEnv* env) {
  if (state == nullptr || env == nullptr) return;
  if (state->service_bridge_class != nullptr) {
    jmethodID shutdown_activities =
        env->GetStaticMethodID(state->service_bridge_class,
                               "shutdownActivities", "()V");
    if (shutdown_activities != nullptr) {
      env->CallStaticVoidMethod(state->service_bridge_class,
                                shutdown_activities);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
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
  if (state->interactive_view_root != nullptr) {
    env->DeleteGlobalRef(state->interactive_view_root);
    state->interactive_view_root = nullptr;
  }
  if (state->pointer_dispatch_root != nullptr) {
    env->DeleteGlobalRef(state->pointer_dispatch_root);
    state->pointer_dispatch_root = nullptr;
  }
  if (state->pointer_dispatch_view_root != nullptr) {
    env->DeleteGlobalRef(state->pointer_dispatch_view_root);
    state->pointer_dispatch_view_root = nullptr;
  }
  state->pointer_dispatch_offset_x = 0.0f;
  state->pointer_dispatch_offset_y = 0.0f;
  state->pointer_dispatch_is_window = false;
  state->pointer_dispatch_outside_only = false;
  state->pointer_down_x = 0.0f;
  state->pointer_down_y = 0.0f;
  state->pointer_touch_slop = 8;
  state->pointer_click_candidate = false;
  if (state->interactive_root != nullptr) {
    env->DeleteGlobalRef(state->interactive_root);
    state->interactive_root = nullptr;
  }
  if (state->hardware_context != nullptr) {
    env->DeleteGlobalRef(state->hardware_context);
    state->hardware_context = nullptr;
  }
  if (state->probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(state->probe_canvas_class);
    state->probe_canvas_class = nullptr;
  }
  if (state->service_bridge_class != nullptr) {
    env->DeleteGlobalRef(state->service_bridge_class);
    state->service_bridge_class = nullptr;
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
