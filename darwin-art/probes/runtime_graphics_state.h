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

// All JNI/HWUI references belong to one GraphicsSession.  The session owns
// this object; probe/input/phase code only receives a borrowed pointer while
// running on the ART owner thread.
struct GraphicsState {
  jclass probe_canvas_class = nullptr;
  jobject interactive_root = nullptr;
  jobject hardware_context = nullptr;
  jobject gpu_render_node = nullptr;
  bool gpu_render_node_recorded = false;
  bool gpu_ripple_overlay_active = false;
  jfloat gpu_ripple_overlay_x = 0.0f;
  jfloat gpu_ripple_overlay_y = 0.0f;
  std::chrono::steady_clock::time_point gpu_ripple_overlay_started{};
  jobject pressed_view = nullptr;
  uint32_t pending_pressed_action = 0;
  jfloat pending_pressed_x = 0.0f;
  jfloat pending_pressed_y = 0.0f;
  jint interactive_width = 0;
  jint interactive_height = 0;
  // Borrowed from the surface owner. GraphicsState never destroys this.
  DarwinArtSurface* gpu_surface = nullptr;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  std::unique_ptr<::android::uirenderer::renderthread::TimeLord> hwui_time_lord;
  std::unique_ptr<::android::uirenderer::AnimationContext> hwui_animation_context;
#endif

  GraphicsState();
  GraphicsState(const GraphicsState&) = delete;
  GraphicsState& operator=(const GraphicsState&) = delete;
  ~GraphicsState();
};

// Graphics state is process-local and is deliberately kept behind this ABI
// boundary.  The presentation, input, and shutdown translation units only
// borrow these slots; this TU owns their storage and cleanup order.
void set_probe_canvas_class(GraphicsState* state, JNIEnv* env, jclass canvas_class);
bool retain_interactive_root(GraphicsState* state, JNIEnv* env, jobject root,
                             jint width, jint height);
bool retain_hardware_context(GraphicsState* state, JNIEnv* env, jobject context);
void shutdown(GraphicsState* state, JNIEnv* env);

#if defined(DARWIN_ART_REAL_GRAPHICS)
::android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_state(GraphicsState* state);
::android::uirenderer::AnimationContext* hwui_animation_context_for_state(GraphicsState* state);
#endif

}  // namespace darwin_art_graphics
