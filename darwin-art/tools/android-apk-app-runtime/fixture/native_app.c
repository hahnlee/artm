#include <jni.h>

JNIEXPORT jint JNICALL
Java_dev_darwinart_simple_MainActivity_nativeAnswer(JNIEnv* env, jclass clazz) {
  (void)env;
  (void)clazz;
  return 42;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
  return JNI_VERSION_1_6;
}
