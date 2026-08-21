#include "runtime_graphics_probe.h"

#include "darwin_art/darwin_art.h"

namespace art {
class Thread;
}

namespace darwin_art_graphics {

// The headless/runtime flavor intentionally has no GraphicsSession owner.
// Keep the process ABI link closed without making the host discover a
// graphics-session capability that only the real-graphics flavor provides.
__attribute__((weak)) int32_t bind_session_for_process(void*) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_NOT_READY;
}

__attribute__((weak)) int32_t bind_session_art_thread(art::Thread*) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_NOT_READY;
}

__attribute__((weak)) GraphicsState* state_for_context(void*) {
  return nullptr;
}

__attribute__((weak)) int32_t finalize_bound_session(GraphicsState*) {
  return DARWIN_ART_STATUS_GRAPHICS_SESSION_NOT_READY;
}

}  // namespace darwin_art_graphics
