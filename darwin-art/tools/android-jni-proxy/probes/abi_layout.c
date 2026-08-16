#include <jni.h>

#include <stddef.h>

_Static_assert(sizeof(void*) == 8, "arm64 pointer width drift");
_Static_assert(sizeof(jint) == 4, "jint width drift");
_Static_assert(sizeof(jboolean) == 1, "jboolean width drift");
_Static_assert(sizeof(JNINativeMethod) == 24, "JNINativeMethod layout drift");
_Static_assert(offsetof(JNINativeMethod, name) == 0,
               "method name offset drift");
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
_Static_assert(offsetof(struct JNINativeInterface, ExceptionOccurred) == 15 * 8,
               "ExceptionOccurred slot drift");
_Static_assert(offsetof(struct JNINativeInterface, ExceptionClear) == 17 * 8,
               "ExceptionClear slot drift");
_Static_assert(offsetof(struct JNINativeInterface, NewGlobalRef) == 21 * 8,
               "NewGlobalRef slot drift");
_Static_assert(offsetof(struct JNINativeInterface, DeleteGlobalRef) == 22 * 8,
               "DeleteGlobalRef slot drift");
_Static_assert(offsetof(struct JNINativeInterface, DeleteLocalRef) == 23 * 8,
               "DeleteLocalRef slot drift");
_Static_assert(offsetof(struct JNINativeInterface, NewLocalRef) == 25 * 8,
               "NewLocalRef slot drift");
_Static_assert(offsetof(struct JNINativeInterface, NewStringUTF) == 167 * 8,
               "NewStringUTF slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetStringUTFLength) ==
                   168 * 8,
               "GetStringUTFLength slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetStringUTFChars) ==
                   169 * 8,
               "GetStringUTFChars slot drift");
_Static_assert(offsetof(struct JNINativeInterface, ReleaseStringUTFChars) ==
                   170 * 8,
               "ReleaseStringUTFChars slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetArrayLength) == 171 * 8,
               "GetArrayLength slot drift");
_Static_assert(offsetof(struct JNINativeInterface, NewByteArray) == 176 * 8,
               "NewByteArray slot drift");
_Static_assert(offsetof(struct JNINativeInterface, GetByteArrayRegion) ==
                   200 * 8,
               "GetByteArrayRegion slot drift");
_Static_assert(offsetof(struct JNINativeInterface, SetByteArrayRegion) ==
                   208 * 8,
               "SetByteArrayRegion slot drift");
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

_Static_assert(_Generic(((struct JNINativeInterface*)0)->ExceptionOccurred,
                   jthrowable (*)(JNIEnv*): 1,
                   default: 0),
               "ExceptionOccurred prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->ExceptionClear,
                   void (*)(JNIEnv*): 1,
                   default: 0),
               "ExceptionClear prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->NewGlobalRef,
                   jobject (*)(JNIEnv*, jobject): 1,
                   default: 0),
               "NewGlobalRef prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->DeleteGlobalRef,
                   void (*)(JNIEnv*, jobject): 1,
                   default: 0),
               "DeleteGlobalRef prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->NewLocalRef,
                   jobject (*)(JNIEnv*, jobject): 1,
                   default: 0),
               "NewLocalRef prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->GetArrayLength,
                   jsize (*)(JNIEnv*, jarray): 1,
                   default: 0),
               "GetArrayLength prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->NewByteArray,
                   jbyteArray (*)(JNIEnv*, jsize): 1,
                   default: 0),
               "NewByteArray prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->GetByteArrayRegion,
                   void (*)(JNIEnv*, jbyteArray, jsize, jsize, jbyte*): 1,
                   default: 0),
               "GetByteArrayRegion prototype drift");
_Static_assert(_Generic(((struct JNINativeInterface*)0)->SetByteArrayRegion,
                   void (*)(JNIEnv*, jbyteArray, jsize, jsize, const jbyte*): 1,
                   default: 0),
               "SetByteArrayRegion prototype drift");

typedef jint (*JniOnLoadSignature)(JavaVM*, void*);
extern jint JNI_OnLoad(JavaVM*, void*);
_Static_assert(_Generic(&JNI_OnLoad, JniOnLoadSignature: 1, default: 0),
               "JNI_OnLoad signature drift");
