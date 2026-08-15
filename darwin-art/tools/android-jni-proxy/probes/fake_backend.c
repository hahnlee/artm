#include "darwin_art_jni_proxy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct FakeState {
  int find_bridge;
  int find_exception;
  int registered;
  int thrown;
} FakeState;

static FakeState kState;
static int kBridgeClass;
static int kExceptionClass;
_Alignas(DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT)
static unsigned char kProxyStorage[DARWIN_ART_JNI_PROXY_STORAGE_SIZE];
static DarwinArtJniProxy* kProxy;

static void* FakeFindClass(void* context, const char* name) {
  FakeState* state = (FakeState*)context;
  if (strcmp(name, "fixture/Bridge") == 0) {
    ++state->find_bridge;
    return &kBridgeClass;
  }
  if (strcmp(name, "java/lang/RuntimeException") == 0) {
    ++state->find_exception;
    return &kExceptionClass;
  }
  return NULL;
}

static int32_t FakeRegisterNatives(void* context, void* clazz,
                                   const DarwinArtJniNativeMethod* methods,
                                   int32_t count) {
  FakeState* state = (FakeState*)context;
  if (clazz != &kBridgeClass || count != 1 || methods == NULL ||
      strcmp(methods[0].name, "nativePing") != 0 ||
      strcmp(methods[0].signature, "()I") != 0 || methods[0].function == NULL)
    return DARWIN_ART_JNI_ERR;
  ++state->registered;
  return DARWIN_ART_JNI_OK;
}

static int32_t FakeThrowNew(void* context, void* clazz, const char* message) {
  FakeState* state = (FakeState*)context;
  if (clazz != &kExceptionClass || strcmp(message, "proxy-fixture") != 0)
    return DARWIN_ART_JNI_ERR;
  ++state->thrown;
  return DARWIN_ART_JNI_OK;
}

void darwin_art_jni_fixture_reset(void) {
  memset(&kState, 0, sizeof(kState));
  const DarwinArtJniBackend backend = {
      .context = &kState,
      .find_class = FakeFindClass,
      .register_natives = FakeRegisterNatives,
      .throw_new = FakeThrowNew,
  };
  kProxy = darwin_art_jni_proxy_init(kProxyStorage, sizeof(kProxyStorage), &backend);
}

void* darwin_art_jni_fixture_vm(void) {
  return darwin_art_jni_proxy_java_vm(kProxy);
}

int32_t darwin_art_jni_fixture_passed(void) {
  return kProxy != NULL && kState.find_bridge == 1 && kState.find_exception == 1 &&
         kState.registered == 1 && kState.thrown == 1;
}
