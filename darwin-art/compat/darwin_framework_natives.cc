#include "darwin_framework_natives.h"
#include "darwin_framework_system_natives.h"

#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void NativeAllocationRegistryApplyFreeFunction(JNIEnv*, jclass,
                                                jlong free_function,
                                                jlong native_ptr) {
  if (free_function == 0 || native_ptr == 0) return;
  using FreeFunction = void (*)(void*);
  reinterpret_cast<FreeFunction>(static_cast<std::uintptr_t>(free_function))(
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(native_ptr)));
}

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

struct DarwinAssetManager {};

struct DarwinTheme {
  explicit DarwinTheme(DarwinAssetManager* owner) : assets(owner) {}

  DarwinAssetManager* assets;
};

#if !defined(DARWIN_ART_REAL_GRAPHICS)
struct DarwinPaint {
  jint flags = 0;
  jint color = 0xff000000;
};

struct DarwinRenderNode {
  explicit DarwinRenderNode(std::string node_name)
      : name(std::move(node_name)) {}

  std::string name;
  jint left = 0;
  jint top = 0;
  jint right = 0;
  jint bottom = 0;
};
#endif

void AssetManagerFinalizer(void* assets) {
  delete static_cast<DarwinAssetManager*>(assets);
}

jlong AssetManagerCreate(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(new DarwinAssetManager());
}

void AssetManagerDestroy(JNIEnv*, jclass, jlong handle) {
  AssetManagerFinalizer(reinterpret_cast<void*>(
      static_cast<std::uintptr_t>(handle)));
}

void ThemeFinalizer(void* theme) { delete static_cast<DarwinTheme*>(theme); }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
void PaintFinalizer(void* paint) { delete static_cast<DarwinPaint*>(paint); }

jlong PaintInit() {
  return reinterpret_cast<std::uintptr_t>(new DarwinPaint());
}

jlong PaintGetNativeFinalizer() {
  return reinterpret_cast<std::uintptr_t>(&PaintFinalizer);
}

void PaintSetFlags(jlong handle, jint flags) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->flags = flags;
  }
}

void PaintSetElegantTextHeight(jlong, jint) {}

jint PaintSetTextLocales(JNIEnv*, jclass, jlong, jstring) { return 0; }

void PaintSetColor(jlong handle, jint color) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->color = color;
  }
}
#endif

jlong AssetManagerGetThemeFreeFunction(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(&ThemeFinalizer);
}

jlong AssetManagerThemeCreate(JNIEnv*, jclass, jlong assets_handle) {
  auto* assets = reinterpret_cast<DarwinAssetManager*>(
      static_cast<std::uintptr_t>(assets_handle));
  return reinterpret_cast<std::uintptr_t>(new DarwinTheme(assets));
}

void AssetManagerThemeApplyStyle(JNIEnv*, jclass, jlong, jlong, jint,
                                 jboolean) {}

void AssetManagerThemeCopy(JNIEnv*, jclass, jlong, jlong, jlong, jlong) {}

jint AssetManagerThemeGetAttributeValue(JNIEnv*, jclass, jlong, jlong, jint,
                                        jobject, jboolean) {
  return 0;
}

jint AssetManagerThemeGetChangingConfigurations(JNIEnv*, jclass, jlong) {
  return 0;
}

void AssetManagerThemeRebase(JNIEnv*, jclass, jlong, jlong, jintArray,
                             jbooleanArray, jint) {}

void AssetManagerApplyStyle(JNIEnv*, jclass, jlong, jlong, jint, jint, jlong,
                            jintArray, jlong, jlong) {
  // Java's TypedArray storage is zero-initialized. Leaving it untouched is an
  // empty style result, so every framework lookup observes its documented
  // default while the real framework-res table parser is brought up.
}

void AssetManagerApplyStyleWithArray(JNIEnv*, jclass, jlong, jlong, jint, jint,
                                     jlong, jintArray, jintArray, jintArray) {}

jboolean AssetManagerResolveAttrs(JNIEnv*, jclass, jlong, jlong, jint, jint,
                                  jintArray, jintArray, jintArray,
                                  jintArray) {
  return JNI_TRUE;
}

jboolean AssetManagerRetrieveAttributes(JNIEnv*, jclass, jlong, jlong,
                                         jintArray, jintArray, jintArray) {
  return JNI_TRUE;
}

