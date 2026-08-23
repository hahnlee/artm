#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <time.h>

#include "base/locks.h"
#include "darwin_art/darwin_art.h"
#include "runtime_hwui_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_probe_internal.h"
#include "runtime_process_state.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#define private public
#include "AnimationContext.h"
#include "renderthread/TimeLord.h"
#undef private
#endif

namespace darwin_art_graphics {

namespace {

bool MotionEventBridgeEnabled() {
  const char* mode = std::getenv("DARWIN_ART_INPUT_MODE");
  if (mode != nullptr) return std::string(mode) == "motion_event";
  return std::getenv("DARWIN_ART_APK_APP_PACKAGE") != nullptr;
}

int64_t MonotonicNanos() {
  struct timespec now = {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

jclass LoadProbeAnimationHost(JNIEnv* env) {
  if (env == nullptr) return nullptr;
  jclass thread_class = env->FindClass("java/lang/Thread");
  jmethodID current_thread = thread_class == nullptr
                                 ? nullptr
                                 : env->GetStaticMethodID(
                                       thread_class, "currentThread",
                                       "()Ljava/lang/Thread;");
  jobject thread = current_thread == nullptr
                       ? nullptr
                       : env->CallStaticObjectMethod(thread_class, current_thread);
  jmethodID get_loader = thread_class == nullptr
                             ? nullptr
                             : env->GetMethodID(thread_class, "getContextClassLoader",
                                                "()Ljava/lang/ClassLoader;");
  jobject loader = get_loader == nullptr
                       ? nullptr
                       : env->CallObjectMethod(thread, get_loader);
  jclass loader_class = loader == nullptr ? nullptr : env->GetObjectClass(loader);
  jmethodID load_class = loader_class == nullptr
                             ? nullptr
                             : env->GetMethodID(loader_class, "loadClass",
                                                "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring name = env->NewStringUTF("dev.darwinart.probe.ProbeAnimationHost");
  jclass helper = load_class == nullptr
                      ? nullptr
                      : static_cast<jclass>(env->CallObjectMethod(loader, load_class, name));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    helper = nullptr;
  }
  env->DeleteLocalRef(name);
  env->DeleteLocalRef(loader_class);
  env->DeleteLocalRef(loader);
  env->DeleteLocalRef(thread);
  env->DeleteLocalRef(thread_class);
  return helper;
}

int32_t dispatch_motion_event(GraphicsState* state, JNIEnv* env, jobject root,
                              uint32_t action, float x, float y,
                              uint64_t event_time_hint,
                              uint64_t down_time_hint) {
  if (state == nullptr || env == nullptr || root == nullptr) return 72;
  if (action > 3u || !std::isfinite(x) || !std::isfinite(y)) return 71;
  const bool down = action == 0u;
  const bool terminal = action == 1u || action == 3u;
  if (!down && !state->pointer_stream_active) return 78;
  const int64_t event_time_nanos = event_time_hint > 0
                                       ? static_cast<int64_t>(event_time_hint)
                                       : MonotonicNanos();
  if (event_time_nanos <= 0) return 79;
  if (down) {
    state->pointer_down_time_nanos = down_time_hint > 0
                                         ? static_cast<int64_t>(down_time_hint)
                                         : event_time_nanos;
  }
  const int64_t down_time_nanos = state->pointer_down_time_nanos;
  if (down_time_nanos <= 0 || down_time_nanos > event_time_nanos) return 79;

  jclass motion_event_class = env->FindClass("android/view/MotionEvent");
  jmethodID obtain = motion_event_class == nullptr
                         ? nullptr
                         : env->GetStaticMethodID(
                               motion_event_class, "obtain",
                               "(JJIFFI)Landroid/view/MotionEvent;");
  if (obtain == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(motion_event_class);
    return 80;
  }
  jobject event = env->CallStaticObjectMethod(
      motion_event_class, obtain, static_cast<jlong>(down_time_nanos / 1000000),
      static_cast<jlong>(event_time_nanos / 1000000), static_cast<jint>(action),
      static_cast<jfloat>(x), static_cast<jfloat>(y), static_cast<jint>(0));
  if (event == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 81;
  }
  // The compact obtain() overload defaults to a generic pointer source on
  // this detached host. Mark the packet as a touchscreen event before it
  // enters ViewRoot so source/tool filtering follows Android semantics.
  jmethodID set_source = env->GetMethodID(motion_event_class, "setSource", "(I)V");
  if (set_source != nullptr && !env->ExceptionCheck()) {
    env->CallVoidMethod(event, set_source, static_cast<jint>(0x1002));
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 81;
  }
  jclass root_class = env->GetObjectClass(root);
  jmethodID recycle = env->GetMethodID(motion_event_class, "recycle", "()V");
  if (recycle == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(root_class);
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 82;
  }
  const int64_t dispatch_start = MonotonicNanos();
  jboolean consumed = JNI_FALSE;
  bool enqueued = false;
  jclass input_host = LoadProbeAnimationHost(env);
  jmethodID enqueue = input_host == nullptr
                          ? nullptr
                          : env->GetStaticMethodID(
                                input_host, "enqueueInputEvent",
                                "(Ljava/lang/Object;)Z");
  if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr &&
      (input_host == nullptr || enqueue == nullptr)) {
    std::cerr << "ART Android MotionEvent: ViewRoot enqueue helper unavailable host="
              << input_host << " method=" << enqueue << "\n";
  }
  if (enqueue != nullptr && !env->ExceptionCheck()) {
    enqueued = env->CallStaticBooleanMethod(input_host, enqueue, event) == JNI_TRUE;
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      enqueued = false;
    }
  } else if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (!enqueued) {
    jmethodID dispatch_touch = root_class == nullptr
                                   ? nullptr
                                   : env->GetMethodID(
                                         root_class, "dispatchTouchEvent",
                                         "(Landroid/view/MotionEvent;)Z");
    if (dispatch_touch == nullptr || env->ExceptionCheck()) {
      env->ExceptionClear();
      env->DeleteLocalRef(input_host);
      env->DeleteLocalRef(root_class);
      env->DeleteLocalRef(event);
      env->DeleteLocalRef(motion_event_class);
      return 82;
    }
    consumed = env->CallBooleanMethod(root, dispatch_touch, event);
  } else {
    consumed = JNI_TRUE;
  }
  const int64_t dispatch_end = MonotonicNanos();
  const bool dispatch_ok = !env->ExceptionCheck();
  if (!dispatch_ok && std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
    std::cerr << "ART Android MotionEvent dispatch exception\n";
    env->ExceptionDescribe();
  }
  // ViewRootImpl takes ownership of an enqueued InputEvent and recycles it
  // after the InputStage chain finishes. DecorView dispatch does not, so the
  // bounded fallback owns the explicit recycle call.
  bool recycle_ok = true;
  if (!enqueued) {
    env->CallVoidMethod(event, recycle);
    recycle_ok = !env->ExceptionCheck();
    if (!recycle_ok && std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
      std::cerr << "ART Android MotionEvent recycle exception\n";
      env->ExceptionDescribe();
    }
  }
  if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
    std::cerr << "ART Android MotionEvent v1 action=" << action
              << " consumed=" << (consumed == JNI_TRUE ? 1 : 0)
              << " path=" << (enqueued ? "viewroot" : "decor")
              << " dispatch_us="
              << (dispatch_end > dispatch_start
                      ? (dispatch_end - dispatch_start) / 1000
                      : 0)
              << " owner=" << pthread_self() << "\n";
  }
  if (!dispatch_ok || !recycle_ok) {
    env->ExceptionClear();
    env->DeleteLocalRef(input_host);
    env->DeleteLocalRef(root_class);
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 83;
  }
  if (down) state->pointer_stream_active = true;
  if (terminal) {
    state->pointer_stream_active = false;
    state->pointer_down_time_nanos = 0;
  }
  state->gpu_ripple_overlay_active = action != 3u;
  if (down) {
    state->gpu_ripple_overlay_x = x;
    state->gpu_ripple_overlay_y = y;
    state->gpu_ripple_overlay_started = std::chrono::steady_clock::now();
  }
  if (action != 2u) state->gpu_render_node_recorded = false;
  env->DeleteLocalRef(root_class);
  env->DeleteLocalRef(input_host);
  env->DeleteLocalRef(event);
  env->DeleteLocalRef(motion_event_class);
  // Input mutates Android state only. The owner-thread vsync loop performs
  // the single RenderNode/Metal submission for the next frame.
  return 0;
}

}  // namespace

