#include <cmath>
#include <cstdlib>
#include <iostream>

#include "base/locks.h"
#include "darwin_art/darwin_art.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_probe_internal.h"
#include "runtime_process_state.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

namespace darwin_art_graphics {

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

}  // namespace darwin_art_graphics
