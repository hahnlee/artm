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
bool RegisterFrameworkAnimationNatives(JNIEnv* env);
bool RegisterFrameworkSupportNatives(JNIEnv* env);
bool RegisterFrameworkResourceNatives(JNIEnv* env);
bool RegisterFrameworkGraphicsNatives(JNIEnv* env);

}  // namespace darwin_art
