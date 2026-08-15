#include <jni.h>
#include <stdint.h>

static jlong NativeAdd(JNIEnv* env, jclass fixture_class, jint left,
                       jlong middle, jint right) {
  (void)env;
  (void)fixture_class;
  return (jlong)left + middle + (jlong)right;
}

static uint64_t Mix(uint64_t digest, uint64_t value) {
  return (digest ^ value) * UINT64_C(1099511628211);
}

static jlong NativeSpill(JNIEnv* env, jclass fixture_class, jboolean z, jbyte b,
                         jchar c, jshort s, jint i, jlong j, jobject reference,
                         jfloat f0, jdouble d0, jfloat f1, jdouble d1, jfloat f2,
                         jdouble d2, jfloat f3, jdouble d3, jfloat f4,
                         jfloat f5, jdouble d4) {
  (void)env;
  (void)fixture_class;
  union {
    jfloat value;
    uint32_t bits;
  } floats[] = {{f0}, {f1}, {f2}, {f3}, {f4}, {f5}};
  union {
    jdouble value;
    uint64_t bits;
  } doubles[] = {{d0}, {d1}, {d2}, {d3}, {d4}};

  uint64_t digest = UINT64_C(1469598103934665603);
  digest = Mix(digest, z);
  digest = Mix(digest, (uint8_t)b);
  digest = Mix(digest, c);
  digest = Mix(digest, (uint16_t)s);
  digest = Mix(digest, (uint32_t)i);
  digest = Mix(digest, (uint64_t)j);
  digest = Mix(digest, reference != NULL);
  for (unsigned index = 0; index < 5; ++index) {
    digest = Mix(digest, floats[index].bits);
    digest = Mix(digest, doubles[index].bits);
  }
  digest = Mix(digest, floats[5].bits);
  return (jlong)digest;
}

static jint NativeUsesEnv(JNIEnv* env, jclass fixture_class) {
  (void)fixture_class;
  if ((*env)->GetVersion(env) != JNI_VERSION_1_6) {
    return -1;
  }
  // This is deliberately invoked after JNI_OnLoad returns. The proxy backend
  // must obtain the current invocation thread's ART JNIEnv, not retain the
  // synchronous load thread's JNIEnv.
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  return string_class == NULL ? -2 : 42;
}

static jint NativeNarrowStack(JNIEnv* env, jclass fixture_class, jint a0,
                              jint a1, jint a2, jint a3, jint a4, jint a5,
                              jboolean z, jbyte b, jchar c, jshort s, jint i,
                              jlong j, jobject reference) {
  (void)env;
  (void)fixture_class;
  return a0 == 10 && a1 == 11 && a2 == 12 && a3 == 13 && a4 == 14 &&
                 a5 == 15 && z == JNI_TRUE && b == (jbyte)0x81 &&
                 c == (jchar)0xabcd && s == (jshort)0x8765 &&
                 i == (jint)0x45678923 &&
                 j == (jlong)INT64_C(0x2233445566778899) && reference != NULL
             ? 42
             : -1;
}

static jobject NativeEcho(JNIEnv* env, jclass fixture_class, jobject value) {
  (void)env;
  (void)fixture_class;
  return value;
}

static jfloat NativeFloat(JNIEnv* env, jclass fixture_class, jfloat value) {
  (void)env;
  (void)fixture_class;
  return value + 0.5f;
}

static jdouble NativeDouble(JNIEnv* env, jclass fixture_class, jdouble value) {
  (void)env;
  (void)fixture_class;
  return value + 0.25;
}

static void NativeVoid(JNIEnv* env, jclass fixture_class) {
  (void)env;
  (void)fixture_class;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
    return JNI_ERR;
  }

  jclass fixture_class =
      (*env)->FindClass(env, "darwin/art/nativefixture/NativeFixture");
  if (fixture_class == NULL) {
    return JNI_ERR;
  }

  const JNINativeMethod methods[] = {
      {"nativeAdd", "(IJI)J", (void*)&NativeAdd},
      {"nativeSpill",
       "(ZBCSIJLjava/lang/Object;FDFDFDFDFFD)J",
       (void*)&NativeSpill},
      {"nativeUsesEnv", "()I", (void*)&NativeUsesEnv},
      {"nativeNarrowStack", "(IIIIIIZBCSIJLjava/lang/Object;)I",
       (void*)&NativeNarrowStack},
      {"nativeEcho", "(Ljava/lang/Object;)Ljava/lang/Object;",
       (void*)&NativeEcho},
      {"nativeFloat", "(F)F", (void*)&NativeFloat},
      {"nativeDouble", "(D)D", (void*)&NativeDouble},
      {"nativeVoid", "()V", (void*)&NativeVoid},
  };
  if ((*env)->RegisterNatives(env, fixture_class, methods, 8) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
}
