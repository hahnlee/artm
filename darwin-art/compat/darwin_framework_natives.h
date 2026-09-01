#pragma once

#include <jni.h>

#include "darwin_framework_input_hint.h"

namespace darwin_art {

enum class FrameworkGraphicsBackend {
  // Temporary Java pixel-array Canvas used by the current executable probe.
  kProbeCanvas,
  // AOSP libandroid_runtime graphics registration and native Skia objects.
  kAndroidGraphics,
};

FrameworkGraphicsBackend GetFrameworkGraphicsBackend();
bool InitializeFrameworkGraphicsRuntime();
void ShutdownFrameworkGraphicsRuntime();
bool InstallFrameworkResourceRuntime(JNIEnv* env);
bool ShutdownFrameworkResourceRuntime(JNIEnv* env);
bool RegisterFrameworkNatives(JNIEnv* env);
bool RegisterMotionEventNatives(JNIEnv* env);
bool RegisterFrameworkBinderNatives(JNIEnv* env);
bool RegisterFrameworkSystemPropertyNatives(JNIEnv* env);
bool RegisterFrameworkAssetManagerNatives(JNIEnv* env);
bool RegisterFrameworkRenderNodeNatives(JNIEnv* env);
bool RegisterFrameworkAnimationNatives(JNIEnv* env);
bool RegisterFrameworkSqliteNatives(JNIEnv* env);
bool RegisterFrameworkSupportNatives(JNIEnv* env);
bool RegisterFrameworkResourceNatives(JNIEnv* env);
bool RegisterFrameworkGraphicsNatives(JNIEnv* env);

// SurfaceFlinger's display edge is supplied by the host frame clock. Pending
// DisplayEventReceivers are delivered on the Android owner/Looper thread;
// their normal asynchronous Handler path then invokes Choreographer.doFrame.
int DispatchFrameworkPendingVsyncs(JNIEnv* env, jlong frame_time_nanos);

// Delivers a host-created Android InputEvent through the WindowInputEventReceiver
// owned by a real ViewRootImpl. The receiver must have been initialized from a
// Darwin InputChannel supplied by IWindowSession.addToDisplay.
bool DispatchFrameworkInputEvent(JNIEnv* env, jobject view_root, jobject event,
                                 bool* handled = nullptr);

// Announces the WindowManager focus assignment through the ViewRoot-owned
// WindowInputEventReceiver. The resulting Handler message is processed before
// host input is admitted, matching InputDispatcher's focus-before-pointer
// ordering.
bool FocusFrameworkViewRoot(JNIEnv* env, jobject view_root);
bool SetFrameworkViewRootFocus(JNIEnv* env, jobject view_root, bool focused);

}  // namespace darwin_art
