#include <jni.h>
#include <stdint.h>

extern int DarwinArtGenericChildValue(void);

static int g_generic_graph_value;

__attribute__((constructor)) static void GenericGraphInitialize(void) {
  g_generic_graph_value = DarwinArtGenericChildValue();
}

static jlong GenericNativeAdd(JNIEnv *env, jclass type, jint left, jlong middle,
                              jint right) {
  return env != 0 && type != 0 ? (jlong)left + middle + (jlong)right : -1;
}

__attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM *vm,
                                                       void *reserved) {
  JNIEnv *env = 0;
  if (vm == 0 || reserved != 0 || g_generic_graph_value != 20 ||
      (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || env == 0) {
    return JNI_ERR;
  }
  jclass fixture =
      (*env)->FindClass(env, "darwin/art/nativefixture/NativeFixture");
  JNINativeMethod method = {
      .name = "nativeAdd",
      .signature = "(IJI)J",
      .fnPtr = (void *)&GenericNativeAdd,
  };
  JNINativeMethod foreign = method;
  foreign.fnPtr = (void *)(uintptr_t)1;
  if (fixture == 0 ||
      (*env)->RegisterNatives(env, fixture, &foreign, 1) == JNI_OK ||
      (*env)->RegisterNatives(env, fixture, &method, 1) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}