jobject find_clickable_view_at(JNIEnv* env, jobject view, jfloat x, jfloat y) {
  if (view == nullptr || x < 0.0f || y < 0.0f || env->ExceptionCheck()) {
    return nullptr;
  }
  jclass view_class = env->FindClass("android/view/View");
  jclass group_class = env->FindClass("android/view/ViewGroup");
  if (view_class == nullptr || group_class == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }
  jmethodID get_width = env->GetMethodID(view_class, "getWidth", "()I");
  jmethodID get_height = env->GetMethodID(view_class, "getHeight", "()I");
  jmethodID get_visibility =
      env->GetMethodID(view_class, "getVisibility", "()I");
  jmethodID is_enabled = env->GetMethodID(view_class, "isEnabled", "()Z");
  jmethodID is_clickable = env->GetMethodID(view_class, "isClickable", "()Z");
  if (get_width == nullptr || get_height == nullptr ||
      get_visibility == nullptr || is_enabled == nullptr ||
      is_clickable == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }
  const jint width = env->CallIntMethod(view, get_width);
  const jint height = env->CallIntMethod(view, get_height);
  const jint visibility = env->CallIntMethod(view, get_visibility);
  const jboolean enabled = env->CallBooleanMethod(view, is_enabled);
  if (env->ExceptionCheck() || visibility != 0 || enabled != JNI_TRUE ||
      x >= static_cast<jfloat>(width) || y >= static_cast<jfloat>(height)) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }

  if (env->IsInstanceOf(view, group_class)) {
    jmethodID get_child_count =
        env->GetMethodID(group_class, "getChildCount", "()I");
    jmethodID get_child_at = env->GetMethodID(
        group_class, "getChildAt", "(I)Landroid/view/View;");
    jmethodID get_left = env->GetMethodID(view_class, "getLeft", "()I");
    jmethodID get_top = env->GetMethodID(view_class, "getTop", "()I");
    if (get_child_count != nullptr && get_child_at != nullptr &&
        get_left != nullptr && get_top != nullptr && !env->ExceptionCheck()) {
      const jint child_count = env->CallIntMethod(view, get_child_count);
      for (jint index = child_count - 1; index >= 0 && !env->ExceptionCheck();
           --index) {
        jobject child = env->CallObjectMethod(view, get_child_at, index);
        if (child == nullptr || env->ExceptionCheck()) {
          env->DeleteLocalRef(child);
          continue;
        }
        const jint left = env->CallIntMethod(child, get_left);
        const jint top = env->CallIntMethod(child, get_top);
        jobject result = find_clickable_view_at(env, child, x - left, y - top);
        env->DeleteLocalRef(child);
        if (result != nullptr) {
          env->DeleteLocalRef(group_class);
          env->DeleteLocalRef(view_class);
          return result;
        }
      }
    }
  }
  jobject result =
      env->CallBooleanMethod(view, is_clickable) == JNI_TRUE &&
              !env->ExceptionCheck()
          ? env->NewLocalRef(view)
          : nullptr;
  env->DeleteLocalRef(group_class);
  env->DeleteLocalRef(view_class);
  return result;
}

