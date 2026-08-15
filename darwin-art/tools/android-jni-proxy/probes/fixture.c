#include <jni.h>

extern JavaVM* darwin_art_jni_fixture_vm(void);

static jint NativePing(JNIEnv* env, jobject receiver) {
  (void)env;
  (void)receiver;
  return 42;
}

static JNINativeMethod kMethods[] = {
    {"nativePing", "()I", (void*)NativePing},
};

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  if ((*env)->GetVersion(env) != JNI_VERSION_1_6) return JNI_ERR;
  if ((*env)->ExceptionCheck(env)) return JNI_ERR;
  jclass bridge = (*env)->FindClass(env, "fixture/Bridge");
  if (bridge == NULL) return JNI_ERR;
  if ((*env)->RegisterNatives(env, bridge, kMethods, 1) != JNI_OK) return JNI_ERR;
  jclass exception = (*env)->FindClass(env, "java/lang/RuntimeException");
  if (exception == NULL) return JNI_ERR;
  if ((*env)->ThrowNew(env, exception, "proxy-fixture") != JNI_OK) return JNI_ERR;
  if (!(*env)->ExceptionCheck(env)) return JNI_ERR;
  return JNI_VERSION_1_6;
}

JNIEXPORT jint jni_proxy_fixture_run(void) {
  JavaVM* vm = darwin_art_jni_fixture_vm();
  return vm == NULL ? JNI_ERR : JNI_OnLoad(vm, NULL);
}
