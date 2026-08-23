#include <mach-o/dyld.h>

#include <cstdint>
#include <iostream>
#include <thread>
#include <string>
#include <utility>

#include <cstddef>

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
    // NSWindow/CAMetalLayer creation is AppKit-main-thread owned.  The
    // Activity itself is deliberately constructed on the attached Android UI
    // thread below, but the drawable owner must exist before that handoff.
    if (darwin_art_graphics::prepare_gpu_surface(
            graphics_state, kApkFrameWidth * window_scale,
            kApkFrameHeight * window_scale) != 0) {
      std::cerr << "ART Android GPU: main-thread surface preparation failed\n";
      return 33;
    }
    // Android runs Activity/View construction on the app's Java main thread,
    // not on the ART bootstrap thread that called LoadNativeLibrary.  Apart
    // from being the correct ownership boundary, this matters for framework
    // widgets: TextView/EditText resolve their helper classes through the
    // attached Java thread's class-loading state.  Keep the bootstrap thread
    // parked while the attached UI thread owns the whole window transaction.
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
      std::cerr << "ART Android window: JavaVM lookup failed\n";
      return 27;
    }
    struct PresentationRefs {
      jobject activity;
      jclass activity_class;
      jclass context_class;
      jclass resources_class;
      jclass view_class;
      jclass canvas_class;
      jclass content_root_class;
      jobject package_manager;
      jobject app_loader;
    } refs{
        env->NewGlobalRef(activity_instance),
        reinterpret_cast<jclass>(env->NewGlobalRef(probe_activity_class)),
        reinterpret_cast<jclass>(env->NewGlobalRef(probe_context_class)),
        reinterpret_cast<jclass>(env->NewGlobalRef(probe_resources_class)),
        reinterpret_cast<jclass>(env->NewGlobalRef(probe_view_class)),
        reinterpret_cast<jclass>(env->NewGlobalRef(probe_canvas_class)),
        reinterpret_cast<jclass>(env->NewGlobalRef(content_root_class)),
        env->NewGlobalRef(package_manager),
        env->NewGlobalRef(app_loader_ref),
    };
    if (refs.activity_class == nullptr ||
        refs.context_class == nullptr || refs.resources_class == nullptr ||
        refs.view_class == nullptr || refs.content_root_class == nullptr ||
        refs.package_manager == nullptr || refs.app_loader == nullptr) {
      std::cerr << "ART Android window: UI thread reference setup failed"
                << " activity_class=" << (refs.activity_class != nullptr)
                << " context=" << (refs.context_class != nullptr)
                << " resources=" << (refs.resources_class != nullptr)
                << " view=" << (refs.view_class != nullptr)
                << " content=" << (refs.content_root_class != nullptr)
                << " package_manager=" << (refs.package_manager != nullptr)
                << " app_loader=" << (refs.app_loader != nullptr)
                << "\n";
      return 27;
    }
    std::thread ui_thread([&]() {
      JNIEnv* ui_env = nullptr;
      if (vm->AttachCurrentThread(&ui_env, nullptr) != JNI_OK ||
          ui_env == nullptr) {
        presentation_status = 27;
        return;
      }
      art::Thread* ui_self = art::Thread::Current();
      jclass activity_class_local =
          reinterpret_cast<jclass>(ui_env->NewLocalRef(refs.activity_class));
      jclass looper_class = ui_env->FindClass("android/os/Looper");
      jmethodID prepare_looper =
          looper_class == nullptr
              ? nullptr
              : ui_env->GetStaticMethodID(looper_class, "prepare", "()V");
      if (prepare_looper != nullptr) {
        ui_env->CallStaticVoidMethod(looper_class, prepare_looper);
      }
      ui_env->DeleteLocalRef(looper_class);
      if (prepare_looper == nullptr || ui_env->ExceptionCheck()) {
        presentation_status = 27;
        vm->DetachCurrentThread();
        return;
      }
      jclass thread_class = ui_env->FindClass("java/lang/Thread");
      jmethodID current_thread =
          thread_class == nullptr
              ? nullptr
              : ui_env->GetStaticMethodID(thread_class, "currentThread",
                                          "()Ljava/lang/Thread;");
      jmethodID set_context_loader =
          thread_class == nullptr
              ? nullptr
              : ui_env->GetMethodID(thread_class, "setContextClassLoader",
                                    "(Ljava/lang/ClassLoader;)V");
      jobject thread =
          current_thread == nullptr
              ? nullptr
              : ui_env->CallStaticObjectMethod(thread_class, current_thread);
      jobject app_loader_local = ui_env->NewLocalRef(refs.app_loader);
      if (set_context_loader != nullptr && thread != nullptr &&
          app_loader_local != nullptr) {
        ui_env->CallVoidMethod(thread, set_context_loader, app_loader_local);
      }
      ui_env->DeleteLocalRef(app_loader_local);
      ui_env->DeleteLocalRef(thread);
      ui_env->DeleteLocalRef(thread_class);
      if (current_thread == nullptr || set_context_loader == nullptr ||
          ui_env->ExceptionCheck()) {
        presentation_status = 27;
        vm->DetachCurrentThread();
        return;
      }
      // ServiceManager is normally initialized by ActivityThread after the
      // app thread has its PathClassLoader.  The detached host has no
      // ActivityThread, so perform the same binder bootstrap now; the native
      // BinderInternal bridge resolves DarwinServiceBridge through this exact
      // thread context loader.
      jclass binder_internal =
          ui_env->FindClass("com/android/internal/os/BinderInternal");
      jmethodID get_context_object =
          binder_internal == nullptr
              ? nullptr
              : ui_env->GetStaticMethodID(
                    binder_internal, "getContextObject",
                    "()Landroid/os/IBinder;");
      jobject context_binder =
          get_context_object == nullptr
              ? nullptr
              : ui_env->CallStaticObjectMethod(binder_internal,
                                                 get_context_object);
      if (ui_env->ExceptionCheck()) {
        ui_env->ExceptionDescribe();
        ui_env->ExceptionClear();
      }
      std::cerr << "ART Android services: context binder="
                << (context_binder != nullptr) << "\n";
      ui_env->DeleteLocalRef(context_binder);
      ui_env->DeleteLocalRef(binder_internal);
      jobject activity_local = nullptr;
      if (refs.activity != nullptr) {
        activity_local = ui_env->NewLocalRef(refs.activity);
      } else {
        jmethodID ui_activity_constructor =
            ui_env->GetMethodID(activity_class_local, "<init>", "()V");
        activity_local = ui_activity_constructor == nullptr
                             ? nullptr
                             : ui_env->NewObject(activity_class_local,
                                                 ui_activity_constructor);
      }
      jclass context_class_local =
          reinterpret_cast<jclass>(ui_env->NewLocalRef(refs.context_class));
      jclass resources_class_local = reinterpret_cast<jclass>(
          ui_env->NewLocalRef(refs.resources_class));
      jclass view_class_local =
          reinterpret_cast<jclass>(ui_env->NewLocalRef(refs.view_class));
      jclass canvas_class_local =
          reinterpret_cast<jclass>(ui_env->NewLocalRef(refs.canvas_class));
      jclass content_root_class_local = reinterpret_cast<jclass>(
          ui_env->NewLocalRef(refs.content_root_class));
      jobject package_manager_local =
          ui_env->NewLocalRef(refs.package_manager);
      presentation_status = darwin_art_presentation::run(
          ui_env, ui_self, activity_local, activity_class_local,
          context_class_local, resources_class_local, view_class_local,
          canvas_class_local, content_root_class_local, package_manager_local,
          true, use_framework_resources, expect_apk_widgets,
          !process_options.apk_app_native_path.empty(), run_framework_button,
          window_scale, framework_res_apk, apk_app_package, apk_app_activity,
          config->app_dex, graphics_state);
      vm->DetachCurrentThread();
    });
    ui_thread.join();
    env->DeleteGlobalRef(refs.activity);
    env->DeleteGlobalRef(refs.activity_class);
    env->DeleteGlobalRef(refs.context_class);
    env->DeleteGlobalRef(refs.resources_class);
    env->DeleteGlobalRef(refs.view_class);
    env->DeleteGlobalRef(refs.canvas_class);
    env->DeleteGlobalRef(refs.content_root_class);
    env->DeleteGlobalRef(refs.package_manager);
    env->DeleteGlobalRef(refs.app_loader);
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
