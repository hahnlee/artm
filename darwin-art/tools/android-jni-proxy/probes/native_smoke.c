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
typedef uint8_t (*ExceptionCheckFunction)(void*);

extern void darwin_art_jni_fixture_reset(void);
extern void* darwin_art_jni_fixture_vm(void);
extern int32_t darwin_art_jni_fixture_passed(void);

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
  darwin_art_jni_fixture_reset();
  void* vm = darwin_art_jni_fixture_vm();
  CHECK(vm != NULL);
  const RawSlot* invoke = *(const RawSlot* const*)vm;
  CHECK(invoke[3] == NULL);
  GetEnvFunction get_env = (GetEnvFunction)invoke[DARWIN_ART_JNI_INVOKE_SLOT_GetEnv];
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
  RegisterNativesFunction register_natives =
      (RegisterNativesFunction)native[DARWIN_ART_JNI_SLOT_RegisterNatives];
  ExceptionCheckFunction exception_check =
      (ExceptionCheckFunction)native[DARWIN_ART_JNI_SLOT_ExceptionCheck];

  CHECK(get_version(env) == DARWIN_ART_JNI_VERSION_1_6);
  CHECK(exception_check(env) == 0);
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
  CHECK(darwin_art_jni_fixture_passed() == 1);
  puts("jni proxy native smoke: PASS");
  return 0;
}
