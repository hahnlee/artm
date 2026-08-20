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
#include "darwin_hwui_gpu_mode.h"
#include "darwin_surface_bridge.h"
#include "runtime_filesystem_probe.h"
#include "runtime_network_probe.h"
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

namespace {

class DarwinHwuiTreeObserver final : public android::uirenderer::TreeObserver {
 public:
  void onMaybeRemovedFromTree(android::uirenderer::RenderNode* node) override {
    node->onRemovedFromTree(nullptr);
  }
};

// View.draw() on a hardware RecordingCanvas stores child views in their own
// RenderNode staging display lists. Android's RenderThread normally promotes
// those lists during a MODE_FULL prepareTree traversal. This standalone host
// owns no CanvasContext yet, so perform only that promotion before replay;
// animation, damage, and layer policy remain untouched.
size_t SyncRecordedRenderNodeTree(android::uirenderer::RenderNode* node,
                                  DarwinHwuiTreeObserver* observer) {
  if (node == nullptr || observer == nullptr) return 0;
  size_t synchronized = 0;
  if (node->mDirtyPropertyFields != 0) {
    node->mDirtyPropertyFields = 0;
    node->syncProperties();
  }
  if (node->mNeedsDisplayListSync) {
    node->mNeedsDisplayListSync = false;
    node->syncDisplayList(*observer, nullptr);
    ++synchronized;
  }
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          synchronized += SyncRecordedRenderNodeTree(child, observer);
        });
  }
  return synchronized;
}

// Android's CanvasContext normally calls AnimatorManager::animateCommon().
// The Darwin host owns the same AnimationContext but intentionally keeps the
// pixel path on the Metal drawable, so reproduce only this no-damage portion
// locally rather than adding a new public HWUI ABI symbol.
void AnimateNodeWithContext(android::uirenderer::RenderNode* node,
                            android::uirenderer::AnimationContext& context) {
  if (node == nullptr) return;
  auto& manager = node->mAnimatorManager;
  const bool debug_animation = std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr;
  if (debug_animation) {
    std::cerr << "ART HWUI animation pulse node=" << node
              << " new=" << manager.mNewAnimators.size()
              << " active=" << manager.mAnimators.size()
              << " handle=" << manager.mAnimationHandle
              << " frame_ms=" << context.frameTimeMs() << "\n";
  }
  if (manager.mAnimationHandle == nullptr) return;
  manager.pushStaging();
  auto new_end = std::remove_if(
      manager.mAnimators.begin(), manager.mAnimators.end(),
      [&context](android::sp<android::uirenderer::BaseRenderNodeAnimator>& animator) {
        const bool finished = animator->animate(context);
        if (finished) animator->detach();
        return finished;
      });
  manager.mAnimators.erase(new_end, manager.mAnimators.end());
  auto* handle = manager.mAnimationHandle;
  node->mProperties.updateMatrix();
  handle->notifyAnimationsRan();
  if (debug_animation) {
    std::cerr << "ART HWUI animation pulse result active="
              << manager.mAnimators.size() << " handle="
              << manager.mAnimationHandle << "\n";
    for (const auto& animator : manager.mAnimators) {
      std::cerr << "ART HWUI animator duration=" << animator->duration()
                << " remaining=" << animator->getRemainingPlayTime()
                << " final=" << animator->finalValue() << "\n";
    }
  }
}

bool HwuiNodeSubtreeHasAnimators(android::uirenderer::RenderNode* node) {
  if (node == nullptr) return false;
  if (!node->mAnimatorManager.mNewAnimators.empty() ||
      !node->mAnimatorManager.mAnimators.empty()) {
    return true;
  }
  bool found = false;
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          found = found || HwuiNodeSubtreeHasAnimators(child);
        });
  }
  return found;
}

void RegisterHwuiNodeSubtreeAnimators(
    android::uirenderer::RenderNode* node,
    android::uirenderer::AnimationContext& context) {
  if (node == nullptr) return;
  if ((!node->mAnimatorManager.mNewAnimators.empty() ||
       !node->mAnimatorManager.mAnimators.empty()) &&
      !node->animators().hasAnimationHandle()) {
    context.addAnimatingRenderNode(*node);
  }
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          RegisterHwuiNodeSubtreeAnimators(child, context);
        });
  }
}

}  // namespace
#endif

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#include "runtime_apk_graph.h"
#endif

extern "C" int darwin_art_elf_jni_fixture_registration_status();
extern "C" int darwin_art_elf_jni_fixture_lifecycle_status();
extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status();

namespace android {
enum JNICallType {
  kJNICallTypeRegular = 1,
};
extern "C" void* NativeBridgeGetTrampoline2(void* handle, const char* name,
                                             const char* shorty, uint32_t len,
                                             JNICallType jni_call_type);
extern "C" void* OpenNativeLibrary(JNIEnv* env, int32_t target_sdk_version,
                                    const char* path, jobject class_loader,
                                    const char* caller_location,
                                    jstring library_path,
                                    bool* needs_native_bridge,
                                    char** error_msg);
extern "C" bool CloseNativeLibrary(void* handle, bool needs_native_bridge,
                                    char** error_msg);
extern "C" void NativeLoaderFreeErrorMessage(char* message);
}  // namespace android

static jint HostPageSize(JNIEnv*, jclass) { return getpagesize(); }

static bool RunAndroidElfSelfTest(JNIEnv* env, JavaVM* vm,
                                  jobject class_loader, const char* path,
                                  std::string* error) {
  bool needs_native_bridge = false;
  char* open_error = nullptr;
  void* handle = android::OpenNativeLibrary(
      env, 35, path, class_loader, nullptr, nullptr, &needs_native_bridge,
      &open_error);
  if (handle == nullptr || open_error != nullptr || !needs_native_bridge) {
    *error = open_error == nullptr ? "Android ELF open failed without detail"
                                   : open_error;
    android::NativeLoaderFreeErrorMessage(open_error);
    if (handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(handle, needs_native_bridge,
                                        &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    return false;
  }
  android::NativeLoaderFreeErrorMessage(open_error);

  void* entry = android::NativeBridgeGetTrampoline2(
      handle, "JNI_OnLoad", nullptr, 0, android::kJNICallTypeRegular);
  using JniOnLoad = jint (*)(JavaVM*, void*);
  const jint version = entry == nullptr
                           ? JNI_ERR
                           : reinterpret_cast<JniOnLoad>(entry)(vm, nullptr);

  char* close_error = nullptr;
  const bool closed =
      android::CloseNativeLibrary(handle, needs_native_bridge, &close_error);
  if (version != JNI_VERSION_1_6 || !closed || close_error != nullptr) {
    *error = close_error == nullptr
                 ? "self-testing JNI_OnLoad returned " +
                       std::to_string(static_cast<int>(version))
                 : close_error;
    android::NativeLoaderFreeErrorMessage(close_error);
    return false;
  }
  android::NativeLoaderFreeErrorMessage(close_error);
  return true;
}

static jlong NativePackedIntegerStack(JNIEnv*, jclass, jint a0, jint a1,
                                      jint a2, jint a3, jint a4, jint a5,
                                      jint spilled, jlong wide, jint tail0,
                                      jint tail1) {
  return a0 == 10 && a1 == 11 && a2 == 12 && a3 == 13 && a4 == 14 &&
                 a5 == 15 && spilled == 0x10203040 &&
                 wide == INT64_C(0x1122334455667788) &&
                 tail0 == 0x50607080 && tail1 == 0x12345678
             ? static_cast<jlong>(UINT64_C(0x13579bdf2468ace0))
             : -1;
}

static jlong NativePackedFloatingStack(JNIEnv*, jclass, jfloat a0, jfloat a1,
                                       jfloat a2, jfloat a3, jfloat a4,
                                       jfloat a5, jfloat a6, jfloat a7,
                                       jfloat spilled, jdouble wide) {
  return a0 == 1.0f && a1 == 2.0f && a2 == 3.0f && a3 == 4.0f &&
                 a4 == 5.0f && a5 == 6.0f && a6 == 7.0f && a7 == 8.0f &&
                 spilled == 9.5f && wide == 10.25
             ? static_cast<jlong>(UINT64_C(0x02468ace13579bdf))
             : -1;
}

static jlong NativePackedReferenceStack(JNIEnv*, jclass, jint a0, jint a1,
                                        jint a2, jint a3, jint a4, jint a5,
                                        jint spilled, jobject reference,
                                        jint tail) {
  return a0 == 20 && a1 == 21 && a2 == 22 && a3 == 23 && a4 == 24 &&
                 a5 == 25 && spilled == 0x23456701 && reference != nullptr &&
                 tail == 0x34567812
             ? static_cast<jlong>(UINT64_C(0x55aa55aa33cc33cc))
             : -1;
}

static jlong NativePackedNarrowStack(JNIEnv*, jclass, jint a0, jint a1,
                                     jint a2, jint a3, jint a4, jint a5,
                                     jboolean bool_value, jbyte byte_value,
                                     jchar char_value, jshort short_value,
                                     jint int_value, jlong long_value) {
  const bool valid =
      a0 == 30 && a1 == 31 && a2 == 32 && a3 == 33 && a4 == 34 && a5 == 35 &&
      bool_value == JNI_TRUE && static_cast<std::uint8_t>(byte_value) == 0x81 &&
      char_value == 0xabcd &&
      static_cast<std::uint16_t>(short_value) == 0x8765 &&
      int_value == 0x45678923 &&
      long_value == INT64_C(0x2233445566778899);
  return valid ? static_cast<jlong>(UINT64_C(0x1122aabb3344ccdd)) : -1;
}

static std::size_t g_frame_width = 0;
static std::size_t g_frame_height = 0;
static jclass g_probe_canvas_class = nullptr;
static jobject g_interactive_root = nullptr;
// Java-owned HWUI RenderNode retained across traversals. Recording through
// RenderNode.beginRecording()/endRecording() gives View.draw() the exact
// Android RecordingCanvas contract (including hardware-only RippleDrawable).
static jobject g_gpu_render_node = nullptr;
// The display list is persistent like Android's ViewRoot/RenderThread path.
// Pointer moves only replay this node; a new recording is needed for the
// initial frame and for pressed/released state transitions.
static bool g_gpu_render_node_recorded = false;
// The Android RippleDrawable remains the source of truth for pressed state
// and animators. This is only a GPU compatibility bridge for the standalone
// RenderNodeDrawable path, which cannot currently bind CanvasProperty shader
// uniforms without a full CanvasContext.
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
// Input is staged immediately before View.draw() so RippleDrawable observes
// the same hardware RecordingCanvas that Android's UI traversal supplies.
// 1 = press, 2 = release/click, 0 = no pending state transition.
static uint32_t g_pending_pressed_action = 0;
static jfloat g_pending_pressed_x = 0.0f;
static jfloat g_pending_pressed_y = 0.0f;
static jint g_interactive_width = 0;
static jint g_interactive_height = 0;
static void* g_host_context = nullptr;
static darwin_art_frame_callback_t g_frame_callback = nullptr;
static DarwinArtSurface* g_gpu_surface = nullptr;
static bool g_apk_elf_loaded = false;
static std::string g_apk_sha256;
static std::string g_apk_root_sha256;
static bool g_direct_apk_loaded = false;
static bool g_network_elf_loaded = false;

static bool IsSha256(const char* value) {
  if (value == nullptr || std::strlen(value) != 64) {
    return false;
  }
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= '0' && *cursor <= '9') ||
          (*cursor >= 'a' && *cursor <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool IsPrivateExtractedRoot(const char* path) {
  if (path == nullptr) {
    return false;
  }
  struct stat path_stat {};
  struct stat followed_stat {};
  if (lstat(path, &path_stat) != 0 || stat(path, &followed_stat) != 0 ||
      !S_ISREG(path_stat.st_mode) || path_stat.st_dev != followed_stat.st_dev ||
      path_stat.st_ino != followed_stat.st_ino ||
      (path_stat.st_mode & 0777) != 0400) {
    return false;
  }
  const std::string root_path(path);
  const std::size_t separator = root_path.rfind('/');
  if (separator == std::string::npos || separator == 0) {
    return false;
  }
  const std::string directory = root_path.substr(0, separator);
  struct stat directory_stat {};
  return lstat(directory.c_str(), &directory_stat) == 0 &&
         S_ISDIR(directory_stat.st_mode) &&
         (directory_stat.st_mode & 0777) == 0500;
}

enum class ProcessPhase {
  kNeverStarted,
  kRunning,
  kAwaitingShutdown,
  kShuttingDown,
  kShutdownComplete,
  kCreateFailed,
  kShutdownFailed,
};

struct ProcessState {
  std::mutex mutex;
  ProcessPhase phase = ProcessPhase::kNeverStarted;
  pthread_t owner_thread{};
  bool owner_thread_valid = false;
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
  std::vector<std::unique_ptr<const art::DexFile>> app_dex_files;
};

static ProcessState g_process_state;

static bool BeginProcessRun() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  if (g_process_state.phase != ProcessPhase::kNeverStarted) {
    return false;
  }
  g_process_state.phase = ProcessPhase::kRunning;
  g_process_state.owner_thread = pthread_self();
  g_process_state.owner_thread_valid = true;
  return true;
}

static void RecordCreatedRuntime(art::Thread* art_thread) {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  CHECK(art::Runtime::Current() != nullptr);
  g_process_state.java_vm = reinterpret_cast<JavaVM*>(
      art::Runtime::Current()->GetJavaVM());
  g_process_state.art_thread = art_thread;
}

static void RecordResourceRuntimeInstalled() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  CHECK(!g_process_state.resource_runtime_installed);
  g_process_state.resource_runtime_installed = true;
}

static void FinishProcessRun() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  g_process_state.phase = g_process_state.java_vm == nullptr
                              ? ProcessPhase::kCreateFailed
                              : ProcessPhase::kAwaitingShutdown;
}

