#pragma once

#include <jni.h>

namespace darwin_art_graphics {
struct GraphicsState;
}

namespace art {
class Thread;
}

namespace darwin_art_registration_phase {

struct Inputs {
  JNIEnv* env;
  art::Thread* self;
  jobject app_loader_ref;
  jclass probe_canvas_class;
  darwin_art_graphics::GraphicsState* graphics_state;
};

// Completes ART's boot-class native registration and root class
// initialization before any application/support class is loaded. Android's
// boot image normally provides this ordering; the detached Darwin runtime has
// to establish it explicitly.
int start(JNIEnv* env, art::Thread* self);

// Installs application-loader-owned state after the APK/support DEX classes
// have been loaded. Returns the process-probe status code (0 success).
int finish(const Inputs& inputs);

}  // namespace darwin_art_registration_phase
