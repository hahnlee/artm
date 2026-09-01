#pragma once

namespace darwin_art {

// Host/AppKit to Android InputEventReceiver cooperative-yield bridge. This
// header intentionally has no JNI dependency so the surface/Metal bridge can
// publish the hint from its lightweight graphics compilation unit.
void NotifyFrameworkInputPending();
void ClearFrameworkInputPending();

}  // namespace darwin_art
