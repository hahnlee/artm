#pragma once

#include <jni.h>

#include <array>
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

// All JNI/HWUI references belong to one GraphicsSession. The session owns
// this object as a single ABI unit; every native consumer includes this header
// and must be rebuilt when its layout changes. Probe/input/phase code only
// receives a borrowed pointer while running on the ART owner thread. The
// incremental graph treats this header as an ABI dependency of the session
// allocator and every field-accessing translation unit.
struct GraphicsState {
  jclass probe_canvas_class = nullptr;
  jclass service_bridge_class = nullptr;
  jobject interactive_root = nullptr;
  jobject interactive_view_root = nullptr;
  jobject focused_view_root = nullptr;
  jmethodID focused_window_view_root_method = nullptr;
  jmethodID window_topology_generation_method = nullptr;
  jint focused_window_generation = -1;
  jobject hardware_context = nullptr;
  jobject gpu_render_node = nullptr;
  bool gpu_render_node_recorded = false;
  jint gpu_last_traversal_barrier = -1;
  bool gpu_ripple_overlay_active = false;
  jfloat gpu_ripple_overlay_x = 0.0f;
  jfloat gpu_ripple_overlay_y = 0.0f;
  std::chrono::steady_clock::time_point gpu_ripple_overlay_started{};
  jobject pressed_view = nullptr;
  jobject pointer_dispatch_root = nullptr;
  jobject pointer_dispatch_view_root = nullptr;
  jfloat pointer_dispatch_offset_x = 0.0f;
  jfloat pointer_dispatch_offset_y = 0.0f;
  bool pointer_dispatch_is_window = false;
  bool pointer_dispatch_outside_only = false;
  uint32_t pending_pressed_action = 0;
  jfloat pending_pressed_x = 0.0f;
  jfloat pending_pressed_y = 0.0f;
  int64_t pointer_down_time_nanos = 0;
  std::array<int64_t, 512> key_down_time_nanos{};
  bool pointer_stream_active = false;
  jfloat pointer_down_x = 0.0f;
  jfloat pointer_down_y = 0.0f;
  jint pointer_touch_slop = 8;
  bool pointer_click_candidate = false;
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
bool retain_service_bridge_class(GraphicsState* state, JNIEnv* env,
                                 jclass bridge_class);
bool retain_interactive_root(GraphicsState* state, JNIEnv* env, jobject root,
                             jint width, jint height);
bool retain_interactive_view_root(GraphicsState* state, JNIEnv* env,
                                  jobject view_root);
bool retain_hardware_context(GraphicsState* state, JNIEnv* env, jobject context);
void begin_activity_transition(GraphicsState* state, JNIEnv* env);
void shutdown(GraphicsState* state, JNIEnv* env);

#if defined(DARWIN_ART_REAL_GRAPHICS)
::android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_state(GraphicsState* state);
::android::uirenderer::AnimationContext* hwui_animation_context_for_state(GraphicsState* state);
#endif

}  // namespace darwin_art_graphics
