#pragma once

#include <jni.h>

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

}  // namespace darwin_art
