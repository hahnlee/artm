#include "runtime_app_bootstrap.h"

#include <cstring>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "base/logging.h"
#include "class_linker.h"
#include "dex/art_dex_file_loader.h"
#include "handle_scope-inl.h"
#include "jni/java_vm_ext.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "runtime.h"
#include "runtime_process_state.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

namespace darwin_art_app {

int load_classes(JNIEnv* env,
                 art::Thread* self,
                 art::ClassLinker* class_linker,
                 art::ScopedObjectAccess& soa,
                 art::StackHandleScope<32>& hs,
                 bool run_apk_app,
                 const char* app_dex,
                 const char* support_dex,
                 const char* native_library_path,
                 const char* activity_descriptor,
                 bool run_direct_apk,
                 const char* direct_apk_path,
                 bool run_elf_jni_fixture,
                 bool run_network_acceptance,
                 bool probe_canvas_backend,
                 ClassSet* out) {
  if (out == nullptr || self == nullptr || class_linker == nullptr ||
      app_dex == nullptr || activity_descriptor == nullptr) {
    return 3;
  }

  std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files =
      darwin_art_process::app_dex_files();
  CHECK(app_dex_files.empty());
  std::string dex_error;
  if (run_apk_app) {
    art::ArtDexFileLoader support_loader(support_dex);
    if (!support_loader.Open(/* verify= */ true,
                             /* verify_checksum= */ true, &dex_error,
                             &app_dex_files)) {
      std::cerr << "ART Darwin support DEX: open failed: " << dex_error << "\n";
      return 3;
    }
  }
  art::ArtDexFileLoader dex_loader(app_dex);
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

  art::Handle<art::mirror::ClassLoader> app_loader =
      hs.NewHandle(soa.Decode<art::mirror::ClassLoader>(
          class_linker->CreatePathClassLoader(self, app_dex_file_ptrs)));
  if (app_loader == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: PathClassLoader creation failed\n";
    return 4;
  }
  out->app_loader = soa.AddLocalReference<jobject>(app_loader.Get());
  // ClassLinker::CreatePathClassLoader is an ART test helper: it allocates and
  // wires BaseDexClassLoader fields directly, without running ClassLoader's
  // Java constructor. Production Android creates the loader through that
  // constructor, which owns maps used by Class.getPackage(), certificates,
  // assertions, and class-loader values. Complete the omitted base
  // construction while preserving the helper-installed boot parent and
  // DexPathList.
  jclass class_loader_class = env->FindClass("java/lang/ClassLoader");
  jmethodID get_parent =
      class_loader_class == nullptr
          ? nullptr
          : env->GetMethodID(class_loader_class, "getParent",
                             "()Ljava/lang/ClassLoader;");
  jmethodID class_loader_constructor =
      class_loader_class == nullptr
          ? nullptr
          : env->GetMethodID(class_loader_class, "<init>",
                             "(Ljava/lang/ClassLoader;)V");
  jobject loader_parent =
      get_parent == nullptr
          ? nullptr
          : env->CallObjectMethod(out->app_loader, get_parent);
  if (class_loader_constructor != nullptr && !env->ExceptionCheck()) {
    env->CallNonvirtualVoidMethod(out->app_loader, class_loader_class,
                                  class_loader_constructor, loader_parent);
  }
  env->DeleteLocalRef(loader_parent);
  env->DeleteLocalRef(class_loader_class);
  if (class_loader_constructor == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: ClassLoader base initialization failed\n";
    return 4;
  }
  for (const auto& dex_file : app_dex_files) {
    if (class_linker->RegisterDexFile(*dex_file, app_loader.Get()) == nullptr) {
      std::cerr << "ART Darwin DEX: registration failed\n";
      return 4;
    }
  }

  auto find = [&](const char* descriptor) -> jclass {
    art::Handle<art::mirror::Class> klass = hs.NewHandle(
        class_linker->FindClass(self, descriptor, std::strlen(descriptor),
                                app_loader));
    if (klass == nullptr || self->IsExceptionPending()) {
      return nullptr;
    }
    return soa.AddLocalReference<jclass>(klass.Get());
  };

  out->hello = find("Ldev/darwinart/probe/Hello;");
  if (out->hello == nullptr) {
    std::cerr << "ART Darwin DEX: Hello class lookup failed\n";
    return 5;
  }
  out->activity = find(activity_descriptor);
  if (out->activity == nullptr) {
    std::cerr << "ART Android framework: Activity subclass lookup failed\n";
    return 21;
  }
  out->context = find("Ldev/darwinart/probe/ProbeContext;");
  out->resources = find("Ldev/darwinart/probe/ProbeResources;");
  out->view = find("Ldev/darwinart/probe/ProbeView;");
  out->content_root = find("Ldev/darwinart/probe/ProbeContentRoot;");
  out->package_manager = find("Ldev/darwinart/probe/ProbePackageManager;");
  if (out->context == nullptr || out->resources == nullptr || out->view == nullptr ||
      out->content_root == nullptr || out->package_manager == nullptr) {
    std::cerr << "ART Android window: framework probe class lookup failed\n";
    return 21;
  }

  if (probe_canvas_backend) {
    out->canvas = find("Ldev/darwinart/probe/ProbeCanvas;");
    if (out->canvas == nullptr) {
      std::cerr << "ART Android window: ProbeCanvas lookup failed\n";
      return 21;
    }
  }
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
          env, direct_apk_path, out->app_loader, nullptr, &direct_error);
    }
    if (!direct_loaded || !direct_error.empty() || env->ExceptionCheck()) {
      std::cerr << "ART Android direct APK JavaVMExt load failed, load_error="
                << direct_error << "\n";
      return 46;
    }
    darwin_art_process::record_direct_apk_loaded();
  }
  if (run_elf_jni_fixture) {
    out->native_fixture = find("Ldarwin/art/nativefixture/NativeFixture;");
  }
  if (run_network_acceptance) {
    out->network_fixture =
        find("Ldev/darwinart/probe/NetworkRuntimeFixture;");
  }
  if ((run_elf_jni_fixture && out->native_fixture == nullptr) ||
      (run_network_acceptance && out->network_fixture == nullptr)) {
    std::cerr << "ART Android window: fixture class lookup failed\n";
    return 21;
  }
  return 0;
}

