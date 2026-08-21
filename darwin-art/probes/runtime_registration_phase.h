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

// Performs the one-time Android framework/native registration sequence for a
// running ART owner thread. Returns the process-probe status code (0 success).
int run(const Inputs& inputs);

}  // namespace darwin_art_registration_phase
