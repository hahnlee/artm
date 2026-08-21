#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_dispatch_pointer(
    uint32_t action, float x, float y) {
  if (action > 2u || !std::isfinite(x) || !std::isfinite(y)) {
    return 71;
  }
  art::Thread* art_thread = darwin_art_process::owner_thread_for_callback();
  jobject root = interactive_root_for_input();
  if (art_thread == nullptr || root == nullptr) return 72;
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }

  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
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

  jobject& pressed_view = pressed_view_for_input();
  uint32_t& pending_action = pending_pressed_action_for_input();
  jfloat& pending_x = pending_pressed_x_for_input();
  jfloat& pending_y = pending_pressed_y_for_input();
  if (action == 0u) {
    gpu_ripple_overlay_active_for_input() = true;
    gpu_ripple_overlay_x_for_input() = x;
    gpu_ripple_overlay_y_for_input() = y;
    gpu_ripple_overlay_started_for_input() = std::chrono::steady_clock::now();
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
  env->DeleteLocalRef(hit);
  env->DeleteLocalRef(view_class);
  const bool rendered =
      !env->ExceptionCheck() &&
      present_content(env, nullptr, root, interactive_width_for_input(),
                      interactive_height_for_input()) == JNI_TRUE;
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

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_pump_framework_frame(
    jlong frame_time_nanos) {
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
  auto* animation_context = hwui_animation_context_for_input();
  auto* time_lord = hwui_time_lord_for_input();
  jobject render_node = gpu_render_node_for_input();
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
