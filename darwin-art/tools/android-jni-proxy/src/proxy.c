#include "darwin_art_jni_proxy.h"
#include "jni_slots.h"

#include <stdint.h>
#include <string.h>

typedef void (*RawJniSlot)(void);

typedef struct ProxyEnvHandle {
  const RawJniSlot* functions;
  struct DarwinArtJniProxy* owner;
} ProxyEnvHandle;

typedef struct ProxyVmHandle {
  const RawJniSlot* functions;
  struct DarwinArtJniProxy* owner;
} ProxyVmHandle;

typedef struct HostEnvHandle {
  const RawJniSlot* functions;
} HostEnvHandle;

struct DarwinArtJniProxy {
  uint64_t magic;
  DarwinArtJniBackend backend;
  ProxyEnvHandle env;
  ProxyVmHandle vm;
  uint8_t exception_pending;
};

static const uint64_t kProxyMagic = UINT64_C(0x4a4e4950524f5859);

_Static_assert(sizeof(void*) == 8,
               "Android 16 arm64 JNI requires 64-bit pointers");
_Static_assert(sizeof(RawJniSlot) == 8,
               "Android 16 arm64 JNI requires 64-bit function pointers");
_Static_assert(sizeof(int32_t) == 4, "JNI jint is 32-bit");
_Static_assert(sizeof(int8_t) == 1, "JNI jbyte is 8-bit");
_Static_assert(sizeof(DarwinArtJniNativeMethod) == 24,
               "JNINativeMethod must remain three pointers");
_Static_assert(_Alignof(DarwinArtJniNativeMethod) == 8,
               "JNINativeMethod arm64 alignment drift");
_Static_assert(sizeof(struct DarwinArtJniProxy) <=
                   DARWIN_ART_JNI_PROXY_STORAGE_SIZE,
               "public proxy storage is too small");
_Static_assert(_Alignof(struct DarwinArtJniProxy) <=
                   DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT,
               "public proxy storage alignment is too small");

static struct DarwinArtJniProxy* EnvOwner(void* raw_env) {
  ProxyEnvHandle* env = (ProxyEnvHandle*)raw_env;
  if (env == NULL || env->owner == NULL || env->owner->magic != kProxyMagic)
    return NULL;
  return env->owner;
}

static struct DarwinArtJniProxy* VmOwner(void* raw_vm) {
  ProxyVmHandle* vm = (ProxyVmHandle*)raw_vm;
  if (vm == NULL || vm->owner == NULL || vm->owner->magic != kProxyMagic)
    return NULL;
  return vm->owner;
}

static void* HostEnv(struct DarwinArtJniProxy* proxy) {
  if (proxy == NULL || proxy->backend.current_env == NULL) return NULL;
  return proxy->backend.current_env(proxy->backend.context);
}

static RawJniSlot HostSlot(void* raw_host_env, uint32_t slot) {
  HostEnvHandle* env = (HostEnvHandle*)raw_host_env;
  if (env == NULL || env->functions == NULL ||
      slot >= DARWIN_ART_JNI_NATIVE_SLOT_COUNT)
    return NULL;
  return env->functions[slot];
}

static int32_t ProxyGetVersion(void* raw_env) {
  return EnvOwner(raw_env) == NULL ? 0 : DARWIN_ART_JNI_VERSION_1_6;
}

static void* ProxyFindClass(void* raw_env, const char* name) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  if (proxy == NULL || name == NULL) return NULL;
  return proxy->backend.find_class(proxy->backend.context, name);
}

static int32_t ProxyThrowNew(void* raw_env, void* clazz, const char* message) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  if (proxy == NULL || clazz == NULL || message == NULL)
    return DARWIN_ART_JNI_ERR;
  const int32_t result =
      proxy->backend.throw_new(proxy->backend.context, clazz, message);
  if (result == DARWIN_ART_JNI_OK) proxy->exception_pending = 1;
  return result;
}

static void* ProxyExceptionOccurred(void* raw_env) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_ExceptionOccurred);
  if (raw == NULL) return NULL;
  return ((void* (*)(void*))raw)(host_env);
}

static void ProxyExceptionClear(void* raw_env) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  if (proxy == NULL) return;
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_ExceptionClear);
  if (raw != NULL) ((void (*)(void*))raw)(host_env);
  proxy->exception_pending = 0;
}

static int32_t ProxyRegisterNatives(void* raw_env, void* clazz,
                                    const DarwinArtJniNativeMethod* methods,
                                    int32_t count) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  if (proxy == NULL || clazz == NULL || methods == NULL || count < 0)
    return DARWIN_ART_JNI_ERR;
  return proxy->backend.register_natives(proxy->backend.context, clazz, methods,
                                         count);
}

