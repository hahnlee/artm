#include "darwin_framework_natives.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)
#include <android/graphics/jni_runtime.h>

namespace android {
int register_android_util_Log(JNIEnv* env);
int register_android_content_AssetManager(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
int register_android_content_res_ApkAssets(JNIEnv* env);
int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);
}  // namespace android
#endif

namespace darwin_art {

bool RegisterFrameworkResourceNatives(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  // Preserve AndroidRuntime.cpp's ownership and registration order. These
  // tables replace the temporary AssetManager table as one atomic resource
  // subsystem; mixing either AssetManager native-handle representation would
  // make Theme/ApkAssets jlong values type-unsafe.
  return android::register_android_util_Log(env) >= 0 &&
         android::register_android_content_AssetManager(env) >= 0 &&
         android::register_android_content_StringBlock(env) >= 0 &&
         android::register_android_content_XmlBlock(env) >= 0 &&
         android::register_android_content_res_ApkAssets(env) >= 0 &&
         android::register_com_android_internal_util_VirtualRefBasePtr(env) >= 0;
#else
  // The baseline probe registers its deliberately small AssetManager table in
  // RegisterFrameworkNatives().
  (void)env;
  return true;
#endif
}

}  // namespace darwin_art
