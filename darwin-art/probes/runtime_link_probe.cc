#include <iostream>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "dex/art_dex_file_loader.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "well_known_classes.h"

static jint HostPageSize(JNIEnv*, jclass) {
  return getpagesize();
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: runtime-link-probe CORE_OJ_JAR CORE_LIBART_JAR CLASSES_DEX\n";
    return 64;
  }

  std::string boot_class_path = std::string(argv[1]) + ":" + argv[2];
  std::cerr << "Mach-O slide: 0x" << std::hex << _dyld_get_image_vmaddr_slide(0) << std::dec
            << "\n";
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
  art::ArtDexFileLoader dex_loader(argv[3]);
  if (!dex_loader.Open(/* verify= */ true,
                       /* verify_checksum= */ true,
                       &dex_error,
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
  jobject loader_ref = class_linker->CreatePathClassLoader(self, app_dex_file_ptrs);
  art::StackHandleScope<2> hs(self);
  art::Handle<art::mirror::ClassLoader> app_loader =
      hs.NewHandle(soa.Decode<art::mirror::ClassLoader>(loader_ref));
  for (const auto& dex_file : app_dex_files) {
    if (class_linker->RegisterDexFile(*dex_file, app_loader.Get()) == nullptr) {
      std::cerr << "ART Darwin DEX: registration failed\n";
      return 4;
    }
  }

  art::Handle<art::mirror::Class> hello = hs.NewHandle(class_linker->FindClass(
      self,
      "Ldev/darwinart/probe/Hello;",
      sizeof("Ldev/darwinart/probe/Hello;") - 1u,
      app_loader));
  if (hello == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: Hello class lookup failed\n";
    return 5;
  }

  jclass hello_class = soa.AddLocalReference<jclass>(hello.Get());
  JNINativeMethod native_method{
      const_cast<char*>("hostPageSize"),
      const_cast<char*>("()I"),
      reinterpret_cast<void*>(&HostPageSize),
  };
  if (self->GetJniEnv()->RegisterNatives(hello_class, &native_method, 1) != JNI_OK) {
    std::cerr << "ART Darwin JNI: RegisterNatives failed\n";
    return 6;
  }
  if (!class_linker->EnsureInitialized(self, hello, true, true)) {
    std::cerr << "ART Darwin JNI: Hello initialization failed\n";
    return 7;
  }
  art::Runtime::Current()->StartMinimalForDarwinProbe();

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

  art::ArtMethod* native_round_trip =
      hello->FindClassMethod("nativeRoundTrip", "()I", art::kRuntimePointerSize);
  if (native_round_trip == nullptr) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I lookup failed\n";
    return 11;
  }
  art::JValue native_result;
  native_round_trip->Invoke(
      self, /* args= */ nullptr, /* args_size= */ 0u, &native_result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I threw\n";
    return 12;
  }
  if (native_result.GetI() != 42) {
    std::cerr << "ART Darwin JNI: expected 42, got " << native_result.GetI() << "\n";
    return 13;
  }

  std::cout << "ART Darwin Runtime::Create: ok\n"
            << "ART Darwin app ClassLoader: PathClassLoader\n"
            << "ART Darwin DEX interpreter: Hello.answer()=" << result.GetI() << "\n"
            << "ART Darwin JNI: hostPageSize()=" << getpagesize()
            << " nativeRoundTrip()=" << native_result.GetI() << "\n";
  return 0;
}
