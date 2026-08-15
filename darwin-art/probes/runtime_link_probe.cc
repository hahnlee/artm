#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_art/darwin_art.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_framework_natives.h"
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

extern "C" int darwin_art_elf_jni_fixture_registration_status();
extern "C" int darwin_art_elf_jni_fixture_lifecycle_status();
extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status();

namespace android {
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
static void* g_host_context = nullptr;
static darwin_art_frame_callback_t g_frame_callback = nullptr;

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
    g_host_context = nullptr;
    g_frame_callback = nullptr;

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
  art::StackHandleScope<12> hs(self);
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
  art::Handle<art::mirror::Class> probe_activity =
      hs.NewHandle(class_linker->FindClass(
          self, "Ldev/darwinart/probe/ProbeActivity;",
          sizeof("Ldev/darwinart/probe/ProbeActivity;") - 1u, app_loader));
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
  if (run_elf_jni_fixture) {
    native_fixture_handle.Assign(class_linker->FindClass(
        self, "Ldarwin/art/nativefixture/NativeFixture;",
        sizeof("Ldarwin/art/nativefixture/NativeFixture;") - 1u, app_loader));
  }
  if (content_root_handle == nullptr || package_manager_handle == nullptr ||
      (run_elf_jni_fixture && native_fixture_handle == nullptr) ||
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
  art::Runtime::Current()->StartMinimalForDarwinProbe(self->GetJniEnv());
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

  JNIEnv* env = self->GetJniEnv();
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
    std::cerr << "ART Android framework: ProbeActivity initialization failed\n"
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
      env->GetMethodID(probe_activity_class, "probeValue", "()I");
  jint activity_result =
      activity_instance == nullptr || probe_value == nullptr
          ? -1
          : env->CallIntMethod(activity_instance, probe_value);
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
  jobjectArray empty_apk_assets =
      apk_assets_class == nullptr
          ? nullptr
          : env->NewObjectArray(0, apk_assets_class, nullptr);
  jfieldID apk_assets_field =
      asset_manager_class == nullptr
          ? nullptr
          : env->GetFieldID(asset_manager_class, "mApkAssets",
                            "[Landroid/content/res/ApkAssets;");
  if (asset_manager != nullptr && apk_assets_field != nullptr &&
      empty_apk_assets != nullptr) {
    env->SetObjectField(asset_manager, apk_assets_field, empty_apk_assets);
  }
  jmethodID probe_resources_constructor =
      probe_resources_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_resources_class, "<init>",
                             "(Landroid/content/res/AssetManager;)V");
  jobject probe_resources =
      probe_resources_constructor == nullptr || asset_manager == nullptr
          ? nullptr
          : env->NewObject(probe_resources_class, probe_resources_constructor,
                           asset_manager);
  if (activity_info == nullptr || application == nullptr ||
      asset_manager == nullptr || empty_apk_assets == nullptr ||
      apk_assets_field == nullptr || probe_resources == nullptr ||
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
  jmethodID configuration_constructor =
      configuration_class == nullptr
          ? nullptr
          : env->GetMethodID(configuration_class, "<init>", "()V");
  jstring package_name = env->NewStringUTF("dev.darwinart.probe");
  jstring class_name = env->NewStringUTF("dev.darwinart.probe.ProbeActivity");
  jstring title = env->NewStringUTF("Darwin ART Probe");
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
  if (attach_base_context != nullptr) {
    env->CallNonvirtualVoidMethod(activity_instance,
                                  context_theme_wrapper_class,
                                  attach_base_context,
                                  probe_context);
  }
  if (attach_base_context == nullptr || env->ExceptionCheck()) {
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
  if (decor_view == nullptr || content_root == nullptr || add_view == nullptr ||
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
      env->GetMethodID(probe_activity_class, "probeOnCreate", "()I");
  jint lifecycle_result =
      probe_on_create == nullptr
          ? -1
          : env->CallIntMethod(activity_instance, probe_on_create);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android lifecycle: Activity.onCreate() threw\n"
              << self->GetException()->Dump() << "\n";
    return 28;
  }
  jmethodID get_decor_view =
      window_class == nullptr
          ? nullptr
          : env->GetMethodID(window_class, "getDecorView",
                             "()Landroid/view/View;");
  jobject attached_decor =
      get_decor_view == nullptr
          ? nullptr
          : env->CallObjectMethod(window, get_decor_view);
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
  const jboolean decor_presented =
      attached_decor == nullptr ||
              !env->IsSameObject(attached_decor, decor_view) ||
              env->ExceptionCheck()
          ? JNI_FALSE
          : PresentContent(env, nullptr, attached_decor, 640, 360);
  jmethodID was_presented =
      env->GetMethodID(probe_view_class, "wasPresented", "()Z");
  const jboolean view_presented =
      decor_presented != JNI_TRUE || probe_view == nullptr ||
              !env->IsInstanceOf(probe_view, probe_view_class) ||
              was_presented == nullptr || env->ExceptionCheck()
          ? JNI_FALSE
          : env->CallBooleanMethod(probe_view, was_presented);
  if (view_presented != JNI_TRUE || g_frame_width != 640 ||
      g_frame_height != 360 || env->ExceptionCheck()) {
    std::cerr << "ART Android view: Activity content presentation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 33;
  }
  env->DeleteLocalRef(application);
  env->DeleteLocalRef(activity_info);
  env->DeleteLocalRef(context_theme_wrapper_class);
  env->DeleteLocalRef(probe_theme);
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
  env->DeleteLocalRef(empty_apk_assets);
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

  if (run_elf_jni_fixture) {
    char* partial_error = nullptr;
    void* partial_handle = android::OpenNativeLibrary(
        env, 35, elf_fixture_path, app_loader_ref, nullptr, nullptr, nullptr,
        &partial_error);
    const bool partial_cleanup_ok =
        partial_handle == nullptr && partial_error != nullptr &&
        darwin_art_elf_jni_fixture_lifecycle_status() == 124567 &&
        darwin_art_elf_jni_fixture_namespace_lifecycle_status() == 5;
    if (partial_handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(partial_handle, true, &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    android::NativeLoaderFreeErrorMessage(partial_error);
    if (!partial_cleanup_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: partial failure cleanup failed, lifecycle="
                << darwin_art_elf_jni_fixture_lifecycle_status()
                << " namespace="
                << darwin_art_elf_jni_fixture_namespace_lifecycle_status()
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
    jmethodID run_acceptance = env->GetStaticMethodID(
        native_fixture_class, "runAcceptance", "()I");
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
    std::cout << "ART Android ELF JNI: graph=child-first+relocated "
                 "providers=bind_builtins+__errno+strlen "
                 "load+JNI_OnLoad+RegisterNatives=installed scalar-ref=all "
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
  if (darwin_art::android_jni::TrampolineLiveCount() != 0) {
    std::cerr << "ART Darwin shutdown: ELF JNI trampolines remain live\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
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
