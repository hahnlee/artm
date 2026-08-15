#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_framework_natives.h"
#include "darwin_icu_natives.h"
#include "darwin_libcore_natives.h"
#include "darwin_window_bridge.h"
#include "dex/art_dex_file_loader.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/throwable.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "well_known_classes.h"

static jint HostPageSize(JNIEnv*, jclass) { return getpagesize(); }

static std::size_t g_frame_width = 0;
static std::size_t g_frame_height = 0;

static double WindowVisibleSeconds() {
  const char* value = std::getenv("DARWIN_ART_WINDOW_SECONDS");
  if (value == nullptr) {
    return 0.0;
  }
  char* end = nullptr;
  const double seconds = std::strtod(value, &end);
  return end == value || seconds < 0.0 || seconds > 30.0 ? 0.0 : seconds;
}

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
  const bool presented = DarwinPresentArgb(
      reinterpret_cast<const std::uint32_t*>(pixels.data()),
      static_cast<std::size_t>(width), static_cast<std::size_t>(height),
      WindowVisibleSeconds());
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
  jclass canvas_class = env->FindClass("dev/darwinart/probe/ProbeCanvas");
  jobject canvas =
      canvas_class == nullptr ? nullptr : env->AllocObject(canvas_class);
  jmethodID initialize =
      canvas_class == nullptr
          ? nullptr
          : env->GetMethodID(canvas_class, "initialize", "(II)V");
  jmethodID snapshot =
      canvas_class == nullptr
          ? nullptr
          : env->GetMethodID(canvas_class, "snapshot", "()[I");
  jclass view_class = env->FindClass("android/view/View");
  jmethodID layout =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "layout", "(IIII)V");
  jmethodID draw =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "draw", "(Landroid/graphics/Canvas;)V");
  jfieldID view_left =
      view_class == nullptr ? nullptr : env->GetFieldID(view_class, "mLeft", "I");
  jfieldID view_top =
      view_class == nullptr ? nullptr : env->GetFieldID(view_class, "mTop", "I");
  jfieldID view_right =
      view_class == nullptr ? nullptr : env->GetFieldID(view_class, "mRight", "I");
  jfieldID view_bottom =
      view_class == nullptr ? nullptr : env->GetFieldID(view_class, "mBottom", "I");
  if (canvas == nullptr || initialize == nullptr || snapshot == nullptr ||
      layout == nullptr || draw == nullptr || view_left == nullptr ||
      view_top == nullptr || view_right == nullptr || view_bottom == nullptr ||
      env->ExceptionCheck()) {
    return JNI_FALSE;
  }

  env->CallVoidMethod(canvas, initialize, width, height);
  // ViewRoot normally installs the surface bounds before the first traversal.
  // The Darwin window policy owns that root, so seed the same bounds before
  // layout. This prevents a detached View from trying to notify Android's
  // accessibility/window services merely because its initial frame changed.
  env->SetIntField(view, view_left, 0);
  env->SetIntField(view, view_top, 0);
  env->SetIntField(view, view_right, width);
  env->SetIntField(view, view_bottom, height);
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  env->CallVoidMethod(view, draw, canvas);
  jintArray pixels = static_cast<jintArray>(env->CallObjectMethod(canvas, snapshot));
  const jboolean presented =
      env->ExceptionCheck() || pixels == nullptr
          ? JNI_FALSE
          : PresentFrame(env, nullptr, width, height, pixels);
  env->DeleteLocalRef(pixels);
  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(canvas);
  env->DeleteLocalRef(canvas_class);
  return presented;
}

