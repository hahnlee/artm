#include <mach-o/dyld.h>

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

#include <cstddef>

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
#include "runtime_process_options.h"
#include "runtime_acceptance_phases.h"
#include "runtime_jni_scope.h"
#include "runtime_shutdown_probe.h"
#include "runtime_frame_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_session.h"
#include "runtime_graphics_phase.h"
#include "runtime_jni_acceptance_probe.h"
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

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#include "runtime_apk_graph.h"
#endif

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

  if (!darwin_art_process::begin_run()) {
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

  std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files =
      darwin_art_process::app_dex_files();
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
    darwin_art_process::record_direct_apk_loaded();
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
  darwin_art_process::record_resource_runtime_installed();
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
    // Headless/runtime flavor intentionally has no GraphicsSession owner.
    // The real-graphics flavor supplies the state and installs the HWUI
    // canvas class; do not make the CPU acceptance path manufacture a GPU
    // owner merely because the common framework backend is selected.
    if (graphics_state != nullptr) {
      darwin_art_graphics::set_probe_canvas_class(graphics_state, env,
                                                  probe_canvas_class);
      if (env->ExceptionCheck()) {
        std::cerr << "ART Android window: ProbeCanvas global root failed\n";
        return 32;
      }
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
  if (graphics_state != nullptr &&
      darwin_art_graphics_phase::present_and_retain(
          graphics_state, env, decor_view, content_root_class, content_root,
          probe_view_class, probe_view, run_apk_app, expect_apk_widgets,
          run_apk_app || run_framework_button, kApkFrameWidth * window_scale,
          kApkFrameHeight * window_scale) != 0) {
    return 33;
  }
  env->DeleteLocalRef(application);
  env->DeleteLocalRef(activity_info);
  env->DeleteLocalRef(context_theme_wrapper_class);
  env->DeleteLocalRef(probe_theme);
  env->DeleteLocalRef(window_background);
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
      collections_ok = darwin_art_elf_probe::run_android_elf_self_test(
          env, vm, app_loader_ref, libcxx_collections_path, &libcxx_error);
      if (collections_ok) {
        exception_ok = darwin_art_elf_probe::run_android_elf_self_test(
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
      tls_ok = darwin_art_elf_probe::run_android_elf_self_test(
          env, vm, app_loader_ref, tls_fixture_path, &tls_error);
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
    darwin_art_process::record_apk_elf_loaded(apk_sha256, apk_root_sha256);
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