int32_t dispatch_pointer_internal(GraphicsState* state, uint32_t action, float x,
                                  float y, uint64_t event_time_nanos,
                                  uint64_t down_time_nanos) {
  if (state == nullptr) return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  if (action > 3u || !std::isfinite(x) || !std::isfinite(y)) {
    return 71;
  }
  art::Thread* art_thread = darwin_art_process::owner_thread_for_callback();
  jobject root = state->interactive_root;
  if (art_thread == nullptr || root == nullptr) return 72;
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }

  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
  if (MotionEventBridgeEnabled()) {
    // The host keeps pulsing the retained RenderNode after ACTION_UP so
    // animations can finish. Those replay samples are not Android pointer
    // events: never synthesize a MOVE after the stream has been terminated.
    // Rendering remains on the same owner thread and is still presented once
    // for the current frame.
    if (action == 2u && !state->pointer_stream_active) {
      // Post-terminal animation replays are not Android pointer events. The
      // frame loop owns their presentation, so this path stays side-effect
      // free and never submits a frame per synthetic MOVE.
      return 0;
    }
    const int32_t status = dispatch_motion_event(
        state, env, root, action, x, y, event_time_nanos, down_time_nanos);
    if (status == 0 || std::getenv("DARWIN_ART_INPUT_MODE") != nullptr ||
        std::getenv("DARWIN_ART_APK_APP_PACKAGE") != nullptr) {
      return status;
    }
    std::cerr << "ART Android input: MotionEvent bridge unavailable status=" << status
              << ", using explicit direct fallback\n";
  }
  jobject hit = find_clickable_view_at(env, root, x, y);
  if (std::getenv("DARWIN_ART_DEBUG_POINTER") != nullptr) {
    std::cerr << "ART Android input debug action=" << action << " x=" << x
              << " y=" << y << " hit=" << hit << "\n";
  }
  jclass view_class = env->FindClass("android/view/View");
  jmethodID set_pressed =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "setPressed", "(Z)V");
  jmethodID perform_click =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "performClick", "()Z");
  jmethodID drawable_hotspot_changed =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "drawableHotspotChanged", "(FF)V");
  if (view_class == nullptr || set_pressed == nullptr ||
      perform_click == nullptr || drawable_hotspot_changed == nullptr ||
      env->ExceptionCheck()) {
    env->DeleteLocalRef(hit);
    env->DeleteLocalRef(view_class);
    return 74;
  }

  jobject& pressed_view = state->pressed_view;
  uint32_t& pending_action = state->pending_pressed_action;
  jfloat& pending_x = state->pending_pressed_x;
  jfloat& pending_y = state->pending_pressed_y;
  if (action == 0u) {
    if (std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS") != nullptr &&
        hit == nullptr) {
      env->DeleteLocalRef(view_class);
      env->DeleteLocalRef(hit);
      std::cerr << "ART Android input: expected APK clickable target was not hit\n";
      return 76;
    }
    state->gpu_ripple_overlay_active = true;
    state->gpu_ripple_overlay_x = x;
    state->gpu_ripple_overlay_y = y;
    state->gpu_ripple_overlay_started = std::chrono::steady_clock::now();
    if (pressed_view != nullptr) {
      env->CallVoidMethod(pressed_view, set_pressed, JNI_FALSE);
      env->DeleteGlobalRef(pressed_view);
      pressed_view = nullptr;
    }
    if (hit != nullptr && !env->ExceptionCheck()) {
      pressed_view = env->NewGlobalRef(hit);
      pending_action = 1;
      pending_x = x;
      pending_y = y;
    }
  } else if (action == 2u) {
    if (pressed_view != nullptr && !env->ExceptionCheck()) {
      env->CallVoidMethod(pressed_view, drawable_hotspot_changed, x, y);
    }
  } else if (action == 1u) {
    if (pressed_view != nullptr) {
      pending_action = 2;
      pending_x = x;
      pending_y = y;
    }
  }
  const bool same_pressed_view =
      action == 1u && hit != nullptr && pressed_view != nullptr &&
      env->IsSameObject(hit, pressed_view) == JNI_TRUE;
  if (action == 1u &&
      std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS") != nullptr &&
      !same_pressed_view) {
    env->DeleteLocalRef(hit);
    env->DeleteLocalRef(view_class);
    std::cerr << "ART Android input: APK pointer release missed pressed target\n";
    return 77;
  }
  env->DeleteLocalRef(hit);
  env->DeleteLocalRef(view_class);
  const bool rendered =
      !env->ExceptionCheck() &&
      present_content(state, env, nullptr, root, state->interactive_width,
                      state->interactive_height) == JNI_TRUE;
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android input: click dispatch threw\n"
              << art_thread->GetException()->Dump() << "\n";
    art_thread->ClearException();
  }
  if (action == 1u && pressed_view != nullptr) {
    if (same_pressed_view && perform_click != nullptr &&
        !env->ExceptionCheck()) {
      env->CallBooleanMethod(pressed_view, perform_click);
    }
    env->DeleteGlobalRef(pressed_view);
    pressed_view = nullptr;
  }
  return rendered ? 0 : 75;
}