// darwin_art_run_process is deliberately a one-shot process ABI for now. ART's
// Runtime::Create() leaves its main thread runnable, which is appropriate for
// the monolithic probe but not for returning to an arbitrary native host. Keep
// this guard outside the managed-work lambda so every normal exit first
// destroys ScopedObjectAccess/StackHandleScope and only then releases the
// mutator lock by returning the ART thread to kNative.
class ScopedProcessRunBoundary final {
 public:
  ScopedProcessRunBoundary() = default;

  ~ScopedProcessRunBoundary() {
    if (art_thread_ == nullptr) {
      FinishProcessRun();
      return;
    }
    CHECK_EQ(art::Thread::Current(), art_thread_);
    const art::ThreadState state = art_thread_->GetState();
    if (state == art::ThreadState::kRunnable) {
      art_thread_->TransitionFromRunnableToSuspended(
          art::ThreadState::kNative);
    } else {
      CHECK_EQ(state, art::ThreadState::kNative);
    }
    FinishProcessRun();
  }

  void SetArtThread(art::Thread* art_thread) {
    DCHECK(art_thread_ == nullptr);
    DCHECK(art_thread != nullptr);
    art_thread_ = art_thread;
  }

 private:
  art::Thread* art_thread_ = nullptr;
};

class ScopedJniLocalFrame final {
 public:
  explicit ScopedJniLocalFrame(JNIEnv* env)
      : env_(env), pushed_(env != nullptr && env->PushLocalFrame(256) == JNI_OK) {}

  ~ScopedJniLocalFrame() {
    if (pushed_) {
      env_->PopLocalFrame(nullptr);
    }
  }

  bool IsValid() const { return pushed_; }

 private:
  JNIEnv* env_;
  bool pushed_;
};

