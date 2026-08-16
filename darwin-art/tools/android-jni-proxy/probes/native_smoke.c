#include "darwin_art_jni_proxy.h"
#include "jni_slots.h"

#include <stdint.h>
#include <stdio.h>

typedef void (*RawSlot)(void);
typedef int32_t (*GetEnvFunction)(void*, void**, int32_t);
typedef int32_t (*GetVersionFunction)(void*);
typedef void* (*FindClassFunction)(void*, const char*);
typedef int32_t (*RegisterNativesFunction)(void*, void*,
                                           const DarwinArtJniNativeMethod*,
                                           int32_t);
typedef int32_t (*ThrowNewFunction)(void*, void*, const char*);
typedef void* (*ExceptionOccurredFunction)(void*);
typedef void (*ExceptionClearFunction)(void*);
typedef uint8_t (*ExceptionCheckFunction)(void*);
typedef void* (*NewReferenceFunction)(void*, void*);
typedef void (*DeleteReferenceFunction)(void*, void*);
typedef void (*DeleteLocalRefFunction)(void*, void*);
typedef void* (*NewStringUtfFunction)(void*, const char*);
typedef int32_t (*GetStringUtfLengthFunction)(void*, void*);
typedef const char* (*GetStringUtfCharsFunction)(void*, void*, uint8_t*);
typedef void (*ReleaseStringUtfCharsFunction)(void*, void*, const char*);
typedef int32_t (*GetArrayLengthFunction)(void*, void*);
typedef void* (*NewByteArrayFunction)(void*, int32_t);
typedef void (*GetByteArrayRegionFunction)(void*, void*, int32_t, int32_t,
                                           int8_t*);
typedef void (*SetByteArrayRegionFunction)(void*, void*, int32_t, int32_t,
                                           const int8_t*);

extern void darwin_art_jni_fixture_reset(void);
extern void* darwin_art_jni_fixture_vm(void);
extern int32_t darwin_art_jni_fixture_passed(void);

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) return __LINE__;                                         \
  } while (0)

