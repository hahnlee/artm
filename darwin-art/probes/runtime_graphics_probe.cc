#include <arpa/inet.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_art/darwin_art.h"
#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_framework_natives.h"
#include "darwin_provider_owners.h"
#include "darwin_hwui_gpu_mode.h"
#include "darwin_surface_bridge.h"
#include "runtime_filesystem_probe.h"
#include "runtime_network_probe.h"
#include "runtime_hwui_probe.h"
#include "runtime_elf_probe.h"
#include "runtime_abi_probe.h"
#include "runtime_process_state.h"
#include "runtime_frame_probe.h"
#include "darwin_icu_natives.h"
#include "darwin_libcore_natives.h"
#include "darwin_openjdk_natives.h"
#include "dex/art_dex_file_loader.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jni/java_vm_ext.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/throwable.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "well_known_classes.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#ifdef HIDDEN
#undef HIDDEN
#endif
#include "hwui/Canvas.h"
#define private public
#define protected public
#include "AnimationContext.h"
#include "Animator.h"
#include "AnimatorManager.h"
#include "renderthread/TimeLord.h"
#include "RenderNode.h"
#undef protected
#undef private
#include "pipeline/skia/RenderNodeDrawable.h"
#include "pipeline/skia/SkiaRecordingCanvas.h"
#include "include/core/SkSurface.h"
#endif

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#include "runtime_apk_graph.h"
#endif

#include "runtime_graphics_probe.h"

