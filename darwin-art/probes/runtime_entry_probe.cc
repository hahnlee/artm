#include <mach-o/dyld.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#include <cstddef>
#include <pthread.h>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_art/darwin_art.h"
#include "darwin_framework_natives.h"
#include "darwin_provider_owners.h"
#include "darwin_art/darwin_art.h"
#include "runtime_network_probe.h"
#include "runtime_hwui_probe.h"
#include "runtime_elf_probe.h"
#include "runtime_abi_probe.h"
#include "runtime_process_state.h"
#include "runtime_process_options.h"
#include "runtime_acceptance_phases.h"
#include "runtime_jni_scope.h"
#include "runtime_frame_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_gpu.h"
#include "runtime_graphics_session.h"
#include "runtime_graphics_phase.h"
#include "runtime_jni_acceptance_probe.h"
#include "runtime_registration_phase.h"
#include "runtime_app_bootstrap.h"
#include "runtime_app_presentation.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jni/java_vm_ext.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/throwable.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "well_known_classes.h"

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#include "runtime_apk_graph.h"
#endif

extern "C" int darwin_art_install_context_loader(JNIEnv* env,
                                                   jobject app_loader);

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_run_process(
    const darwin_art_process_config_t* config,
    darwin_art_process_result_t* run_result) {
  darwin_art_process::ProcessConfigBounds config_bounds;
  std::string config_error;
  const int config_status = darwin_art_process::ValidateProcessConfig(
      config, run_result, &config_bounds, &config_error);
  if (config_status != 0) {
    std::cerr << "darwin_art_run_process: " << config_error << "\n";
    return config_status;
  }
  const uint64_t heap_initial = config_bounds.heap_initial_bytes;
  const uint64_t heap_maximum = config_bounds.heap_maximum_bytes;

  const darwin_art_lifecycle_hooks_t* lifecycle_hooks =
      config->struct_size >=
                  offsetof(darwin_art_process_config_t, lifecycle_hooks) +
                      sizeof(config->lifecycle_hooks)
          ? config->lifecycle_hooks
          : nullptr;
  if (!darwin_art_process::begin_run(lifecycle_hooks)) {
    std::cerr << "darwin_art_run_process: process already started\n";
    return DARWIN_ART_STATUS_PROCESS_ALREADY_STARTED;
  }
  // The graphics sidecar is an additive tail of the ABI. Never read it from
  // a legacy prefix, and never overload host_context with graphics state.
  if (config->struct_size >=
          offsetof(darwin_art_process_config_t, graphics_session_context) +
              sizeof(config->graphics_session_context) &&
      config->graphics_session_context != nullptr &&
      darwin_art_graphics::bind_session_for_process(
          config->graphics_session_context) != 0) {
    std::cerr << "darwin_art_run_process: graphics session binding failed\n";
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  auto* graphics_state = darwin_art_graphics::state_for_context(
      config->graphics_session_context);
  darwin_art_process::record_graphics_state(graphics_state);
  darwin_art_process::ScopedRunBoundary process_boundary;
  darwin_art_process::ProcessOptions process_options;
  std::string options_error;
  const int options_status =
      darwin_art_process::LoadProcessOptions(&process_options, &options_error);
  if (options_status != 0) {
    std::cerr << options_error << "\n";
    return options_status;
  }
  const char* elf_fixture_path = process_options.elf_fixture_path.c_str();
  const char* generic_elf_path = process_options.generic_elf_path.c_str();
  const char* apk_elf_path = process_options.apk_elf_path.c_str();
  const char* apk_sha256 = process_options.apk_sha256.c_str();
  const char* apk_root_sha256 = process_options.apk_root_sha256.c_str();
  const char* direct_apk_path = process_options.direct_apk_path.c_str();
  const char* direct_apk_root = process_options.direct_apk_root.c_str();
  const char* libcxx_collections_path =
      process_options.libcxx_collections_path.c_str();
  const char* libcxx_exception_path = process_options.libcxx_exception_path.c_str();
  const char* tls_fixture_path = process_options.tls_fixture_path.c_str();
  const char* network_fixture_path = process_options.network_fixture_path.c_str();
  const char* apk_app_package = process_options.apk_app_package.c_str();
  const char* apk_app_activity = process_options.apk_app_activity.c_str();
  const char* apk_app_descriptor = process_options.apk_app_descriptor.c_str();
  const char* apk_app_support_dex = process_options.apk_app_support_dex.c_str();
  const char* apk_app_native_path = process_options.apk_app_native_path.c_str();
  const char* framework_res_apk = process_options.framework_res_apk.c_str();
  const bool run_elf_jni_fixture = process_options.run_elf_jni_fixture;
  const bool run_generic_elf = process_options.run_generic_elf;
  const bool run_apk_elf = process_options.run_apk_elf;
  const bool run_direct_apk = process_options.run_direct_apk;
  const bool run_libcxx_acceptance = process_options.run_libcxx_acceptance;
  const bool run_tls_acceptance = process_options.run_tls_acceptance;
  const bool run_network_acceptance = process_options.run_network_acceptance;
  const bool has_apk_app_identity_environment =
      process_options.has_apk_app_identity_environment;
  const bool run_apk_app = process_options.run_apk_app;
  const bool run_framework_button = process_options.run_framework_button;
  const bool use_framework_resources = process_options.use_framework_resources;
  const jint window_scale = process_options.window_scale;
  constexpr jint kApkFrameWidth = 360;
  constexpr jint kApkFrameHeight = 640;
  const bool expect_apk_widgets = process_options.expect_apk_widgets;

  // Darwin's malloc zones can claim the fixed compressed-reference window
  // while RuntimeArgumentMap is being assembled. Reserve ART's bounded arena
  // after the one-shot process gate, but before the launcher performs its first
  // heap allocation. MemMap::Init itself is process-global and not safe for
  // concurrent callers.
  art::MemMap::Init();

  darwin_art_frame_probe::configure(config->host_context, config->frame_callback);

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
  darwin_art_process::record_created_runtime(self);
  if (self == nullptr) {
    std::cerr << "ART Darwin DEX: no current thread\n";
    return 2;
  }
  if (config->struct_size >=
          offsetof(darwin_art_process_config_t, graphics_session_context) +
              sizeof(config->graphics_session_context) &&
      config->graphics_session_context != nullptr &&
      darwin_art_graphics::bind_session_art_thread(self) != 0) {
    std::cerr << "ART graphics: session ART-thread binding failed\n";
    return 33;
  }
  JNIEnv* env = self->GetJniEnv();

  process_boundary.set_art_thread(self);
  if (config->provider_acquire != nullptr) {
    darwin_art::providers::darwin_art_provider_install_hooks(
        config->provider_context, config->provider_acquire,
        config->provider_release);
    darwin_art_process::record_provider_hooks_installed();
  }
  return [&]() -> int32_t {

  art::interpreter::UnstartedRuntime::Initialize();
  art::ScopedObjectAccess soa(self);
  darwin_art_jni_scope::ScopedLocalFrame local_frame(self->GetJniEnv());
  if (!local_frame.valid()) {
    std::cerr << "ART Darwin JNI: local frame allocation failed\n";
    return 34;
  }
  art::WellKnownClasses::Init(self->GetJniEnv());

  art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();
  art::StackHandleScope<32> hs(self);
  const char* activity_descriptor =
      run_apk_app ? apk_app_descriptor : "Ldev/darwinart/probe/ProbeActivity;";
  darwin_art_app::ClassSet app_classes;
  const int app_status = darwin_art_app::load_classes(
      self->GetJniEnv(), self, class_linker, soa, hs, run_apk_app, config->app_dex,
      apk_app_support_dex, apk_app_native_path, activity_descriptor,
      run_direct_apk, direct_apk_path,
      run_elf_jni_fixture, run_network_acceptance,
      darwin_art::GetFrameworkGraphicsBackend() ==
          darwin_art::FrameworkGraphicsBackend::kProbeCanvas,
      &app_classes);
  if (app_status != 0) {
    return app_status;
  }
  jobject app_loader_ref = app_classes.app_loader;
  jclass hello_class = app_classes.hello;
  jclass probe_activity_class = app_classes.activity;
  jclass probe_context_class = app_classes.context;
  jclass probe_resources_class = app_classes.resources;
  jclass probe_view_class = app_classes.view;
  jclass probe_canvas_class = app_classes.canvas;
  jclass content_root_class = app_classes.content_root;
  jclass package_manager_class = app_classes.package_manager;
  jclass native_fixture_class = app_classes.native_fixture;
  jclass network_fixture_class = app_classes.network_fixture;
  art::Handle<art::mirror::Class> hello =
      hs.NewHandle(soa.Decode<art::mirror::Class>(hello_class));
  art::Handle<art::mirror::Class> probe_activity =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_activity_class));
  art::Handle<art::mirror::Class> probe_context_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_context_class));
  art::Handle<art::mirror::Class> probe_resources_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_resources_class));
  art::Handle<art::mirror::Class> probe_view_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_view_class));
  art::Handle<art::mirror::Class> content_root_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(content_root_class));
  art::Handle<art::mirror::Class> package_manager_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(package_manager_class));
  art::MutableHandle<art::mirror::Class> native_fixture_handle(
      hs.NewHandle(soa.Decode<art::mirror::Class>(native_fixture_class)));
  art::MutableHandle<art::mirror::Class> network_fixture_handle(
      hs.NewHandle(soa.Decode<art::mirror::Class>(network_fixture_class)));
  art::Handle<art::mirror::Class> framework_activity = hs.NewHandle(
      class_linker->FindSystemClass(self, "Landroid/app/Activity;"));
  if (framework_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity class lookup failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 21;
  }
  if (content_root_handle == nullptr || package_manager_handle == nullptr ||
      (run_elf_jni_fixture && native_fixture_handle == nullptr) ||
      (run_network_acceptance && network_fixture_handle == nullptr) ||
      self->IsExceptionPending()) {
    std::cerr << "ART Android window: Darwin Canvas/Window lookup failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 21;
  }

  const int registration_status = darwin_art_registration_phase::run(
      {.env = env,
       .self = self,
       .app_loader_ref = app_loader_ref,
       .probe_canvas_class = probe_canvas_class,
       .graphics_state = graphics_state});
  if (registration_status != 0) {
    return registration_status;
  }

  // Android's managed System.load/Runtime.nativeLoad path reaches
  // JavaVMExt only after the app PathClassLoader and thread context loader
  // have been installed.  Loading earlier makes JNI_OnLoad observe the boot
  // loader and breaks RegisterNatives for app-owned classes.
  if (run_apk_app && !process_options.apk_app_native_path.empty()) {
    if (darwin_art_app::install_native_library_path(
            env, app_loader_ref, apk_app_native_path) != 0) {
      std::cerr << "ART Android APK: PathClassLoader native path setup failed\n";
      return 46;
    }
    // Real Android APKs load JNI libraries from the managed
    // System.load/Runtime.nativeLoad path during class initialization.  Do
    // not eagerly call JavaVMExt here as that would make the subsequent
    // System.loadLibrary a recursive second load of the same image.  The
    // explicit loader remains available for the isolated fixture/direct APK
    // gates, while the normal app path follows the platform ordering.
    const char* managed_native_load =
        std::getenv("DARWIN_ART_APK_MANAGED_NATIVE_LOAD");
    if (managed_native_load == nullptr || std::strcmp(managed_native_load, "1") != 0) {
      const int native_status = darwin_art_app::load_native_library(
          env, self, app_loader_ref, apk_app_native_path);
      if (native_status != 0) {
        return native_status;
      }
    }
  }

  if (!class_linker->EnsureInitialized(self, probe_activity, true, true)) {
    std::cerr << "ART Android framework: launcher Activity initialization failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return 22;
  }
  jmethodID activity_constructor =
      env->GetMethodID(probe_activity_class, "<init>", "()V");
  // APK Activity construction is deferred to the attached Java UI thread.
  // Activity/Fragment hosts capture thread identity during construction.
  jobject activity_instance =
      run_apk_app || activity_constructor == nullptr
          ? nullptr
          : env->NewObject(probe_activity_class, activity_constructor);
  jmethodID probe_value =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_activity_class, "probeValue", "()I");
  jint activity_result =
      run_apk_app
          ? 42
          : (activity_instance == nullptr || probe_value == nullptr
                 ? -1
                 : env->CallIntMethod(activity_instance, probe_value));
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android framework: ProbeActivity constructor threw\n";
    env->ExceptionDescribe();
    env->ExceptionClear();
    return 23;
  }
  if (activity_result != 42) {
    std::cerr << "ART Android framework: expected 42, got " << activity_result
              << "\n";
    return 24;
  }

  jobject package_manager =
      package_manager_class == nullptr
          ? nullptr
          : soa.AddLocalReference<jobject>(
                package_manager_handle->AllocObject(self));
  if (package_manager == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: package feature stub failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 27;
  }
  jint lifecycle_result = 43;
  int presentation_status = 0;
  if (run_apk_app) {
    // NSWindow/CAMetalLayer and the ART process owner are both the host's
    // current thread. Keep the Android application owner on that same thread:
    // this is the thread recorded by begin_run(), bound to the ART runtime,
    // and later used by graphics/input callbacks.
    if (darwin_art_graphics::prepare_gpu_surface(
            graphics_state, kApkFrameWidth * window_scale,
            kApkFrameHeight * window_scale) != 0) {
      std::cerr << "ART Android GPU: main-thread surface preparation failed\n";
      return 33;
    }
    // registration_phase already prepared the main Looper and installed the
    // APK PathClassLoader on this exact ART owner thread. Reassert the
    // context-loader binding here as an explicit precondition for Activity
    // construction, then bootstrap the service bridge before any framework
    // object can consult it.
    jclass looper_class = env->FindClass("android/os/Looper");
    jmethodID my_looper =
        looper_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(looper_class, "myLooper",
                                     "()Landroid/os/Looper;");
    jmethodID main_looper =
        looper_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(looper_class, "getMainLooper",
                                     "()Landroid/os/Looper;");
    jobject owner_looper =
        my_looper == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(looper_class, my_looper);
    jobject process_main_looper =
        main_looper == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(looper_class, main_looper);
    if (looper_class == nullptr || my_looper == nullptr ||
        main_looper == nullptr || owner_looper == nullptr ||
        process_main_looper == nullptr ||
        !env->IsSameObject(owner_looper, process_main_looper) ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android owner: main Looper is not prepared on owner\n";
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      env->DeleteLocalRef(process_main_looper);
      env->DeleteLocalRef(owner_looper);
      env->DeleteLocalRef(looper_class);
      return 25;
    }
    env->DeleteLocalRef(process_main_looper);
    env->DeleteLocalRef(owner_looper);
    env->DeleteLocalRef(looper_class);
    if (darwin_art_install_context_loader(env, app_loader_ref) != 0) {
      std::cerr << "ART Android owner: context ClassLoader setup failed\n";
      return 27;
    }
    jclass binder_internal =
        env->FindClass("com/android/internal/os/BinderInternal");
    jmethodID get_context_object =
        binder_internal == nullptr
            ? nullptr
            : env->GetStaticMethodID(binder_internal, "getContextObject",
                                     "()Landroid/os/IBinder;");
    jobject context_binder =
        get_context_object == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(binder_internal, get_context_object);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    std::cerr << "ART Android owner: thread="
              << reinterpret_cast<uintptr_t>(pthread_self())
              << " art=" << self << " looper=main context_loader=1"
              << " service_bridge=" << (context_binder != nullptr) << "\n";
    env->DeleteLocalRef(context_binder);
    env->DeleteLocalRef(binder_internal);

    activity_instance = activity_constructor == nullptr
                            ? nullptr
                            : env->NewObject(probe_activity_class,
                                             activity_constructor);
    if (activity_instance == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android owner: Activity construction failed\n";
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      return 23;
    }
    presentation_status = darwin_art_presentation::run(
        env, self, activity_instance, probe_activity_class, probe_context_class,
        probe_resources_class, probe_view_class, probe_canvas_class,
        content_root_class, package_manager, true, use_framework_resources,
        expect_apk_widgets, !process_options.apk_app_native_path.empty(),
        run_framework_button, window_scale, framework_res_apk, apk_app_package,
        apk_app_activity, config->app_dex, graphics_state);
  } else {
    presentation_status = darwin_art_presentation::run(
        env, self, activity_instance, probe_activity_class, probe_context_class,
        probe_resources_class, probe_view_class, probe_canvas_class,
        content_root_class, package_manager, false, use_framework_resources,
        expect_apk_widgets, !process_options.apk_app_native_path.empty(),
        run_framework_button, window_scale, framework_res_apk, apk_app_package,
        apk_app_activity, config->app_dex, graphics_state);
  }
  if (presentation_status != 0) {
    return presentation_status;
  }

  darwin_art_jni_acceptance_phase::Results jni_results;
  const int jni_status = darwin_art_jni_acceptance_phase::run(
      env, self, class_linker, hello, hello_class, &jni_results);
  if (jni_status != 0) {
    return jni_status;
  }

  if (run_network_acceptance &&
      darwin_art_network_phase::run(env, network_fixture_path,
                                    app_loader_ref, network_fixture_class) != 0) {
    return 47;
  }

  if (run_elf_jni_fixture) {
    const darwin_art_elf_probe::FixtureGraphAcceptance fixture_input{
        .env = env,
        .self = self,
        .app_loader_ref = app_loader_ref,
        .native_fixture_class = native_fixture_class,
        .elf_fixture_path = elf_fixture_path,
        .generic_elf_path = generic_elf_path,
        .libcxx_collections_path = libcxx_collections_path,
        .libcxx_exception_path = libcxx_exception_path,
        .tls_fixture_path = tls_fixture_path,
        .apk_sha256 = apk_sha256,
        .apk_root_sha256 = apk_root_sha256,
        .run_generic_elf = run_generic_elf,
        .run_apk_elf = run_apk_elf,
        .run_libcxx_acceptance = run_libcxx_acceptance,
        .run_tls_acceptance = run_tls_acceptance,
    };
    const int fixture_status =
        darwin_art_elf_probe::run_fixture_graph_acceptance(fixture_input);
    if (fixture_status != 0) {
      return fixture_status;
    }
  }

  run_result->hello_answer = jni_results.hello_answer;
  run_result->native_round_trip = jni_results.native_round_trip;
  run_result->arraycopy_result = jni_results.arraycopy_result;
  run_result->activity_probe_result = activity_result;
  run_result->lifecycle_result = lifecycle_result;
  const auto frame_dimensions = darwin_art_frame_probe::dimensions();
  run_result->frame_width = static_cast<uint32_t>(frame_dimensions.width);
  run_result->frame_height = static_cast<uint32_t>(frame_dimensions.height);
  return 0;
  }();
}