static void* ProxyNewGlobalRef(void* raw_env, void* reference) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_NewGlobalRef);
  if (raw == NULL) return NULL;
  return ((void* (*)(void*, void*))raw)(host_env, reference);
}

static void ProxyDeleteGlobalRef(void* raw_env, void* reference) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_DeleteGlobalRef);
  if (raw == NULL) return;
  ((void (*)(void*, void*))raw)(host_env, reference);
}

static void ProxyDeleteLocalRef(void* raw_env, void* reference) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_DeleteLocalRef);
  if (raw == NULL) return;
  ((void (*)(void*, void*))raw)(host_env, reference);
}

static void* ProxyGetDirectBufferAddress(void* raw_env, void* buffer) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetDirectBufferAddress);
  if (raw == NULL) return NULL;
  return ((void* (*)(void*, void*))raw)(host_env, buffer);
}

static int64_t ProxyGetDirectBufferCapacity(void* raw_env, void* buffer) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetDirectBufferCapacity);
  if (raw == NULL) return -1;
  return ((int64_t (*)(void*, void*))raw)(host_env, buffer);
}

static void* ProxyNewLocalRef(void* raw_env, void* reference) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_NewLocalRef);
  if (raw == NULL) return NULL;
  return ((void* (*)(void*, void*))raw)(host_env, reference);
}

static void* ProxyNewStringUTF(void* raw_env, const char* bytes) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_NewStringUTF);
  if (raw == NULL || bytes == NULL) return NULL;
  return ((void* (*)(void*, const char*))raw)(host_env, bytes);
}

static int32_t ProxyGetStringUTFLength(void* raw_env, void* string) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetStringUTFLength);
  if (raw == NULL || string == NULL) return 0;
  return ((int32_t (*)(void*, void*))raw)(host_env, string);
}

static const char* ProxyGetStringUTFChars(void* raw_env, void* string,
                                          uint8_t* is_copy) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetStringUTFChars);
  if (raw == NULL || string == NULL) return NULL;
  return ((const char* (*)(void*, void*, uint8_t*))raw)(host_env, string,
                                                        is_copy);
}

static void ProxyReleaseStringUTFChars(void* raw_env, void* string,
                                       const char* bytes) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw =
      HostSlot(host_env, DARWIN_ART_JNI_SLOT_ReleaseStringUTFChars);
  if (raw == NULL || string == NULL || bytes == NULL) return;
  ((void (*)(void*, void*, const char*))raw)(host_env, string, bytes);
}

static int32_t ProxyGetArrayLength(void* raw_env, void* array) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetArrayLength);
  if (raw == NULL) return 0;
  return ((int32_t (*)(void*, void*))raw)(host_env, array);
}

static void* ProxyNewByteArray(void* raw_env, int32_t length) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_NewByteArray);
  if (raw == NULL) return NULL;
  return ((void* (*)(void*, int32_t))raw)(host_env, length);
}

static void ProxyGetByteArrayRegion(void* raw_env, void* array, int32_t start,
                                    int32_t length, int8_t* bytes) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_GetByteArrayRegion);
  if (raw == NULL || (length > 0 && bytes == NULL)) return;
  ((void (*)(void*, void*, int32_t, int32_t, int8_t*))raw)(
      host_env, array, start, length, bytes);
}

static void ProxySetByteArrayRegion(void* raw_env, void* array, int32_t start,
                                    int32_t length, const int8_t* bytes) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_SetByteArrayRegion);
  if (raw == NULL || (length > 0 && bytes == NULL)) return;
  ((void (*)(void*, void*, int32_t, int32_t, const int8_t*))raw)(
      host_env, array, start, length, bytes);
}

static uint8_t ProxyExceptionCheck(void* raw_env) {
  struct DarwinArtJniProxy* proxy = EnvOwner(raw_env);
  if (proxy == NULL) return 1;
  void* host_env = HostEnv(proxy);
  RawJniSlot raw = HostSlot(host_env, DARWIN_ART_JNI_SLOT_ExceptionCheck);
  if (raw != NULL) return ((uint8_t (*)(void*))raw)(host_env);
  return proxy->exception_pending;
}

static int32_t ProxyGetEnv(void* raw_vm, void** output, int32_t version) {
  struct DarwinArtJniProxy* proxy = VmOwner(raw_vm);
  if (output == NULL) return DARWIN_ART_JNI_ERR;
  *output = NULL;
  if (proxy == NULL) return DARWIN_ART_JNI_ERR;
  if (version != DARWIN_ART_JNI_VERSION_1_6) return DARWIN_ART_JNI_EVERSION;
  *output = &proxy->env;
  return DARWIN_ART_JNI_OK;
}