int load_native_library(JNIEnv* env, art::Thread* self, jobject app_loader,
                        const char* native_library_path) {
  if (env == nullptr || self == nullptr || app_loader == nullptr ||
      native_library_path == nullptr || native_library_path[0] == '\0') {
    return 46;
  }
  std::string native_error;
  bool loaded = false;
  {
    // ART's JavaVMExt loader may enter NativeBridge and execute JNI_OnLoad;
    // keep the owner thread in the native state exactly as the platform
    // Runtime.nativeLoad path does, but invoke it only after the app
    // ClassLoader has been installed by the registration phase.
    art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
    loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
        env, native_library_path, app_loader, nullptr, &native_error);
  }
  if (!loaded || !native_error.empty() || env->ExceptionCheck()) {
    std::cerr << "ART Android APK JNI: JavaVMExt load failed path="
              << native_library_path << " error=" << native_error << "\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 46;
  }
  std::cerr << "ART Android APK JNI: JavaVMExt+NativeBridge load ok\n";
  return 0;
}

int install_native_library_path(JNIEnv* env, jobject app_loader,
                                const char* native_library_path) {
  if (env == nullptr || app_loader == nullptr || native_library_path == nullptr ||
      native_library_path[0] == '\0') {
    return 46;
  }
  const std::string path(native_library_path);
  const std::string::size_type slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return 46;
  }
  const std::string directory = path.substr(0, slash);
  jclass array_list_class = env->FindClass("java/util/ArrayList");
  jclass loader_class = env->GetObjectClass(app_loader);
  if (array_list_class == nullptr || loader_class == nullptr) {
    return 46;
  }
  jmethodID array_list_ctor =
      env->GetMethodID(array_list_class, "<init>", "()V");
  jmethodID add = env->GetMethodID(array_list_class, "add", "(Ljava/lang/Object;)Z");
  jmethodID add_native_path = env->GetMethodID(
      loader_class, "addNativePath", "(Ljava/util/Collection;)V");
  jobject paths = array_list_ctor == nullptr
                      ? nullptr
                      : env->NewObject(array_list_class, array_list_ctor);
  jstring directory_string = env->NewStringUTF(directory.c_str());
  if (paths == nullptr || directory_string == nullptr || add == nullptr ||
      add_native_path == nullptr) {
    return 46;
  }
  env->CallBooleanMethod(paths, add, directory_string);
  env->CallVoidMethod(app_loader, add_native_path, paths);
  const bool failed = env->ExceptionCheck();
  env->DeleteLocalRef(directory_string);
  env->DeleteLocalRef(paths);
  env->DeleteLocalRef(loader_class);
  env->DeleteLocalRef(array_list_class);
  if (failed) {
    return 46;
  }
  std::cerr << "ART Android APK: PathClassLoader nativeLibraryPath installed dir="
            << directory << "\n";
  return 0;
}

}  // namespace darwin_art_app
