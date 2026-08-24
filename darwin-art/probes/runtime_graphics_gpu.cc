#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "darwin_hwui_gpu_mode.h"
#include "darwin_surface_bridge.h"
#include "mirror/throwable.h"
#include "runtime_graphics_gpu.h"
#include "runtime_graphics_state.h"
#include "runtime_frame_probe.h"
#include "runtime_hwui_probe.h"
#include "thread-current-inl.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#ifdef HIDDEN
#undef HIDDEN
#endif
#define private public
#define protected public
#include "AnimationContext.h"
#include "Animator.h"
#include "AnimatorManager.h"
#include "renderthread/TimeLord.h"
#include "RenderNode.h"
#undef protected
#undef private
#endif

namespace darwin_art_graphics {

#if defined(DARWIN_ART_REAL_GRAPHICS)
namespace {

bool view_subtree_needs_recording(JNIEnv* env, jobject view, jclass group_class,
                                  jmethodID is_dirty,
                                  jmethodID is_layout_requested,
                                  jmethodID get_child_count,
                                  jmethodID get_child_at, int depth) {
  if (view == nullptr || depth > 32 || env->ExceptionCheck()) return true;
  if (env->CallBooleanMethod(view, is_dirty) == JNI_TRUE ||
      env->CallBooleanMethod(view, is_layout_requested) == JNI_TRUE) {
    return true;
  }
  if (!env->IsInstanceOf(view, group_class)) return false;
  const jint count = env->CallIntMethod(view, get_child_count);
  for (jint index = 0; index < count && !env->ExceptionCheck(); ++index) {
    jobject child = env->CallObjectMethod(view, get_child_at, index);
    const bool child_needs_recording = view_subtree_needs_recording(
        env, child, group_class, is_dirty, is_layout_requested,
        get_child_count, get_child_at, depth + 1);
    if (child != nullptr) env->DeleteLocalRef(child);
    if (child_needs_recording) return true;
  }
  return env->ExceptionCheck();
}

}  // namespace

int prepare_gpu_surface(GraphicsState* state, jint width, jint height) {
  if (state == nullptr) return 1;
  if (state->gpu_surface != nullptr) return 0;
  const bool run_apk_app = std::getenv("DARWIN_ART_APK_APP_PACKAGE") != nullptr;
  const char* app_label = std::getenv("DARWIN_ART_APK_APP_LABEL");
  DarwinArtSurfaceCreateInfo info{
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .title = run_apk_app && app_label != nullptr && app_label[0] != '\0'
                   ? app_label
                   : "Darwin ART · HWUI Metal",
      .visible = true,
  };
  DarwinArtSurfaceResult result = DARWIN_ART_SURFACE_OK;
  state->gpu_surface = darwin_art_surface_create(&info, &result);
  if (state->gpu_surface == nullptr) {
    std::cerr << "ART HWUI GPU: surface initialization failed status="
              << result << "\n";
    return static_cast<int>(result);
  }
  darwin_art_surface_set_active_gpu(state->gpu_surface);
  return 0;
}

jboolean attach_hardware_hierarchy_on_owner(GraphicsState* state, JNIEnv* env,
                                            jobject view) {
  if (state == nullptr || env == nullptr || view == nullptr ||
      state->hardware_context == nullptr) {
    return JNI_FALSE;
  }
  std::cerr << "ART HWUI GPU: owner-thread attach begin\n";
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
                             : env->GetMethodID(
                                   thread_class, "getContextClassLoader",
                                   "()Ljava/lang/ClassLoader;");
  jobject loader = get_loader == nullptr
                       ? nullptr
                       : env->CallObjectMethod(thread, get_loader);
  jclass loader_class = loader == nullptr ? nullptr : env->GetObjectClass(loader);
  jmethodID load_class = loader_class == nullptr
                             ? nullptr
                             : env->GetMethodID(
                                   loader_class, "loadClass",
                                   "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring helper_name = env->NewStringUTF("dev.darwinart.probe.ProbeAnimationHost");
  jclass helper = load_class == nullptr
                      ? nullptr
                      : static_cast<jclass>(env->CallObjectMethod(
                            loader, load_class, helper_name));
  jmethodID attach = helper == nullptr
                         ? nullptr
                         : env->GetStaticMethodID(
                               helper, "attachHardwareHierarchy",
                               "(Ljava/lang/Object;Ljava/lang/Object;)Z");
  jboolean result = (attach == nullptr || env->ExceptionCheck())
                        ? JNI_FALSE
                        : env->CallStaticBooleanMethod(
                              helper, attach, view, state->hardware_context);
  if (result == JNI_TRUE && !env->ExceptionCheck()) {
    jclass view_class = env->FindClass("android/view/View");
    jfieldID attach_info_field =
        view_class == nullptr
            ? nullptr
            : env->GetFieldID(view_class, "mAttachInfo",
                              "Landroid/view/View$AttachInfo;");
    jobject attach_info =
        attach_info_field == nullptr
            ? nullptr
            : env->GetObjectField(view, attach_info_field);
    jclass attach_info_class =
        attach_info == nullptr ? nullptr : env->GetObjectClass(attach_info);
    jfieldID window_token_field =
        attach_info_class == nullptr
            ? nullptr
            : env->GetFieldID(attach_info_class, "mWindowToken",
                              "Landroid/os/IBinder;");
    jclass binder_class = env->FindClass("android/os/Binder");
    jmethodID binder_constructor =
        binder_class == nullptr
            ? nullptr
            : env->GetMethodID(binder_class, "<init>", "()V");
    jobject window_token =
        binder_constructor == nullptr
            ? nullptr
            : env->NewObject(binder_class, binder_constructor);
    if (attach_info != nullptr && window_token_field != nullptr &&
        window_token != nullptr && !env->ExceptionCheck()) {
      env->SetObjectField(attach_info, window_token_field, window_token);
    } else {
      result = JNI_FALSE;
    }
    env->DeleteLocalRef(window_token);
    env->DeleteLocalRef(binder_class);
    env->DeleteLocalRef(attach_info_class);
    env->DeleteLocalRef(attach_info);
    env->DeleteLocalRef(view_class);
  }
  if (env->ExceptionCheck()) {
    std::cerr << "ART HWUI GPU: owner-thread ViewRoot attach failed\n";
    env->ExceptionDescribe();
    env->ExceptionClear();
    result = JNI_FALSE;
  }
  std::cerr << "ART HWUI GPU: owner-thread attach end result="
            << (result == JNI_TRUE ? 1 : 0) << "\n";
  env->DeleteLocalRef(helper_name);
  env->DeleteLocalRef(loader_class);
  env->DeleteLocalRef(loader);
  env->DeleteLocalRef(thread);
  env->DeleteLocalRef(thread_class);
  return result;
}

jboolean present_gpu_content(GraphicsState* state, JNIEnv* env, jobject view,
                                  jint width, jint height) {
  if (state == nullptr) return JNI_FALSE;
  if (!darwin_art::hwui_gpu_enabled()) {
    return JNI_FALSE;
  }
  if (state->gpu_surface == nullptr) {
    if (prepare_gpu_surface(state, width, height) != 0) {
      return JNI_FALSE;
    }
  }
  // Keep retained replay for unchanged frames, but honor Android's normal
  // invalidation contract. App-side Handler/Choreographer work can dirty a
  // TextView or request layout without another pointer action (for example a
  // running stopwatch). Those frames must rebuild the display list; otherwise
  // state advances in Java while Metal keeps presenting stale pixels.
  bool view_needs_recording = true;
  jclass dirty_view_class = env->FindClass("android/view/View");
  jclass dirty_group_class = env->FindClass("android/view/ViewGroup");
  jmethodID is_dirty = dirty_view_class == nullptr
                           ? nullptr
                           : env->GetMethodID(dirty_view_class, "isDirty", "()Z");
  jmethodID is_layout_requested =
      dirty_view_class == nullptr
          ? nullptr
          : env->GetMethodID(dirty_view_class, "isLayoutRequested", "()Z");
  jmethodID get_child_count =
      dirty_group_class == nullptr
          ? nullptr
          : env->GetMethodID(dirty_group_class, "getChildCount", "()I");
  jmethodID get_child_at =
      dirty_group_class == nullptr
          ? nullptr
          : env->GetMethodID(dirty_group_class, "getChildAt",
                             "(I)Landroid/view/View;");
  if (is_dirty != nullptr && is_layout_requested != nullptr &&
      get_child_count != nullptr && get_child_at != nullptr &&
      !env->ExceptionCheck()) {
    view_needs_recording = view_subtree_needs_recording(
        env, view, dirty_group_class, is_dirty,
        is_layout_requested, get_child_count, get_child_at, 0);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    view_needs_recording = true;
  }
  if (dirty_group_class != nullptr) env->DeleteLocalRef(dirty_group_class);
  if (dirty_view_class != nullptr) env->DeleteLocalRef(dirty_view_class);
  // ACTION_MOVE remains replay-only while the hierarchy is clean. Re-recording
  // every 16 ms would replace display-list-owned CanvasProperty references and
  // make native RenderNode animations appear static.
  if (state->gpu_render_node_recorded && state->pending_pressed_action == 0 &&
      !view_needs_recording) {
    return darwin_art_hwui::render_node_to_surface(
               env, view, state->gpu_render_node, state->gpu_surface, width, height,
               state->gpu_ripple_overlay_active, state->gpu_ripple_overlay_x,
               state->gpu_ripple_overlay_y, state->gpu_ripple_overlay_started)
               ? JNI_TRUE
               : JNI_FALSE;
  }

  jclass render_node_class = env->FindClass("android/graphics/RenderNode");
  jfieldID native_render_node =
      render_node_class == nullptr
          ? nullptr
          : env->GetFieldID(render_node_class, "mNativeRenderNode", "J");
  // The helper lives in the app DEX, so resolve it through the content
  // classloader rather than FindClass (which is rooted at boot on this
  // standalone ART thread).
  jclass animation_host_class = nullptr;
  jclass thread_class = env->FindClass("java/lang/Thread");
  jmethodID current_thread =
      thread_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(thread_class, "currentThread",
                                   "()Ljava/lang/Thread;");
  jobject thread = current_thread == nullptr
                       ? nullptr
                       : env->CallStaticObjectMethod(thread_class, current_thread);
  jmethodID get_class_loader =
      thread_class == nullptr
          ? nullptr
          : env->GetMethodID(thread_class, "getContextClassLoader",
                             "()Ljava/lang/ClassLoader;");
  jobject class_loader = get_class_loader == nullptr
                             ? nullptr
                             : env->CallObjectMethod(thread, get_class_loader);
  jclass class_loader_class = class_loader == nullptr
                                  ? nullptr
                                  : env->GetObjectClass(class_loader);
  jmethodID load_class =
      class_loader_class == nullptr
          ? nullptr
          : env->GetMethodID(class_loader_class, "loadClass",
                             "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring helper_name = env->NewStringUTF("dev.darwinart.probe.ProbeAnimationHost");
  jobject helper_class = load_class == nullptr
                             ? nullptr
                             : env->CallObjectMethod(class_loader, load_class,
                                                     helper_name);
  if (!env->ExceptionCheck()) {
    animation_host_class = static_cast<jclass>(helper_class);
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(helper_name);
  env->DeleteLocalRef(class_loader_class);
  env->DeleteLocalRef(class_loader);
  env->DeleteLocalRef(thread);
  env->DeleteLocalRef(thread_class);
  if (animation_host_class == nullptr) {
    std::cerr << "ART HWUI GPU: app AnimationHost helper class unavailable\n";
  }
  jclass animation_host_interface =
      env->FindClass("android/graphics/RenderNode$AnimationHost");
  jmethodID animation_host_create =
      animation_host_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(animation_host_class, "create",
                                   "(Ljava/lang/Class;)Ljava/lang/Object;");
  jmethodID prepare_view_pagers =
      animation_host_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(animation_host_class, "prepareViewPagers",
                                   "(Ljava/lang/Object;)V");
  jmethodID dispatch_pre_draw =
      animation_host_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(animation_host_class, "dispatchPreDraw",
                                   "(Ljava/lang/Object;)Z");
  jmethodID invalidate_view_tree =
      animation_host_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(animation_host_class, "invalidateViewTree",
                                   "(Ljava/lang/Object;)V");
  jmethodID render_node_create =
      render_node_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(
                render_node_class, "create",
                "(Ljava/lang/String;Landroid/graphics/RenderNode$AnimationHost;)"
                "Landroid/graphics/RenderNode;");
  jmethodID begin_recording =
      render_node_class == nullptr
          ? nullptr
          : env->GetMethodID(render_node_class, "beginRecording",
                             "(II)Landroid/graphics/RecordingCanvas;");
  jmethodID end_recording =
      render_node_class == nullptr
          ? nullptr
          : env->GetMethodID(render_node_class, "endRecording", "()V");
  jmethodID set_position =
      render_node_class == nullptr
          ? nullptr
          : env->GetMethodID(render_node_class, "setPosition", "(IIII)Z");
  if (state->gpu_render_node == nullptr && render_node_create != nullptr &&
      animation_host_create != nullptr &&
      animation_host_interface != nullptr &&
      !env->ExceptionCheck()) {
    jstring node_name = env->NewStringUTF("Darwin ART HWUI root");
    jobject host = env->CallStaticObjectMethod(
        animation_host_class, animation_host_create, animation_host_interface);
    std::cerr << "ART HWUI GPU: animation host=" << host << "\n";
    jobject node = env->CallStaticObjectMethod(render_node_class, render_node_create,
                                               node_name, host);
    std::cerr << "ART HWUI GPU: RenderNode.create node=" << node << "\n";
    if (node != nullptr && !env->ExceptionCheck()) {
      state->gpu_render_node = env->NewGlobalRef(node);
    }
    env->DeleteLocalRef(host);
    env->DeleteLocalRef(node);
    env->DeleteLocalRef(node_name);
  }
  // Supply only Android's AttachInfo bookkeeping to the detached hierarchy.
  // ProbeAnimationHost deliberately does not dispatch the full ViewRoot
  // attach lifecycle (there is no window session or traversal thread), but
  // ViewGroup child recording still needs mAttachInfo to take the normal
  // hardware path. This is a one-time owner-thread operation.
  if (state->gpu_render_node != nullptr && state->hardware_context != nullptr &&
      !state->gpu_render_node_recorded) {
    attach_hardware_hierarchy_on_owner(state, env, view);
  }
  jobject java_canvas =
      state->gpu_render_node == nullptr || begin_recording == nullptr ||
              env->ExceptionCheck()
          ? nullptr
          : env->CallObjectMethod(state->gpu_render_node, begin_recording, width,
                                  height);
  if (java_canvas == nullptr || native_render_node == nullptr ||
      end_recording == nullptr || set_position == nullptr ||
      env->ExceptionCheck()) {
    if (env->ExceptionCheck()) {
      art::Thread* self = art::Thread::Current();
      if (self != nullptr && self->IsExceptionPending()) {
        std::cerr << "ART HWUI GPU: RenderNode setup exception\n"
                  << self->GetException()->Dump() << "\n";
      }
    }
    if (env->ExceptionCheck()) {
      std::cerr << "ART HWUI GPU: RenderNode begin exception\n";
      art::Thread* self = art::Thread::Current();
      if (self != nullptr && self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
    }
    if (java_canvas != nullptr && state->gpu_render_node != nullptr &&
        end_recording != nullptr) {
      env->ExceptionClear();
      env->CallVoidMethod(state->gpu_render_node, end_recording);
      env->ExceptionClear();
    }
    env->ExceptionClear();
    env->DeleteLocalRef(animation_host_class);
    env->DeleteLocalRef(animation_host_interface);
    env->DeleteLocalRef(render_node_class);
    std::cerr << "ART HWUI GPU: RenderNode.beginRecording failed\n";
    return JNI_FALSE;
  }
  bool recording_ended = false;
  auto dump_pending_exception = [&](const char* phase) {
    if (!env->ExceptionCheck()) return;
    std::cerr << "ART HWUI GPU: managed exception during " << phase << "\n";
    art::Thread* self = art::Thread::Current();
    if (self != nullptr && self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
  };
  auto finish_recording = [&]() -> bool {
    if (recording_ended) return true;
    // endRecording is the Java-side promotion boundary. Always close a
    // recording, including failure paths, so RenderNode never remains in the
    // "recording in progress" state for the next frame.
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->CallVoidMethod(state->gpu_render_node, end_recording);
    recording_ended = true;
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    return ok;
  };
  env->CallBooleanMethod(state->gpu_render_node, set_position, 0, 0, width, height);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    finish_recording();
    env->DeleteLocalRef(java_canvas);
    env->DeleteLocalRef(animation_host_class);
    env->DeleteLocalRef(animation_host_interface);
    env->DeleteLocalRef(render_node_class);
    std::cerr << "ART HWUI GPU: RenderNode.setPosition failed\n";
    return JNI_FALSE;
  }
  jclass view_class = env->FindClass("android/view/View");
  jmethodID draw = view_class == nullptr
                       ? nullptr
                       : env->GetMethodID(view_class, "draw",
                                          "(Landroid/graphics/Canvas;)V");
  jmethodID measure = view_class == nullptr
                          ? nullptr
                          : env->GetMethodID(view_class, "measure", "(II)V");
  jmethodID layout = view_class == nullptr
                         ? nullptr
                         : env->GetMethodID(view_class, "layout", "(IIII)V");
  jmethodID set_pressed = view_class == nullptr
                              ? nullptr
                              : env->GetMethodID(view_class, "setPressed", "(Z)V");
  jmethodID perform_click = view_class == nullptr
                                ? nullptr
                                : env->GetMethodID(view_class, "performClick", "()Z");
  jmethodID drawable_hotspot_changed =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "drawableHotspotChanged", "(FF)V");
  auto get_view_field = [&](const char* name) -> jfieldID {
    return view_class == nullptr || env->ExceptionCheck()
               ? nullptr
               : env->GetFieldID(view_class, name, "I");
  };
  jfieldID view_left = get_view_field("mLeft");
  jfieldID view_top = get_view_field("mTop");
  jfieldID view_right = get_view_field("mRight");
  jfieldID view_bottom = get_view_field("mBottom");
  if (draw == nullptr || measure == nullptr || layout == nullptr ||
      view_left == nullptr || view_top == nullptr || view_right == nullptr ||
      view_bottom == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    finish_recording();
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(java_canvas);
    env->DeleteLocalRef(animation_host_class);
    env->DeleteLocalRef(animation_host_interface);
    env->DeleteLocalRef(render_node_class);
    return JNI_FALSE;
  }
  // The standalone probe has no ViewRoot/ThreadedRenderer to perform the
  // normal measure/layout pass. Give the real widget an exact portrait
  // viewport before recording so Button/TextView emits its display list.
  constexpr jint kMeasureExactly = 0x40000000;
  const jint width_spec = kMeasureExactly | (width & 0x3fffffff);
  const jint height_spec = kMeasureExactly | (height & 0x3fffffff);
  env->CallVoidMethod(view, measure, width_spec, height_spec);
  dump_pending_exception("View.measure");
  // Match the ViewRoot traversal contract used by the existing raster probe:
  // seed detached root bounds before layout so its children receive a stable
  // first hardware-recording pass without window-service callbacks.
  env->SetIntField(view, view_left, 0);
  env->SetIntField(view, view_top, 0);
  env->SetIntField(view, view_right, width);
  env->SetIntField(view, view_bottom, height);
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  if (env->ExceptionCheck()) {
    dump_pending_exception("View.layout");
    env->ExceptionClear();
    finish_recording();
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(java_canvas);
    env->DeleteLocalRef(animation_host_class);
    env->DeleteLocalRef(animation_host_interface);
    env->DeleteLocalRef(render_node_class);
    std::cerr << "ART HWUI GPU: View measure/layout failed\n";
    return JNI_FALSE;
  }
  if (std::getenv("DARWIN_ART_SKOTTIE_METAL") != nullptr) {
    darwin_art_hwui::hide_skottie_backing_views(env, view);
  }
  // A real ViewRoot performs the first ViewPager population while the
  // hierarchy is attached to a window.  This standalone probe has no
  // ViewRoot, so support-library ViewPager's drawing-order cache would remain
  // null and its getChildDrawingOrder() would throw during DecorView.draw().
  // Re-run the same public population hook on every pager in the hierarchy;
  // this changes no APK code and keeps the Android view traversal intact.
  // A real ViewRoot owns an app Looper and window-system thread. This
  // detached host intentionally does not synthesize one; the content root
  // is measured/layouted directly and recorded into the persistent RenderNode.
  if (prepare_view_pagers != nullptr && !env->ExceptionCheck()) {
    env->CallStaticVoidMethod(animation_host_class, prepare_view_pagers, view);
    if (env->ExceptionCheck()) {
      dump_pending_exception("ProbeAnimationHost.prepareViewPagers");
      env->ExceptionClear();
    }
  }
  if (dispatch_pre_draw != nullptr && !env->ExceptionCheck()) {
    env->CallStaticBooleanMethod(animation_host_class, dispatch_pre_draw, view);
    if (env->ExceptionCheck()) {
      dump_pending_exception("ProbeAnimationHost.dispatchPreDraw");
      env->ExceptionClear();
    }
  }
  if (invalidate_view_tree != nullptr && !env->ExceptionCheck()) {
    env->CallStaticVoidMethod(animation_host_class, invalidate_view_tree, view);
    if (env->ExceptionCheck()) {
      dump_pending_exception("ProbeAnimationHost.invalidateViewTree");
      env->ExceptionClear();
    }
  }
  const uint32_t pending_pressed_action = state->pending_pressed_action;
  const jfloat pending_pressed_x = state->pending_pressed_x;
  const jfloat pending_pressed_y = state->pending_pressed_y;
  state->pending_pressed_action = 0;
  if (pending_pressed_action != 0 && state->pressed_view != nullptr &&
      set_pressed != nullptr && !env->ExceptionCheck()) {
    if (drawable_hotspot_changed != nullptr) {
      env->CallVoidMethod(state->pressed_view, drawable_hotspot_changed,
                          pending_pressed_x, pending_pressed_y);
      dump_pending_exception("View.drawableHotspotChanged");
    }
    env->CallVoidMethod(state->pressed_view, set_pressed,
                        pending_pressed_action == 1 ? JNI_TRUE : JNI_FALSE);
    dump_pending_exception("View.setPressed");
  }
  env->CallVoidMethod(view, draw, java_canvas);
  dump_pending_exception("View.draw");
  const bool draw_ok = !env->ExceptionCheck();
  const bool recording_ok = finish_recording();
  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(java_canvas);
  env->DeleteLocalRef(animation_host_class);
  env->DeleteLocalRef(animation_host_interface);
  env->DeleteLocalRef(render_node_class);
  if (!draw_ok || !recording_ok) {
    std::cerr << "ART HWUI GPU: View.draw failed\n";
    env->ExceptionClear();
    return JNI_FALSE;
  }

  auto* node = reinterpret_cast<android::uirenderer::RenderNode*>(
      static_cast<std::uintptr_t>(env->GetLongField(
          state->gpu_render_node, native_render_node)));
  if (node == nullptr) {
    std::cerr << "ART HWUI GPU: Java RenderNode native pointer missing\n";
    return JNI_FALSE;
  }
  node->mValid = true;
  std::cerr << "ART HWUI GPU: node staging needs=" << node->mNeedsDisplayListSync
            << " stagingContent=" << node->mStagingDisplayList.hasContent()
            << " stagingSize=" << node->mStagingDisplayList.getUsedSize()
            << " activeContent=" << node->mDisplayList.hasContent() << "\n";
  const size_t synchronized_nodes =
      darwin_art_hwui::sync_recorded_render_node_tree(node);
  std::cerr << "ART HWUI GPU: synchronized RenderNodes=" << synchronized_nodes
            << " activeSize=" << node->mDisplayList.getUsedSize()
            << " childNodes="
            << (node->mDisplayList.asSkiaDl() == nullptr
                    ? 0
                    : node->mDisplayList.asSkiaDl()->mChildNodes.size())
            << "\n";
  if (!node->mDisplayList || node->mDisplayList.isEmpty()) {
    std::cerr << "ART HWUI GPU: Java RenderNode produced empty display list\n";
    return JNI_FALSE;
  }
  if (darwin_art_hwui::node_subtree_has_animators(node)) {
    if (state->hwui_animation_context == nullptr) {
      state->hwui_time_lord = std::make_unique<
          android::uirenderer::renderthread::TimeLord>();
      state->hwui_time_lord->setFrameInterval(16666666);
      state->hwui_animation_context =
          std::make_unique<android::uirenderer::AnimationContext>(
              *state->hwui_time_lord);
    }
    darwin_art_hwui::register_node_subtree_animators(
        node, *state->hwui_animation_context);
  }

  if (!darwin_art_hwui::render_node_to_surface(
          env, view, state->gpu_render_node, state->gpu_surface, width, height,
          state->gpu_ripple_overlay_active, state->gpu_ripple_overlay_x,
          state->gpu_ripple_overlay_y, state->gpu_ripple_overlay_started)) {
    std::cerr << "ART HWUI GPU: drawable submit failed\n";
    return JNI_FALSE;
  }
  // Direct Metal presentation intentionally does not invoke the CPU frame
  // callback. Publish only the drawable dimensions so GPU-only APK
  // acceptance can validate the presented surface without a readback.
  darwin_art_frame_probe::record_dimensions(width, height);
  state->gpu_render_node_recorded = true;
  return JNI_TRUE;
}
#else
int prepare_gpu_surface(GraphicsState*, jint, jint) { return 0; }
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
jboolean present_gpu_content(GraphicsState*, JNIEnv*, jobject, jint, jint) {
  return JNI_FALSE;
}
#endif

}  // namespace darwin_art_graphics
