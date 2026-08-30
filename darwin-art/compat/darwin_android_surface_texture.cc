#include "darwin_angle_egl.h"
#include "darwin_android_surface_texture.h"
#include "darwin_art_bionic_socket_broker.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/surface_texture.h>
#include <surfacetexture/surface_texture_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

namespace {
constexpr unsigned int kGlTextureExternalOes = 0x8D65;

struct QueuedBuffer {
  AHardwareBuffer* buffer = nullptr;
  int32_t slot = -1;
  int fence = -1;
  android_dataspace dataspace = HAL_DATASPACE_UNKNOWN;
  int64_t timestamp_ns = 0;
};

void ReleaseFence(int fence) {
  if (fence >= 0) (void)darwin_art_bionic_socket_broker_close(fence);
}

void ReleaseQueued(void* producer, QueuedBuffer* queued,
                   int release_fence = -1) {
  if (queued == nullptr || queued->buffer == nullptr) {
    ReleaseFence(release_fence);
    return;
  }
  darwin_art_android_ANativeWindow_release_consumer_slot(
      producer, queued->slot, release_fence);
  ReleaseFence(queued->fence);
  AHardwareBuffer_release(queued->buffer);
  *queued = QueuedBuffer{};
}
}  // namespace

struct ASurfaceTexture {
  std::atomic<uint32_t> references{1};
  std::mutex mutex;
  void* producer = nullptr;
  std::deque<QueuedBuffer> pending;
  QueuedBuffer current;
  unsigned int texture_target = kGlTextureExternalOes;
  uint32_t attached_texture = 0;
  bool consumer_owned = false;
  bool abandoned = false;
};

namespace {
void QueueBuffer(void* context, AHardwareBuffer* buffer, int32_t slot,
                 int fence, int32_t dataspace) {
  auto* texture = static_cast<ASurfaceTexture*>(context);
  if (texture == nullptr || buffer == nullptr) {
    ReleaseFence(fence);
    return;
  }
  AHardwareBuffer_acquire(buffer);
  std::lock_guard<std::mutex> lock(texture->mutex);
  if (texture->abandoned) {
    AHardwareBuffer_release(buffer);
    darwin_art_android_ANativeWindow_release_consumer_slot(
        texture->producer, slot, -1);
    ReleaseFence(fence);
    return;
  }
  texture->pending.push_back(QueuedBuffer{
      .buffer = buffer,
      .slot = slot,
      .fence = fence,
      .dataspace = static_cast<android_dataspace>(dataspace),
      .timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count(),
  });
}

void Identity(float* matrix) {
  if (matrix == nullptr) return;
  std::fill(matrix, matrix + 16, 0.0f);
  matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}
}  // namespace

extern "C" ASurfaceTexture* darwin_art_android_surface_texture_create(
    uint32_t width, uint32_t height, int32_t format, uint32_t texture) {
  auto* surface_texture = new (std::nothrow) ASurfaceTexture();
  if (surface_texture == nullptr) return nullptr;
  surface_texture->attached_texture = texture;
  surface_texture->producer = darwin_art_android_ANativeWindow_create(
      static_cast<int32_t>(width), static_cast<int32_t>(height), format);
  if (surface_texture->producer == nullptr) {
    delete surface_texture;
    return nullptr;
  }
  darwin_art_android_ANativeWindow_set_queue_callback(
      surface_texture->producer, &QueueBuffer, surface_texture);
  return surface_texture;
}

extern "C" void* darwin_art_android_surface_texture_producer(
    ASurfaceTexture* surface_texture) {
  return surface_texture == nullptr ? nullptr : surface_texture->producer;
}

extern "C" void darwin_art_android_surface_texture_set_default_size(
    ASurfaceTexture* surface_texture, uint32_t width, uint32_t height) {
  if (surface_texture == nullptr || width == 0 || height == 0) return;
  (void)darwin_art_android_ANativeWindow_setBuffersGeometry(
      surface_texture->producer, static_cast<int32_t>(width),
      static_cast<int32_t>(height), 0);
}

extern "C" void darwin_art_android_surface_texture_abandon(
    ASurfaceTexture* surface_texture) {
  if (surface_texture == nullptr) return;
  std::lock_guard<std::mutex> lock(surface_texture->mutex);
  surface_texture->abandoned = true;
}

extern "C" ASurfaceTexture* ASurfaceTexture_fromSurfaceTexture(
    JNIEnv* env, jobject object) {
  if (env == nullptr || object == nullptr) return nullptr;
  jclass clazz = env->GetObjectClass(object);
  jfieldID field = clazz == nullptr
                       ? nullptr
                       : env->GetFieldID(clazz, "mSurfaceTexture", "J");
  auto* texture = field == nullptr
                      ? nullptr
                      : reinterpret_cast<ASurfaceTexture*>(
                            static_cast<uintptr_t>(env->GetLongField(object,
                                                                     field)));
  env->DeleteLocalRef(clazz);
  if (texture != nullptr)
    texture->references.fetch_add(1, std::memory_order_relaxed);
  return texture;
}

