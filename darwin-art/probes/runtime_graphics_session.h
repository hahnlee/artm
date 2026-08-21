#pragma once

#include <cstdint>

#include "darwin_art/darwin_art.h"

namespace darwin_art_graphics {

// Session state is an opaque owner-thread token.  The implementation keeps
// JNI/HWUI objects out of this header and delegates cleanup to the existing
// runtime_graphics_state owner.
darwin_art_graphics_session_t* create_session();
int32_t close_session(darwin_art_graphics_session_t* session);
int32_t destroy_session(darwin_art_graphics_session_t* session);
int32_t dispatch_pointer(darwin_art_graphics_session_t* session,
                         uint32_t action, float x, float y);
int32_t pump_frame(darwin_art_graphics_session_t* session,
                   int64_t frame_time_nanos);

}  // namespace darwin_art_graphics