int32_t dispatch_pointer(GraphicsState* state, uint32_t action, float x,
                         float y) {
  return dispatch_pointer_internal(state, action, x, y, 0, 0);
}

int32_t dispatch_pointer_v2(GraphicsState* state,
                             const DarwinArtPointerEventV2* event) {
  if (event == nullptr || event->version != 2 ||
      event->size < sizeof(DarwinArtPointerEventV2) ||
      event->pointer_count == 0 || event->pointer_count > 16) {
    return 71;
  }
  return dispatch_pointer_internal(state, event->action, event->x, event->y,
                                   event->event_time_nanos,
                                   event->down_time_nanos);
}

int32_t pump_frame(GraphicsState* state, jlong frame_time_nanos) {
  if (state == nullptr) return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  if (frame_time_nanos <= 0) {
    struct timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 74;
    frame_time_nanos = static_cast<jlong>(now.tv_sec) * 1000000000LL +
                       static_cast<jlong>(now.tv_nsec);
  }
  art::Thread* art_thread = darwin_art_process::owner_thread_for_callback();
  if (art_thread == nullptr) return 72;
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }
  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
#if defined(DARWIN_ART_REAL_GRAPHICS)
  auto* animation_context = state->hwui_animation_context.get();
  auto* time_lord = state->hwui_time_lord.get();
  jobject render_node = state->gpu_render_node;
  if (animation_context != nullptr && time_lord != nullptr &&
      render_node != nullptr) {
    jclass render_node_class = env->FindClass("android/graphics/RenderNode");
    jfieldID native_render_node =
        render_node_class == nullptr
            ? nullptr
            : env->GetFieldID(render_node_class, "mNativeRenderNode", "J");
    auto* node = native_render_node == nullptr
                     ? nullptr
                     : reinterpret_cast<android::uirenderer::RenderNode*>(
                           static_cast<std::uintptr_t>(env->GetLongField(
                               render_node, native_render_node)));
    if (node != nullptr && !env->ExceptionCheck()) {
      time_lord->vsyncReceived(frame_time_nanos, frame_time_nanos, 0,
                               frame_time_nanos + 16666666, 16666666);
      animation_context->startFrame(android::uirenderer::TreeInfo::MODE_FULL);
      darwin_art_hwui::animate_node_with_context(node, *animation_context);
    }
    env->ExceptionClear();
    env->DeleteLocalRef(render_node_class);
  }