int main(void) {
  darwin_art_jni_fixture_reset();
  void* vm = darwin_art_jni_fixture_vm();
  CHECK(vm != NULL);
  const RawSlot* invoke = *(const RawSlot* const*)vm;
  CHECK(invoke[3] == NULL);
  GetEnvFunction get_env =
      (GetEnvFunction)invoke[DARWIN_ART_JNI_INVOKE_SLOT_GetEnv];
  void* env = (void*)(uintptr_t)1;
  CHECK(get_env(vm, &env, 0x00010008) == DARWIN_ART_JNI_EVERSION);
  CHECK(env == NULL);
  CHECK(get_env(vm, &env, DARWIN_ART_JNI_VERSION_1_6) == DARWIN_ART_JNI_OK);
  CHECK(env != NULL);

  const RawSlot* native = *(const RawSlot* const*)env;
  CHECK(native[5] == NULL);
  GetVersionFunction get_version =
      (GetVersionFunction)native[DARWIN_ART_JNI_SLOT_GetVersion];
  FindClassFunction find_class =
      (FindClassFunction)native[DARWIN_ART_JNI_SLOT_FindClass];
  ThrowNewFunction throw_new =
      (ThrowNewFunction)native[DARWIN_ART_JNI_SLOT_ThrowNew];
  ExceptionOccurredFunction exception_occurred =
      (ExceptionOccurredFunction)native[DARWIN_ART_JNI_SLOT_ExceptionOccurred];
  ExceptionClearFunction exception_clear =
      (ExceptionClearFunction)native[DARWIN_ART_JNI_SLOT_ExceptionClear];
  RegisterNativesFunction register_natives =
      (RegisterNativesFunction)native[DARWIN_ART_JNI_SLOT_RegisterNatives];
  ExceptionCheckFunction exception_check =
      (ExceptionCheckFunction)native[DARWIN_ART_JNI_SLOT_ExceptionCheck];
  DeleteLocalRefFunction delete_local_ref =
      (DeleteLocalRefFunction)native[DARWIN_ART_JNI_SLOT_DeleteLocalRef];
  NewReferenceFunction new_global_ref =
      (NewReferenceFunction)native[DARWIN_ART_JNI_SLOT_NewGlobalRef];
  DeleteReferenceFunction delete_global_ref =
      (DeleteReferenceFunction)native[DARWIN_ART_JNI_SLOT_DeleteGlobalRef];
  NewReferenceFunction new_local_ref =
      (NewReferenceFunction)native[DARWIN_ART_JNI_SLOT_NewLocalRef];
  NewStringUtfFunction new_string_utf =
      (NewStringUtfFunction)native[DARWIN_ART_JNI_SLOT_NewStringUTF];
  GetStringUtfLengthFunction get_string_utf_length =
      (GetStringUtfLengthFunction)
          native[DARWIN_ART_JNI_SLOT_GetStringUTFLength];
  GetStringUtfCharsFunction get_string_utf_chars =
      (GetStringUtfCharsFunction)native[DARWIN_ART_JNI_SLOT_GetStringUTFChars];
  ReleaseStringUtfCharsFunction release_string_utf_chars =
      (ReleaseStringUtfCharsFunction)
          native[DARWIN_ART_JNI_SLOT_ReleaseStringUTFChars];
  GetArrayLengthFunction get_array_length =
      (GetArrayLengthFunction)native[DARWIN_ART_JNI_SLOT_GetArrayLength];
  NewByteArrayFunction new_byte_array =
      (NewByteArrayFunction)native[DARWIN_ART_JNI_SLOT_NewByteArray];
  GetByteArrayRegionFunction get_byte_array_region =
      (GetByteArrayRegionFunction)
          native[DARWIN_ART_JNI_SLOT_GetByteArrayRegion];
  SetByteArrayRegionFunction set_byte_array_region =
      (SetByteArrayRegionFunction)
          native[DARWIN_ART_JNI_SLOT_SetByteArrayRegion];

  CHECK(get_version(env) == DARWIN_ART_JNI_VERSION_1_6);
  CHECK(exception_check(env) == 0);
  CHECK(new_string_utf(env, "unavailable") == NULL);
  CHECK(get_string_utf_length(env, (void*)(uintptr_t)1) == 0);
  CHECK(get_string_utf_chars(env, (void*)(uintptr_t)1, NULL) == NULL);
  release_string_utf_chars(env, (void*)(uintptr_t)1, "ignored");
  CHECK(new_global_ref(env, (void*)(uintptr_t)1) == NULL);
  delete_global_ref(env, (void*)(uintptr_t)1);
  CHECK(new_local_ref(env, (void*)(uintptr_t)1) == NULL);
  delete_local_ref(env, (void*)(uintptr_t)1);
  CHECK(new_byte_array(env, 4) == NULL);
  CHECK(get_array_length(env, (void*)(uintptr_t)1) == 0);
  int8_t region[2] = {1, 2};
  set_byte_array_region(env, (void*)(uintptr_t)1, 0, 2, region);
  get_byte_array_region(env, (void*)(uintptr_t)1, 0, 2, region);
  void* bridge = find_class(env, "fixture/Bridge");
  CHECK(bridge != NULL);
  const DarwinArtJniNativeMethod method = {
      .name = "nativePing",
      .signature = "()I",
      .function = (void*)(uintptr_t)1,
  };
  CHECK(register_natives(env, bridge, &method, 1) == DARWIN_ART_JNI_OK);
  void* exception = find_class(env, "java/lang/RuntimeException");
  CHECK(exception != NULL);
  CHECK(throw_new(env, exception, "proxy-fixture") == DARWIN_ART_JNI_OK);
  CHECK(exception_check(env) == 1);
  CHECK(exception_occurred(env) == NULL);
  exception_clear(env);
  CHECK(exception_check(env) == 0);
  CHECK(darwin_art_jni_fixture_passed() == 1);
  puts("jni proxy native smoke: PASS");
  return 0;
}