namespace darwin_art_graphics {

static jclass g_probe_canvas_class = nullptr;
static jobject g_interactive_root = nullptr;
static jobject g_gpu_render_node = nullptr;
static bool g_gpu_render_node_recorded = false;
static bool g_gpu_ripple_overlay_active = false;
static jfloat g_gpu_ripple_overlay_x = 0.0f;
static jfloat g_gpu_ripple_overlay_y = 0.0f;
static std::chrono::steady_clock::time_point g_gpu_ripple_overlay_started;
#if defined(DARWIN_ART_REAL_GRAPHICS)
static std::unique_ptr<android::uirenderer::renderthread::TimeLord>
    g_hwui_time_lord;
static std::unique_ptr<android::uirenderer::AnimationContext>
    g_hwui_animation_context;
#endif
static jobject g_pressed_view = nullptr;
static uint32_t g_pending_pressed_action = 0;
static jfloat g_pending_pressed_x = 0.0f;
static jfloat g_pending_pressed_y = 0.0f;
static jint g_interactive_width = 0;
static jint g_interactive_height = 0;
static DarwinArtSurface* g_gpu_surface = nullptr;

jobject interactive_root_for_input() { return g_interactive_root; }
jint interactive_width_for_input() { return g_interactive_width; }
jint interactive_height_for_input() { return g_interactive_height; }
jobject& pressed_view_for_input() { return g_pressed_view; }
uint32_t& pending_pressed_action_for_input() { return g_pending_pressed_action; }
jfloat& pending_pressed_x_for_input() { return g_pending_pressed_x; }
jfloat& pending_pressed_y_for_input() { return g_pending_pressed_y; }
bool& gpu_ripple_overlay_active_for_input() {
  return g_gpu_ripple_overlay_active;
}
jfloat& gpu_ripple_overlay_x_for_input() { return g_gpu_ripple_overlay_x; }
jfloat& gpu_ripple_overlay_y_for_input() { return g_gpu_ripple_overlay_y; }
std::chrono::steady_clock::time_point&
gpu_ripple_overlay_started_for_input() {
  return g_gpu_ripple_overlay_started;
}
#if defined(DARWIN_ART_REAL_GRAPHICS)
jobject gpu_render_node_for_input() { return g_gpu_render_node; }
android::uirenderer::renderthread::TimeLord* hwui_time_lord_for_input() {
  return g_hwui_time_lord.get();
}
android::uirenderer::AnimationContext* hwui_animation_context_for_input() {
  return g_hwui_animation_context.get();
}
#endif

void set_probe_canvas_class(JNIEnv* env, jclass canvas_class) {
  if (env != nullptr && g_probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(g_probe_canvas_class);
    g_probe_canvas_class = nullptr;
  }
  if (env != nullptr && canvas_class != nullptr) {
    g_probe_canvas_class =
        static_cast<jclass>(env->NewGlobalRef(canvas_class));
  }
}

bool retain_interactive_root(JNIEnv* env, jobject root, jint width, jint height) {
  if (env == nullptr || root == nullptr || env->ExceptionCheck()) {
    return false;
  }
  if (g_interactive_root != nullptr) {
    env->DeleteGlobalRef(g_interactive_root);
    g_interactive_root = nullptr;
  }
  g_interactive_root = env->NewGlobalRef(root);
  g_interactive_width = width;
  g_interactive_height = height;
  return g_interactive_root != nullptr && !env->ExceptionCheck();
}

void shutdown(JNIEnv* env) {
  if (env == nullptr) return;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (g_hwui_animation_context != nullptr) {
    g_hwui_animation_context->destroy();
    g_hwui_animation_context.reset();
  }
  g_hwui_time_lord.reset();
#endif
  if (g_gpu_render_node != nullptr) {
    env->DeleteGlobalRef(g_gpu_render_node);
    g_gpu_render_node = nullptr;
  }
  if (g_pressed_view != nullptr) {
    env->DeleteGlobalRef(g_pressed_view);
    g_pressed_view = nullptr;
  }
  if (g_interactive_root != nullptr) {
    env->DeleteGlobalRef(g_interactive_root);
    g_interactive_root = nullptr;
  }
  if (g_probe_canvas_class != nullptr) {
    env->DeleteGlobalRef(g_probe_canvas_class);
    g_probe_canvas_class = nullptr;
  }
  g_gpu_render_node_recorded = false;
  g_gpu_ripple_overlay_active = false;
  g_interactive_width = 0;
  g_interactive_height = 0;
  g_gpu_surface = nullptr;
}

#if defined(DARWIN_ART_REAL_GRAPHICS)
// Production GPU frame path: View.draw() records into AOSP's Skia
// RecordingCanvas, the resulting RenderNode is replayed directly into the
// CAMetalLayer drawable. No Bitmap, Java int[] or IOSurface CPU mapping is
// created in this path.
static jboolean ReplayGpuRenderNode(JNIEnv* env, jint width, jint height) {
  if (g_gpu_surface == nullptr || g_gpu_render_node == nullptr) {
    return JNI_FALSE;
  }
  jclass render_node_class = env->FindClass("android/graphics/RenderNode");
  jfieldID native_render_node =
      render_node_class == nullptr
          ? nullptr
          : env->GetFieldID(render_node_class, "mNativeRenderNode", "J");
  auto* node = native_render_node == nullptr
                   ? nullptr
                   : reinterpret_cast<android::uirenderer::RenderNode*>(
                         static_cast<std::uintptr_t>(env->GetLongField(
                             g_gpu_render_node, native_render_node)));
  if (node == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(render_node_class);
    return JNI_FALSE;
  }
  DarwinArtGpuFrame* frame = darwin_art_surface_gpu_begin(g_gpu_surface);
  if (frame == nullptr) {
    env->DeleteLocalRef(render_node_class);
    return JNI_FALSE;
  }
  auto* canvas = static_cast<SkCanvas*>(darwin_art_surface_gpu_canvas(frame));
  if (canvas == nullptr) {
    darwin_art_surface_gpu_end(g_gpu_surface, frame);
    env->DeleteLocalRef(render_node_class);
    return JNI_FALSE;
  }
  canvas->clear(SK_ColorTRANSPARENT);
  android::uirenderer::skiapipeline::RenderNodeDrawable drawable(
      node, canvas, false);
  drawable.forceDraw(canvas);
  if (g_gpu_ripple_overlay_active) {
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() -
                                  g_gpu_ripple_overlay_started)
                                  .count();
    const float progress = std::clamp(static_cast<float>(elapsed_ms / 2200.0),
                                      0.0f, 1.0f);
    SkPaint ripple_paint;
    ripple_paint.setAntiAlias(true);
    ripple_paint.setColor(SkColorSetARGB(
        static_cast<U8CPU>(24.0f + (1.0f - progress) * 64.0f), 30, 30, 30));
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(104.0f, 298.0f, 256.0f, 342.0f));
    canvas->drawCircle(g_gpu_ripple_overlay_x, g_gpu_ripple_overlay_y,
                       8.0f + progress * 76.0f, ripple_paint);
    canvas->restore();
  }
  const DarwinArtSurfaceResult result =
      darwin_art_surface_gpu_end(g_gpu_surface, frame);
  env->DeleteLocalRef(render_node_class);
  if (result != DARWIN_ART_SURFACE_OK) {
    return JNI_FALSE;
  }
  darwin_art_frame_probe::record_dimensions(width, height);
  return JNI_TRUE;
}