#endif
  jclass choreographer = env->FindClass("android/view/Choreographer");
  jmethodID get_instance = choreographer == nullptr
                                ? nullptr
                                : env->GetStaticMethodID(
                                      choreographer, "getInstance",
                                      "()Landroid/view/Choreographer;");
  jmethodID do_frame = choreographer == nullptr
                           ? nullptr
                           : env->GetMethodID(
                                 choreographer, "doFrame",
                                 "(JILandroid/view/DisplayEventReceiver$VsyncEventData;)V");
  jclass vsync_data_class =
      env->FindClass("android/view/DisplayEventReceiver$VsyncEventData");
  jmethodID vsync_data_ctor =
      vsync_data_class == nullptr
          ? nullptr
          : env->GetMethodID(vsync_data_class, "<init>", "()V");
  jfieldID frame_interval =
      vsync_data_class == nullptr
          ? nullptr
          : env->GetFieldID(vsync_data_class, "frameInterval", "J");
  jfieldID frame_timelines_length =
      vsync_data_class == nullptr
          ? nullptr
          : env->GetFieldID(vsync_data_class, "frameTimelinesLength", "I");
  jfieldID preferred_frame_timeline =
      vsync_data_class == nullptr
          ? nullptr
          : env->GetFieldID(vsync_data_class, "preferredFrameTimelineIndex", "I");
  jobject instance = get_instance == nullptr || env->ExceptionCheck()
                         ? nullptr
                         : env->CallStaticObjectMethod(choreographer, get_instance);
  jobject vsync_data =
      vsync_data_ctor == nullptr || env->ExceptionCheck()
          ? nullptr
          : env->NewObject(vsync_data_class, vsync_data_ctor);
  if (vsync_data != nullptr && !env->ExceptionCheck()) {
    env->SetLongField(vsync_data, frame_interval, 16666666);
    env->SetIntField(vsync_data, frame_timelines_length, 1);
    env->SetIntField(vsync_data, preferred_frame_timeline, 0);
  }
  if (instance != nullptr && do_frame != nullptr && vsync_data != nullptr &&
      !env->ExceptionCheck()) {
    const jlong monotonic_nanos = static_cast<jlong>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    env->CallVoidMethod(instance, do_frame, monotonic_nanos, 0, vsync_data);
  }
  const bool ok = !env->ExceptionCheck();
  if (!ok) {
    std::cerr << "ART Android frame pulse threw\n"
              << art_thread->GetException()->Dump() << "\n";
    art_thread->ClearException();
  }
  env->DeleteLocalRef(vsync_data);
  env->DeleteLocalRef(vsync_data_class);
  env->DeleteLocalRef(instance);
  env->DeleteLocalRef(choreographer);
  return ok && do_frame != nullptr ? 0 : 75;
}

}  // namespace darwin_art_graphics

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_dispatch_pointer(
    uint32_t, float, float) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_dispatch_pointer_v2(
    const DarwinArtPointerEventV2*) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_pump_framework_frame(
    jlong) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
}
