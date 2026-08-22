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
jboolean present_gpu_content(GraphicsState* state, JNIEnv* env, jobject view,
                                  jint width, jint height) {
  if (state == nullptr) return JNI_FALSE;
  if (!darwin_art::hwui_gpu_enabled()) {
    return JNI_FALSE;
  }
  if (state->gpu_surface == nullptr) {
    DarwinArtSurfaceCreateInfo info{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .title = "Darwin ART · HWUI Metal",
        .visible = true,
    };
    DarwinArtSurfaceResult result = DARWIN_ART_SURFACE_OK;
    state->gpu_surface = darwin_art_surface_create(&info, &result);
    if (state->gpu_surface == nullptr) {
      std::cerr << "ART HWUI GPU: surface initialization failed status="
                << result << "\n";
      return JNI_FALSE;
    }
    darwin_art_surface_set_active_gpu(state->gpu_surface);
  }

  // ACTION_MOVE is intentionally replay-only. Re-recording View.draw() every
  // 16 ms replaces the display-list-owned CanvasProperty references and makes
  // RippleDrawable appear static even while RenderNodeAnimators advance.
  if (state->gpu_render_node_recorded && state->pending_pressed_action == 0) {
    return darwin_art_hwui::render_node_to_surface(
               env, state->gpu_render_node, state->gpu_surface, width, height,
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
  jobject java_canvas =
      state->gpu_render_node == nullptr || begin_recording == nullptr ||
              env->ExceptionCheck()
          ? nullptr
          : env->CallObjectMethod(state->gpu_render_node, begin_recording, width,
                                  height);
  if (java_canvas != nullptr && std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr) {
    jclass canvas_class = env->FindClass("android/graphics/Canvas");
    jmethodID is_hw = canvas_class == nullptr
                          ? nullptr
                          : env->GetMethodID(canvas_class, "isHardwareAccelerated", "()Z");
    if (is_hw != nullptr && !env->ExceptionCheck()) {
      std::cerr << "ART HWUI RecordingCanvas hardware="
                << env->CallBooleanMethod(java_canvas, is_hw) << "\n";
    }
    env->ExceptionClear();
    env->DeleteLocalRef(canvas_class);
  }
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
  // Match the ViewRoot traversal contract used by the existing raster probe:
  // seed detached root bounds before layout so its children receive a stable
  // first hardware-recording pass without window-service callbacks.
  env->SetIntField(view, view_left, 0);
  env->SetIntField(view, view_top, 0);
  env->SetIntField(view, view_right, width);
  env->SetIntField(view, view_bottom, height);
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  if (env->ExceptionCheck()) {
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
  const uint32_t pending_pressed_action = state->pending_pressed_action;
  const jfloat pending_pressed_x = state->pending_pressed_x;
  const jfloat pending_pressed_y = state->pending_pressed_y;
  state->pending_pressed_action = 0;
  if (pending_pressed_action != 0 && state->pressed_view != nullptr &&
      set_pressed != nullptr && !env->ExceptionCheck()) {
    if (drawable_hotspot_changed != nullptr) {
      env->CallVoidMethod(state->pressed_view, drawable_hotspot_changed,
                          pending_pressed_x, pending_pressed_y);
    }
    env->CallVoidMethod(state->pressed_view, set_pressed,
                        pending_pressed_action == 1 ? JNI_TRUE : JNI_FALSE);
  }
  env->CallVoidMethod(view, draw, java_canvas);
  const bool draw_ok = !env->ExceptionCheck();
  const bool recording_ok = finish_recording();
  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(java_canvas);
  env->DeleteLocalRef(animation_host_class);
  env->DeleteLocalRef(animation_host_interface);
  env->DeleteLocalRef(render_node_class);
  if (!draw_ok || !recording_ok) {
    std::cerr << "ART HWUI GPU: View.draw failed\n";
    art::Thread* self = art::Thread::Current();
    if (self != nullptr && self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
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
  darwin_art_hwui::sync_recorded_render_node_tree(node);
  if (std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr) {
    std::cerr << "ART HWUI animation inspect root new="
              << node->mAnimatorManager.mNewAnimators.size()
              << " active=" << node->mAnimatorManager.mAnimators.size()
              << " handle=" << node->mAnimatorManager.mAnimationHandle
              << " children=" << node->mDisplayList.getUsedSize() << "\n";
  }
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
    if (std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr) {
      std::cerr << "ART HWUI animation registered context="
                << state->hwui_animation_context.get() << " has="
                << state->hwui_animation_context->hasAnimations() << "\n";
    }
  }

  if (!darwin_art_hwui::render_node_to_surface(
          env, state->gpu_render_node, state->gpu_surface, width, height,
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
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
jboolean present_gpu_content(GraphicsState*, JNIEnv*, jobject, jint, jint) {
  return JNI_FALSE;
}
#endif

}  // namespace darwin_art_graphics