static jboolean PresentGpuContent(JNIEnv* env, jobject view, jint width,
                                  jint height) {
  if (!darwin_art::hwui_gpu_enabled()) {
    return JNI_FALSE;
  }
  if (g_gpu_surface == nullptr) {
    DarwinArtSurfaceCreateInfo info{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .title = "Darwin ART · HWUI Metal",
        .visible = true,
    };
    DarwinArtSurfaceResult result = DARWIN_ART_SURFACE_OK;
    g_gpu_surface = darwin_art_surface_create(&info, &result);
    if (g_gpu_surface == nullptr) {
      std::cerr << "ART HWUI GPU: surface initialization failed status="
                << result << "\n";
      return JNI_FALSE;
    }
    darwin_art_surface_set_active_gpu(g_gpu_surface);
  }

  // ACTION_MOVE is intentionally replay-only. Re-recording View.draw() every
  // 16 ms replaces the display-list-owned CanvasProperty references and makes
  // RippleDrawable appear static even while RenderNodeAnimators advance.
  if (g_gpu_render_node_recorded && g_pending_pressed_action == 0) {
    return ReplayGpuRenderNode(env, width, height);
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
  if (g_gpu_render_node == nullptr && render_node_create != nullptr &&
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
      g_gpu_render_node = env->NewGlobalRef(node);
    }
    env->DeleteLocalRef(host);
    env->DeleteLocalRef(node);
    env->DeleteLocalRef(node_name);
  }
  jobject java_canvas =
      g_gpu_render_node == nullptr || begin_recording == nullptr ||
              env->ExceptionCheck()
          ? nullptr
          : env->CallObjectMethod(g_gpu_render_node, begin_recording, width,
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
    if (java_canvas != nullptr && g_gpu_render_node != nullptr &&
        end_recording != nullptr) {
      env->ExceptionClear();
      env->CallVoidMethod(g_gpu_render_node, end_recording);
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
    env->CallVoidMethod(g_gpu_render_node, end_recording);
    recording_ended = true;
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    return ok;
  };
  env->CallBooleanMethod(g_gpu_render_node, set_position, 0, 0, width, height);
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
  const uint32_t pending_pressed_action = g_pending_pressed_action;
  const jfloat pending_pressed_x = g_pending_pressed_x;
  const jfloat pending_pressed_y = g_pending_pressed_y;
  g_pending_pressed_action = 0;
  if (pending_pressed_action != 0 && g_pressed_view != nullptr &&
      set_pressed != nullptr && !env->ExceptionCheck()) {
    if (drawable_hotspot_changed != nullptr) {
      env->CallVoidMethod(g_pressed_view, drawable_hotspot_changed,
                          pending_pressed_x, pending_pressed_y);
    }
    env->CallVoidMethod(g_pressed_view, set_pressed,
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
          g_gpu_render_node, native_render_node)));
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
    if (g_hwui_animation_context == nullptr) {
      g_hwui_time_lord = std::make_unique<
          android::uirenderer::renderthread::TimeLord>();
      g_hwui_time_lord->setFrameInterval(16666666);
      g_hwui_animation_context =
          std::make_unique<android::uirenderer::AnimationContext>(
              *g_hwui_time_lord);
    }
    darwin_art_hwui::register_node_subtree_animators(
        node, *g_hwui_animation_context);
    if (std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr) {
      std::cerr << "ART HWUI animation registered context="
                << g_hwui_animation_context.get() << " has="
                << g_hwui_animation_context->hasAnimations() << "\n";
    }
  }

  DarwinArtGpuFrame* frame = darwin_art_surface_gpu_begin(g_gpu_surface);
  if (frame == nullptr) {
    std::cerr << "ART HWUI GPU: drawable begin failed\n";
    return JNI_FALSE;
  }
  auto* canvas = static_cast<SkCanvas*>(darwin_art_surface_gpu_canvas(frame));
  if (canvas == nullptr) {
    darwin_art_surface_gpu_end(g_gpu_surface, frame);
    std::cerr << "ART HWUI GPU: drawable canvas unavailable\n";
    return JNI_FALSE;
  }
  // Match SkiaPipeline's non-opaque frame initialization. The framework
  // DecorView/theme then owns the visible window background.
  canvas->clear(SK_ColorTRANSPARENT);
  android::uirenderer::skiapipeline::RenderNodeDrawable drawable(
      node, canvas, false);
  drawable.forceDraw(canvas);
  if (g_gpu_ripple_overlay_active) {
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() -
                                  g_gpu_ripple_overlay_started)
                                  .count();
    const float progress = std::clamp(static_cast<float>(elapsed_ms / 2200.0),
                                      0.0f, 1.0f);
    SkPaint ripple_paint;
    ripple_paint.setAntiAlias(true);
    ripple_paint.setColor(SkColorSetARGB(
        static_cast<U8CPU>(24.0f + (1.0f - progress) * 64.0f), 30, 30, 30));
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(104.0f, 298.0f, 256.0f, 342.0f));
    canvas->drawCircle(g_gpu_ripple_overlay_x, g_gpu_ripple_overlay_y,
                       8.0f + progress * 76.0f, ripple_paint);
    canvas->restore();
  }
  const DarwinArtSurfaceResult result =
      darwin_art_surface_gpu_end(g_gpu_surface, frame);
  if (result != DARWIN_ART_SURFACE_OK) {
    std::cerr << "ART HWUI GPU: drawable submit failed status=" << result
              << "\n";
    return JNI_FALSE;
  }
  darwin_art_frame_probe::record_dimensions(width, height);
  g_gpu_render_node_recorded = true;
  return JNI_TRUE;
}
#endif

