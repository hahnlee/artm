#include "runtime_registration_phase.h"

#include <iostream>

#include "darwin_framework_natives.h"
#include "darwin_icu_natives.h"
#include "darwin_libcore_natives.h"
#include "darwin_openjdk_natives.h"
#include "runtime_filesystem_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_state.h"
#include "runtime_process_state.h"
#include "runtime.h"
#include "thread-current-inl.h"
#include "mirror/throwable.h"

extern "C" int darwin_art_install_context_loader(JNIEnv* env,
                                                   jobject app_loader);

namespace darwin_art_registration_phase {

int run(const Inputs& inputs) {
  if (inputs.env == nullptr || inputs.self == nullptr ||
      inputs.app_loader_ref == nullptr) {
    return 4;
  }
  JNIEnv* env = inputs.env;
  art::Thread* self = inputs.self;

  art::Runtime::Current()->StartMinimalForDarwinProbe(env);
  if (!InstallProbeAndroidSystemRoot()) {
    std::cerr << "ART Android filesystem: test system root install failed\n";
    return 40;
  }
  if (!darwin_art::RegisterLibcoreNatives(env)) {
    std::cerr << "ART Darwin libcore: native registration failed\n";
    return 17;
  }
  register_java_lang_Math(env);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Darwin OpenJDK: Math native registration failed\n";
    return 37;
  }
  if (!darwin_art::RegisterIcuCharsetNatives(env)) {
    std::cerr << "ART Darwin ICU: charset native registration failed\n";
    return 20;
  }
  if (!darwin_art::RegisterFrameworkNatives(env)) {
    std::cerr << "ART Darwin framework: native registration failed\n";
    return 26;
  }
  art::Runtime::Current()->FinishMinimalForDarwinProbe();
  if (!darwin_art::InstallFrameworkResourceRuntime(env)) {
    std::cerr << "ART Darwin resources: AndroidRuntime ownership install failed\n";
    return 38;
  }
  darwin_art_process::record_resource_runtime_installed();
  if (!darwin_art::RegisterFrameworkResourceNatives(env)) {
    std::cerr << "ART Darwin resources: native registration failed\n";
    return 39;
  }
  if (!darwin_art::RegisterFrameworkGraphicsNatives(env)) {
    std::cerr << "ART Darwin graphics: native registration failed\n";
    return 35;
  }

  // ActivityThread performs this after minimal runtime startup. Keep this
  // JNI-only bridge independent from the process/activity entry TU.
  if (darwin_art_install_context_loader(env, inputs.app_loader_ref) != 0) {
    return 4;
  }

  if (darwin_art::GetFrameworkGraphicsBackend() ==
      darwin_art::FrameworkGraphicsBackend::kProbeCanvas) {
    // Headless/runtime flavor intentionally has no GraphicsSession owner.
    // The real-graphics flavor supplies the state and installs the HWUI
    // canvas class; never manufacture a GPU owner in the CPU acceptance path.
    if (inputs.graphics_state != nullptr) {
      darwin_art_graphics::set_probe_canvas_class(inputs.graphics_state, env,
                                                   inputs.probe_canvas_class);
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
  return 0;
}

}  // namespace darwin_art_registration_phase