void AssetManagerSetApkAssets(JNIEnv*, jclass, jlong, jobjectArray, jboolean,
                              jboolean) {}

void AssetManagerSetConfiguration(
    JNIEnv*, jclass, jlong, jint, jint, jstring, jobjectArray, jint, jint,
    jint, jint, jint, jint, jint, jint, jint, jint, jint, jint, jint, jint,
    jint, jint, jboolean) {}

jobjectArray AssetManagerGetLocales(JNIEnv* env, jclass, jlong, jboolean) {
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray result =
      string_class == nullptr ? nullptr : env->NewObjectArray(0, string_class, nullptr);
  env->DeleteLocalRef(string_class);
  return result;
}

#if !defined(DARWIN_ART_REAL_GRAPHICS)
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

jboolean RenderNodeHasIdentityMatrix(jlong) {
  // The software bridge has no transform mutators yet, so every node remains
  // in its default identity state.
  return JNI_TRUE;
}
#endif

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

}  // namespace

namespace darwin_art {

bool RegisterFrameworkSupportNatives(JNIEnv* env) {
  JNINativeMethod native_allocation_methods[] = {
      {const_cast<char*>("applyFreeFunction"), const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&NativeAllocationRegistryApplyFreeFunction)},
  };
  return Register(env, "libcore/util/NativeAllocationRegistry",
                  native_allocation_methods,
                  static_cast<jint>(std::size(native_allocation_methods)));
}

