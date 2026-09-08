#pragma once

#include <jni.h>

namespace darwin_art {

// Registers the Java MediaCodec/MediaCodecList bridge. The implementation is
// deliberately kept outside the general framework native TU so codec state,
// host framework probing, and the graphics bridge do not share a translation
// unit or cache invalidation boundary.
bool RegisterDarwinMediaCodecNatives(JNIEnv* env);

// Runs the public Java configure/setOutputSurface/release contract and checks
// that native producer references survive and retire at the AOSP boundaries.
bool VerifyDarwinMediaCodecSurfaceLifecycle(JNIEnv* env);

}  // namespace darwin_art