static jboolean PresentFrame(JNIEnv* env, jclass, jint width, jint height,
                             jintArray argb) {
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
      argb == nullptr) {
    return JNI_FALSE;
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (env->GetArrayLength(argb) != static_cast<jsize>(pixel_count)) {
    return JNI_FALSE;
  }
  std::vector<jint> pixels(pixel_count);
  env->GetIntArrayRegion(argb, 0, static_cast<jsize>(pixel_count), pixels.data());
  if (env->ExceptionCheck()) {
    return JNI_FALSE;
  }
  const bool presented =
      g_frame_callback == nullptr ||
      g_frame_callback(g_host_context,
                       reinterpret_cast<const std::uint32_t*>(pixels.data()),
                       static_cast<std::uint32_t>(width),
                       static_cast<std::uint32_t>(height),
                       static_cast<std::size_t>(width) * sizeof(std::uint32_t)) != 0;
  if (presented) {
    g_frame_width = static_cast<std::size_t>(width);
    g_frame_height = static_cast<std::size_t>(height);
  }
  return presented ? JNI_TRUE : JNI_FALSE;
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
  g_frame_width = static_cast<std::size_t>(width);
  g_frame_height = static_cast<std::size_t>(height);
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
  DarwinHwuiTreeObserver tree_observer;
  std::cerr << "ART HWUI GPU: node staging needs=" << node->mNeedsDisplayListSync
            << " stagingContent=" << node->mStagingDisplayList.hasContent()
            << " stagingSize=" << node->mStagingDisplayList.getUsedSize()
            << " activeContent=" << node->mDisplayList.hasContent() << "\n";
  SyncRecordedRenderNodeTree(node, &tree_observer);
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
  if (HwuiNodeSubtreeHasAnimators(node)) {
    if (g_hwui_animation_context == nullptr) {
      g_hwui_time_lord = std::make_unique<
          android::uirenderer::renderthread::TimeLord>();
      g_hwui_time_lord->setFrameInterval(16666666);
      g_hwui_animation_context =
          std::make_unique<android::uirenderer::AnimationContext>(
              *g_hwui_time_lord);
    }
    RegisterHwuiNodeSubtreeAnimators(node, *g_hwui_animation_context);
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
  g_frame_width = static_cast<std::size_t>(width);
  g_frame_height = static_cast<std::size_t>(height);
  g_gpu_render_node_recorded = true;
  return JNI_TRUE;
}
#endif

static jboolean PresentContent(JNIEnv* env, jclass, jobject view, jint width,
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
  const jboolean presented = PresentFrame(env, nullptr, width, height, pixels);
  release_render_target();
  return presented;
}

static jobject FindClickableViewAt(JNIEnv* env, jobject view, jfloat x,
                                   jfloat y) {
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
        jobject result =
            FindClickableViewAt(env, child, x - left, y - top);
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
  art::Thread* art_thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    if (g_process_state.phase != ProcessPhase::kAwaitingShutdown ||
        !g_process_state.owner_thread_valid ||
        pthread_equal(g_process_state.owner_thread, pthread_self()) == 0 ||
        g_process_state.art_thread == nullptr || g_interactive_root == nullptr) {
      return 72;
    }
    art_thread = g_process_state.art_thread;
  }
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }

  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
  jobject hit = FindClickableViewAt(env, g_interactive_root, x, y);
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

  if (action == 0u) {
    g_gpu_ripple_overlay_active = true;
    g_gpu_ripple_overlay_x = x;
    g_gpu_ripple_overlay_y = y;
    g_gpu_ripple_overlay_started = std::chrono::steady_clock::now();
    if (g_pressed_view != nullptr) {
      env->CallVoidMethod(g_pressed_view, set_pressed, JNI_FALSE);
      env->DeleteGlobalRef(g_pressed_view);
      g_pressed_view = nullptr;
    }
    if (hit != nullptr && !env->ExceptionCheck()) {
      g_pressed_view = env->NewGlobalRef(hit);
      g_pending_pressed_action = 1;
      g_pending_pressed_x = x;
      g_pending_pressed_y = y;
    }
  } else if (action == 2u) {
    if (g_pressed_view != nullptr && !env->ExceptionCheck()) {
      env->CallVoidMethod(g_pressed_view, drawable_hotspot_changed, x, y);
    }
  } else if (action == 1u) {
    if (g_pressed_view != nullptr) {
      g_pending_pressed_action = 2;
      g_pending_pressed_x = x;
      g_pending_pressed_y = y;
    }
  }
  const bool same_pressed_view =
      action == 1u && hit != nullptr && g_pressed_view != nullptr &&
      env->IsSameObject(hit, g_pressed_view) == JNI_TRUE;
  env->DeleteLocalRef(hit);
  env->DeleteLocalRef(view_class);
  const bool rendered = !env->ExceptionCheck() &&
                        PresentContent(env, nullptr, g_interactive_root,
                                       g_interactive_width,
                                       g_interactive_height) == JNI_TRUE;
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android input: click dispatch threw\n"
              << art_thread->GetException()->Dump() << "\n";
    art_thread->ClearException();
  }
  if (action == 1u && g_pressed_view != nullptr) {
    if (same_pressed_view && perform_click != nullptr &&
        !env->ExceptionCheck()) {
      env->CallBooleanMethod(g_pressed_view, perform_click);
    }
    env->DeleteGlobalRef(g_pressed_view);
    g_pressed_view = nullptr;
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
  (void)frame_time_nanos;
  art::Thread* art_thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    if (g_process_state.phase != ProcessPhase::kAwaitingShutdown ||
        !g_process_state.owner_thread_valid ||
        pthread_equal(g_process_state.owner_thread, pthread_self()) == 0 ||
        g_process_state.art_thread == nullptr) {
      return 72;
    }
    art_thread = g_process_state.art_thread;
  }
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }
  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (g_hwui_animation_context != nullptr && g_gpu_render_node != nullptr) {
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
    if (node != nullptr && !env->ExceptionCheck()) {
      g_hwui_time_lord->vsyncReceived(frame_time_nanos, frame_time_nanos, 0,
                                      frame_time_nanos + 16666666, 16666666);
      g_hwui_animation_context->startFrame(
          android::uirenderer::TreeInfo::MODE_FULL);
      AnimateNodeWithContext(node, *g_hwui_animation_context);
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
    // The framework constructor creates its seven default FrameTimeline
    // entries. Keep one preferred timeline and provide the actual display
    // interval so Choreographer's frame-time bookkeeping can advance.
    env->SetLongField(vsync_data, frame_interval, 16666666);
    env->SetIntField(vsync_data, frame_timelines_length, 1);
    env->SetIntField(vsync_data, preferred_frame_timeline, 0);
  }
  if (instance != nullptr && do_frame != nullptr && vsync_data != nullptr &&
      !env->ExceptionCheck()) {
    // Match Android's System.nanoTime() domain. The host-side Rust Instant is
    // intentionally not used as a timestamp because its public API exposes
    // only a process-relative duration.
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

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_run_process(
    const darwin_art_process_config_t* config,
    darwin_art_process_result_t* run_result) {
  if (config == nullptr || run_result == nullptr ||
      config->struct_size < sizeof(darwin_art_process_config_t) ||
      run_result->struct_size < sizeof(darwin_art_process_result_t) ||
      config->abi_version != DARWIN_ART_ABI_VERSION ||
      run_result->abi_version != DARWIN_ART_ABI_VERSION ||
      config->core_oj_jar == nullptr || config->core_libart_jar == nullptr ||
      config->framework_jar == nullptr || config->core_icu4j_jar == nullptr ||
      config->app_dex == nullptr) {
    std::cerr << "darwin_art_run_process: invalid ABI/configuration\n";
    return 64;
  }

  const uint64_t heap_initial = config->heap_initial_bytes == 0
                                    ? 64u * 1024u * 1024u
                                    : config->heap_initial_bytes;
  const uint64_t heap_maximum = config->heap_maximum_bytes == 0
                                    ? 64u * 1024u * 1024u
                                    : config->heap_maximum_bytes;
  if (heap_initial > heap_maximum || heap_maximum > 256u * 1024u * 1024u) {
    std::cerr << "darwin_art_run_process: invalid heap bounds\n";
    return 65;
  }

  if (!BeginProcessRun()) {
    std::cerr << "darwin_art_run_process: process already started\n";
    return DARWIN_ART_STATUS_PROCESS_ALREADY_STARTED;
  }
  ScopedProcessRunBoundary process_boundary;
  const char* elf_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_ELF_JNI_FIXTURE");
  const bool run_elf_jni_fixture =
      elf_fixture_path != nullptr && elf_fixture_path[0] != '\0';
  const char* generic_elf_path =
      std::getenv("DARWIN_ART_ANDROID_ELF_GENERIC_FIXTURE");
  const bool run_generic_elf =
      generic_elf_path != nullptr && generic_elf_path[0] != '\0';
  const char* apk_elf_path =
      std::getenv("DARWIN_ART_ANDROID_APK_ELF_FIXTURE");
  const char* apk_sha256 = std::getenv("DARWIN_ART_ANDROID_APK_SHA256");
  const char* apk_root_sha256 =
      std::getenv("DARWIN_ART_ANDROID_APK_ROOT_SHA256");
  const bool run_apk_elf =
      apk_elf_path != nullptr && apk_elf_path[0] != '\0' &&
      IsSha256(apk_sha256) && IsSha256(apk_root_sha256) &&
      IsPrivateExtractedRoot(apk_elf_path) &&
      run_generic_elf && std::strcmp(apk_elf_path, generic_elf_path) == 0;
  const char* direct_apk_path =
      std::getenv("DARWIN_ART_DIRECT_APK_FIXTURE");
  const char* direct_apk_root = std::getenv("DARWIN_ART_DIRECT_APK_ROOT");
  const bool run_direct_apk =
      direct_apk_path != nullptr && direct_apk_path[0] != '\0' &&
      direct_apk_root != nullptr && direct_apk_root[0] != '\0';
  const char* libcxx_collections_path =
      std::getenv("DARWIN_ART_ANDROID_LIBCXX_COLLECTIONS_FIXTURE");
  const char* libcxx_exception_path =
      std::getenv("DARWIN_ART_ANDROID_LIBCXX_EXCEPTION_FIXTURE");
  const bool run_libcxx_acceptance =
      libcxx_collections_path != nullptr && libcxx_collections_path[0] != '\0' &&
      libcxx_exception_path != nullptr && libcxx_exception_path[0] != '\0';
  const char* tls_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_TLS_FIXTURE");
  const bool run_tls_acceptance =
      tls_fixture_path != nullptr && tls_fixture_path[0] != '\0';
  const char* network_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_NETWORK_FIXTURE");
  const bool run_network_acceptance =
      network_fixture_path != nullptr && network_fixture_path[0] != '\0';
  const char* apk_app_package = std::getenv("DARWIN_ART_APK_APP_PACKAGE");
  const char* apk_app_activity = std::getenv("DARWIN_ART_APK_APP_ACTIVITY");
  const char* apk_app_descriptor =
      std::getenv("DARWIN_ART_APK_APP_DESCRIPTOR");
  const char* apk_app_support_dex =
      std::getenv("DARWIN_ART_APK_APP_SUPPORT_DEX");
  const char* framework_res_apk =
      std::getenv("DARWIN_ART_FRAMEWORK_RES_APK");
  const char* window_scale_value =
      std::getenv("DARWIN_ART_WINDOW_SCALE");
  const bool has_apk_app_identity_environment =
      apk_app_package != nullptr || apk_app_activity != nullptr ||
      apk_app_descriptor != nullptr || apk_app_support_dex != nullptr;
  const bool run_apk_app =
      apk_app_package != nullptr && apk_app_package[0] != '\0' &&
      apk_app_activity != nullptr && apk_app_activity[0] != '\0' &&
      apk_app_descriptor != nullptr && apk_app_descriptor[0] == 'L' &&
      apk_app_support_dex != nullptr && apk_app_support_dex[0] != '\0' &&
      framework_res_apk != nullptr && framework_res_apk[0] == '/' &&
      std::strlen(apk_app_descriptor) >= 3u &&
      std::strlen(apk_app_descriptor) <= 513u &&
      apk_app_descriptor[std::strlen(apk_app_descriptor) - 1u] == ';';
  const bool run_framework_button =
      !has_apk_app_identity_environment &&
      std::getenv("DARWIN_ART_TEST_FONTS_XML") != nullptr &&
      framework_res_apk != nullptr && framework_res_apk[0] == '/';
  const bool use_framework_resources = run_apk_app || run_framework_button;
  const bool valid_window_scale =
      window_scale_value == nullptr ||
      std::strcmp(window_scale_value, "1") == 0 ||
      std::strcmp(window_scale_value, "2") == 0;
  const jint window_scale =
      run_apk_app && window_scale_value != nullptr &&
              std::strcmp(window_scale_value, "2") == 0
          ? 2
          : 1;
  constexpr jint kApkFrameWidth = 360;
  constexpr jint kApkFrameHeight = 640;
  const bool expect_apk_widgets =
      run_apk_app &&
      std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS") != nullptr &&
      std::strcmp(std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS"), "1") == 0;
  if ((has_apk_app_identity_environment && !run_apk_app) ||
      (framework_res_apk != nullptr && !use_framework_resources) ||
      !valid_window_scale) {
    std::cerr << "ART Android APK app environment is incomplete or invalid\n";
    return 48;
  }
  if (run_network_acceptance &&
      (run_elf_jni_fixture || run_generic_elf || run_apk_elf ||
       run_libcxx_acceptance || run_tls_acceptance || run_direct_apk)) {
    std::cerr << "ART Android network fixture requires an isolated process\n";
    return 47;
  }

  // Darwin's malloc zones can claim the fixed compressed-reference window
  // while RuntimeArgumentMap is being assembled. Reserve ART's bounded arena
  // after the one-shot process gate, but before the launcher performs its first
  // heap allocation. MemMap::Init itself is process-global and not safe for
  // concurrent callers.
  art::MemMap::Init();

  g_host_context = config->host_context;
  g_frame_callback = config->frame_callback;
  g_frame_width = 0;
  g_frame_height = 0;

  if (!darwin_art::InitializeFrameworkGraphicsRuntime()) {
    std::cerr << "ART Darwin graphics: runtime initialization failed\n";
    return 36;
  }

  std::string boot_class_path =
      std::string(config->core_oj_jar) + ":" + config->core_libart_jar + ":" +
      config->framework_jar + ":" + config->core_icu4j_jar;
  std::cerr << "Mach-O slide: 0x" << std::hex << _dyld_get_image_vmaddr_slide(0)
            << std::dec << "\n";
  art::Locks::Init();
  art::RuntimeArgumentMap options;
  options.Set(art::RuntimeArgumentMap::BootClassPath,
              art::ParseStringList<':'>::Split(boot_class_path));
  options.Set(art::RuntimeArgumentMap::BootClassPathLocations,
              art::ParseStringList<':'>::Split(boot_class_path));
  options.Set(art::RuntimeArgumentMap::Interpret, true);
  options.Set(art::RuntimeArgumentMap::UseJitCompilation, false);
  // Android's normal launcher always supplies a concrete growth limit. A
  // directly constructed RuntimeArgumentMap leaves this key at zero, which
  // MallocSpace interprets as zero capacity rather than "unlimited".
  options.Set(art::RuntimeArgumentMap::HeapGrowthLimit,
              art::MemoryKiB(heap_initial));
  options.Set(art::RuntimeArgumentMap::MemoryMaximumSize,
              art::MemoryKiB(heap_maximum));
  art::LogVerbosity verbosity{};
  verbosity.heap = true;
  options.Set(art::RuntimeArgumentMap::Verbose, verbosity);

  if (!art::Runtime::Create(std::move(options))) {
    return 1;
  }

  art::Thread* self = art::Thread::Current();
  RecordCreatedRuntime(self);
  if (self == nullptr) {
    std::cerr << "ART Darwin DEX: no current thread\n";
    return 2;
  }
  JNIEnv* env = self->GetJniEnv();

  process_boundary.SetArtThread(self);
  return [&]() -> int32_t {

  art::interpreter::UnstartedRuntime::Initialize();
  art::ScopedObjectAccess soa(self);
  ScopedJniLocalFrame local_frame(self->GetJniEnv());
  if (!local_frame.IsValid()) {
    std::cerr << "ART Darwin JNI: local frame allocation failed\n";
    return 34;
  }
  art::WellKnownClasses::Init(self->GetJniEnv());

  std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files =
      g_process_state.app_dex_files;
  CHECK(app_dex_files.empty());
  std::string dex_error;
  if (run_apk_app) {
    art::ArtDexFileLoader support_loader(apk_app_support_dex);
    if (!support_loader.Open(/* verify= */ true,
                             /* verify_checksum= */ true, &dex_error,
                             &app_dex_files)) {
      std::cerr << "ART Darwin support DEX: open failed: " << dex_error << "\n";
      return 3;
    }
  }
  art::ArtDexFileLoader dex_loader(config->app_dex);
  if (!dex_loader.Open(/* verify= */ true,
                       /* verify_checksum= */ true, &dex_error,
                       &app_dex_files)) {
    std::cerr << "ART Darwin DEX: open failed: " << dex_error << "\n";
    return 3;
  }
  std::vector<const art::DexFile*> app_dex_file_ptrs;
  app_dex_file_ptrs.reserve(app_dex_files.size());
  for (const auto& dex_file : app_dex_files) {
    app_dex_file_ptrs.push_back(dex_file.get());
  }

  art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();
  jobject loader_ref =
      class_linker->CreatePathClassLoader(self, app_dex_file_ptrs);
  art::StackHandleScope<13> hs(self);
  art::Handle<art::mirror::ClassLoader> app_loader =
      hs.NewHandle(soa.Decode<art::mirror::ClassLoader>(loader_ref));
  jobject app_loader_ref = soa.AddLocalReference<jobject>(app_loader.Get());
  for (const auto& dex_file : app_dex_files) {
    if (class_linker->RegisterDexFile(*dex_file, app_loader.Get()) == nullptr) {
      std::cerr << "ART Darwin DEX: registration failed\n";
      return 4;
    }
  }

  art::Handle<art::mirror::Class> hello = hs.NewHandle(class_linker->FindClass(
      self, "Ldev/darwinart/probe/Hello;",
      sizeof("Ldev/darwinart/probe/Hello;") - 1u, app_loader));
  if (hello == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: Hello class lookup failed\n";
    return 5;
  }

  art::Handle<art::mirror::Class> framework_activity = hs.NewHandle(
      class_linker->FindSystemClass(self, "Landroid/app/Activity;"));
  if (framework_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity class lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  const char* activity_descriptor =
      run_apk_app ? apk_app_descriptor : "Ldev/darwinart/probe/ProbeActivity;";
  art::Handle<art::mirror::Class> probe_activity = hs.NewHandle(
      class_linker->FindClass(self, activity_descriptor,
                              std::strlen(activity_descriptor), app_loader));
  if (probe_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_context_handle =
      hs.NewHandle(class_linker->FindClass(
          self, "Ldev/darwinart/probe/ProbeContext;",
          sizeof("Ldev/darwinart/probe/ProbeContext;") - 1u, app_loader));
  if (probe_context_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Context subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_resources_handle =
      hs.NewHandle(class_linker->FindClass(
          self, "Ldev/darwinart/probe/ProbeResources;",
          sizeof("Ldev/darwinart/probe/ProbeResources;") - 1u, app_loader));
  if (probe_resources_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Resources subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_view_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/ProbeView;",
                              sizeof("Ldev/darwinart/probe/ProbeView;") - 1u,
                              app_loader));
  if (probe_view_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android view: ProbeView lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> content_root_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/ProbeContentRoot;",
                              sizeof("Ldev/darwinart/probe/ProbeContentRoot;") - 1u,
                              app_loader));
  art::Handle<art::mirror::Class> package_manager_handle = hs.NewHandle(
      class_linker->FindClass(self,
                              "Ldev/darwinart/probe/ProbePackageManager;",
                              sizeof("Ldev/darwinart/probe/ProbePackageManager;") - 1u,
                              app_loader));
  art::MutableHandle<art::mirror::Class> native_fixture_handle(
      hs.NewHandle<art::mirror::Class>(nullptr));
  art::MutableHandle<art::mirror::Class> network_fixture_handle(
      hs.NewHandle<art::mirror::Class>(nullptr));
  if (run_direct_apk) {
    if (run_elf_jni_fixture) {
      std::cerr << "ART Android direct APK must run in its isolated host flavor\n";
      return 46;
    }
    std::string direct_error;
    bool direct_loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      direct_loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          self->GetJniEnv(), direct_apk_path, app_loader_ref, nullptr,
          &direct_error);
    }
    if (!direct_loaded || !direct_error.empty() ||
        self->GetJniEnv()->ExceptionCheck()) {
      std::cerr << "ART Android direct APK JavaVMExt load failed, load_error="
                << direct_error << "\n";
      return 46;
    }
    g_direct_apk_loaded = true;
  }

  if (run_elf_jni_fixture) {
    native_fixture_handle.Assign(class_linker->FindClass(
        self, "Ldarwin/art/nativefixture/NativeFixture;",
        sizeof("Ldarwin/art/nativefixture/NativeFixture;") - 1u, app_loader));
  }
  if (run_network_acceptance) {
    network_fixture_handle.Assign(class_linker->FindClass(
        self, "Ldev/darwinart/probe/NetworkRuntimeFixture;",
        sizeof("Ldev/darwinart/probe/NetworkRuntimeFixture;") - 1u,
        app_loader));
  }
  if (content_root_handle == nullptr || package_manager_handle == nullptr ||
      (run_elf_jni_fixture && native_fixture_handle == nullptr) ||
      (run_network_acceptance && network_fixture_handle == nullptr) ||
      self->IsExceptionPending()) {
    std::cerr << "ART Android window: Darwin Canvas/Window lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }

  jclass hello_class = soa.AddLocalReference<jclass>(hello.Get());
  jclass probe_activity_class =
      soa.AddLocalReference<jclass>(probe_activity.Get());
  jclass probe_context_class =
      soa.AddLocalReference<jclass>(probe_context_handle.Get());
  jclass probe_resources_class =
      soa.AddLocalReference<jclass>(probe_resources_handle.Get());
  jclass probe_view_class =
      soa.AddLocalReference<jclass>(probe_view_handle.Get());
  jclass probe_canvas_class = nullptr;
  if (darwin_art::GetFrameworkGraphicsBackend() ==
      darwin_art::FrameworkGraphicsBackend::kProbeCanvas) {
    art::ObjPtr<art::mirror::Class> probe_canvas = class_linker->FindClass(
        self, "Ldev/darwinart/probe/ProbeCanvas;",
        sizeof("Ldev/darwinart/probe/ProbeCanvas;") - 1u, app_loader);
    if (probe_canvas == nullptr || self->IsExceptionPending()) {
      std::cerr << "ART Android window: ProbeCanvas lookup failed\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 21;
    }
    probe_canvas_class = soa.AddLocalReference<jclass>(probe_canvas);
  }
  jclass content_root_class =
      soa.AddLocalReference<jclass>(content_root_handle.Get());
  jclass package_manager_class =
      soa.AddLocalReference<jclass>(package_manager_handle.Get());
  jclass native_fixture_class =
      run_elf_jni_fixture
          ? soa.AddLocalReference<jclass>(native_fixture_handle.Get())
          : nullptr;
  jclass network_fixture_class =
      run_network_acceptance
          ? soa.AddLocalReference<jclass>(network_fixture_handle.Get())
          : nullptr;
  art::Runtime::Current()->StartMinimalForDarwinProbe(self->GetJniEnv());
  if (!InstallProbeAndroidSystemRoot()) {
    std::cerr << "ART Android filesystem: test system root install failed\n";
    return 40;
  }
  if (!darwin_art::RegisterLibcoreNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin libcore: native registration failed\n";
    return 17;
  }
  register_java_lang_Math(self->GetJniEnv());
  if (self->GetJniEnv()->ExceptionCheck()) {
    std::cerr << "ART Darwin OpenJDK: Math native registration failed\n";
    return 37;
  }
  if (!darwin_art::RegisterIcuCharsetNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin ICU: charset native registration failed\n";
    return 20;
  }
  if (!darwin_art::RegisterFrameworkNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin framework: native registration failed\n";
    return 26;
  }
  art::Runtime::Current()->FinishMinimalForDarwinProbe();
  if (!darwin_art::InstallFrameworkResourceRuntime(self->GetJniEnv())) {
    std::cerr << "ART Darwin resources: AndroidRuntime ownership install failed\n";
    return 38;
  }
  RecordResourceRuntimeInstalled();
  if (!darwin_art::RegisterFrameworkResourceNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin resources: native registration failed\n";
    return 39;
  }
  if (!darwin_art::RegisterFrameworkGraphicsNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin graphics: native registration failed\n";
    return 35;
  }

  // Android's ActivityThread installs the application PathClassLoader as the
  // managed thread context loader. Native framework bridges reached from a
  // boot-class method (for example ServiceManagerProxy) otherwise cannot find
  // process-local service implementations packaged in the probe/APK DEX. Do
  // this only after FinishMinimalForDarwinProbe: Thread.currentThread() is not
  // legal while ART is still in unstarted-runtime mode.
  jclass thread_class = env->FindClass("java/lang/Thread");
  jmethodID current_thread =
      thread_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(thread_class, "currentThread",
                                   "()Ljava/lang/Thread;");
  jmethodID set_context_loader =
      thread_class == nullptr
          ? nullptr
          : env->GetMethodID(thread_class, "setContextClassLoader",
                             "(Ljava/lang/ClassLoader;)V");
  jobject managed_thread =
      current_thread == nullptr
          ? nullptr
          : env->CallStaticObjectMethod(thread_class, current_thread);
  if (managed_thread == nullptr || set_context_loader == nullptr ||
      app_loader_ref == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Darwin DEX: context ClassLoader setup failed\n";
    return 4;
  }
  env->CallVoidMethod(managed_thread, set_context_loader, app_loader_ref);
  env->DeleteLocalRef(managed_thread);
  env->DeleteLocalRef(thread_class);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Darwin DEX: context ClassLoader install failed\n";
    return 4;
  }

  if (darwin_art::GetFrameworkGraphicsBackend() ==
      darwin_art::FrameworkGraphicsBackend::kProbeCanvas) {
    g_probe_canvas_class =
        static_cast<jclass>(env->NewGlobalRef(probe_canvas_class));
    if (g_probe_canvas_class == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android window: ProbeCanvas global root failed\n";
      return 32;
    }
  }
  jclass looper_class = env->FindClass("android/os/Looper");
  jmethodID prepare_main_looper =
      looper_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(looper_class, "prepareMainLooper", "()V");
  if (prepare_main_looper != nullptr) {
    env->CallStaticVoidMethod(looper_class, prepare_main_looper);
  }
  env->DeleteLocalRef(looper_class);
  if (prepare_main_looper == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android framework: Looper.prepareMainLooper() failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 25;
  }

  if (!class_linker->EnsureInitialized(self, probe_activity, true, true)) {
    std::cerr << "ART Android framework: launcher Activity initialization failed\n"
              << self->GetException()->Dump() << "\n";
    return 22;
  }
  jmethodID activity_constructor =
      env->GetMethodID(probe_activity_class, "<init>", "()V");
  jobject activity_instance =
      activity_constructor == nullptr
          ? nullptr
          : env->NewObject(probe_activity_class, activity_constructor);
  jmethodID probe_value =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_activity_class, "probeValue", "()I");
  jint activity_result =
      run_apk_app
          ? (activity_instance == nullptr ? -1 : 42)
          : (activity_instance == nullptr || probe_value == nullptr
                 ? -1
                 : env->CallIntMethod(activity_instance, probe_value));
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android framework: ProbeActivity constructor threw\n"
              << self->GetException()->Dump() << "\n";
    return 23;
  }
  if (activity_result != 42) {
    std::cerr << "ART Android framework: expected 42, got " << activity_result
              << "\n";
    return 24;
  }

  jclass activity_class = env->GetSuperclass(probe_activity_class);
  jclass activity_info_class =
      env->FindClass("android/content/pm/ActivityInfo");
  jclass application_class = env->FindClass("android/app/Application");
  jclass intent_class = env->FindClass("android/content/Intent");
  jclass component_name_class =
      env->FindClass("android/content/ComponentName");
  jclass configuration_class =
      env->FindClass("android/content/res/Configuration");
  jmethodID configuration_constructor =
      configuration_class == nullptr
          ? nullptr
          : env->GetMethodID(configuration_class, "<init>", "()V");
  jclass asset_manager_class =
      env->FindClass("android/content/res/AssetManager");
  jmethodID activity_info_constructor =
      activity_info_class == nullptr
          ? nullptr
          : env->GetMethodID(activity_info_class, "<init>", "()V");
  jmethodID application_constructor =
      application_class == nullptr
          ? nullptr
          : env->GetMethodID(application_class, "<init>", "()V");
  jobject activity_info =
      activity_info_constructor == nullptr
          ? nullptr
          : env->NewObject(activity_info_class, activity_info_constructor);
  jobject application =
      application_constructor == nullptr
          ? nullptr
          : env->NewObject(application_class, application_constructor);
  jmethodID asset_manager_constructor =
      asset_manager_class == nullptr
          ? nullptr
          : env->GetMethodID(asset_manager_class, "<init>", "(Z)V");
  jobject asset_manager =
      asset_manager_constructor == nullptr
          ? nullptr
          : env->NewObject(asset_manager_class, asset_manager_constructor,
                           JNI_TRUE);
  jclass apk_assets_class = env->FindClass("android/content/res/ApkAssets");
  jobject framework_apk_assets = nullptr;
  jstring framework_res_path = nullptr;
  jobjectArray configured_apk_assets = nullptr;
  if (use_framework_resources && apk_assets_class != nullptr &&
      asset_manager != nullptr) {
    jmethodID load_from_path = env->GetStaticMethodID(
        apk_assets_class, "loadFromPath",
        "(Ljava/lang/String;)Landroid/content/res/ApkAssets;");
    framework_res_path = env->NewStringUTF(framework_res_apk);
    framework_apk_assets =
        load_from_path == nullptr || framework_res_path == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(apk_assets_class, load_from_path,
                                          framework_res_path);
    configured_apk_assets =
        framework_apk_assets == nullptr
            ? nullptr
            : env->NewObjectArray(1, apk_assets_class, framework_apk_assets);
  } else if (apk_assets_class != nullptr) {
    configured_apk_assets = env->NewObjectArray(0, apk_assets_class, nullptr);
  }
  jfieldID apk_assets_field =
      asset_manager_class == nullptr
          ? nullptr
          : env->GetFieldID(asset_manager_class, "mApkAssets",
                            "[Landroid/content/res/ApkAssets;");
  if (!use_framework_resources && asset_manager != nullptr &&
      apk_assets_field != nullptr &&
      configured_apk_assets != nullptr) {
    env->SetObjectField(asset_manager, apk_assets_field,
                        configured_apk_assets);
  } else if (use_framework_resources && asset_manager != nullptr &&
             apk_assets_field != nullptr && configured_apk_assets != nullptr) {
    jfieldID asset_manager_object =
        env->GetFieldID(asset_manager_class, "mObject", "J");
    jmethodID native_set_apk_assets = env->GetStaticMethodID(
        asset_manager_class, "nativeSetApkAssets",
        "(J[Landroid/content/res/ApkAssets;ZZ)V");
    if (asset_manager_object != nullptr && native_set_apk_assets != nullptr) {
      const jlong native_asset_manager =
          env->GetLongField(asset_manager, asset_manager_object);
      env->CallStaticVoidMethod(asset_manager_class, native_set_apk_assets,
                                native_asset_manager, configured_apk_assets,
                                JNI_FALSE, JNI_FALSE);
      if (!env->ExceptionCheck()) {
        env->SetObjectField(asset_manager, apk_assets_field,
                            configured_apk_assets);
      }
    }
  }
  jmethodID probe_resources_constructor =
      probe_resources_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_resources_class, "<init>",
                             "(Landroid/content/res/AssetManager;Z)V");
  jmethodID configure_display_scale =
      probe_resources_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(probe_resources_class,
                                   "configureDisplayScale", "(I)V");
  if (configure_display_scale != nullptr) {
    env->CallStaticVoidMethod(probe_resources_class, configure_display_scale,
                              window_scale);
  }
  jobject probe_resources =
      probe_resources_constructor == nullptr || asset_manager == nullptr ||
              env->ExceptionCheck()
          ? nullptr
          : env->NewObject(probe_resources_class, probe_resources_constructor,
                           asset_manager,
                           use_framework_resources ? JNI_TRUE : JNI_FALSE);
  if (activity_info == nullptr || application == nullptr ||
      asset_manager == nullptr || configured_apk_assets == nullptr ||
      apk_assets_field == nullptr || configure_display_scale == nullptr ||
      probe_resources == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android resources: bootstrap construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jobject package_manager =
      package_manager_class == nullptr
          ? nullptr
          : soa.AddLocalReference<jobject>(
                package_manager_handle->AllocObject(self));
  if (package_manager == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: package feature stub failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jmethodID probe_context_constructor =
      probe_context_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_context_class, "<init>",
                             "(Landroid/content/res/Resources;"
                             "Landroid/content/pm/PackageManager;)V");
  jobject probe_context =
      probe_context_constructor == nullptr || probe_resources == nullptr ||
              package_manager == nullptr
          ? nullptr
          : env->NewObject(probe_context_class, probe_context_constructor,
                           probe_resources, package_manager);
  if (probe_context == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: ProbeContext construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jmethodID intent_constructor = intent_class == nullptr
                                    ? nullptr
                                    : env->GetMethodID(intent_class, "<init>", "()V");
  jmethodID component_name_constructor =
      component_name_class == nullptr
          ? nullptr
          : env->GetMethodID(component_name_class, "<init>",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID set_component =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setComponent",
                             "(Landroid/content/ComponentName;)Landroid/content/Intent;");
  jstring package_name = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  jstring class_name = env->NewStringUTF(
      run_apk_app ? apk_app_activity : "dev.darwinart.probe.ProbeActivity");
  jstring title =
      env->NewStringUTF(run_apk_app ? "Darwin ART APK" : "Darwin ART Probe");
  jobject component_name =
      component_name_constructor == nullptr
          ? nullptr
          : env->NewObject(component_name_class, component_name_constructor,
                           package_name, class_name);
  jobject intent = intent_constructor == nullptr
                       ? nullptr
                       : env->NewObject(intent_class, intent_constructor);
  jobject configuration =
      configuration_constructor == nullptr
          ? nullptr
          : env->NewObject(configuration_class, configuration_constructor);
  if (intent != nullptr && set_component != nullptr && component_name != nullptr) {
    jobject configured_intent =
        env->CallObjectMethod(intent, set_component, component_name);
    env->DeleteLocalRef(configured_intent);
  }
  static constexpr const char* kActivityAttachSignature =
      "(Landroid/content/Context;Landroid/app/ActivityThread;"
      "Landroid/app/Instrumentation;Landroid/os/IBinder;I"
      "Landroid/app/Application;Landroid/content/Intent;"
      "Landroid/content/pm/ActivityInfo;Ljava/lang/CharSequence;"
      "Landroid/app/Activity;Ljava/lang/String;"
      "Landroid/app/Activity$NonConfigurationInstances;"
      "Landroid/content/res/Configuration;Ljava/lang/String;"
      "Lcom/android/internal/app/IVoiceInteractor;Landroid/view/Window;"
      "Landroid/view/ViewRootImpl$ActivityConfigCallback;"
      "Landroid/os/IBinder;Landroid/os/IBinder;)V";
  jmethodID attach_activity =
      activity_class == nullptr
          ? nullptr
          : env->GetMethodID(activity_class, "attach", kActivityAttachSignature);
  if (activity_info == nullptr || application == nullptr ||
      probe_context == nullptr || intent == nullptr || configuration == nullptr ||
      attach_activity == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jclass context_theme_wrapper_class =
      env->FindClass("android/view/ContextThemeWrapper");
  jmethodID attach_base_context =
      context_theme_wrapper_class == nullptr
          ? nullptr
          : env->GetMethodID(context_theme_wrapper_class, "attachBaseContext",
                             "(Landroid/content/Context;)V");
  if (!run_apk_app && attach_base_context != nullptr) {
    env->CallNonvirtualVoidMethod(activity_instance,
                                  context_theme_wrapper_class,
                                  attach_base_context,
                                  probe_context);
  }
  if ((!run_apk_app && attach_base_context == nullptr) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: base Context preparation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 30;
  }
  env->CallNonvirtualVoidMethod(
      activity_instance,
      activity_class,
      attach_activity,
      probe_context,
      nullptr,
      nullptr,
      nullptr,
      static_cast<jint>(1),
      application,
      intent,
      activity_info,
      title,
      nullptr,
      nullptr,
      nullptr,
      configuration,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() threw\n"
              << self->GetException()->Dump() << "\n";
    return 30;
  }
  jmethodID get_window =
      env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject window = get_window == nullptr
                       ? nullptr
                       : env->CallObjectMethod(activity_instance, get_window);
  jclass window_class = env->FindClass("android/view/Window");
  jclass phone_window_class =
      env->FindClass("com/android/internal/policy/PhoneWindow");
  if (window == nullptr || phone_window_class == nullptr ||
      !env->IsInstanceOf(window, phone_window_class) || env->ExceptionCheck()) {
    std::cerr << "ART Android window: PhoneWindow attachment failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }

  jmethodID get_probe_theme = env->GetMethodID(
      probe_context_class, "getTheme", "()Landroid/content/res/Resources$Theme;");
  jobject probe_theme =
      get_probe_theme == nullptr
          ? nullptr
          : env->CallObjectMethod(probe_context, get_probe_theme);
  const bool use_framework_material_theme = use_framework_resources;
  if (use_framework_material_theme && probe_theme != nullptr) {
    jclass theme_class = env->GetObjectClass(probe_theme);
    jclass framework_style_class = env->FindClass("android/R$style");
    jfieldID framework_light_no_action_bar =
        framework_style_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_style_class,
                                    "Theme_Material_Light_NoActionBar", "I");
    jmethodID apply_style =
        theme_class == nullptr
            ? nullptr
            : env->GetMethodID(theme_class, "applyStyle", "(IZ)V");
    if (framework_light_no_action_bar != nullptr && apply_style != nullptr) {
      const jint style = env->GetStaticIntField(framework_style_class,
                                                framework_light_no_action_bar);
      env->CallVoidMethod(probe_theme, apply_style, style, JNI_TRUE);
    }
    env->DeleteLocalRef(framework_style_class);
    env->DeleteLocalRef(theme_class);
  }
  jmethodID set_activity_theme = env->GetMethodID(
      context_theme_wrapper_class, "setTheme",
      "(Landroid/content/res/Resources$Theme;)V");
  if (probe_theme == nullptr || set_activity_theme == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity theme setup failed\n";
    return 31;
  }
  env->CallVoidMethod(activity_instance, set_activity_theme, probe_theme);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.setTheme() threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  // A detached hierarchy should observe accessibility as disabled until the
  // Darwin service bridge exists. Seed the framework singleton without
  // invoking its Binder-backed constructor.
  jclass accessibility_class =
      env->FindClass("android/view/accessibility/AccessibilityManager");
  jobject accessibility =
      accessibility_class == nullptr ? nullptr : env->AllocObject(accessibility_class);
  jclass object_class = env->FindClass("java/lang/Object");
  jmethodID object_constructor =
      object_class == nullptr
          ? nullptr
          : env->GetMethodID(object_class, "<init>", "()V");
  jobject accessibility_lock =
      object_constructor == nullptr
          ? nullptr
          : env->NewObject(object_class, object_constructor);
  jfieldID accessibility_lock_field =
      accessibility_class == nullptr
          ? nullptr
          : env->GetFieldID(accessibility_class, "mLock", "Ljava/lang/Object;");
  jfieldID accessibility_instance =
      accessibility_class == nullptr
          ? nullptr
          : env->GetStaticFieldID(
                accessibility_class, "sInstance",
                "Landroid/view/accessibility/AccessibilityManager;");
  if (accessibility == nullptr || accessibility_lock == nullptr ||
      accessibility_lock_field == nullptr || accessibility_instance == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: accessibility stub setup failed\n";
    return 31;
  }
  env->SetObjectField(accessibility, accessibility_lock_field,
                      accessibility_lock);
  env->SetStaticObjectField(accessibility_class, accessibility_instance,
                            accessibility);

  jmethodID get_window_attributes =
      window_class == nullptr
          ? nullptr
          : env->GetMethodID(window_class, "getAttributes",
                             "()Landroid/view/WindowManager$LayoutParams;");
  jobject window_attributes =
      get_window_attributes == nullptr
          ? nullptr
          : env->CallObjectMethod(window, get_window_attributes);
  jclass decor_view_class =
      env->FindClass("com/android/internal/policy/DecorView");
  jmethodID decor_view_constructor =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(
                decor_view_class, "<init>",
                "(Landroid/content/Context;I"
                "Lcom/android/internal/policy/PhoneWindow;"
                "Landroid/view/WindowManager$LayoutParams;)V");
  jobject decor_view =
      decor_view_constructor == nullptr || window_attributes == nullptr
          ? nullptr
          : env->NewObject(decor_view_class, decor_view_constructor,
                           activity_instance, static_cast<jint>(-1), window,
                           window_attributes);
  // PhoneWindow.installDecor() normally resolves windowBackground from the
  // active Theme and installs it on DecorView. The standalone launcher builds
  // the same objects directly, so preserve that framework-owned resource path
  // explicitly instead of substituting a host color.
  jobject window_background = nullptr;
  if (use_framework_resources && decor_view != nullptr) {
    jclass typed_value_class = env->FindClass("android/util/TypedValue");
    jmethodID typed_value_constructor =
        typed_value_class == nullptr
            ? nullptr
            : env->GetMethodID(typed_value_class, "<init>", "()V");
    jobject typed_value =
        typed_value_constructor == nullptr
            ? nullptr
            : env->NewObject(typed_value_class, typed_value_constructor);
    jclass theme_class = env->GetObjectClass(probe_theme);
    jmethodID resolve_attribute =
        theme_class == nullptr
            ? nullptr
            : env->GetMethodID(theme_class, "resolveAttribute",
                               "(ILandroid/util/TypedValue;Z)Z");
    jclass framework_attr_class = env->FindClass("android/R$attr");
    jfieldID window_background_attr =
        framework_attr_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_attr_class, "windowBackground",
                                    "I");
    jfieldID typed_value_resource_id =
        typed_value_class == nullptr
            ? nullptr
            : env->GetFieldID(typed_value_class, "resourceId", "I");
    jmethodID get_drawable =
        probe_resources_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  probe_resources_class, "getDrawable",
                  "(ILandroid/content/res/Resources$Theme;)"
                  "Landroid/graphics/drawable/Drawable;");
    if (typed_value != nullptr && resolve_attribute != nullptr &&
        window_background_attr != nullptr &&
        typed_value_resource_id != nullptr && get_drawable != nullptr) {
      const jint attr = env->GetStaticIntField(framework_attr_class,
                                               window_background_attr);
      const jboolean resolved = env->CallBooleanMethod(
          probe_theme, resolve_attribute, attr, typed_value, JNI_TRUE);
      const jint resource_id =
          resolved == JNI_TRUE && !env->ExceptionCheck()
              ? env->GetIntField(typed_value, typed_value_resource_id)
              : 0;
      if (resource_id != 0) {
        window_background = env->CallObjectMethod(
            probe_resources, get_drawable, resource_id, probe_theme);
      }
    }
    env->DeleteLocalRef(framework_attr_class);
    env->DeleteLocalRef(theme_class);
    env->DeleteLocalRef(typed_value);
    env->DeleteLocalRef(typed_value_class);
  }
  jmethodID content_root_constructor =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "<init>",
                             "(Landroid/content/Context;)V");
  jobject content_root =
      content_root_constructor == nullptr
          ? nullptr
          : env->NewObject(content_root_class, content_root_constructor,
                           activity_instance);
  jmethodID add_view =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(decor_view_class, "addView",
                             "(Landroid/view/View;)V");
  jfieldID phone_decor =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mDecor",
                            "Lcom/android/internal/policy/DecorView;");
  jfieldID phone_content_parent =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mContentParent",
                            "Landroid/view/ViewGroup;");
  if (decor_view == nullptr ||
      (use_framework_resources && window_background == nullptr) ||
      content_root == nullptr || add_view == nullptr ||
      phone_decor == nullptr || phone_content_parent == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }
  env->CallVoidMethod(decor_view, add_view, content_root);
  env->SetObjectField(window, phone_decor, decor_view);
  env->SetObjectField(window, phone_content_parent, content_root);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView attachment threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  jmethodID probe_on_create =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "onCreate",
                             "(Landroid/os/Bundle;)V")
          : env->GetMethodID(probe_activity_class, "probeOnCreate", "()I");
  jint lifecycle_result = -1;
  if (probe_on_create != nullptr) {
    if (run_apk_app) {
      env->CallVoidMethod(activity_instance, probe_on_create, nullptr);
      lifecycle_result = env->ExceptionCheck() ? -1 : 43;
    } else {
      lifecycle_result =
          env->CallIntMethod(activity_instance, probe_on_create);
    }
  }
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android lifecycle: Activity.onCreate() threw\n"
              << self->GetException()->Dump() << "\n";
    return 28;
  }
  // PhoneWindow applies the resolved theme background while installing its
  // decor.  The standalone launcher supplies the decor before Activity's
  // setContentView(), so finish the same Android-owned operation after the
  // activity has installed its content.  Going through PhoneWindow keeps the
  // Drawable callback/window-background state in the framework path.
  if (use_framework_resources && window_background != nullptr) {
    jmethodID set_window_background =
        phone_window_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  phone_window_class, "setBackgroundDrawable",
                  "(Landroid/graphics/drawable/Drawable;)V");
    if (set_window_background == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android window: PhoneWindow background setup failed\n";
      env->ExceptionClear();
      return 31;
    }
    env->CallVoidMethod(window, set_window_background, window_background);
    if (env->ExceptionCheck()) {
      std::cerr << "ART Android window: PhoneWindow background setup threw\n"
                << self->GetException()->Dump() << "\n";
      return 31;
    }
  }
  // We just installed this exact DecorView in PhoneWindow.mDecor above.  The
  // detached probe Window has no ViewRoot to lazily materialize a decor, so
  // relying on PhoneWindow.getDecorView() here can legitimately return null.
  // Keep the authoritative local object instead.
  jobject attached_decor = decor_view;
  jmethodID get_child_at =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "getChildAt",
                             "(I)Landroid/view/View;");
  jobject probe_view =
      get_child_at == nullptr
          ? nullptr
          : env->CallObjectMethod(content_root, get_child_at,
                                  static_cast<jint>(0));
  bool widgets_valid = true;
  if (expect_apk_widgets) {
    static constexpr const char* kWidgetTags[]{
        "title", "checkbox", "radio", "toggle", "seek", "progress", "button"};
    static constexpr const char* kWidgetClasses[]{
        "android/widget/TextView",      "android/widget/CheckBox",
        "android/widget/RadioButton",  "android/widget/ToggleButton",
        "android/widget/SeekBar",      "android/widget/ProgressBar",
        "android/widget/Button"};
    jmethodID find_view_with_tag =
        content_root_class == nullptr
            ? nullptr
            : env->GetMethodID(content_root_class, "findViewWithTag",
                               "(Ljava/lang/Object;)Landroid/view/View;");
    widgets_valid = find_view_with_tag != nullptr;
    for (std::size_t index = 0;
         widgets_valid && index < std::size(kWidgetTags); ++index) {
      jstring tag = env->NewStringUTF(kWidgetTags[index]);
      jobject widget =
          tag == nullptr
              ? nullptr
              : env->CallObjectMethod(content_root, find_view_with_tag, tag);
      jclass widget_class = env->FindClass(kWidgetClasses[index]);
      widgets_valid = tag != nullptr && widget != nullptr &&
                      widget_class != nullptr &&
                      env->IsInstanceOf(widget, widget_class) &&
                      !env->ExceptionCheck();
      env->DeleteLocalRef(widget_class);
      env->DeleteLocalRef(widget);
      env->DeleteLocalRef(tag);
    }
  }
  if (!widgets_valid) {
    std::cerr << "ART Android APK: framework widget set is incomplete\n";
    return 33;
  }
  const jboolean decor_presented =
      attached_decor == nullptr || env->ExceptionCheck()
          ? JNI_FALSE
          : PresentContent(env, nullptr, attached_decor,
                           kApkFrameWidth * window_scale,
                           kApkFrameHeight * window_scale);
  jmethodID was_presented =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_view_class, "wasPresented", "()Z");
  const jboolean view_presented =
      run_apk_app
          ? (decor_presented == JNI_TRUE && probe_view != nullptr
                 ? JNI_TRUE
                 : JNI_FALSE)
          : (decor_presented != JNI_TRUE || probe_view == nullptr ||
                     !env->IsInstanceOf(probe_view, probe_view_class) ||
                     was_presented == nullptr || env->ExceptionCheck()
                 ? JNI_FALSE
                 : env->CallBooleanMethod(probe_view, was_presented));
  if (view_presented != JNI_TRUE ||
      g_frame_width !=
          static_cast<std::size_t>(kApkFrameWidth * window_scale) ||
      g_frame_height !=
          static_cast<std::size_t>(kApkFrameHeight * window_scale) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android view: Activity content presentation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 33;
  }
  if (run_apk_app || run_framework_button) {
    if (g_interactive_root != nullptr) {
      env->DeleteGlobalRef(g_interactive_root);
    }
    g_interactive_root = env->NewGlobalRef(attached_decor);
    g_interactive_width = kApkFrameWidth * window_scale;
    g_interactive_height = kApkFrameHeight * window_scale;
    if (g_interactive_root == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android input: retaining DecorView failed\n";
      return 33;
    }
  }
  env->DeleteLocalRef(application);
  env->DeleteLocalRef(activity_info);
  env->DeleteLocalRef(context_theme_wrapper_class);
  env->DeleteLocalRef(probe_theme);
  env->DeleteLocalRef(window_background);
  env->DeleteLocalRef(attached_decor);
  env->DeleteLocalRef(content_root);
  env->DeleteLocalRef(decor_view);
  env->DeleteLocalRef(decor_view_class);
  env->DeleteLocalRef(window_attributes);
  env->DeleteLocalRef(accessibility_lock);
  env->DeleteLocalRef(object_class);
  env->DeleteLocalRef(accessibility);
  env->DeleteLocalRef(accessibility_class);
  env->DeleteLocalRef(phone_window_class);
  env->DeleteLocalRef(window_class);
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(probe_view);
  env->DeleteLocalRef(configuration);
  env->DeleteLocalRef(component_name);
  env->DeleteLocalRef(intent);
  env->DeleteLocalRef(title);
  env->DeleteLocalRef(class_name);
  env->DeleteLocalRef(package_name);
  env->DeleteLocalRef(configuration_class);
  env->DeleteLocalRef(component_name_class);
  env->DeleteLocalRef(intent_class);
  env->DeleteLocalRef(configured_apk_assets);
  env->DeleteLocalRef(framework_res_path);
  env->DeleteLocalRef(framework_apk_assets);
  env->DeleteLocalRef(apk_assets_class);
  env->DeleteLocalRef(asset_manager);
  env->DeleteLocalRef(asset_manager_class);
  env->DeleteLocalRef(probe_context);
  env->DeleteLocalRef(probe_resources);
  env->DeleteLocalRef(probe_resources_class);
  env->DeleteLocalRef(probe_view_class);
  env->DeleteLocalRef(probe_canvas_class);
  env->DeleteLocalRef(content_root_class);
  env->DeleteLocalRef(probe_context_class);
  env->DeleteLocalRef(application_class);
  env->DeleteLocalRef(activity_info_class);
  env->DeleteLocalRef(activity_class);
  env->DeleteLocalRef(activity_instance);
  if (lifecycle_result != 43) {
    std::cerr << "ART Android lifecycle: expected 43, got " << lifecycle_result
              << "\n";
    return 29;
  }

  JNINativeMethod native_methods[]{
      {const_cast<char*>("hostPageSize"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&HostPageSize)},
      {const_cast<char*>("nativePackedIntegerStack"),
       const_cast<char*>("(IIIIIIIJII)J"),
       reinterpret_cast<void*>(&NativePackedIntegerStack)},
      {const_cast<char*>("nativePackedFloatingStack"),
       const_cast<char*>("(FFFFFFFFFD)J"),
       reinterpret_cast<void*>(&NativePackedFloatingStack)},
      {const_cast<char*>("nativePackedReferenceStack"),
       const_cast<char*>("(IIIIIIILjava/lang/Object;I)J"),
       reinterpret_cast<void*>(&NativePackedReferenceStack)},
      {const_cast<char*>("nativePackedNarrowStack"),
       const_cast<char*>("(IIIIIIZBCSIJ)J"),
       reinterpret_cast<void*>(&NativePackedNarrowStack)},
  };
  if (self->GetJniEnv()->RegisterNatives(
          hello_class, native_methods, std::size(native_methods)) != JNI_OK) {
    std::cerr << "ART Darwin JNI: RegisterNatives failed\n";
    return 6;
  }
  if (!class_linker->EnsureInitialized(self, hello, true, true)) {
    std::cerr << "ART Darwin JNI: Hello initialization failed\n";
    return 7;
  }
  art::ArtMethod* answer =
      hello->FindClassMethod("answer", "()I", art::kRuntimePointerSize);
  if (answer == nullptr) {
    std::cerr << "ART Darwin DEX: answer()I lookup failed\n";
    return 8;
  }

  art::JValue result;
  answer->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u, &result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: answer()I threw\n";
    return 9;
  }
  if (result.GetI() != 42) {
    std::cerr << "ART Darwin DEX: expected 42, got " << result.GetI() << "\n";
    return 10;
  }

  art::ArtMethod* native_round_trip = hello->FindClassMethod(
      "nativeRoundTrip", "()I", art::kRuntimePointerSize);
  if (native_round_trip == nullptr) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I lookup failed\n";
    return 11;
  }
  art::JValue native_result;
  native_round_trip->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u,
                            &native_result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I threw\n";
    return 12;
  }
  if (native_result.GetI() != 42) {
    std::cerr << "ART Darwin JNI: expected 42, got " << native_result.GetI()
              << "\n";
    return 13;
  }

  art::ArtMethod* native_stack_pcs = hello->FindClassMethod(
      "nativeStackPcsRoundTrip", "()I", art::kRuntimePointerSize);
  if (native_stack_pcs == nullptr) {
    std::cerr << "ART Darwin JNI PCS: nativeStackPcsRoundTrip()I lookup failed\n";
    return 34;
  }
  art::JValue native_stack_pcs_result;
  native_stack_pcs->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u,
                           &native_stack_pcs_result, "I");
  if (self->IsExceptionPending() || native_stack_pcs_result.GetI() != 42) {
    std::cerr << "ART Darwin JNI PCS: packed stack argument matrix failed result="
              << native_stack_pcs_result.GetI() << "\n";
    return 35;
  }

  art::ArtMethod* runtime_native_arraycopy = hello->FindClassMethod(
      "runtimeNativeArraycopy", "()I", art::kRuntimePointerSize);
  if (runtime_native_arraycopy == nullptr) {
    std::cerr
        << "ART runtime native: runtimeNativeArraycopy()I lookup failed\n";
    return 14;
  }
  art::JValue arraycopy_result;
  runtime_native_arraycopy->Invoke(self, /* args= */ nullptr,
                                   /* args_size= */ 0u, &arraycopy_result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART runtime native: runtimeNativeArraycopy()I threw\n";
    return 15;
  }
  if (arraycopy_result.GetI() != 42) {
    std::cerr << "ART runtime native: expected 42, got "
              << arraycopy_result.GetI() << "\n";
    return 16;
  }

  jmethodID java_main =
      env->GetStaticMethodID(hello_class, "main", "([Ljava/lang/String;)V");
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray java_args = string_class == nullptr
                               ? nullptr
                               : env->NewObjectArray(1, string_class, nullptr);
  jstring message = env->NewStringUTF("Hello from Darwin ART main: 안녕");
  if (java_main == nullptr || string_class == nullptr || java_args == nullptr ||
      message == nullptr) {
    std::cerr << "ART Darwin launcher: main(String[]) setup failed\n";
    return 18;
  }
  env->SetObjectArrayElement(java_args, 0, message);
  env->CallStaticVoidMethod(hello_class, java_main, java_args);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(java_args);
  env->DeleteLocalRef(string_class);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Darwin launcher: main(String[]) threw\n"
              << self->GetException()->Dump() << "\n";
    env->ExceptionDescribe();
    return 19;
  }

  if (run_network_acceptance) {
    std::string load_error;
    bool loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, network_fixture_path, app_loader_ref, network_fixture_class,
          &load_error);
    }
    if (!loaded || !load_error.empty() || env->ExceptionCheck()) {
      std::cerr << "ART Android network JavaVMExt load failed: " << load_error
                << "\n";
      return 47;
    }
    g_network_elf_loaded = true;
    BoundedLoopbackHttpServer server;
    if (!server.Start()) {
      std::cerr << "ART Android network loopback listener failed\n";
      return 47;
    }
    jmethodID loopback = env->GetStaticMethodID(
        network_fixture_class, "nativeLoopbackHttp", "(I)I");
    const jint network_result =
        loopback == nullptr
            ? -1
            : env->CallStaticIntMethod(network_fixture_class, loopback,
                                       static_cast<jint>(server.port()));
    const bool server_ok = server.Stop();
    if (network_result != 42 || !server_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android network loopback failed, result="
                << network_result << " server=" << server_ok << "\n";
      return 47;
    }
    std::cout << "ART Android network: JavaVMExt+JNI_OnLoad loopback-HTTP=42 "
                 "socket+DNS=closed Internet=no\n"
              << std::flush;
  }

  if (run_elf_jni_fixture) {
    if (!run_generic_elf) {
      std::cerr << "ART Android ELF generic graph path is missing\n";
      return 40;
    }
    if (!run_apk_elf) {
      std::cerr << "ART Android APK ELF extraction/hash boundary is missing\n";
      return 45;
    }
    if (!run_libcxx_acceptance) {
      std::cerr << "ART Android libc++ fixture paths are missing\n";
      return 43;
    }
    if (!run_tls_acceptance) {
      std::cerr << "ART Android TLS fixture path is missing\n";
      return 44;
    }
    std::string libcxx_error;
    bool collections_ok = false;
    bool exception_ok = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      JavaVM* vm =
          reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
      collections_ok = RunAndroidElfSelfTest(
          env, vm, app_loader_ref, libcxx_collections_path, &libcxx_error);
      if (collections_ok) {
        exception_ok = RunAndroidElfSelfTest(
            env, vm, app_loader_ref, libcxx_exception_path, &libcxx_error);
      }
    }
    if (!collections_ok || !exception_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android libc++ acceptance failed: " << libcxx_error
                << "\n";
      return 43;
    }
    std::cout << "ART Android libc++: real-r28c collections=189 "
                 "exception-cleanup=73 unload=sequential\n"
              << std::flush;
    std::string tls_error;
    bool tls_ok = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      JavaVM* vm =
          reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
      tls_ok = RunAndroidElfSelfTest(env, vm, app_loader_ref, tls_fixture_path,
                                    &tls_error);
    }
    if (!tls_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF TLS acceptance failed: " << tls_error
                << "\n";
      return 44;
    }
    std::cout << "ART Android ELF TLS: local-TLSDESC threads=4 align=64 "
                 "unload=quiescent\n"
              << std::flush;
    std::string generic_load_error;
    bool generic_loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      generic_loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, generic_elf_path, app_loader_ref, nullptr, &generic_load_error);
    }
    if (!generic_loaded || !generic_load_error.empty() || env->ExceptionCheck() ||
        darwin_art_elf_jni_fixture_registration_status() != 0) {
      std::cerr << "ART Android ELF generic graph load failed, load_error="
                << generic_load_error << "\n";
      return 40;
    }
    jmethodID generic_native_add =
        env->GetStaticMethodID(native_fixture_class, "nativeAdd", "(IJI)J");
    const jlong generic_add_result =
        generic_native_add == nullptr
            ? -1
            : env->CallStaticLongMethod(native_fixture_class,
                                        generic_native_add, 10, jlong{20}, 12);
    if (generic_add_result != 42 || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF generic RegisterNatives failed, result="
                << generic_add_result << "\n";
      return 40;
    }
    // generic_elf_path and apk_elf_path are required to be the same extracted
    // root. The successful JavaVMExt load above is therefore the APK execution
    // evidence; loading the same SONAME a second time would only exercise ART's
    // path cache and acquire no additional graph ownership.
    g_apk_elf_loaded = true;
    g_apk_sha256 = apk_sha256;
    g_apk_root_sha256 = apk_root_sha256;
    char *partial_error = nullptr;
    void *partial_handle =
        android::OpenNativeLibrary(env, 35, elf_fixture_path, app_loader_ref,
                                   nullptr, nullptr, nullptr, &partial_error);
    const bool partial_cleanup_ok =
        partial_handle == nullptr && partial_error != nullptr &&
        darwin_art_elf_jni_fixture_lifecycle_status() == 124567 &&
        darwin_art_elf_jni_fixture_namespace_lifecycle_status() == 5;
    if (partial_handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(partial_handle, true, &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    const std::string partial_error_text =
        partial_error == nullptr ? "<none>" : partial_error;
    android::NativeLoaderFreeErrorMessage(partial_error);
    if (!partial_cleanup_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: partial failure cleanup failed, lifecycle="
                << darwin_art_elf_jni_fixture_lifecycle_status()
                << " namespace="
                << darwin_art_elf_jni_fixture_namespace_lifecycle_status()
                << " error=" << partial_error_text
                << "\n";
      return 40;
    }
    std::string load_error;
    bool loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, elf_fixture_path, app_loader_ref, native_fixture_class,
          &load_error);
    }
    const int bridge_status =
        darwin_art_elf_jni_fixture_registration_status();
    const int lifecycle_status =
        darwin_art_elf_jni_fixture_lifecycle_status();
    const int namespace_status =
        darwin_art_elf_jni_fixture_namespace_lifecycle_status();
    if (!loaded || !load_error.empty() || bridge_status != 0x7f ||
        lifecycle_status != 123 || namespace_status != 3 ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: load/registration failed, status="
                << bridge_status << " lifecycle=" << lifecycle_status
                << " namespace=" << namespace_status
                << " load_error=" << load_error << "\n";
      return 41;
    }
    jmethodID run_acceptance =
        env->GetStaticMethodID(native_fixture_class, "runAcceptance", "()I");
    const jint acceptance =
        run_acceptance == nullptr
            ? -3
            : env->CallStaticIntMethod(native_fixture_class, run_acceptance);
    if (acceptance != 42 || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: nativeAdd/nativeSpill failed, result="
                << acceptance << "\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 42;
    }
    std::cout
        << "ART Android ELF JNI: graph=child-first+relocated "
           "providers=bind_builtins+__errno+strlen+fs-random-ctor+scanf+"
           "swprintf+ioctl+strftime+sendfile "
           "load+JNI_OnLoad+RegisterNatives=generic+fixture scalar-ref=all "
           "nativeUsesEnv=current stack-repack=ok\n"
        << std::flush;
  }

  run_result->hello_answer = result.GetI();
  run_result->native_round_trip = native_result.GetI();
  run_result->arraycopy_result = arraycopy_result.GetI();
  run_result->activity_probe_result = activity_result;
  run_result->lifecycle_result = lifecycle_result;
  run_result->frame_width = static_cast<uint32_t>(g_frame_width);
  run_result->frame_height = static_cast<uint32_t>(g_frame_height);
  return 0;
  }();
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_shutdown_process() {
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    switch (g_process_state.phase) {
      case ProcessPhase::kShutdownComplete:
        return DARWIN_ART_STATUS_SHUTDOWN_ALREADY_COMPLETED;
      case ProcessPhase::kShutdownFailed:
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      case ProcessPhase::kNeverStarted:
      case ProcessPhase::kCreateFailed:
      case ProcessPhase::kRunning:
      case ProcessPhase::kShuttingDown:
        return DARWIN_ART_STATUS_SHUTDOWN_NOT_READY;
      case ProcessPhase::kAwaitingShutdown:
        break;
    }

    if (!g_process_state.owner_thread_valid ||
        pthread_equal(g_process_state.owner_thread, pthread_self()) == 0 ||
        (g_process_state.art_thread != nullptr &&
         art::Thread::Current() != g_process_state.art_thread)) {
      return DARWIN_ART_STATUS_SHUTDOWN_WRONG_THREAD;
    }
    g_process_state.phase = ProcessPhase::kShuttingDown;
    java_vm = g_process_state.java_vm;
    art_thread = g_process_state.art_thread;
    resource_runtime_installed = g_process_state.resource_runtime_installed;
  }

  CHECK(java_vm != nullptr);
  if (art_thread != nullptr) {
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
    {
      art::ScopedObjectAccess soa(art_thread);
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: clearing pending exception: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      if (g_probe_canvas_class != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_probe_canvas_class);
        g_probe_canvas_class = nullptr;
      }
      if (g_pressed_view != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_pressed_view);
        g_pressed_view = nullptr;
      }
      if (g_interactive_root != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_interactive_root);
        g_interactive_root = nullptr;
      }
      // AnimationContext owns RenderNode animation handles and may invoke
      // Java-side finish listeners while the VM is still alive.  Drain it
      // before releasing the persistent Java RenderNode reference; otherwise
      // AOSP's AnimationHandle destructor correctly fail-stops on shutdown.
