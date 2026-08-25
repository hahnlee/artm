#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <time.h>

#include "base/locks.h"
#include "darwin_framework_natives.h"
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

bool g_motion_event_archive_probe_done = false;

int64_t MonotonicNanos() {
  struct timespec now = {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

// Process only messages that Android's main Looper could dispatch without
// blocking. The standalone host owns the ART/UI thread, so calling
// Looper.loop() would take over that thread permanently. Peeking at the queue
// first preserves MessageQueue.next() semantics (including sync barriers) and
// lets the host interleave due Handler work with native events and Metal
// frames just like one ViewRoot traversal.
bool DispatchDueMainMessages(JNIEnv* env, size_t limit = 128) {
  jclass looper_class = env->FindClass("android/os/Looper");
  jclass queue_class = env->FindClass("android/os/MessageQueue");
  jclass message_class = env->FindClass("android/os/Message");
  jclass handler_class = env->FindClass("android/os/Handler");
  jclass clock_class = env->FindClass("android/os/SystemClock");
  jmethodID my_queue = looper_class == nullptr
                           ? nullptr
                           : env->GetStaticMethodID(
                                 looper_class, "myQueue",
                                 "()Landroid/os/MessageQueue;");
  jmethodID queue_next = queue_class == nullptr
                             ? nullptr
                             : env->GetMethodID(queue_class, "next",
                                                "()Landroid/os/Message;");
  jfieldID queue_messages =
      queue_class == nullptr
          ? nullptr
          : env->GetFieldID(queue_class, "mMessages", "Landroid/os/Message;");
  jfieldID message_when = message_class == nullptr
                              ? nullptr
                              : env->GetFieldID(message_class, "when", "J");
  jfieldID message_next =
      message_class == nullptr
          ? nullptr
          : env->GetFieldID(message_class, "next", "Landroid/os/Message;");
  jfieldID message_target =
      message_class == nullptr
          ? nullptr
          : env->GetFieldID(message_class, "target", "Landroid/os/Handler;");
  jmethodID is_asynchronous =
      message_class == nullptr
          ? nullptr
          : env->GetMethodID(message_class, "isAsynchronous", "()Z");
  jmethodID recycle = message_class == nullptr
                          ? nullptr
                          : env->GetMethodID(message_class, "recycleUnchecked",
                                             "()V");
  jmethodID dispatch =
      handler_class == nullptr
          ? nullptr
          : env->GetMethodID(handler_class, "dispatchMessage",
                             "(Landroid/os/Message;)V");
  jmethodID uptime_millis =
      clock_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(clock_class, "uptimeMillis", "()J");
  const bool resolved = my_queue != nullptr && queue_next != nullptr &&
                        queue_messages != nullptr && message_when != nullptr &&
                        message_next != nullptr && message_target != nullptr &&
                        is_asynchronous != nullptr && recycle != nullptr &&
                        dispatch != nullptr && uptime_millis != nullptr &&
                        !env->ExceptionCheck();
  jobject queue = resolved
                      ? env->CallStaticObjectMethod(looper_class, my_queue)
                      : nullptr;
  bool ok = resolved && queue != nullptr && !env->ExceptionCheck();
  for (size_t dispatched = 0; ok && dispatched < limit; ++dispatched) {
    jobject head = env->GetObjectField(queue, queue_messages);
    if (head == nullptr || env->ExceptionCheck()) {
      if (head != nullptr) env->DeleteLocalRef(head);
      break;
    }
    const jlong now = env->CallStaticLongMethod(clock_class, uptime_millis);
    jobject target = env->GetObjectField(head, message_target);
    bool selectable = target != nullptr &&
                      env->GetLongField(head, message_when) <= now;
    if (target != nullptr) env->DeleteLocalRef(target);
    if (!selectable) {
      // A null target is a synchronization barrier. MessageQueue.next() may
      // pass it only when a due asynchronous message exists behind it.
      jobject candidate = env->GetObjectField(head, message_next);
      while (candidate != nullptr && !env->ExceptionCheck()) {
        jobject candidate_target = env->GetObjectField(candidate, message_target);
        const bool due_async = candidate_target != nullptr &&
                               env->CallBooleanMethod(candidate, is_asynchronous) ==
                                   JNI_TRUE &&
                               env->GetLongField(candidate, message_when) <= now;
        if (candidate_target != nullptr) env->DeleteLocalRef(candidate_target);
        if (due_async) {
          selectable = true;
          env->DeleteLocalRef(candidate);
          break;
        }
        jobject next = env->GetObjectField(candidate, message_next);
        env->DeleteLocalRef(candidate);
        candidate = next;
      }
    }
    env->DeleteLocalRef(head);
    if (!selectable || env->ExceptionCheck()) break;

    jobject message = env->CallObjectMethod(queue, queue_next);
    if (message == nullptr || env->ExceptionCheck()) {
      if (message != nullptr) env->DeleteLocalRef(message);
      ok = false;
      break;
    }
    jobject dispatch_target = env->GetObjectField(message, message_target);
    if (dispatch_target == nullptr || env->ExceptionCheck()) {
      if (dispatch_target != nullptr) env->DeleteLocalRef(dispatch_target);
      env->DeleteLocalRef(message);
      ok = false;
      break;
    }
    env->CallVoidMethod(dispatch_target, dispatch, message);
    if (!env->ExceptionCheck()) env->CallVoidMethod(message, recycle);
    env->DeleteLocalRef(dispatch_target);
    env->DeleteLocalRef(message);
    ok = !env->ExceptionCheck();
  }
  if (queue != nullptr) env->DeleteLocalRef(queue);
  if (clock_class != nullptr) env->DeleteLocalRef(clock_class);
  if (handler_class != nullptr) env->DeleteLocalRef(handler_class);
  if (message_class != nullptr) env->DeleteLocalRef(message_class);
  if (queue_class != nullptr) env->DeleteLocalRef(queue_class);
  if (looper_class != nullptr) env->DeleteLocalRef(looper_class);
  return ok;
}

// Native compositor callbacks do not have a Java caller frame, so FindClass
// only sees the boot class loader. Resolve APK/runtime helper classes through
// the process thread's context loader instead.
jclass LoadContextClass(JNIEnv* env, const char* binary_name) {
  if (env == nullptr || binary_name == nullptr) return nullptr;
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
                             : env->GetMethodID(thread_class,
                                                "getContextClassLoader",
                                                "()Ljava/lang/ClassLoader;");
  jobject loader = get_loader == nullptr
                       ? nullptr
                       : env->CallObjectMethod(thread, get_loader);
  jclass loader_class = loader == nullptr ? nullptr : env->GetObjectClass(loader);
  jmethodID load_class = loader_class == nullptr
                             ? nullptr
                             : env->GetMethodID(loader_class, "loadClass",
                                                "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring name = env->NewStringUTF(binary_name);
  jclass loaded = load_class == nullptr
                      ? nullptr
                      : static_cast<jclass>(
                            env->CallObjectMethod(loader, load_class, name));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    loaded = nullptr;
  }
  if (name != nullptr) env->DeleteLocalRef(name);
  if (loader_class != nullptr) env->DeleteLocalRef(loader_class);
  if (loader != nullptr) env->DeleteLocalRef(loader);
  if (thread != nullptr) env->DeleteLocalRef(thread);
  if (thread_class != nullptr) env->DeleteLocalRef(thread_class);
  return loaded;
}

void ActivateCurrentHostSurfaces(GraphicsState* state, JNIEnv* env) {
  static bool debug_reported = false;
  jclass bridge = state == nullptr || state->service_bridge_class == nullptr
                      ? nullptr
                      : static_cast<jclass>(
                            env->NewLocalRef(state->service_bridge_class));
  jmethodID activate =
      bridge == nullptr
          ? nullptr
          : env->GetStaticMethodID(bridge, "activateCurrentHostSurfaces",
                                   "()V");
  if (activate != nullptr && !env->ExceptionCheck()) {
    env->CallStaticVoidMethod(bridge, activate);
  }
  if (!debug_reported && std::getenv("DARWIN_ART_DEBUG_POINTER") != nullptr) {
    std::cerr << "ART Android host surface activation bridge="
              << (bridge != nullptr) << " method=" << (activate != nullptr)
              << " exception=" << env->ExceptionCheck() << "\n";
    if (env->ExceptionCheck()) env->ExceptionDescribe();
    debug_reported = true;
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (bridge != nullptr) env->DeleteLocalRef(bridge);
}

void DebugWindowManagerViews(JNIEnv* env) {
  static bool dumped = false;
  if (dumped || std::getenv("DARWIN_ART_DEBUG_WINDOW_LAYERS") == nullptr) return;
  jclass global_class = env->FindClass("android/view/WindowManagerGlobal");
  jmethodID get_instance =
      global_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(global_class, "getInstance",
                                   "()Landroid/view/WindowManagerGlobal;");
  jobject global = get_instance == nullptr
                       ? nullptr
                       : env->CallStaticObjectMethod(global_class, get_instance);
  jfieldID views_field =
      global_class == nullptr
          ? nullptr
          : env->GetFieldID(global_class, "mViews", "Ljava/util/ArrayList;");
  jfieldID params_field =
      global_class == nullptr
          ? nullptr
          : env->GetFieldID(global_class, "mParams", "Ljava/util/ArrayList;");
  jobject views = global == nullptr || views_field == nullptr
                      ? nullptr
                      : env->GetObjectField(global, views_field);
  jobject params = global == nullptr || params_field == nullptr
                       ? nullptr
                       : env->GetObjectField(global, params_field);
  jclass list_class = env->FindClass("java/util/ArrayList");
  jmethodID size = list_class == nullptr
                       ? nullptr
                       : env->GetMethodID(list_class, "size", "()I");
  jmethodID get = list_class == nullptr
                      ? nullptr
                      : env->GetMethodID(list_class, "get", "(I)Ljava/lang/Object;");
  jclass view_class = env->FindClass("android/view/View");
  jmethodID get_width = view_class == nullptr
                            ? nullptr
                            : env->GetMethodID(view_class, "getWidth", "()I");
  jmethodID get_height = view_class == nullptr
                             ? nullptr
                             : env->GetMethodID(view_class, "getHeight", "()I");
  jmethodID get_measured_width =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "getMeasuredWidth", "()I");
  jmethodID get_measured_height =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "getMeasuredHeight", "()I");
  jclass params_class = env->FindClass("android/view/WindowManager$LayoutParams");
  jfieldID x_field = params_class == nullptr
                         ? nullptr
                         : env->GetFieldID(params_class, "x", "I");
  jfieldID y_field = params_class == nullptr
                         ? nullptr
                         : env->GetFieldID(params_class, "y", "I");
  jfieldID type_field = params_class == nullptr
                            ? nullptr
                            : env->GetFieldID(params_class, "type", "I");
  jfieldID gravity_field = params_class == nullptr
                               ? nullptr
                               : env->GetFieldID(params_class, "gravity", "I");
  jfieldID width_field = params_class == nullptr
                             ? nullptr
                             : env->GetFieldID(params_class, "width", "I");
  jfieldID height_field = params_class == nullptr
                              ? nullptr
                              : env->GetFieldID(params_class, "height", "I");
  if (!env->ExceptionCheck() && views != nullptr && params != nullptr &&
      size != nullptr && get != nullptr) {
    const jint count = env->CallIntMethod(views, size);
    if (count > 0 && !env->ExceptionCheck()) {
      bool any_sized = false;
      std::cerr << "ART Android windows: count=" << count << "\n";
      for (jint index = 0; index < count; ++index) {
        jobject window_view = env->CallObjectMethod(views, get, index);
        jobject window_params = env->CallObjectMethod(params, get, index);
        if (window_view != nullptr && window_params != nullptr &&
            !env->ExceptionCheck()) {
          const jint window_width = env->CallIntMethod(window_view, get_width);
          const jint window_height = env->CallIntMethod(window_view, get_height);
          any_sized = any_sized || (window_width > 0 && window_height > 0);
          std::cerr << "ART Android window[" << index << "] view=" << window_view
                    << " size=" << window_width
                    << "x" << window_height
                    << " measured="
                    << env->CallIntMethod(window_view, get_measured_width) << "x"
                    << env->CallIntMethod(window_view, get_measured_height)
                    << " pos=" << env->GetIntField(window_params, x_field) << ","
                    << env->GetIntField(window_params, y_field)
                    << " layout=" << env->GetIntField(window_params, width_field)
                    << "x" << env->GetIntField(window_params, height_field)
                    << " gravity="
                    << env->GetIntField(window_params, gravity_field) << "\n";
        }
        if (window_params != nullptr) env->DeleteLocalRef(window_params);
        if (window_view != nullptr) env->DeleteLocalRef(window_view);
      }
      dumped = any_sized;
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (params_class != nullptr) env->DeleteLocalRef(params_class);
  if (view_class != nullptr) env->DeleteLocalRef(view_class);
  if (list_class != nullptr) env->DeleteLocalRef(list_class);
  if (params != nullptr) env->DeleteLocalRef(params);
  if (views != nullptr) env->DeleteLocalRef(views);
  if (global != nullptr) env->DeleteLocalRef(global);
  if (global_class != nullptr) env->DeleteLocalRef(global_class);
}

bool sync_interactive_surface_size(GraphicsState* state, JNIEnv* env) {
  if (state == nullptr || env == nullptr || state->gpu_surface == nullptr ||
      state->interactive_root == nullptr) {
    return true;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (!darwin_art_surface_get_size(state->gpu_surface, &width, &height) ||
      width == 0 || height == 0) {
    return false;
  }
  if (state->interactive_width == static_cast<jint>(width) &&
      state->interactive_height == static_cast<jint>(height)) {
    return true;
  }
  if (std::getenv("DARWIN_ART_DEBUG_RESIZE") != nullptr) {
    std::cerr << "ART Android resize: " << state->interactive_width << "x"
              << state->interactive_height << " -> " << width << "x"
              << height << "\n";
  }
  state->gpu_render_node_recorded = false;
  const jboolean rendered = present_content(
      state, env, nullptr, state->interactive_root, static_cast<jint>(width),
      static_cast<jint>(height));
  if (rendered == JNI_TRUE) {
    state->interactive_width = static_cast<jint>(width);
    state->interactive_height = static_cast<jint>(height);
    return true;
  }
  return false;
}

jclass LoadProbeAnimationHost(JNIEnv* env) {
  return LoadContextClass(env, "dev.darwinart.probe.ProbeAnimationHost");
}

void ClearPointerDispatchRoot(GraphicsState* state, JNIEnv* env) {
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
}

bool SelectPointerDispatchRoot(GraphicsState* state, JNIEnv* env,
                               jobject main_root, jfloat x, jfloat y) {
  ClearPointerDispatchRoot(state, env);
  jobject selected = nullptr;
  jobject selected_view_root = nullptr;
  jint selected_x = 0;
  jint selected_y = 0;
  jclass global_class = env->FindClass("android/view/WindowManagerGlobal");
  jmethodID get_instance =
      global_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(global_class, "getInstance",
                                   "()Landroid/view/WindowManagerGlobal;");
  jobject global = get_instance == nullptr
                       ? nullptr
                       : env->CallStaticObjectMethod(global_class, get_instance);
  jfieldID views_field =
      global_class == nullptr
          ? nullptr
          : env->GetFieldID(global_class, "mViews", "Ljava/util/ArrayList;");
  jfieldID params_field =
      global_class == nullptr
          ? nullptr
          : env->GetFieldID(global_class, "mParams", "Ljava/util/ArrayList;");
  jfieldID roots_field =
      global_class == nullptr
          ? nullptr
          : env->GetFieldID(global_class, "mRoots", "Ljava/util/ArrayList;");
  jobject views = global == nullptr || views_field == nullptr
                      ? nullptr
                      : env->GetObjectField(global, views_field);
  jobject params = global == nullptr || params_field == nullptr
                       ? nullptr
                       : env->GetObjectField(global, params_field);
  jobject roots = global == nullptr || roots_field == nullptr
                      ? nullptr
                      : env->GetObjectField(global, roots_field);
  jclass list_class = env->FindClass("java/util/ArrayList");
  jmethodID size = list_class == nullptr
                       ? nullptr
                       : env->GetMethodID(list_class, "size", "()I");
  jmethodID get = list_class == nullptr
                      ? nullptr
                      : env->GetMethodID(list_class, "get", "(I)Ljava/lang/Object;");
  jclass view_class = env->FindClass("android/view/View");
  jmethodID get_width = view_class == nullptr
                            ? nullptr
                            : env->GetMethodID(view_class, "getWidth", "()I");
  jmethodID get_height = view_class == nullptr
                             ? nullptr
                             : env->GetMethodID(view_class, "getHeight", "()I");
  jclass params_class = env->FindClass("android/view/WindowManager$LayoutParams");
  jfieldID x_field = params_class == nullptr
                         ? nullptr
                         : env->GetFieldID(params_class, "x", "I");
  jfieldID y_field = params_class == nullptr
                         ? nullptr
                         : env->GetFieldID(params_class, "y", "I");
  jfieldID type_field = params_class == nullptr
                            ? nullptr
                            : env->GetFieldID(params_class, "type", "I");
  jfieldID token_field =
      params_class == nullptr
          ? nullptr
          : env->GetFieldID(params_class, "token", "Landroid/os/IBinder;");
  jobject current_activity_token = nullptr;
  if (!env->ExceptionCheck() && views != nullptr && params != nullptr &&
      roots != nullptr &&
      size != nullptr && get != nullptr && get_width != nullptr &&
      get_height != nullptr && x_field != nullptr && y_field != nullptr &&
      type_field != nullptr && token_field != nullptr) {
    const jint count = env->CallIntMethod(views, size);
    for (jint index = 0;
         index < count && current_activity_token == nullptr; ++index) {
      jobject layer = env->CallObjectMethod(views, get, index);
      jobject layout_params = env->CallObjectMethod(params, get, index);
      if (layer != nullptr && layout_params != nullptr &&
          env->IsSameObject(layer, main_root) == JNI_TRUE) {
        current_activity_token =
            env->GetObjectField(layout_params, token_field);
      }
      if (layout_params != nullptr) env->DeleteLocalRef(layout_params);
      if (layer != nullptr) env->DeleteLocalRef(layer);
    }
    for (jint index = count - 1; index >= 0 && selected == nullptr; --index) {
      jobject layer = env->CallObjectMethod(views, get, index);
      jobject layout_params = env->CallObjectMethod(params, get, index);
      if (layer != nullptr && layout_params != nullptr && !env->ExceptionCheck()) {
        const jint left = env->GetIntField(layout_params, x_field);
        const jint top = env->GetIntField(layout_params, y_field);
        const jint type = env->GetIntField(layout_params, type_field);
        const jint width = env->CallIntMethod(layer, get_width);
        const jint height = env->CallIntMethod(layer, get_height);
        const bool current_activity =
            env->IsSameObject(layer, main_root) == JNI_TRUE;
        jobject layer_token = env->GetObjectField(layout_params, token_field);
        const bool current_activity_token_owner =
            current_activity_token != nullptr && layer_token != nullptr &&
            env->IsSameObject(layer_token, current_activity_token) == JNI_TRUE;
        // WindowManagerGlobal retains the outgoing Activity root until its
        // teardown traversal completes. Android InputDispatcher targets the
        // focused current application token, not whichever stale base
        // application entry happens to be last in mViews. Dialog windows may
        // also use TYPE_APPLICATION, so retain every layer owned by the
        // foreground Activity token in addition to ordinary sub-windows.
        const bool eligible = current_activity || current_activity_token_owner ||
                              type >= 1000;
        if (std::getenv("DARWIN_ART_DEBUG_POINTER") != nullptr) {
          std::cerr << "ART Android input window index=" << index
                    << " type=" << type << " current=" << current_activity
                    << " token_owner=" << current_activity_token_owner
                    << " bounds=" << left << "," << top << "-"
                    << (left + width) << "," << (top + height)
                    << " eligible=" << eligible << "\n";
        }
        if (eligible && x >= left && y >= top && x < left + width &&
            y < top + height) {
          selected = env->NewLocalRef(layer);
          selected_view_root = env->CallObjectMethod(roots, get, index);
          selected_x = left;
          selected_y = top;
        }
        if (layer_token != nullptr) env->DeleteLocalRef(layer_token);
      }
      if (layout_params != nullptr) env->DeleteLocalRef(layout_params);
      if (layer != nullptr) env->DeleteLocalRef(layer);
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (current_activity_token != nullptr)
    env->DeleteLocalRef(current_activity_token);
  if (params_class != nullptr) env->DeleteLocalRef(params_class);
  if (view_class != nullptr) env->DeleteLocalRef(view_class);
  if (list_class != nullptr) env->DeleteLocalRef(list_class);
  if (params != nullptr) env->DeleteLocalRef(params);
  if (roots != nullptr) env->DeleteLocalRef(roots);
  if (views != nullptr) env->DeleteLocalRef(views);
  if (global != nullptr) env->DeleteLocalRef(global);
  if (global_class != nullptr) env->DeleteLocalRef(global_class);
  jobject dispatch_root = selected == nullptr ? main_root : selected;
  state->pointer_dispatch_root = env->NewGlobalRef(dispatch_root);
  jobject dispatch_view_root = selected_view_root == nullptr
                                   ? state->interactive_view_root
                                   : selected_view_root;
  if (dispatch_view_root != nullptr) {
    state->pointer_dispatch_view_root = env->NewGlobalRef(dispatch_view_root);
  }
  state->pointer_dispatch_offset_x = static_cast<jfloat>(selected_x);
  state->pointer_dispatch_offset_y = static_cast<jfloat>(selected_y);
  state->pointer_dispatch_is_window = selected != nullptr;
  if (selected != nullptr) env->DeleteLocalRef(selected);
  if (selected_view_root != nullptr) env->DeleteLocalRef(selected_view_root);
  return state->pointer_dispatch_root != nullptr && !env->ExceptionCheck();
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

  if (down && !SelectPointerDispatchRoot(state, env, root, x, y)) {
    env->ExceptionClear();
    return 82;
  }

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
  jobject dispatch_root = state->pointer_dispatch_root == nullptr
                              ? root
                              : state->pointer_dispatch_root;
  const jfloat local_x = x - state->pointer_dispatch_offset_x;
  const jfloat local_y = y - state->pointer_dispatch_offset_y;
  if (down && std::getenv("DARWIN_ART_DEBUG_POINTER") != nullptr) {
    jobject debug_hit =
        find_clickable_view_at(env, dispatch_root, local_x, local_y);
    std::cerr << "ART Android MotionEvent target x=" << x << " y=" << y
              << " local_x=" << local_x << " local_y=" << local_y
              << " offset_x=" << state->pointer_dispatch_offset_x
              << " offset_y=" << state->pointer_dispatch_offset_y
              << " hit=" << debug_hit;
    if (debug_hit != nullptr) {
      jclass object_class = env->FindClass("java/lang/Object");
      jmethodID to_string =
          object_class == nullptr
              ? nullptr
              : env->GetMethodID(object_class, "toString", "()Ljava/lang/String;");
      jstring description =
          to_string == nullptr
              ? nullptr
              : static_cast<jstring>(env->CallObjectMethod(debug_hit, to_string));
      if (description != nullptr && !env->ExceptionCheck()) {
        const char* utf = env->GetStringUTFChars(description, nullptr);
        if (utf != nullptr) {
          std::cerr << " view=" << utf;
          env->ReleaseStringUTFChars(description, utf);
        }
      }
      if (description != nullptr) env->DeleteLocalRef(description);
      if (object_class != nullptr) env->DeleteLocalRef(object_class);
    }
    std::cerr << "\n";
    if (debug_hit != nullptr) env->DeleteLocalRef(debug_hit);
    jclass debug_view_class = env->FindClass("android/view/View");
    jmethodID debug_find_by_id =
        debug_view_class == nullptr
            ? nullptr
            : env->GetMethodID(debug_view_class, "findViewById",
                               "(I)Landroid/view/View;");
    jobject debug_overlay =
        debug_find_by_id == nullptr
            ? nullptr
            : env->CallObjectMethod(dispatch_root, debug_find_by_id,
                                    static_cast<jint>(0x7f0901a3));
    if (debug_overlay != nullptr && !env->ExceptionCheck()) {
      jmethodID get_location = env->GetMethodID(
          debug_view_class, "getLocationInWindow", "([I)V");
      jmethodID get_width =
          env->GetMethodID(debug_view_class, "getWidth", "()I");
      jmethodID get_height =
          env->GetMethodID(debug_view_class, "getHeight", "()I");
      jmethodID is_enabled =
          env->GetMethodID(debug_view_class, "isEnabled", "()Z");
      jintArray location = env->NewIntArray(2);
      if (get_location != nullptr && get_width != nullptr &&
          get_height != nullptr && is_enabled != nullptr &&
          location != nullptr && !env->ExceptionCheck()) {
        env->CallVoidMethod(debug_overlay, get_location, location);
        jint coordinates[2] = {};
        env->GetIntArrayRegion(location, 0, 2, coordinates);
        std::cerr << "ART Android debug parameter_overlay bounds="
                  << coordinates[0] << "," << coordinates[1] << "-"
                  << coordinates[0] +
                         env->CallIntMethod(debug_overlay, get_width)
                  << ","
                  << coordinates[1] +
                         env->CallIntMethod(debug_overlay, get_height)
                  << " enabled="
                  << (env->CallBooleanMethod(debug_overlay, is_enabled) ==
                      JNI_TRUE)
                  << " attached=";
        jmethodID is_attached = env->GetMethodID(
            debug_view_class, "isAttachedToWindow", "()Z");
        std::cerr << (is_attached != nullptr &&
                      env->CallBooleanMethod(debug_overlay, is_attached) ==
                          JNI_TRUE);
        jclass overlay_class = env->GetObjectClass(debug_overlay);
        jfieldID dispatcher_field =
            overlay_class == nullptr
                ? nullptr
                : env->GetFieldID(overlay_class, "f", "Lccb;");
        jobject dispatcher =
            dispatcher_field == nullptr
                ? nullptr
                : env->GetObjectField(debug_overlay, dispatcher_field);
        jclass dispatcher_class =
            dispatcher == nullptr ? nullptr : env->GetObjectClass(dispatcher);
        jfieldID handlers_field =
            dispatcher_class == nullptr
                ? nullptr
                : env->GetFieldID(dispatcher_class, "h", "Ljava/util/List;");
        jobject handlers =
            handlers_field == nullptr
                ? nullptr
                : env->GetObjectField(dispatcher, handlers_field);
        jclass set_class = env->FindClass("java/util/List");
        jmethodID set_size =
            set_class == nullptr
                ? nullptr
                : env->GetMethodID(set_class, "size", "()I");
        std::cerr << " handlers="
                  << (handlers == nullptr || set_size == nullptr
                          ? -1
                          : env->CallIntMethod(handlers, set_size))
                  << " target=";
        jfieldID target_field =
            overlay_class == nullptr
                ? nullptr
                : env->GetFieldID(overlay_class, "h", "Landroid/view/View;");
        jobject target =
            target_field == nullptr
                ? nullptr
                : env->GetObjectField(debug_overlay, target_field);
        std::cerr << target;
        if (target != nullptr && !env->ExceptionCheck()) {
          jmethodID target_width =
              env->GetMethodID(debug_view_class, "getWidth", "()I");
          jmethodID target_height =
              env->GetMethodID(debug_view_class, "getHeight", "()I");
          std::cerr << " target_size="
                    << env->CallIntMethod(target, target_width) << "x"
                    << env->CallIntMethod(target, target_height);
          jclass surface_view_class = env->FindClass("android/view/SurfaceView");
          if (surface_view_class != nullptr &&
              env->IsInstanceOf(target, surface_view_class)) {
            jmethodID get_holder = env->GetMethodID(
                surface_view_class, "getHolder",
                "()Landroid/view/SurfaceHolder;");
            jobject holder = get_holder == nullptr
                                 ? nullptr
                                 : env->CallObjectMethod(target, get_holder);
            jclass holder_class = env->FindClass("android/view/SurfaceHolder");
            jmethodID get_surface =
                holder_class == nullptr
                    ? nullptr
                    : env->GetMethodID(holder_class, "getSurface",
                                       "()Landroid/view/Surface;");
            jobject surface =
                holder == nullptr || get_surface == nullptr
                    ? nullptr
                    : env->CallObjectMethod(holder, get_surface);
            jclass surface_class = env->FindClass("android/view/Surface");
            jmethodID is_valid =
                surface_class == nullptr
                    ? nullptr
                    : env->GetMethodID(surface_class, "isValid", "()Z");
            std::cerr << " surface_valid="
                      << (surface != nullptr && is_valid != nullptr &&
                          env->CallBooleanMethod(surface, is_valid) == JNI_TRUE);
            if (surface_class != nullptr)
              env->DeleteLocalRef(surface_class);
            if (surface != nullptr) env->DeleteLocalRef(surface);
            if (holder_class != nullptr) env->DeleteLocalRef(holder_class);
            if (holder != nullptr) env->DeleteLocalRef(holder);
          }
          if (surface_view_class != nullptr)
            env->DeleteLocalRef(surface_view_class);
        }
        std::cerr << "\n";
        if (target != nullptr) env->DeleteLocalRef(target);
        if (set_class != nullptr) env->DeleteLocalRef(set_class);
        if (handlers != nullptr) env->DeleteLocalRef(handlers);
        if (dispatcher_class != nullptr)
          env->DeleteLocalRef(dispatcher_class);
        if (dispatcher != nullptr) env->DeleteLocalRef(dispatcher);
        if (overlay_class != nullptr) env->DeleteLocalRef(overlay_class);
      }
      if (location != nullptr) env->DeleteLocalRef(location);
    }
    if (debug_overlay != nullptr) env->DeleteLocalRef(debug_overlay);
    if (debug_view_class != nullptr) env->DeleteLocalRef(debug_view_class);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (down) {
    state->pointer_down_x = x;
    state->pointer_down_y = y;
    state->pointer_click_candidate = true;
    state->pointer_touch_slop = 8;
    jclass view_class = env->FindClass("android/view/View");
    jmethodID get_context =
        view_class == nullptr
            ? nullptr
            : env->GetMethodID(view_class, "getContext",
                               "()Landroid/content/Context;");
    jobject context = get_context == nullptr
                          ? nullptr
                          : env->CallObjectMethod(dispatch_root, get_context);
    jclass config_class = env->FindClass("android/view/ViewConfiguration");
    jmethodID get_config =
        config_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(
                  config_class, "get",
                  "(Landroid/content/Context;)Landroid/view/ViewConfiguration;");
    jobject config = context == nullptr || get_config == nullptr
                         ? nullptr
                         : env->CallStaticObjectMethod(config_class, get_config, context);
    jmethodID get_touch_slop =
        config_class == nullptr
            ? nullptr
            : env->GetMethodID(config_class, "getScaledTouchSlop", "()I");
    if (config != nullptr && get_touch_slop != nullptr &&
        !env->ExceptionCheck()) {
      state->pointer_touch_slop =
          std::max<jint>(1, env->CallIntMethod(config, get_touch_slop));
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (config != nullptr) env->DeleteLocalRef(config);
    if (config_class != nullptr) env->DeleteLocalRef(config_class);
    if (context != nullptr) env->DeleteLocalRef(context);
    if (view_class != nullptr) env->DeleteLocalRef(view_class);
  } else if (action == 2u && state->pointer_click_candidate) {
    const float dx = x - state->pointer_down_x;
    const float dy = y - state->pointer_down_y;
    const float slop = static_cast<float>(state->pointer_touch_slop);
    if (dx * dx + dy * dy > slop * slop) {
      state->pointer_click_candidate = false;
    }
  }
  jmethodID offset_location =
      env->GetMethodID(motion_event_class, "offsetLocation", "(FF)V");
  if (offset_location == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    ClearPointerDispatchRoot(state, env);
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 82;
  }
  if (state->pointer_dispatch_offset_x != 0.0f ||
      state->pointer_dispatch_offset_y != 0.0f) {
    env->CallVoidMethod(event, offset_location,
                        -state->pointer_dispatch_offset_x,
                        -state->pointer_dispatch_offset_y);
  }
  jclass root_class = env->GetObjectClass(dispatch_root);
  jmethodID recycle = env->GetMethodID(motion_event_class, "recycle", "()V");
  if (recycle == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    ClearPointerDispatchRoot(state, env);
    env->DeleteLocalRef(root_class);
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 82;
  }
  const bool has_view_root = state->pointer_dispatch_view_root != nullptr;
  // Legacy probes can still run without a WindowManager attachment. Keep
  // their bounded click shim isolated from the APK path; a real ViewRoot owns
  // click cancellation, touch slop, and PerformClick scheduling itself.
  if (!has_view_root && down) {
    if (state->pressed_view != nullptr) {
      env->DeleteGlobalRef(state->pressed_view);
      state->pressed_view = nullptr;
    }
    jobject hit = find_clickable_view_at(env, dispatch_root, local_x, local_y);
    if (hit != nullptr && !env->ExceptionCheck()) {
      state->pressed_view = env->NewGlobalRef(hit);
    }
    if (hit != nullptr) env->DeleteLocalRef(hit);
  }
  const int64_t dispatch_start = MonotonicNanos();
  jboolean consumed = JNI_FALSE;
  bool enqueued = false;
  jclass input_host = LoadProbeAnimationHost(env);
  if (input_host != nullptr && !g_motion_event_archive_probe_done &&
      std::getenv("DARWIN_ART_DEBUG_MOTION_EVENT_ARCHIVE") != nullptr) {
    jmethodID archive_probe =
        env->GetStaticMethodID(input_host, "motionEventArchiveProbe", "()I");
    if (archive_probe != nullptr && !env->ExceptionCheck()) {
      const jint probe_status =
          env->CallStaticIntMethod(input_host, archive_probe);
      if (!env->ExceptionCheck()) {
        std::cerr << "ART Android MotionEvent archive probe status="
                  << probe_status << " history=1 copy=1\n";
        g_motion_event_archive_probe_done = true;
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  jclass view_root_class =
      has_view_root ? env->GetObjectClass(state->pointer_dispatch_view_root)
                    : nullptr;
  // Product APKs enter through the InputChannel-bound
  // WindowInputEventReceiver created by ViewRootImpl.setView(). This mirrors
  // Android's native input transport boundary and keeps ViewRoot's input
  // stages, finish acknowledgements, touch mode, and gesture ownership intact.
  if (has_view_root) {
    bool framework_handled = false;
    enqueued = darwin_art::DispatchFrameworkInputEvent(
        env, state->pointer_dispatch_view_root, event, &framework_handled);
    consumed = framework_handled ? JNI_TRUE : JNI_FALSE;
  }
  if (env->ExceptionCheck()) {
    if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
      std::cerr << "ART Android MotionEvent: InputChannel delivery failed\n";
      jthrowable error = env->ExceptionOccurred();
      env->ExceptionClear();
      jclass throwable_class = env->FindClass("java/lang/Throwable");
      jmethodID to_string =
          throwable_class == nullptr
              ? nullptr
              : env->GetMethodID(throwable_class, "toString",
                                 "()Ljava/lang/String;");
      jstring description =
          error == nullptr || to_string == nullptr
              ? nullptr
              : static_cast<jstring>(env->CallObjectMethod(error, to_string));
      if (description != nullptr && !env->ExceptionCheck()) {
        const char* utf = env->GetStringUTFChars(description, nullptr);
        if (utf != nullptr) {
          std::cerr << "ART Android MotionEvent exception=" << utf << "\n";
          env->ReleaseStringUTFChars(description, utf);
        }
      }
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (description != nullptr) env->DeleteLocalRef(description);
      if (throwable_class != nullptr) env->DeleteLocalRef(throwable_class);
      if (error != nullptr) env->DeleteLocalRef(error);
    }
    env->ExceptionClear();
    enqueued = false;
  }
  if (has_view_root && !enqueued) {
    ClearPointerDispatchRoot(state, env);
    env->DeleteLocalRef(view_root_class);
    env->DeleteLocalRef(input_host);
    env->DeleteLocalRef(root_class);
    env->DeleteLocalRef(event);
    env->DeleteLocalRef(motion_event_class);
    return 84;
  }
  if (!has_view_root) {
    jmethodID dispatch_touch = root_class == nullptr
                                   ? nullptr
                                   : env->GetMethodID(
                                         root_class, "dispatchTouchEvent",
                                         "(Landroid/view/MotionEvent;)Z");
    if (dispatch_touch == nullptr || env->ExceptionCheck()) {
      env->ExceptionClear();
      ClearPointerDispatchRoot(state, env);
      env->DeleteLocalRef(view_root_class);
      env->DeleteLocalRef(input_host);
      env->DeleteLocalRef(root_class);
      env->DeleteLocalRef(event);
      env->DeleteLocalRef(motion_event_class);
      return 82;
    }
    consumed = env->CallBooleanMethod(dispatch_root, dispatch_touch, event);
  }
  if (!enqueued && terminal && state->pressed_view != nullptr) {
    jobject hit = action == 1u && !env->ExceptionCheck()
                      ? find_clickable_view_at(env, dispatch_root, local_x, local_y)
                      : nullptr;
    const bool same_target =
        hit != nullptr && !env->ExceptionCheck() &&
        env->IsSameObject(hit, state->pressed_view) == JNI_TRUE;
    if (state->pointer_click_candidate && same_target &&
        !env->ExceptionCheck()) {
      jclass view_class = env->FindClass("android/view/View");
      jfieldID perform_click_runnable =
          view_class == nullptr
              ? nullptr
              : env->GetFieldID(view_class, "mPerformClick",
                                "Landroid/view/View$PerformClick;");
      jmethodID remove_callbacks =
          view_class == nullptr
              ? nullptr
              : env->GetMethodID(view_class, "removeCallbacks",
                                 "(Ljava/lang/Runnable;)Z");
      jmethodID perform_click =
          view_class == nullptr
              ? nullptr
              : env->GetMethodID(view_class, "performClick", "()Z");
      jobject runnable =
          perform_click_runnable == nullptr
              ? nullptr
              : env->GetObjectField(state->pressed_view,
                                    perform_click_runnable);
      if (runnable != nullptr && remove_callbacks != nullptr &&
          !env->ExceptionCheck()) {
        env->CallBooleanMethod(state->pressed_view, remove_callbacks, runnable);
      }
      if (perform_click != nullptr && !env->ExceptionCheck()) {
        env->CallBooleanMethod(state->pressed_view, perform_click);
      }
      if (runnable != nullptr) env->DeleteLocalRef(runnable);
      if (view_class != nullptr) env->DeleteLocalRef(view_class);
    }
    if (hit != nullptr) env->DeleteLocalRef(hit);
    env->DeleteGlobalRef(state->pressed_view);
    state->pressed_view = nullptr;
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
    std::cerr << "ART Android MotionEvent ABI2 action=" << action
              << " consumed=" << (consumed == JNI_TRUE ? 1 : 0)
              << " path=" << (enqueued ? "input-channel" : "decor")
              << " window=" << (state->pointer_dispatch_is_window ? 1 : 0)
              << " dispatch_us="
              << (dispatch_end > dispatch_start
                      ? (dispatch_end - dispatch_start) / 1000
                      : 0)
              << " owner=" << pthread_self() << "\n";
  }
  if (!dispatch_ok || !recycle_ok) {
    env->ExceptionClear();
    ClearPointerDispatchRoot(state, env);
    env->DeleteLocalRef(view_root_class);
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
    state->pointer_click_candidate = false;
    ClearPointerDispatchRoot(state, env);
  }
  state->gpu_ripple_overlay_active = action != 3u;
  if (down) {
    state->gpu_ripple_overlay_x = x;
    state->gpu_ripple_overlay_y = y;
    state->gpu_ripple_overlay_started = std::chrono::steady_clock::now();
  }
  if (action != 2u) state->gpu_render_node_recorded = false;
  env->DeleteLocalRef(view_root_class);
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

int32_t dispatch_key_v1(GraphicsState* state,
                        const DarwinArtKeyEventV1* event) {
  if (state == nullptr || event == nullptr || event->version != 1 ||
      event->size < sizeof(DarwinArtKeyEventV1) || event->action > 1 ||
      state->interactive_view_root == nullptr) {
    return 85;
  }
  art::Thread* art_thread = darwin_art_process::owner_thread_for_callback();
  if (art_thread == nullptr || art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }
  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
  jclass key_event_class = env->FindClass("android/view/KeyEvent");
  jmethodID constructor =
      key_event_class == nullptr
          ? nullptr
          : env->GetMethodID(key_event_class, "<init>", "(JJIIIIIIII)V");
  jobject key_event =
      constructor == nullptr
          ? nullptr
          : env->NewObject(
                key_event_class, constructor,
                static_cast<jlong>(event->down_time_nanos / 1000000ULL),
                static_cast<jlong>(event->event_time_nanos / 1000000ULL),
                static_cast<jint>(event->action),
                static_cast<jint>(event->key_code),
                static_cast<jint>(event->repeat_count),
                static_cast<jint>(event->meta_state),
                static_cast<jint>(event->device_id),
                static_cast<jint>(event->scan_code),
                static_cast<jint>(event->flags),
                static_cast<jint>(event->source));
  bool handled = false;
  const bool delivered =
      key_event != nullptr && !env->ExceptionCheck() &&
      darwin_art::DispatchFrameworkInputEvent(
          env, state->interactive_view_root, key_event, &handled);
  if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
    std::cerr << "ART Android KeyEvent action=" << event->action
              << " key=" << event->key_code
              << " device=" << event->device_id
              << " path=input-channel delivered=" << (delivered ? 1 : 0)
              << " handled=" << (handled ? 1 : 0)
              << "\n";
  }
  env->DeleteLocalRef(key_event);
  env->DeleteLocalRef(key_event_class);
  if (env->ExceptionCheck()) {
    if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
      env->ExceptionDescribe();
    }
    env->ExceptionClear();
  }
  return delivered ? 0 : 86;
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
  if (!sync_interactive_surface_size(state, env)) return 75;
  if (!DispatchDueMainMessages(env)) {
    if (env->ExceptionCheck()) {
      std::cerr << "ART Android main message dispatch threw\n"
                << art_thread->GetException()->Dump() << "\n";
      art_thread->ClearException();
    }
    return 75;
  }
  ActivateCurrentHostSurfaces(state, env);
  DebugWindowManagerViews(env);
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
  bool ok = !env->ExceptionCheck();
  if (ok) ok = DispatchDueMainMessages(env);
  if (!ok) {
    std::cerr << "ART Android frame pulse threw\n"
              << art_thread->GetException()->Dump() << "\n";
    art_thread->ClearException();
  }
  // Detached ViewRootImpl has no WindowManager traversal scheduler to turn an
  // invalidated Android hierarchy into a new display list.  Input listeners
  // still mutate the real APK views synchronously, so complete the same frame
  // by recording/replaying the retained root after Choreographer callbacks.
  // ACTION_DOWN/UP marked the RenderNode dirty; MOVE keeps the persistent
  // replay path and avoids a full display-list rebuild.
  if (ok && state->interactive_root != nullptr) {
    ok = present_content(state, env, nullptr, state->interactive_root,
                         state->interactive_width,
                         state->interactive_height) == JNI_TRUE &&
         !env->ExceptionCheck();
    if (!ok && env->ExceptionCheck()) {
      std::cerr << "ART Android frame presentation threw\n"
                << art_thread->GetException()->Dump() << "\n";
      art_thread->ClearException();
    }
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

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_dispatch_key_v1(
    const DarwinArtKeyEventV1*) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_pump_framework_frame(
    jlong) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
}