static const RawJniSlot kNativeTable[DARWIN_ART_JNI_NATIVE_SLOT_COUNT] = {
    [DARWIN_ART_JNI_SLOT_GetVersion] = (RawJniSlot)ProxyGetVersion,
    [DARWIN_ART_JNI_SLOT_FindClass] = (RawJniSlot)ProxyFindClass,
    [DARWIN_ART_JNI_SLOT_ThrowNew] = (RawJniSlot)ProxyThrowNew,
    [DARWIN_ART_JNI_SLOT_ExceptionOccurred] =
        (RawJniSlot)ProxyExceptionOccurred,
    [DARWIN_ART_JNI_SLOT_ExceptionClear] = (RawJniSlot)ProxyExceptionClear,
    [DARWIN_ART_JNI_SLOT_NewGlobalRef] = (RawJniSlot)ProxyNewGlobalRef,
    [DARWIN_ART_JNI_SLOT_DeleteGlobalRef] = (RawJniSlot)ProxyDeleteGlobalRef,
    [DARWIN_ART_JNI_SLOT_DeleteLocalRef] = (RawJniSlot)ProxyDeleteLocalRef,
    [DARWIN_ART_JNI_SLOT_GetDirectBufferAddress] =
        (RawJniSlot)ProxyGetDirectBufferAddress,
    [DARWIN_ART_JNI_SLOT_GetDirectBufferCapacity] =
        (RawJniSlot)ProxyGetDirectBufferCapacity,
    [DARWIN_ART_JNI_SLOT_NewLocalRef] = (RawJniSlot)ProxyNewLocalRef,
    [DARWIN_ART_JNI_SLOT_NewStringUTF] = (RawJniSlot)ProxyNewStringUTF,
    [DARWIN_ART_JNI_SLOT_GetStringUTFLength] =
        (RawJniSlot)ProxyGetStringUTFLength,
    [DARWIN_ART_JNI_SLOT_GetStringUTFChars] =
        (RawJniSlot)ProxyGetStringUTFChars,
    [DARWIN_ART_JNI_SLOT_ReleaseStringUTFChars] =
        (RawJniSlot)ProxyReleaseStringUTFChars,
    [DARWIN_ART_JNI_SLOT_GetArrayLength] = (RawJniSlot)ProxyGetArrayLength,
    [DARWIN_ART_JNI_SLOT_NewByteArray] = (RawJniSlot)ProxyNewByteArray,
    [DARWIN_ART_JNI_SLOT_GetByteArrayRegion] =
        (RawJniSlot)ProxyGetByteArrayRegion,
    [DARWIN_ART_JNI_SLOT_SetByteArrayRegion] =
        (RawJniSlot)ProxySetByteArrayRegion,
    [DARWIN_ART_JNI_SLOT_RegisterNatives] = (RawJniSlot)ProxyRegisterNatives,
    [DARWIN_ART_JNI_SLOT_ExceptionCheck] = (RawJniSlot)ProxyExceptionCheck,
};

static const RawJniSlot kInvokeTable[DARWIN_ART_JNI_INVOKE_SLOT_COUNT] = {
    [DARWIN_ART_JNI_INVOKE_SLOT_GetEnv] = (RawJniSlot)ProxyGetEnv,
};

DarwinArtJniProxy*
darwin_art_jni_proxy_init(void* storage, size_t storage_size,
                          const DarwinArtJniBackend* backend) {
  if (storage == NULL || storage_size < sizeof(struct DarwinArtJniProxy) ||
      (uintptr_t)storage % _Alignof(struct DarwinArtJniProxy) != 0 ||
      backend == NULL || backend->find_class == NULL ||
      backend->register_natives == NULL || backend->throw_new == NULL)
    return NULL;
  struct DarwinArtJniProxy* proxy = (struct DarwinArtJniProxy*)storage;
  memset(proxy, 0, sizeof(*proxy));
  proxy->backend = *backend;
  proxy->env.functions = kNativeTable;
  proxy->env.owner = proxy;
  proxy->vm.functions = kInvokeTable;
  proxy->vm.owner = proxy;
  proxy->magic = kProxyMagic;
  return proxy;
}

void* darwin_art_jni_proxy_java_vm(DarwinArtJniProxy* proxy) {
  if (proxy == NULL || proxy->magic != kProxyMagic) return NULL;
  return &proxy->vm;
}