#if defined(DARWIN_ART_REAL_GRAPHICS)
      if (g_hwui_animation_context != nullptr) {
        g_hwui_animation_context->destroy();
        g_hwui_animation_context.reset();
      }
      g_hwui_time_lord.reset();
#endif
      if (g_gpu_render_node != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_gpu_render_node);
        g_gpu_render_node = nullptr;
      }
      g_gpu_render_node_recorded = false;
      g_gpu_ripple_overlay_active = false;
      g_interactive_width = 0;
      g_interactive_height = 0;
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: global reference cleanup threw: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      if (!darwin_art::ShutdownLibcoreNatives()) {
        std::cerr << "ART Darwin shutdown: libcore host state restore failed\n";
        std::lock_guard<std::mutex> lock(g_process_state.mutex);
        g_process_state.phase = ProcessPhase::kShutdownFailed;
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
      if (resource_runtime_installed &&
          !darwin_art::ShutdownFrameworkResourceRuntime(
              art_thread->GetJniEnv())) {
        std::cerr << "ART Darwin shutdown: AndroidRuntime ownership uninstall failed\n";
        std::lock_guard<std::mutex> lock(g_process_state.mutex);
        g_process_state.phase = ProcessPhase::kShutdownFailed;
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
    }
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
  }

  // DestroyJavaVM deletes Runtime::Current(). Registered DexFile pointers must
  // remain valid through ClassLinker/Heap teardown, so release their owning
  // storage only after this returns successfully. Do not touch art_thread after
  // this call: its Thread object is owned by the destroyed Runtime.
  const jint destroy_result = java_vm->DestroyJavaVM();
  if (destroy_result != JNI_OK) {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  g_host_context = nullptr;
  g_frame_callback = nullptr;
  if (darwin_art::android_jni::TrampolineLiveCount() != 0) {
    std::cerr << "ART Darwin shutdown: ELF JNI trampolines remain live\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (g_network_elf_loaded &&
      (darwin_art_bionic_socket_broker_is_active() != 0 ||
       darwin_art_bionic_socket_broker_live_objects() != 0 ||
       darwin_art_bionic_dns_live_results_for_test() != 0 ||
       darwin_art_bionic_dns_retired_results_for_test() != 0)) {
    std::cerr << "ART Darwin shutdown: network owner did not quiesce\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (g_apk_elf_loaded) {
    std::cout << "ART Android APK ELF: apk-sha256=" << g_apk_sha256
              << " root-sha256=" << g_apk_root_sha256
              << " graph=root+child+grandchild load=JavaVMExt+NativeBridge "
                 "unload=shutdown-trampolines-zero\n"
              << std::flush;
  }
  if (g_direct_apk_loaded) {
    std::cout << "ART Android direct APK ELF: source=readonly-fd-slices "
                 "copy=0 extract=0 alignment=16384 graph=root+child+grandchild "
                 "load=JavaVMExt+NativeBridge JNI_OnLoad=0x00010006 "
                 "unload=shutdown-trampolines-zero authority=isolated-process\n"
              << std::flush;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_lifecycle_status() != 1234567) {
    std::cerr << "ART Darwin shutdown: ELF JNI graph finalizer order failed, status="
              << darwin_art_elf_jni_fixture_lifecycle_status() << "\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_namespace_lifecycle_status() != 5) {
    std::cerr << "ART Darwin shutdown: Bionic namespace teardown order failed, status="
              << darwin_art_elf_jni_fixture_namespace_lifecycle_status() << "\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }

  darwin_art::ShutdownIcuCharsetNatives();
  darwin_art::ShutdownFrameworkGraphicsRuntime();
  g_process_state.app_dex_files.clear();
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.java_vm = nullptr;
    g_process_state.art_thread = nullptr;
    g_process_state.resource_runtime_installed = false;
    g_process_state.phase = ProcessPhase::kShutdownComplete;
  }
  return 0;
}