extern "C" ANativeWindow* ASurfaceTexture_acquireANativeWindow(
    ASurfaceTexture* st) {
  if (st == nullptr) return nullptr;
  darwin_art_android_ANativeWindow_acquire(st->producer);
  return static_cast<ANativeWindow*>(st->producer);
}

extern "C" int ASurfaceTexture_attachToGLContext(ASurfaceTexture* st,
                                                   uint32_t texture) {
  if (st == nullptr) return -1;
  std::lock_guard<std::mutex> lock(st->mutex);
  st->attached_texture = texture;
  return 0;
}

extern "C" int ASurfaceTexture_detachFromGLContext(ASurfaceTexture* st) {
  if (st == nullptr) return -1;
  std::lock_guard<std::mutex> lock(st->mutex);
  st->attached_texture = 0;
  return 0;
}

extern "C" int ASurfaceTexture_updateTexImage(ASurfaceTexture* st) {
  return st == nullptr || st->abandoned ? -1 : 0;
}

extern "C" void ASurfaceTexture_getTransformMatrix(ASurfaceTexture*,
                                                      float matrix[16]) {
  Identity(matrix);
}

extern "C" int64_t ASurfaceTexture_getTimestamp(ASurfaceTexture* st) {
  if (st == nullptr) return 0;
  std::lock_guard<std::mutex> lock(st->mutex);
  if (!st->pending.empty()) return st->pending.back().timestamp_ns;
  return st->current.timestamp_ns;
}

namespace android {
ANativeWindow* ASurfaceTexture_routeAcquireANativeWindow(ASurfaceTexture* st) {
  return ASurfaceTexture_acquireANativeWindow(st);
}

int ASurfaceTexture_routeAttachToGLContext(ASurfaceTexture* st,
                                           uint32_t texture) {
  return ASurfaceTexture_attachToGLContext(st, texture);
}

int ASurfaceTexture_routeDetachFromGLContext(ASurfaceTexture* st) {
  return ASurfaceTexture_detachFromGLContext(st);
}

void ASurfaceTexture_routeRelease(ASurfaceTexture* st) {
  ASurfaceTexture_release(st);
}

int ASurfaceTexture_routeUpdateTexImage(ASurfaceTexture* st) {
  return ASurfaceTexture_updateTexImage(st);
}

void ASurfaceTexture_routeGetTransformMatrix(ASurfaceTexture* st,
                                             float matrix[16]) {
  ASurfaceTexture_getTransformMatrix(st, matrix);
}

int64_t ASurfaceTexture_routeGetTimestamp(ASurfaceTexture* st) {
  return ASurfaceTexture_getTimestamp(st);
}

ASurfaceTexture* ASurfaceTexture_routeFromSurfaceTexture(JNIEnv* env,
                                                         jobject object) {
  return ASurfaceTexture_fromSurfaceTexture(env, object);
}

unsigned int ASurfaceTexture_getCurrentTextureTarget(ASurfaceTexture* st) {
  return st == nullptr ? kGlTextureExternalOes : st->texture_target;
}

void ASurfaceTexture_takeConsumerOwnership(ASurfaceTexture* st) {
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  st->consumer_owned = true;
}

void ASurfaceTexture_releaseConsumerOwnership(ASurfaceTexture* st) {
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  st->consumer_owned = false;
  while (!st->pending.empty()) {
    QueuedBuffer queued = st->pending.front();
    st->pending.pop_front();
    ReleaseQueued(st->producer, &queued);
  }
  ReleaseQueued(st->producer, &st->current);
}

AHardwareBuffer* ASurfaceTexture_dequeueBuffer(
    ASurfaceTexture* st, int* outSlotid, android_dataspace* outDataspace,
    AHdrMetadataType* outHdrType, android_cta861_3_metadata* outCta861_3,
    android_smpte2086_metadata* outSmpte2086, float* outTransformMatrix,
    uint32_t* outTransform, bool* outNewContent,
    ASurfaceTexture_createReleaseFence createFence,
    ASurfaceTexture_fenceWait fenceWait, void* fenceHandle,
    ARect* currentCrop) {
  if (outNewContent != nullptr) *outNewContent = false;
  if (st == nullptr) return nullptr;

  std::lock_guard<std::mutex> lock(st->mutex);
  if (st->abandoned || !st->consumer_owned) return nullptr;

  while (st->pending.size() > 1) {
    QueuedBuffer stale = st->pending.front();
    st->pending.pop_front();
    ReleaseQueued(st->producer, &stale);
  }
  if (!st->pending.empty()) {
    int release_fence = -1;
    if (st->current.buffer != nullptr && createFence != nullptr) {
      EGLSyncKHR egl_fence = EGL_NO_SYNC_KHR;
      EGLDisplay display = EGL_NO_DISPLAY;
      (void)createFence(true, &egl_fence, &display, &release_fence,
                        fenceHandle);
    }
    ReleaseQueued(st->producer, &st->current, release_fence);
    st->current = st->pending.front();
    st->pending.pop_front();
    if (st->current.fence >= 0 && fenceWait != nullptr) {
      (void)fenceWait(st->current.fence, fenceHandle);
      ReleaseFence(st->current.fence);
      st->current.fence = -1;
    }
    if (outNewContent != nullptr) *outNewContent = true;
  }
  if (st->current.buffer == nullptr) return nullptr;

  AHardwareBuffer_Desc description{};
  AHardwareBuffer_describe(st->current.buffer, &description);
  if (outSlotid != nullptr) *outSlotid = st->current.slot;
  if (outDataspace != nullptr) *outDataspace = st->current.dataspace;
  if (outHdrType != nullptr) *outHdrType = static_cast<AHdrMetadataType>(0);
  if (outCta861_3 != nullptr) std::memset(outCta861_3, 0, sizeof(*outCta861_3));
  if (outSmpte2086 != nullptr)
    std::memset(outSmpte2086, 0, sizeof(*outSmpte2086));
  Identity(outTransformMatrix);
  if (outTransform != nullptr) *outTransform = 0;
  if (currentCrop != nullptr) {
    *currentCrop = ARect{0, 0, static_cast<int32_t>(description.width),
                         static_cast<int32_t>(description.height)};
  }
  AHardwareBuffer_acquire(st->current.buffer);
  return st->current.buffer;
}
}  // namespace android

