#include <jni.h>

#include <stddef.h>

_Static_assert(sizeof(void*) == 8, "arm64 pointer width drift");
_Static_assert(sizeof(jint) == 4, "jint width drift");
_Static_assert(sizeof(jboolean) == 1, "jboolean width drift");
_Static_assert(sizeof(JNINativeMethod) == 24, "JNINativeMethod layout drift");
_Static_assert(offsetof(JNINativeMethod, name) == 0, "method name offset drift");
_Static_assert(offsetof(JNINativeMethod, signature) == 8,
               "method signature offset drift");
_Static_assert(offsetof(JNINativeMethod, fnPtr) == 16,
               "method function offset drift");
_Static_assert(offsetof(struct JNINativeInterface, GetVersion) == 4 * 8,
               "GetVersion slot drift");
_Static_assert(offsetof(struct JNINativeInterface, FindClass) == 6 * 8,
               "FindClass slot drift");
_Static_assert(offsetof(struct JNINativeInterface, ThrowNew) == 14 * 8,
               "ThrowNew slot drift");
_Static_assert(offsetof(struct JNINativeInterface, DeleteLocalRef) == 23 * 8,
               "DeleteLocalRef slot drift");
_Static_assert(offsetof(struct JNINativeInterface, NewStringUTF) == 167 * 8,
               "NewStringUTF slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetStringUTFLength) == 168 * 8,
               "GetStringUTFLength slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetStringUTFChars) == 169 * 8,
               "GetStringUTFChars slot drift");
_Static_assert(offsetof(struct JNINativeInterface, ReleaseStringUTFChars) ==
                   170 * 8,
               "ReleaseStringUTFChars slot drift");
_Static_assert(offsetof(struct JNINativeInterface, RegisterNatives) == 215 * 8,
               "RegisterNatives slot drift");
_Static_assert(offsetof(struct JNINativeInterface, ExceptionCheck) == 228 * 8,
               "ExceptionCheck slot drift");
_Static_assert(sizeof(struct JNINativeInterface) == 233 * 8,
               "JNIEnv table size drift");
_Static_assert(offsetof(struct JNIInvokeInterface, GetEnv) == 6 * 8,
               "GetEnv slot drift");
_Static_assert(sizeof(struct JNIInvokeInterface) == 8 * 8,
               "JavaVM table size drift");

typedef jint (*JniOnLoadSignature)(JavaVM*, void*);
extern jint JNI_OnLoad(JavaVM*, void*);
_Static_assert(_Generic(&JNI_OnLoad, JniOnLoadSignature: 1, default: 0),
               "JNI_OnLoad signature drift");