bool RegisterFrameworkNatives(JNIEnv* env) {
  using namespace framework_system;
  JNINativeMethod message_queue_methods[] = {
      {const_cast<char*>("nativeInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&message_queue_native_init)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_destroy)},
      {const_cast<char*>("nativePollOnce"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&message_queue_native_poll_once)},
      {const_cast<char*>("nativeWake"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_wake)},
      {const_cast<char*>("nativeIsPolling"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&message_queue_native_is_polling)},
      {const_cast<char*>("nativeSetFileDescriptorEvents"),
       const_cast<char*>("(JII)V"),
       reinterpret_cast<void*>(&message_queue_native_set_file_descriptor_events)},
  };
  if (!Register(env, "android/os/MessageQueue", message_queue_methods,
                static_cast<jint>(std::size(message_queue_methods)))) {
    return false;
  }

  if (!RegisterFrameworkAnimationNatives(env)) {
    return false;
  }

  JNINativeMethod event_log_methods[] = {
      {const_cast<char*>("writeEvent"),
       const_cast<char*>("(I[Ljava/lang/Object;)I"),
       reinterpret_cast<void*>(&event_log_write_event)},
  };
  if (!Register(env, "android/util/EventLog", event_log_methods,
                static_cast<jint>(std::size(event_log_methods)))) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod log_methods[] = {
      {const_cast<char*>("isLoggable"),
       const_cast<char*>("(Ljava/lang/String;I)Z"),
       reinterpret_cast<void*>(&log_is_loggable)},
      {const_cast<char*>("println_native"),
       const_cast<char*>(
           "(IILjava/lang/String;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&log_println)},
  };
  if (!Register(env, "android/util/Log", log_methods,
                static_cast<jint>(std::size(log_methods)))) {
    return false;
  }
#endif

  JNINativeMethod trace_methods[] = {
      {const_cast<char*>("nativeIsTagEnabled"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&trace_is_tag_enabled)},
  };
  if (!Register(env, "android/os/Trace", trace_methods,
                static_cast<jint>(std::size(trace_methods)))) {
    return false;
  }

  JNINativeMethod system_clock_methods[] = {
      {const_cast<char*>("currentThreadTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_current_thread_time_millis)},
      {const_cast<char*>("elapsedRealtime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime)},
      {const_cast<char*>("elapsedRealtimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime_nanos)},
      {const_cast<char*>("uptimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_millis)},
      {const_cast<char*>("uptimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_nanos)},
  };
  if (!Register(env, "android/os/SystemClock", system_clock_methods,
                static_cast<jint>(std::size(system_clock_methods)))) {
    return false;
  }

  if (!RegisterFrameworkBinderNatives(env)) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod render_node_methods[] = {
      {const_cast<char*>("nCreate"),
       const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&RenderNodeCreate)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&RenderNodeGetNativeFinalizer)},
      {const_cast<char*>("nSetLeftTopRightBottom"),
       const_cast<char*>("(JIIII)Z"),
       reinterpret_cast<void*>(&RenderNodeSetLeftTopRightBottom)},
      {const_cast<char*>("nHasIdentityMatrix"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&RenderNodeHasIdentityMatrix)},
  };
  if (!Register(env, "android/graphics/RenderNode", render_node_methods,
                static_cast<jint>(std::size(render_node_methods)))) {
    return false;
  }

  JNINativeMethod paint_methods[] = {
      {const_cast<char*>("nInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintInit)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintGetNativeFinalizer)},
      {const_cast<char*>("nSetFlags"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetFlags)},
      {const_cast<char*>("nSetElegantTextHeight"),
       const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetElegantTextHeight)},
      {const_cast<char*>("nSetTextLocales"),
       const_cast<char*>("(JLjava/lang/String;)I"),
       reinterpret_cast<void*>(&PaintSetTextLocales)},
      {const_cast<char*>("nSetColor"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetColor)},
  };
  if (!Register(env, "android/graphics/Paint", paint_methods,
                static_cast<jint>(std::size(paint_methods)))) {
    return false;
  }
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod asset_manager_methods[] = {
      {const_cast<char*>("nativeCreate"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&AssetManagerCreate)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&AssetManagerDestroy)},
      {const_cast<char*>("nativeGetThemeFreeFunction"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&AssetManagerGetThemeFreeFunction)},
      {const_cast<char*>("nativeThemeCreate"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&AssetManagerThemeCreate)},
      {const_cast<char*>("nativeThemeApplyStyle"),
       const_cast<char*>("(JJIZ)V"),
       reinterpret_cast<void*>(&AssetManagerThemeApplyStyle)},
      {const_cast<char*>("nativeThemeCopy"), const_cast<char*>("(JJJJ)V"),
       reinterpret_cast<void*>(&AssetManagerThemeCopy)},
      {const_cast<char*>("nativeThemeGetAttributeValue"),
       const_cast<char*>("(JJILandroid/util/TypedValue;Z)I"),
       reinterpret_cast<void*>(&AssetManagerThemeGetAttributeValue)},
      {const_cast<char*>("nativeThemeGetChangingConfigurations"),
       const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&AssetManagerThemeGetChangingConfigurations)},
      {const_cast<char*>("nativeThemeRebase"),
       const_cast<char*>("(JJ[I[ZI)V"),
       reinterpret_cast<void*>(&AssetManagerThemeRebase)},
      {const_cast<char*>("nativeApplyStyle"),
       const_cast<char*>("(JJIIJ[IJJ)V"),
       reinterpret_cast<void*>(&AssetManagerApplyStyle)},
      {const_cast<char*>("nativeApplyStyleWithArray"),
       const_cast<char*>("(JJIIJ[I[I[I)V"),
       reinterpret_cast<void*>(&AssetManagerApplyStyleWithArray)},
      {const_cast<char*>("nativeResolveAttrs"),
       const_cast<char*>("(JJII[I[I[I[I)Z"),
       reinterpret_cast<void*>(&AssetManagerResolveAttrs)},
      {const_cast<char*>("nativeRetrieveAttributes"),
       const_cast<char*>("(JJ[I[I[I)Z"),
       reinterpret_cast<void*>(&AssetManagerRetrieveAttributes)},
      {const_cast<char*>("nativeSetApkAssets"),
       const_cast<char*>("(J[Landroid/content/res/ApkAssets;ZZ)V"),
       reinterpret_cast<void*>(&AssetManagerSetApkAssets)},
      {const_cast<char*>("nativeSetConfiguration"),
       const_cast<char*>(
           "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIZ)V"),
       reinterpret_cast<void*>(&AssetManagerSetConfiguration)},
      {const_cast<char*>("nativeGetLocales"),
       const_cast<char*>("(JZ)[Ljava/lang/String;"),
       reinterpret_cast<void*>(&AssetManagerGetLocales)},
  };
  if (!Register(env, "android/content/res/AssetManager",
                asset_manager_methods,
                static_cast<jint>(std::size(asset_manager_methods)))) {
    return false;
  }
#endif

  if (!RegisterFrameworkSystemPropertyNatives(env)) {
    return false;
  }

  return true;
}

}  // namespace darwin_art