extern "C" void ASurfaceTexture_release(ASurfaceTexture* st) {
  if (st == nullptr ||
      st->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  darwin_art_android_ANativeWindow_set_queue_callback(st->producer, nullptr,
                                                       nullptr);
  {
    std::lock_guard<std::mutex> lock(st->mutex);
    while (!st->pending.empty()) {
      QueuedBuffer queued = st->pending.front();
      st->pending.pop_front();
      ReleaseQueued(st->producer, &queued);
    }
    ReleaseQueued(st->producer, &st->current);
  }
  darwin_art_android_ANativeWindow_release(st->producer);
  delete st;
}

namespace {
struct SurfaceTextureFields {
  jfieldID texture = nullptr;
  jfieldID producer = nullptr;
};
SurfaceTextureFields g_surface_texture_fields;

ASurfaceTexture* JavaSurfaceTexture(JNIEnv* env, jobject object) {
  if (env == nullptr || object == nullptr ||
      g_surface_texture_fields.texture == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<ASurfaceTexture*>(static_cast<uintptr_t>(
      env->GetLongField(object, g_surface_texture_fields.texture)));
}

void SurfaceTextureNativeInit(JNIEnv* env, jobject object, jboolean detached,
                              jint texture_name, jboolean, jobject) {
  auto* texture = darwin_art_android_surface_texture_create(
      1, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      detached == JNI_TRUE ? 0u : static_cast<uint32_t>(texture_name));
  if (texture == nullptr) return;
  env->SetLongField(object, g_surface_texture_fields.texture,
                    reinterpret_cast<jlong>(texture));
  env->SetLongField(object, g_surface_texture_fields.producer,
                    reinterpret_cast<jlong>(texture->producer));
}

void SurfaceTextureNativeFinalize(JNIEnv* env, jobject object) {
  ASurfaceTexture* texture = JavaSurfaceTexture(env, object);
  env->SetLongField(object, g_surface_texture_fields.texture, 0);
  env->SetLongField(object, g_surface_texture_fields.producer, 0);
  ASurfaceTexture_release(texture);
}

void SurfaceTextureNativeSetDefaultBufferSize(JNIEnv* env, jobject object,
                                              jint width, jint height) {
  if (width > 0 && height > 0) {
    darwin_art_android_surface_texture_set_default_size(
        JavaSurfaceTexture(env, object), static_cast<uint32_t>(width),
        static_cast<uint32_t>(height));
  }
}

void SurfaceTextureNativeUpdateTexImage(JNIEnv* env, jobject object) {
  (void)ASurfaceTexture_updateTexImage(JavaSurfaceTexture(env, object));
}

void SurfaceTextureNativeReleaseTexImage(JNIEnv*, jobject) {}

jint SurfaceTextureNativeDetach(JNIEnv* env, jobject object) {
  return ASurfaceTexture_detachFromGLContext(JavaSurfaceTexture(env, object));
}

jint SurfaceTextureNativeAttach(JNIEnv* env, jobject object, jint texture) {
  return ASurfaceTexture_attachToGLContext(JavaSurfaceTexture(env, object),
                                           static_cast<uint32_t>(texture));
}

void SurfaceTextureNativeGetTransformMatrix(JNIEnv* env, jobject object,
                                            jfloatArray output) {
  if (output == nullptr || env->GetArrayLength(output) < 16) return;
  jfloat* matrix = env->GetFloatArrayElements(output, nullptr);
  if (matrix == nullptr) return;
  ASurfaceTexture_getTransformMatrix(JavaSurfaceTexture(env, object), matrix);
  env->ReleaseFloatArrayElements(output, matrix, 0);
}

jlong SurfaceTextureNativeGetTimestamp(JNIEnv* env, jobject object) {
  return ASurfaceTexture_getTimestamp(JavaSurfaceTexture(env, object));
}

jint SurfaceTextureNativeGetDataSpace(JNIEnv* env, jobject object) {
  ASurfaceTexture* texture = JavaSurfaceTexture(env, object);
  if (texture == nullptr) return HAL_DATASPACE_UNKNOWN;
  std::lock_guard<std::mutex> lock(texture->mutex);
  if (!texture->pending.empty()) return texture->pending.back().dataspace;
  return texture->current.dataspace;
}

void SurfaceTextureNativeRelease(JNIEnv* env, jobject object) {
  darwin_art_android_surface_texture_abandon(JavaSurfaceTexture(env, object));
}

jboolean SurfaceTextureNativeIsReleased(JNIEnv* env, jobject object) {
  ASurfaceTexture* texture = JavaSurfaceTexture(env, object);
  if (texture == nullptr) return JNI_TRUE;
  std::lock_guard<std::mutex> lock(texture->mutex);
  return texture->abandoned ? JNI_TRUE : JNI_FALSE;
}
}  // namespace

extern "C" jlong darwin_art_android_surface_texture_acquire_producer(
    JNIEnv* env, jobject object) {
  ASurfaceTexture* texture = JavaSurfaceTexture(env, object);
  if (texture == nullptr || texture->abandoned) return 0;
  darwin_art_android_ANativeWindow_acquire(texture->producer);
  return reinterpret_cast<jlong>(texture->producer);
}

namespace darwin_art {
bool RegisterDarwinSurfaceTextureNatives(JNIEnv* env) {
  jclass clazz = env->FindClass("android/graphics/SurfaceTexture");
  if (clazz == nullptr) return false;
  g_surface_texture_fields.texture =
      env->GetFieldID(clazz, "mSurfaceTexture", "J");
  g_surface_texture_fields.producer = env->GetFieldID(clazz, "mProducer", "J");
  if (g_surface_texture_fields.texture == nullptr ||
      g_surface_texture_fields.producer == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(clazz);
    return false;
  }
  JNINativeMethod methods[] = {
      {const_cast<char*>("nativeInit"),
       const_cast<char*>("(ZIZLjava/lang/ref/WeakReference;)V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeInit)},
      {const_cast<char*>("nativeFinalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeFinalize)},
      {const_cast<char*>("nativeSetDefaultBufferSize"),
       const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeSetDefaultBufferSize)},
      {const_cast<char*>("nativeUpdateTexImage"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeUpdateTexImage)},
      {const_cast<char*>("nativeReleaseTexImage"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeReleaseTexImage)},
      {const_cast<char*>("nativeDetachFromGLContext"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SurfaceTextureNativeDetach)},
      {const_cast<char*>("nativeAttachToGLContext"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&SurfaceTextureNativeAttach)},
      {const_cast<char*>("nativeGetTransformMatrix"),
       const_cast<char*>("([F)V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeGetTransformMatrix)},
      {const_cast<char*>("nativeGetTimestamp"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceTextureNativeGetTimestamp)},
      {const_cast<char*>("nativeGetDataSpace"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SurfaceTextureNativeGetDataSpace)},
      {const_cast<char*>("nativeRelease"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SurfaceTextureNativeRelease)},
      {const_cast<char*>("nativeIsReleased"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&SurfaceTextureNativeIsReleased)},
  };
  const bool success =
      env->RegisterNatives(clazz, methods,
                           static_cast<jint>(sizeof(methods) / sizeof(methods[0]))) == JNI_OK;
  env->DeleteLocalRef(clazz);
  return success;
}
}  // namespace darwin_art
