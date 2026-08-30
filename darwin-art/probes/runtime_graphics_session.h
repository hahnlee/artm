#pragma once

#include <cstdint>

#include "darwin_art/darwin_art.h"
#include "darwin_surface_bridge.h"
#include "runtime_graphics_state.h"

namespace art {
class Thread;
}

namespace darwin_art_graphics {

// Binds the pre-created opaque handle to the one process run. These calls are
// made on the ART owner thread; no JNI/HWUI object crosses the ABI.
int32_t bind_session_for_process(void* context);
int32_t bind_session_art_thread(art::Thread* thread);
GraphicsState* state_for_context(void* context);

// Session state is an opaque owner-thread token.  The implementation keeps
// JNI/HWUI objects out of this header and delegates cleanup to the existing
// runtime_graphics_state owner.
darwin_art_graphics_session_t* create_session();
int32_t close_session(darwin_art_graphics_session_t* session);
// Marks the bound session finalized after GraphicsState::shutdown has released
// all JNI/HWUI references, but before DestroyJavaVM.  Rust may then drop the
// opaque owner after VM teardown without re-entering ART.
int32_t finalize_bound_session(GraphicsState* state);
int32_t destroy_session(darwin_art_graphics_session_t* session);
int32_t dispatch_pointer(darwin_art_graphics_session_t* session,
                         uint32_t action, float x, float y);
int32_t dispatch_pointer_v2(darwin_art_graphics_session_t* session,
                            const DarwinArtPointerEventV2* event);
int32_t dispatch_key_v1(darwin_art_graphics_session_t* session,
                        const DarwinArtKeyEventV1* event);
int32_t pump_main_looper(darwin_art_graphics_session_t* session);
int32_t pump_frame(darwin_art_graphics_session_t* session,
                   int64_t frame_time_nanos);

}  // namespace darwin_art_graphics