jboolean present_content(JNIEnv* env, jclass, jobject view, jint width,
                               jint height) {
  if (view == nullptr || width <= 0 || height <= 0 || width > 4096 ||
      height > 4096) {
    return JNI_FALSE;
  }
  const darwin_art::FrameworkGraphicsBackend graphics_backend =
      darwin_art::GetFrameworkGraphicsBackend();
  const bool use_real_graphics =
      graphics_backend ==
      darwin_art::FrameworkGraphicsBackend::kAndroidGraphics;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (darwin_art::hwui_gpu_enabled()) {
    return PresentGpuContent(env, view, width, height);
  }
#endif
  jclass canvas_class = nullptr;
  jclass real_canvas_class = nullptr;
  jclass bitmap_class = nullptr;
  jclass bitmap_config_class = nullptr;
  jclass view_class = nullptr;
  jobject bitmap = nullptr;
  jobject canvas = nullptr;
  jintArray pixels = nullptr;
  jmethodID snapshot = nullptr;
  auto release_render_target = [&]() {
    env->DeleteLocalRef(pixels);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(canvas);
    env->DeleteLocalRef(bitmap);
    env->DeleteLocalRef(real_canvas_class);
    env->DeleteLocalRef(bitmap_config_class);
    env->DeleteLocalRef(bitmap_class);
  };
  if (!use_real_graphics) {
    canvas_class = g_probe_canvas_class;
    if (canvas_class == nullptr) {
      std::cerr << "ART Android view: ProbeCanvas class is not rooted\n";
      return JNI_FALSE;
    }
    art::ScopedObjectAccess soa(env);
    art::ObjPtr<art::mirror::Class> canvas_mirror =
        soa.Decode<art::mirror::Class>(canvas_class);
    canvas = soa.AddLocalReference<jobject>(canvas_mirror->AllocObject(soa.Self()));
    jmethodID initialize =
        canvas == nullptr || env->ExceptionCheck()
            ? nullptr
            : env->GetMethodID(canvas_class, "initialize", "(II)V");
    if (!env->ExceptionCheck()) {
      snapshot = env->GetMethodID(canvas_class, "snapshot", "()[I");
    }
    if (canvas != nullptr && initialize != nullptr && snapshot != nullptr &&
        !env->ExceptionCheck()) {
      env->CallVoidMethod(canvas, initialize, width, height);
    }
  } else {
    bitmap_class = env->FindClass("android/graphics/Bitmap");
    if (!env->ExceptionCheck()) {
      bitmap_config_class = env->FindClass("android/graphics/Bitmap$Config");
    }
    if (!env->ExceptionCheck()) {
      real_canvas_class = env->FindClass("android/graphics/Canvas");
    }
    canvas_class = real_canvas_class;
    jfieldID argb_8888 = nullptr;
    jobject bitmap_config = nullptr;
    jmethodID create_bitmap = nullptr;
    jmethodID canvas_constructor = nullptr;
    if (bitmap_class != nullptr && bitmap_config_class != nullptr &&
        canvas_class != nullptr && !env->ExceptionCheck()) {
      argb_8888 = env->GetStaticFieldID(
          bitmap_config_class, "ARGB_8888",
          "Landroid/graphics/Bitmap$Config;");
    }
    if (argb_8888 != nullptr && !env->ExceptionCheck()) {
      bitmap_config =
          env->GetStaticObjectField(bitmap_config_class, argb_8888);
    }
    if (bitmap_config != nullptr && !env->ExceptionCheck()) {
      create_bitmap = env->GetStaticMethodID(
          bitmap_class, "createBitmap",
          "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    }
    if (create_bitmap != nullptr && !env->ExceptionCheck()) {
      bitmap = env->CallStaticObjectMethod(bitmap_class, create_bitmap, width,
                                           height, bitmap_config);
    }
    if (bitmap != nullptr && !env->ExceptionCheck()) {
      canvas_constructor = env->GetMethodID(
          canvas_class, "<init>", "(Landroid/graphics/Bitmap;)V");
    }
    if (canvas_constructor != nullptr && !env->ExceptionCheck()) {
      canvas = env->NewObject(canvas_class, canvas_constructor, bitmap);
    }
    env->DeleteLocalRef(bitmap_config);
  }
  const bool real_target_missing = use_real_graphics && bitmap == nullptr;
  if (canvas == nullptr || real_target_missing || env->ExceptionCheck()) {
    std::cerr << "ART Android view: "
              << (use_real_graphics
                      ? "Bitmap/Canvas(Bitmap) setup failed\n"
                      : "ProbeCanvas setup failed\n");
    art::Thread* self = art::Thread::Current();
    if (self != nullptr && self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    release_render_target();
    return JNI_FALSE;
  }
  view_class = env->FindClass("android/view/View");
  auto get_view_method = [&](const char* name,
                             const char* signature) -> jmethodID {
    return view_class == nullptr || env->ExceptionCheck()
               ? nullptr
               : env->GetMethodID(view_class, name, signature);
  };
  auto get_view_field = [&](const char* name) -> jfieldID {
    return view_class == nullptr || env->ExceptionCheck()
               ? nullptr
               : env->GetFieldID(view_class, name, "I");
  };
  jmethodID layout =
      get_view_method("layout", "(IIII)V");
  jmethodID measure =
      get_view_method("measure", "(II)V");
  jmethodID draw =
      get_view_method("draw", "(Landroid/graphics/Canvas;)V");
  jfieldID view_left = get_view_field("mLeft");
  jfieldID view_top = get_view_field("mTop");
  jfieldID view_right = get_view_field("mRight");
  jfieldID view_bottom = get_view_field("mBottom");
  if (canvas == nullptr || measure == nullptr || layout == nullptr ||
      draw == nullptr || view_left == nullptr || view_top == nullptr ||
      view_right == nullptr || view_bottom == nullptr ||
      env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }

  constexpr jint kExactly = 0x40000000;
  env->CallVoidMethod(view, measure, kExactly | width, kExactly | height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  // ViewRoot normally installs the surface bounds before the first traversal.
  // The Darwin window policy owns that root, so seed the same bounds before
  // layout. This prevents a detached View from trying to notify Android's
  // accessibility/window services merely because its initial frame changed.
  env->SetIntField(view, view_left, 0);
  env->SetIntField(view, view_top, 0);
  env->SetIntField(view, view_right, width);
  env->SetIntField(view, view_bottom, height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  env->CallVoidMethod(view, draw, canvas);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  if (!use_real_graphics) {
    pixels = static_cast<jintArray>(env->CallObjectMethod(canvas, snapshot));
  } else {
    const jsize pixel_count = static_cast<jsize>(width * height);
    pixels = env->NewIntArray(pixel_count);
    jmethodID get_pixels =
        bitmap_class == nullptr || env->ExceptionCheck()
            ? nullptr
            : env->GetMethodID(bitmap_class, "getPixels", "([IIIIIII)V");
    if (pixels != nullptr && get_pixels != nullptr && !env->ExceptionCheck()) {
      env->CallVoidMethod(bitmap, get_pixels, pixels, 0, width, 0, 0, width,
                          height);
    }
  }
  if (env->ExceptionCheck() || pixels == nullptr) {
    release_render_target();
    return JNI_FALSE;
  }
  const jboolean presented =
      darwin_art_frame_probe::present(env, width, height, pixels);
  release_render_target();
  return presented;
}

}  // namespace darwin_art_graphics
