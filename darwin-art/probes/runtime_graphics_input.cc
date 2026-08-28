#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <time.h>

#include "base/locks.h"
#include "darwin_android_platform.h"
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
  static bool logged_sync_barrier = false;
  static bool logged_native_poll_error = false;
  const int initial_native_dispatch =
      darwin_art_android_platform_poll_current_looper();
  if (initial_native_dispatch < 0) {
    // Android's MessageQueue does not terminate the Java loop when the native
    // Looper reports POLL_ERROR (for example, an interrupted or concurrently
    // closed native registration). Keep Handler/Choreographer work alive and
    // let the next non-blocking poll recover.
    if (!logged_native_poll_error) {
      logged_native_poll_error = true;
      std::cerr << "ART Android Looper: native poll failed; continuing main "
                   "queue\n";
    }
  }
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
  jfieldID message_what = message_class == nullptr
                              ? nullptr
                              : env->GetFieldID(message_class, "what", "I");
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
                        message_what != nullptr &&
                        message_next != nullptr && message_target != nullptr &&
                        is_asynchronous != nullptr && recycle != nullptr &&
                        dispatch != nullptr && uptime_millis != nullptr &&
                        !env->ExceptionCheck();
  jobject queue = resolved
                      ? env->CallStaticObjectMethod(looper_class, my_queue)
                      : nullptr;
  bool ok = resolved && queue != nullptr && !env->ExceptionCheck();
  static int debug_queue_frames = 20;
  for (size_t dispatched = 0; ok && dispatched < limit; ++dispatched) {
    jobject head = env->GetObjectField(queue, queue_messages);
    if (head == nullptr || env->ExceptionCheck()) {
      if (debug_queue_frames > 0 &&
          std::getenv("DARWIN_ART_DEBUG_MAIN_QUEUE") != nullptr) {
        --debug_queue_frames;
        std::cerr << "ART Android Looper: queue empty\n";
      }
      if (head != nullptr) env->DeleteLocalRef(head);
      break;
    }
    const jlong now = env->CallStaticLongMethod(clock_class, uptime_millis);
    jobject target = env->GetObjectField(head, message_target);
    if (debug_queue_frames > 0 &&
        std::getenv("DARWIN_ART_DEBUG_MAIN_QUEUE") != nullptr) {
      --debug_queue_frames;
      std::cerr << "ART Android Looper: head what="
                << env->GetIntField(head, message_what)
                << " when=" << env->GetLongField(head, message_when)
                << " now=" << now << " target=" << target << "\n";
    }
    bool selectable = target != nullptr &&
                      env->GetLongField(head, message_when) <= now;
    if (target == nullptr && !logged_sync_barrier &&
        std::getenv("DARWIN_ART_DEBUG_MAIN_QUEUE") != nullptr) {
      logged_sync_barrier = true;
      std::cerr << "ART Android Looper: pending synchronization barrier when="
                << env->GetLongField(head, message_when) << " now=" << now
                << "\n";
    }
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
  if (ok && darwin_art_android_platform_poll_current_looper() < 0 &&
      !logged_native_poll_error) {
    logged_native_poll_error = true;
    std::cerr << "ART Android Looper: native poll failed; continuing main "
                 "queue\n";
  }
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
      const jboolean clicked =
          env->CallBooleanMethod(pressed_view, perform_click);
      if (std::getenv("DARWIN_ART_DEBUG_POINTER") != nullptr) {
        std::cerr << "ART Android input debug performClick="
                  << (clicked == JNI_TRUE ? 1 : 0)
                  << " exception=" << (env->ExceptionCheck() ? 1 : 0)
                  << "\n";
      }
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

void DebugChromeOmniboxState(JNIEnv* env, jobject view_root) {
  if (std::getenv("DARWIN_ART_DEBUG_CHROME_OMNIBOX") == nullptr) return;
  jclass root_class = env->GetObjectClass(view_root);
  jmethodID get_view =
      root_class == nullptr
          ? nullptr
          : env->GetMethodID(root_class, "getView", "()Landroid/view/View;");
  jobject root = get_view == nullptr
                     ? nullptr
                     : env->CallObjectMethod(view_root, get_view);
  jclass view_class = env->FindClass("android/view/View");
  jmethodID find_focus =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "findFocus", "()Landroid/view/View;");
  jobject focus = root == nullptr || find_focus == nullptr
                      ? nullptr
                      : env->CallObjectMethod(root, find_focus);
  jclass focus_class = focus == nullptr ? nullptr : env->GetObjectClass(focus);
  jfieldID listener_field =
      focus_class == nullptr
          ? nullptr
          : env->GetFieldID(focus_class, "D", "Landroid/view/View$OnKeyListener;");
  if (listener_field == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    std::cerr << "ART Chrome omnibox: focused view is not UrlBar focus="
              << focus << "\n";
    env->DeleteLocalRef(focus_class);
    env->DeleteLocalRef(focus);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(root);
    env->DeleteLocalRef(root_class);
    return;
  }
  jobject listener = listener_field == nullptr
                         ? nullptr
                         : env->GetObjectField(focus, listener_field);
  if (listener == nullptr && !env->ExceptionCheck()) {
    jfieldID tab_listener_field =
        env->GetFieldID(focus_class, "K", "Landroid/view/View$OnKeyListener;");
    if (tab_listener_field != nullptr && !env->ExceptionCheck()) {
      listener = env->GetObjectField(focus, tab_listener_field);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  jclass listener_class =
      listener == nullptr ? nullptr : env->GetObjectClass(listener);
  jfieldID mediator_field =
      listener_class == nullptr
          ? nullptr
          : env->GetFieldID(listener_class, "q0", "Lum0;");
  jobject mediator = mediator_field == nullptr
                         ? nullptr
                         : env->GetObjectField(listener, mediator_field);
  jclass mediator_class =
      mediator == nullptr ? nullptr : env->GetObjectClass(mediator);
  jfieldID model_field =
      mediator_class == nullptr
          ? nullptr
          : env->GetFieldID(mediator_class, "d", "Lon0;");
  jobject model = model_field == nullptr
                      ? nullptr
                      : env->GetObjectField(mediator, model_field);
  jclass model_class = model == nullptr ? nullptr : env->GetObjectClass(model);
  jmethodID ready_method =
      model_class == nullptr ? nullptr : env->GetMethodID(model_class, "p", "()Z");
  jmethodID count_method =
      model_class == nullptr ? nullptr : env->GetMethodID(model_class, "n", "()I");
  const jboolean ready = ready_method == nullptr
                             ? JNI_FALSE
                             : env->CallBooleanMethod(model, ready_method);
  const jint count = count_method == nullptr ? -1 : env->CallIntMethod(model, count_method);
  jfieldID controller_field =
      model_class == nullptr
          ? nullptr
          : env->GetFieldID(
                model_class, "U",
                "Lorg/chromium/chrome/browser/omnibox/suggestions/AutocompleteController;");
  jobject controller = controller_field == nullptr
                           ? nullptr
                           : env->GetObjectField(model, controller_field);
  jclass controller_class =
      controller == nullptr ? nullptr : env->GetObjectClass(controller);
  jfieldID native_field = controller_class == nullptr
                              ? nullptr
                              : env->GetFieldID(controller_class, "b", "J");
  const jlong native_pointer = native_field == nullptr
                                   ? 0
                                   : env->GetLongField(controller, native_field);
  jfieldID state_field =
      model_class == nullptr
          ? nullptr
          : env->GetFieldID(model_class, "I", "Lym0;");
  jobject omnibox_state = state_field == nullptr
                              ? nullptr
                              : env->GetObjectField(model, state_field);
  jclass omnibox_state_class =
      omnibox_state == nullptr ? nullptr : env->GetObjectClass(omnibox_state);
  jmethodID state_method =
      omnibox_state_class == nullptr
          ? nullptr
          : env->GetMethodID(omnibox_state_class, "e", "()I");
  jmethodID input_mode_method =
      omnibox_state_class == nullptr
          ? nullptr
          : env->GetMethodID(omnibox_state_class, "f", "()I");
  const jint page_state = state_method == nullptr
                              ? -1
                              : env->CallIntMethod(omnibox_state, state_method);
  const jint input_mode = input_mode_method == nullptr
                              ? -1
                              : env->CallIntMethod(omnibox_state, input_mode_method);
  jfieldID loading_field = model_class == nullptr
                               ? nullptr
                               : env->GetFieldID(model_class, "c0", "Z");
  const jboolean loading = loading_field == nullptr
                               ? JNI_FALSE
                               : env->GetBooleanField(model, loading_field);
  jfieldID text_provider_field =
      model_class == nullptr
          ? nullptr
          : env->GetFieldID(model_class, "u", "Lyzg;");
  jobject text_provider = text_provider_field == nullptr
                              ? nullptr
                              : env->GetObjectField(model, text_provider_field);
  jclass text_provider_class =
      text_provider == nullptr ? nullptr : env->GetObjectClass(text_provider);
  jmethodID text_method =
      text_provider_class == nullptr
          ? nullptr
          : env->GetMethodID(text_provider_class, "b", "()Ljava/lang/String;");
  jstring typed = text_method == nullptr
                      ? nullptr
                      : static_cast<jstring>(
                            env->CallObjectMethod(text_provider, text_method));
  const char* typed_utf =
      typed == nullptr ? nullptr : env->GetStringUTFChars(typed, nullptr);
  jmethodID match_method =
      controller_class == nullptr
          ? nullptr
          : env->GetMethodID(
                controller_class, "a",
                "(Ljava/lang/String;)Lorg/chromium/components/omnibox/AutocompleteMatch;");
  jobject match = match_method == nullptr || typed == nullptr
                      ? nullptr
                      : env->CallObjectMethod(controller, match_method, typed);
  jclass match_class = match == nullptr ? nullptr : env->GetObjectClass(match);
  jfieldID destination_field =
      match_class == nullptr
          ? nullptr
          : env->GetFieldID(match_class, "l", "Lorg/chromium/url/GURL;");
  jobject destination = destination_field == nullptr
                            ? nullptr
                            : env->GetObjectField(match, destination_field);
  jclass gurl_class = destination == nullptr ? nullptr : env->GetObjectClass(destination);
  jmethodID gurl_spec_method =
      gurl_class == nullptr
          ? nullptr
          : env->GetMethodID(gurl_class, "k", "()Ljava/lang/String;");
  jstring destination_spec =
      gurl_spec_method == nullptr
          ? nullptr
          : static_cast<jstring>(
                env->CallObjectMethod(destination, gurl_spec_method));
  const char* destination_utf =
      destination_spec == nullptr
          ? nullptr
          : env->GetStringUTFChars(destination_spec, nullptr);
  jclass class_class = env->FindClass("java/lang/Class");
  jmethodID get_class_loader =
      class_class == nullptr
          ? nullptr
          : env->GetMethodID(class_class, "getClassLoader",
                             "()Ljava/lang/ClassLoader;");
  jobject app_loader = get_class_loader == nullptr
                           ? nullptr
                           : env->CallObjectMethod(focus_class,
                                                   get_class_loader);
  jclass loader_class =
      app_loader == nullptr ? nullptr : env->GetObjectClass(app_loader);
  jmethodID load_class =
      loader_class == nullptr
          ? nullptr
          : env->GetMethodID(loader_class, "loadClass",
                             "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring native_mux_name = env->NewStringUTF("J.N");
  jclass native_mux_class =
      load_class == nullptr || native_mux_name == nullptr
          ? nullptr
          : static_cast<jclass>(env->CallObjectMethod(
                app_loader, load_class, native_mux_name));
  jmethodID should_handle_method =
      native_mux_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(native_mux_class, "ZO", "(ILjava/lang/Object;)Z");
  const jboolean handled_without_navigation =
      should_handle_method == nullptr || destination == nullptr
          ? JNI_FALSE
          : env->CallStaticBooleanMethod(native_mux_class, should_handle_method,
                                         66, destination);
  jfieldID native_ready_field =
      listener_class == nullptr ? nullptr
                                : env->GetFieldID(listener_class, "v0", "Z");
  const jboolean native_ready =
      native_ready_field == nullptr
          ? JNI_FALSE
          : env->GetBooleanField(listener, native_ready_field);
  jfieldID tab_provider_field =
      listener_class == nullptr
          ? nullptr
          : env->GetFieldID(listener_class, "t", "Lx28;");
  jobject tab_provider = tab_provider_field == nullptr
                             ? nullptr
                             : env->GetObjectField(listener, tab_provider_field);
  jclass tab_provider_class =
      tab_provider == nullptr ? nullptr : env->GetObjectClass(tab_provider);
  jmethodID get_tab_method =
      tab_provider_class == nullptr
          ? nullptr
          : env->GetMethodID(
                tab_provider_class, "h",
                "()Lorg/chromium/chrome/browser/tab/Tab;");
  jobject tab = get_tab_method == nullptr
                    ? nullptr
                    : env->CallObjectMethod(tab_provider, get_tab_method);
  jclass tab_class = tab == nullptr ? nullptr : env->GetObjectClass(tab);
  jmethodID get_web_contents_method =
      tab_class == nullptr
          ? nullptr
          : env->GetMethodID(
                tab_class, "getWebContents",
                "()Lorg/chromium/content_public/browser/WebContents;");
  jobject web_contents = get_web_contents_method == nullptr
                             ? nullptr
                             : env->CallObjectMethod(tab, get_web_contents_method);
  jclass web_contents_class =
      web_contents == nullptr ? nullptr : env->GetObjectClass(web_contents);
  jmethodID get_navigation_controller_method =
      web_contents_class == nullptr
          ? nullptr
          : env->GetMethodID(
                web_contents_class, "v",
                "()Lorg/chromium/content_public/browser/NavigationController;");
  jobject navigation_controller =
      get_navigation_controller_method == nullptr
          ? nullptr
          : env->CallObjectMethod(web_contents,
                                  get_navigation_controller_method);
  jclass navigation_controller_class =
      navigation_controller == nullptr
          ? nullptr
          : env->GetObjectClass(navigation_controller);
  jfieldID navigation_native_field =
      navigation_controller_class == nullptr
          ? nullptr
          : env->GetFieldID(navigation_controller_class, "a", "J");
  const jlong navigation_native =
      navigation_native_field == nullptr
          ? 0
          : env->GetLongField(navigation_controller, navigation_native_field);
  jmethodID current_url_method =
      web_contents_class == nullptr
          ? nullptr
          : env->GetMethodID(web_contents_class, "L",
                             "()Lorg/chromium/url/GURL;");
  jobject current_url = current_url_method == nullptr
                            ? nullptr
                            : env->CallObjectMethod(web_contents,
                                                    current_url_method);
  jclass current_url_class =
      current_url == nullptr ? nullptr : env->GetObjectClass(current_url);
  jmethodID current_url_spec_method =
      current_url_class == nullptr
          ? nullptr
          : env->GetMethodID(current_url_class, "k", "()Ljava/lang/String;");
  jstring current_url_spec =
      current_url_spec_method == nullptr
          ? nullptr
          : static_cast<jstring>(
                env->CallObjectMethod(current_url, current_url_spec_method));
  const char* current_url_utf =
      current_url_spec == nullptr
          ? nullptr
          : env->GetStringUTFChars(current_url_spec, nullptr);
  jclass object_class = env->FindClass("java/lang/Object");
  jmethodID to_string =
      object_class == nullptr
          ? nullptr
          : env->GetMethodID(object_class, "toString", "()Ljava/lang/String;");
  jstring text = focus == nullptr || to_string == nullptr
                     ? nullptr
                     : static_cast<jstring>(env->CallObjectMethod(focus, to_string));
  const char* utf = text == nullptr ? nullptr : env->GetStringUTFChars(text, nullptr);
  std::cerr << "ART Chrome omnibox: focus=" << focus
            << " listener=" << listener << " mediator=" << mediator
            << " model=" << model << " ready=" << (ready == JNI_TRUE ? 1 : 0)
            << " suggestions=" << count << " controller=" << controller
            << " native=" << native_pointer
            << " page_state=" << page_state
            << " input_mode=" << input_mode
            << " loading=" << (loading == JNI_TRUE ? 1 : 0)
            << " location_native=" << (native_ready == JNI_TRUE ? 1 : 0)
            << " tab=" << tab << " web_contents=" << web_contents
            << " navigation=" << navigation_controller
            << " navigation_native=" << navigation_native
            << " typed=" << (typed_utf == nullptr ? "(null)" : typed_utf)
            << " match=" << match
            << " destination="
            << (destination_utf == nullptr ? "(null)" : destination_utf)
            << " native_handled="
            << (handled_without_navigation == JNI_TRUE ? 1 : 0)
            << " current_url="
            << (current_url_utf == nullptr ? "(null)" : current_url_utf)
            << " text=" << (utf == nullptr ? "(null)" : utf) << "\n";
  if (typed_utf != nullptr) env->ReleaseStringUTFChars(typed, typed_utf);
  if (destination_utf != nullptr) {
    env->ReleaseStringUTFChars(destination_spec, destination_utf);
  }
  if (current_url_utf != nullptr) {
    env->ReleaseStringUTFChars(current_url_spec, current_url_utf);
  }
  if (utf != nullptr) env->ReleaseStringUTFChars(text, utf);
  env->DeleteLocalRef(match);
  env->DeleteLocalRef(current_url_spec);
  env->DeleteLocalRef(current_url_class);
  env->DeleteLocalRef(current_url);
  env->DeleteLocalRef(native_mux_class);
  env->DeleteLocalRef(native_mux_name);
  env->DeleteLocalRef(loader_class);
  env->DeleteLocalRef(app_loader);
  env->DeleteLocalRef(class_class);
  env->DeleteLocalRef(destination_spec);
  env->DeleteLocalRef(gurl_class);
  env->DeleteLocalRef(destination);
  env->DeleteLocalRef(match_class);
  env->DeleteLocalRef(typed);
  env->DeleteLocalRef(navigation_controller_class);
  env->DeleteLocalRef(navigation_controller);
  env->DeleteLocalRef(web_contents_class);
  env->DeleteLocalRef(web_contents);
  env->DeleteLocalRef(tab_class);
  env->DeleteLocalRef(tab);
  env->DeleteLocalRef(tab_provider_class);
  env->DeleteLocalRef(tab_provider);
  env->DeleteLocalRef(text_provider_class);
  env->DeleteLocalRef(text_provider);
  env->DeleteLocalRef(omnibox_state_class);
  env->DeleteLocalRef(omnibox_state);
  env->DeleteLocalRef(text);
  env->DeleteLocalRef(object_class);
  env->DeleteLocalRef(controller_class);
  env->DeleteLocalRef(controller);
  env->DeleteLocalRef(model_class);
  env->DeleteLocalRef(model);
  env->DeleteLocalRef(mediator_class);
  env->DeleteLocalRef(mediator);
  env->DeleteLocalRef(listener_class);
  env->DeleteLocalRef(listener);
  env->DeleteLocalRef(focus_class);
  env->DeleteLocalRef(focus);
  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(root);
  env->DeleteLocalRef(root_class);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
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
  const int64_t event_time_nanos = event->event_time_nanos > 0
                                       ? static_cast<int64_t>(event->event_time_nanos)
                                       : MonotonicNanos();
  if (event_time_nanos <= 0) {
    env->DeleteLocalRef(key_event_class);
    return 87;
  }
  int64_t down_time_nanos = event->down_time_nanos > 0
                                ? static_cast<int64_t>(event->down_time_nanos)
                                : event_time_nanos;
  const size_t key_index = static_cast<size_t>(event->key_code);
  if (key_index < state->key_down_time_nanos.size()) {
    if (event->action == 0) {
      state->key_down_time_nanos[key_index] = down_time_nanos;
    } else if (event->down_time_nanos == 0 &&
               state->key_down_time_nanos[key_index] > 0) {
      down_time_nanos = state->key_down_time_nanos[key_index];
    }
  }
  jobject key_event =
      constructor == nullptr
          ? nullptr
          : env->NewObject(
                key_event_class, constructor,
                static_cast<jlong>(down_time_nanos / 1000000LL),
                static_cast<jlong>(event_time_nanos / 1000000LL),
                static_cast<jint>(event->action),
                static_cast<jint>(event->key_code),
                static_cast<jint>(event->repeat_count),
                static_cast<jint>(event->meta_state),
                static_cast<jint>(event->device_id),
                static_cast<jint>(event->scan_code),
                static_cast<jint>(event->flags),
                static_cast<jint>(event->source));
  if (event->action == 1 && key_index < state->key_down_time_nanos.size()) {
    state->key_down_time_nanos[key_index] = 0;
  }
  bool handled = false;
  const bool delivered =
      key_event != nullptr && !env->ExceptionCheck() &&
      darwin_art::DispatchFrameworkInputEvent(
          env, state->interactive_view_root, key_event, &handled);
  bool delivered_to_focused_view = false;
  if (delivered && !handled) {
    // The compatibility InputChannel owns a detached receiver, so an event that
    // finishes unhandled does not continue through ViewPostImeInputStage as it
    // would under Android's WindowManager-managed ViewRootImpl. Complete that
    // terminal stage generically by dispatching to the currently focused View.
    // The finished-event result prevents a second delivery when the channel
    // path already consumed the event.
    jclass root_class = env->GetObjectClass(state->interactive_view_root);
    jmethodID get_view = root_class == nullptr
                             ? nullptr
                             : env->GetMethodID(root_class, "getView",
                                                "()Landroid/view/View;");
    jobject root = get_view == nullptr
                       ? nullptr
                       : env->CallObjectMethod(state->interactive_view_root,
                                               get_view);
    jclass view_class = root == nullptr ? nullptr : env->GetObjectClass(root);
    jmethodID find_focus = view_class == nullptr
                               ? nullptr
                               : env->GetMethodID(view_class, "findFocus",
                                                  "()Landroid/view/View;");
    jobject focus = find_focus == nullptr
                        ? nullptr
                        : env->CallObjectMethod(root, find_focus);
    jclass focus_class = focus == nullptr ? nullptr : env->GetObjectClass(focus);
    jmethodID dispatch = focus_class == nullptr
                             ? nullptr
                             : env->GetMethodID(focus_class, "dispatchKeyEvent",
                                                "(Landroid/view/KeyEvent;)Z");
    if (dispatch != nullptr && !env->ExceptionCheck()) {
      delivered_to_focused_view = true;
      handled = env->CallBooleanMethod(focus, dispatch, key_event) == JNI_TRUE;
    }
    env->DeleteLocalRef(focus_class);
    env->DeleteLocalRef(focus);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(root);
    env->DeleteLocalRef(root_class);
  }
  if (event->action == 0 &&
      (event->key_code == 66 || event->key_code == 131)) {
    DebugChromeOmniboxState(env, state->interactive_view_root);
  }
  if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
    std::cerr << "ART Android KeyEvent action=" << event->action
              << " key=" << event->key_code
              << " device=" << event->device_id
              << " path=input-channel"
              << (delivered_to_focused_view ? "+focused-view" : "")
              << " delivered=" << (delivered ? 1 : 0)
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
  // SurfaceFlinger normally signals DisplayEventReceiver on its registered
  // Looper. The Darwin surface loop is the display clock, so publish exactly
  // one pending edge here on the Android owner thread and let the framework's
  // normal async onVsync -> Handler -> Choreographer.doFrame path run below.
  const int delivered_vsyncs =
      darwin_art::DispatchFrameworkPendingVsyncs(env, frame_time_nanos);
  bool ok = delivered_vsyncs >= 0 && !env->ExceptionCheck();
  if (ok) ok = DispatchDueMainMessages(env);
  if (!ok) {
    if (env->ExceptionCheck() && art_thread->GetException() != nullptr) {
      std::cerr << "ART Android frame pulse threw\n"
                << art_thread->GetException()->Dump() << "\n";
      art_thread->ClearException();
    } else {
      std::cerr << "ART Android frame pulse failed without a pending Java exception\n";
    }
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
  return ok ? 0 : 75;
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
