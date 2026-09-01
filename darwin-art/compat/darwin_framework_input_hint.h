#pragma once

#include "darwin_surface_bridge.h"

namespace darwin_art {

enum class DarwinArtInputPacketKind : uint32_t {
  kPointer = 1,
  kKey = 2,
};

struct DarwinArtInputPacket {
  DarwinArtInputPacketKind kind = DarwinArtInputPacketKind::kPointer;
  DarwinArtPointerEventV2 pointer{};
  DarwinArtKeyEventV1 key{};
};

// Host/AppKit to Android InputEventReceiver cooperative-yield bridge. This
// header intentionally has no JNI dependency so the surface/Metal bridge can
// publish the hint from its lightweight graphics compilation unit.
void NotifyFrameworkInputPending();
void ClearFrameworkInputPending();

// Payload bridge used by the AppKit producer and the Android owner consumer.
// The channel implementation owns bounded storage; these functions return
// false when no focused channel is available so legacy mailbox fallback can
// preserve old probe behavior.
bool EnqueueFrameworkPointerPacket(const DarwinArtPointerEventV2& packet);
bool DequeueFrameworkPointerPacket(DarwinArtPointerEventV2* packet);
bool EnqueueFrameworkKeyPacket(const DarwinArtKeyEventV1& packet);
bool DequeueFrameworkKeyPacket(DarwinArtKeyEventV1* packet);

}  // namespace darwin_art
