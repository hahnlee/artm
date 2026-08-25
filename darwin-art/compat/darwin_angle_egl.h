#pragma once

#include <jni.h>

namespace darwin_art {

// Registers the Android EGL10 facade and the small GLES20 bootstrap surface
// used by GLSurfaceView. The implementation delegates to ANGLE's Darwin
// dylibs and keeps Android's Java handle objects as the public ABI.
bool RegisterDarwinAngleEglNatives(JNIEnv* env);

// Receives generic SurfaceView window geometry from the framework bridge.
// This is intentionally independent of any APK class.
void ConfigureDarwinAngleHostSurface(jint x, jint y, jint width, jint height);

}  // namespace darwin_art
