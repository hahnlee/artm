#include "darwin_framework_natives.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace {

#if !defined(DARWIN_ART_REAL_GRAPHICS)
struct DarwinRenderNode {
  explicit DarwinRenderNode(std::string node_name)
      : name(std::move(node_name)) {}

  std::string name;
  jint left = 0;
  jint top = 0;
  jint right = 0;
  jint bottom = 0;
};

std::optional<std::string> JavaString(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) {
    return std::nullopt;
  }
  std::string result(utf);
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

void RenderNodeFinalizer(void* render_node) {
  delete static_cast<DarwinRenderNode*>(render_node);
}

jlong RenderNodeCreate(JNIEnv* env, jclass, jstring name) {
  std::optional<std::string> node_name = JavaString(env, name);
  return reinterpret_cast<std::uintptr_t>(
      new DarwinRenderNode(node_name.value_or("")));
}

jlong RenderNodeGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&RenderNodeFinalizer);
}

jboolean RenderNodeSetLeftTopRightBottom(jlong handle, jint left, jint top,
                                         jint right, jint bottom) {
  auto* node = reinterpret_cast<DarwinRenderNode*>(
      static_cast<std::uintptr_t>(handle));
  if (node == nullptr) {
    return JNI_FALSE;
  }
  const bool changed = node->left != left || node->top != top ||
                       node->right != right || node->bottom != bottom;
  node->left = left;
  node->top = top;
  node->right = right;
  node->bottom = bottom;
  return changed ? JNI_TRUE : JNI_FALSE;
}

jboolean RenderNodeHasIdentityMatrix(jlong) { return JNI_TRUE; }

bool Register(JNIEnv* env, const char* class_name, JNINativeMethod* methods,
              jint method_count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) {
    return false;
  }
  const bool registered =
      env->RegisterNatives(klass, methods, method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}
#endif

}  // namespace

namespace darwin_art {

bool RegisterFrameworkRenderNodeNatives(JNIEnv* env) {
#if defined(DARWIN_ART_REAL_GRAPHICS)
  (void)env;
  return true;
#else
  JNINativeMethod methods[] = {
      {const_cast<char*>("nCreate"), const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&RenderNodeCreate)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&RenderNodeGetNativeFinalizer)},
      {const_cast<char*>("nSetLeftTopRightBottom"),
       const_cast<char*>("(JIIII)Z"),
       reinterpret_cast<void*>(&RenderNodeSetLeftTopRightBottom)},
      {const_cast<char*>("nHasIdentityMatrix"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&RenderNodeHasIdentityMatrix)},
  };
  return Register(env, "android/graphics/RenderNode", methods,
                  static_cast<jint>(std::size(methods)));
#endif
}

}  // namespace darwin_art