int main(int argc, char** argv) {
  // Darwin's malloc zones can claim the fixed compressed-reference window
  // while RuntimeArgumentMap is being assembled. Reserve ART's bounded arena
  // before the launcher performs its first heap allocation.
  art::MemMap::Init();

  if (argc != 6) {
    std::cerr << "usage: runtime-link-probe CORE_OJ_JAR CORE_LIBART_JAR "
                 "FRAMEWORK_JAR CORE_ICU4J_JAR CLASSES_DEX\n";
    return 64;
  }

  std::string boot_class_path =
      std::string(argv[1]) + ":" + argv[2] + ":" + argv[3] + ":" + argv[4];
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
              art::MemoryKiB(64 * 1024 * 1024));
  options.Set(art::RuntimeArgumentMap::MemoryMaximumSize,
              art::MemoryKiB(64 * 1024 * 1024));
  art::LogVerbosity verbosity{};
  verbosity.heap = true;
  options.Set(art::RuntimeArgumentMap::Verbose, verbosity);

  if (!art::Runtime::Create(std::move(options))) {
    return 1;
  }

  art::Thread* self = art::Thread::Current();
  if (self == nullptr) {
    std::cerr << "ART Darwin DEX: no current thread\n";
    return 2;
  }

  art::interpreter::UnstartedRuntime::Initialize();
  art::ScopedObjectAccess soa(self);
  art::WellKnownClasses::Init(self->GetJniEnv());

  std::vector<std::unique_ptr<const art::DexFile>> app_dex_files;
  std::string dex_error;
  art::ArtDexFileLoader dex_loader(argv[5]);
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
  art::StackHandleScope<9> hs(self);
  art::Handle<art::mirror::ClassLoader> app_loader =
      hs.NewHandle(soa.Decode<art::mirror::ClassLoader>(loader_ref));
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
  art::Handle<art::mirror::Class> probe_canvas_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/ProbeCanvas;",
                              sizeof("Ldev/darwinart/probe/ProbeCanvas;") - 1u,
                              app_loader));
  art::Handle<art::mirror::Class> darwin_window_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/DarwinWindow;",
                              sizeof("Ldev/darwinart/probe/DarwinWindow;") - 1u,
                              app_loader));
  if (probe_canvas_handle == nullptr || darwin_window_handle == nullptr ||
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
  jclass darwin_window_class =
      soa.AddLocalReference<jclass>(darwin_window_handle.Get());
  art::Runtime::Current()->StartMinimalForDarwinProbe(self->GetJniEnv());
  if (!darwin_art::RegisterLibcoreNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin libcore: native registration failed\n";
    return 17;
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

  JNIEnv* env = self->GetJniEnv();
  JNINativeMethod present_method{
      const_cast<char*>("presentContent"),
      const_cast<char*>("(Landroid/view/View;II)Z"),
      reinterpret_cast<void*>(&PresentContent),
  };
  if (env->RegisterNatives(darwin_window_class, &present_method, 1) != JNI_OK ||
      !class_linker->EnsureInitialized(self, darwin_window_handle, true, true)) {
    std::cerr << "ART Android window: DarwinWindow native setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 32;
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
  jobject probe_resources = probe_resources_class == nullptr
                                ? nullptr
                                : env->AllocObject(probe_resources_class);
  jmethodID probe_context_constructor =
      probe_context_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_context_class, "<init>",
                             "(Landroid/content/res/Resources;)V");
  jobject probe_context =
      probe_context_constructor == nullptr || probe_resources == nullptr
          ? nullptr
          : env->NewObject(probe_context_class, probe_context_constructor,
                           probe_resources);
  jmethodID darwin_window_constructor =
      darwin_window_class == nullptr
          ? nullptr
          : env->GetMethodID(darwin_window_class, "<init>",
                             "(Landroid/content/Context;)V");
  jobject darwin_window =
      darwin_window_constructor == nullptr || probe_context == nullptr
          ? nullptr
          : env->NewObject(darwin_window_class, darwin_window_constructor,
                           probe_context);
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
      probe_context == nullptr || darwin_window == nullptr || intent == nullptr ||
      configuration == nullptr || attach_activity == nullptr ||
      env->ExceptionCheck()) {
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
  jfieldID activity_window = env->GetFieldID(
      activity_class, "mWindow", "Landroid/view/Window;");
  jclass window_class = env->FindClass("android/view/Window");
  jmethodID set_window_callback =
      window_class == nullptr
          ? nullptr
          : env->GetMethodID(window_class, "setCallback",
                             "(Landroid/view/Window$Callback;)V");
  if (activity_window == nullptr || set_window_callback == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: DarwinWindow policy setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }
  env->CallVoidMethod(darwin_window, set_window_callback, activity_instance);
  env->SetObjectField(activity_instance, activity_window, darwin_window);
  env->DeleteLocalRef(window_class);
  jmethodID get_window =
      env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject window = get_window == nullptr
                       ? nullptr
                       : env->CallObjectMethod(activity_instance, get_window);
  const bool attached_darwin_window =
      window != nullptr && env->IsInstanceOf(window, darwin_window_class);
  if (!attached_darwin_window || env->ExceptionCheck()) {
    std::cerr << "ART Android window: DarwinWindow attachment failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
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
  jmethodID get_content = env->GetMethodID(
      darwin_window_class, "getContent", "()Landroid/view/View;");
  jobject probe_view =
      get_content == nullptr
          ? nullptr
          : env->CallObjectMethod(darwin_window, get_content);
  jmethodID was_presented =
      env->GetMethodID(probe_view_class, "wasPresented", "()Z");
  const jboolean view_presented =
      probe_view == nullptr ||
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
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(darwin_window);
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
  env->DeleteLocalRef(probe_context);
  env->DeleteLocalRef(probe_resources);
  env->DeleteLocalRef(probe_resources_class);
  env->DeleteLocalRef(probe_view_class);
  env->DeleteLocalRef(darwin_window_class);
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

  JNINativeMethod native_method{
      const_cast<char*>("hostPageSize"),
      const_cast<char*>("()I"),
      reinterpret_cast<void*>(&HostPageSize),
  };
  if (self->GetJniEnv()->RegisterNatives(hello_class, &native_method, 1) !=
      JNI_OK) {
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

  std::cout << "ART Darwin Runtime::Create: ok\n"
            << "ART Darwin app ClassLoader: PathClassLoader\n"
            << "ART Darwin DEX interpreter: Hello.answer()=" << result.GetI()
            << "\n"
            << "ART Darwin JNI: hostPageSize()=" << getpagesize()
            << " nativeRoundTrip()=" << native_result.GetI() << "\n"
            << "ART runtime native: System.arraycopy()="
            << arraycopy_result.GetI() << "\n"
            << "ART Android framework: ProbeActivity().probeValue()="
            << activity_result << "\n"
            << "ART Android window: Activity.attach()=DarwinWindow\n"
            << "ART Android view: Activity.setContentView()->View.draw(Canvas)="
            << g_frame_width << "x"
            << g_frame_height << "\n"
            << "ART Android lifecycle: Activity.onCreate()=" << lifecycle_result
            << "\n"
            << "ART Darwin launcher: main(String[])=ok\n";
  return 0;
}
