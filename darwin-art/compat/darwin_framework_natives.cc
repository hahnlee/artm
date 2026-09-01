#include "darwin_framework_natives.h"
#include "darwin_android_platform.h"
#include "darwin_android_surface_texture.h"
#include "darwin_audio_track.h"
#include "darwin_angle_egl.h"
#include "darwin_framework_system_natives.h"
#include "darwin_motion_event_natives.h"
#include "darwin_media_codec.h"
#include "darwin_security_trust.h"

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <atomic>
#include <new>
#include <unordered_map>
#include <vector>

#include <android/surface_control.h>

namespace {

void NativeAllocationRegistryApplyFreeFunction(JNIEnv*, jclass,
                                                jlong free_function,
                                                jlong native_ptr) {
  if (free_function == 0 || native_ptr == 0) return;
  using FreeFunction = void (*)(void*);
  reinterpret_cast<FreeFunction>(static_cast<std::uintptr_t>(free_function))(
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(native_ptr)));
}

// Android's HandlerThread calls this before starting framework animation and
// GL worker loops. Darwin does not expose Android's numeric scheduler
// priorities, so keep the call successful and leave the host QoS unchanged.
void ProcessSetThreadPriority(JNIEnv*, jclass, jint) {}
void ProcessSetThreadPriorityForTid(JNIEnv*, jclass, jint, jint) {}
jint ProcessGetThreadPriority(JNIEnv*, jclass, jint) {
  // Match the process-state facade's virtual Android scheduler contract. The
  // guest nice value is a hint and must never be forwarded with a guest tid to
  // Darwin's process scheduler, where that number could identify an unrelated
  // host process. Until the thread broker carries per-thread QoS, every live
  // guest thread therefore reports Android's default nice value.
  return 0;
}

void SurfaceControlFinalizer(void* control) {
  ASurfaceControl_release(reinterpret_cast<ASurfaceControl*>(control));
}
void SurfaceTransactionFinalizer(void* transaction) {
  ASurfaceTransaction_delete(
      reinterpret_cast<ASurfaceTransaction*>(transaction));
}
jlong SurfaceControlNativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&SurfaceControlFinalizer);
}
jlong SurfaceTransactionNativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&SurfaceTransactionFinalizer);
}
jlong NextEglHandle();
jlong SurfaceControlNativeCreate(JNIEnv* env, jclass, jobject, jstring name,
                                 jint, jint,
                                 jint, jint, jlong parent, jobject) {
  const char* utf = name == nullptr ? nullptr : env->GetStringUTFChars(name, nullptr);
  ASurfaceControl* control =
      parent == 0
          ? reinterpret_cast<ASurfaceControl*>(
                darwin_art_android_surface_control_create_root(
                    utf == nullptr ? "SurfaceControl" : utf))
          : ASurfaceControl_create(reinterpret_cast<ASurfaceControl*>(parent),
                                   utf == nullptr ? "SurfaceControl" : utf);
  if (utf != nullptr) env->ReleaseStringUTFChars(name, utf);
  return reinterpret_cast<jlong>(control);
}
jlong SurfaceControlNativeGetHandle(JNIEnv*, jclass, jlong native_object) {
  // SurfaceControl's public handle is a stable reference to the same native
  // layer identity; Binder serialization is supplied by the compositor bridge.
  return native_object;
}
jlong SurfaceControlNativeCopy(JNIEnv*, jclass, jlong native_object) {
  auto* control = reinterpret_cast<ASurfaceControl*>(native_object);
  if (control != nullptr) ASurfaceControl_acquire(control);
  return native_object;
}
void SurfaceControlNativeDisconnect(JNIEnv*, jclass, jlong) {}
jlong SurfaceControlNativeCreateTransaction(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(ASurfaceTransaction_create());
}
void SurfaceControlNativeSetTransformHint(JNIEnv*, jclass, jlong, jint) {}
void SurfaceControlNativeSetFrameRateCategory(JNIEnv*, jclass, jlong, jlong,
                                              jint, jboolean) {}
void SurfaceControlNativeClearTransaction(JNIEnv*, jclass, jlong transaction) {
  darwin_art_android_surface_transaction_clear(
      reinterpret_cast<void*>(transaction));
}
void SurfaceControlNativeMergeTransaction(JNIEnv*, jclass, jlong destination,
                                          jlong source) {
  darwin_art_android_surface_transaction_merge(
      reinterpret_cast<void*>(destination), reinterpret_cast<void*>(source));
}
void SurfaceControlNativeTransactionNoop1(JNIEnv*, jclass, jlong) {}
void SurfaceControlNativeTransactionNoop2(JNIEnv*, jclass, jlong, jlong) {}
void SurfaceControlNativeTransactionNoop3(JNIEnv*, jclass, jlong, jlong,
                                          jlong) {}
ASurfaceTransaction* SurfaceTransaction(jlong handle) {
  return reinterpret_cast<ASurfaceTransaction*>(handle);
}
ASurfaceControl* SurfaceControl(jlong handle) {
  return reinterpret_cast<ASurfaceControl*>(handle);
}

constexpr jint kSurfaceControlLayerHidden = 0x01;
constexpr bool SurfaceControlFlagsChangeVisibility(jint mask) {
  return (mask & kSurfaceControlLayerHidden) != 0;
}
static_assert(SurfaceControlFlagsChangeVisibility(0x01));
static_assert(!SurfaceControlFlagsChangeVisibility(0x02));  // OPAQUE
static_assert(!SurfaceControlFlagsChangeVisibility(0x40));  // SKIP_SCREENSHOT
static_assert(!SurfaceControlFlagsChangeVisibility(0x80));  // SECURE

void SurfaceControlNativeSetFlags(JNIEnv*, jclass, jlong transaction,
                                  jlong control, jint flags, jint mask) {
  // layer_state_t::eLayerHidden is bit 0.  nativeSetFlags also carries
  // independent state such as OPAQUE, SECURE, SKIP_SCREENSHOT, and
  // backpressure; those bits must not change layer visibility.
  if (!SurfaceControlFlagsChangeVisibility(mask)) return;
  ASurfaceTransaction_setVisibility(
      SurfaceTransaction(transaction), SurfaceControl(control),
      (flags & kSurfaceControlLayerHidden) == 0
          ? ASURFACE_TRANSACTION_VISIBILITY_SHOW
          : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
}
void SurfaceControlNativeSetPosition(JNIEnv*, jclass, jlong transaction,
                                     jlong control, jfloat x, jfloat y) {
  ASurfaceTransaction_setPosition(SurfaceTransaction(transaction),
                                  SurfaceControl(control),
                                  static_cast<int32_t>(std::lround(x)),
                                  static_cast<int32_t>(std::lround(y)));
}
void SurfaceControlNativeSetScale(JNIEnv*, jclass, jlong transaction,
                                  jlong control, jfloat x, jfloat y) {
  ASurfaceTransaction_setScale(SurfaceTransaction(transaction),
                               SurfaceControl(control), x, y);
}
void SurfaceControlNativeSetLayer(JNIEnv*, jclass, jlong transaction,
                                  jlong control, jint layer) {
  ASurfaceTransaction_setZOrder(SurfaceTransaction(transaction),
                                SurfaceControl(control), layer);
}
void SurfaceControlNativeSetRelativeLayer(JNIEnv*, jclass, jlong transaction,
                                          jlong control, jlong relative_to,
                                          jint layer) {
  darwin_art_android_surface_transaction_set_relative_layer(
      reinterpret_cast<void*>(transaction), reinterpret_cast<void*>(control),
      reinterpret_cast<void*>(relative_to), layer);
}
void SurfaceControlNativeReparent(JNIEnv*, jclass, jlong transaction,
                                  jlong control, jlong parent) {
  ASurfaceTransaction_reparent(SurfaceTransaction(transaction),
                               SurfaceControl(control), SurfaceControl(parent));
}
void SurfaceControlNativeSetAlpha(JNIEnv*, jclass, jlong transaction,
                                  jlong control, jfloat alpha) {
  ASurfaceTransaction_setBufferAlpha(SurfaceTransaction(transaction),
                                     SurfaceControl(control), alpha);
}
void SurfaceControlNativeSetMatrix(JNIEnv*, jclass, jlong transaction,
                                   jlong control, jfloat dsdx, jfloat,
                                   jfloat, jfloat dtdy) {
  ASurfaceTransaction_setScale(SurfaceTransaction(transaction),
                               SurfaceControl(control), dsdx, dtdy);
}
void SurfaceControlNativeSetWindowCrop(JNIEnv*, jclass, jlong transaction,
                                       jlong control, jint left, jint top,
                                       jint right, jint bottom) {
  const ARect crop{left, top, right, bottom};
  ASurfaceTransaction_setCrop(SurfaceTransaction(transaction),
                              SurfaceControl(control), crop);
}
void SurfaceControlNativeSetBufferTransform(JNIEnv*, jclass,
                                            jlong transaction, jlong control,
                                            jint transform) {
  ASurfaceTransaction_setBufferTransform(SurfaceTransaction(transaction),
                                         SurfaceControl(control), transform);
}
void SurfaceControlNativeApplyTransaction(JNIEnv*, jclass, jlong transaction,
                                          jboolean, jboolean) {
  ASurfaceTransaction_apply(
      reinterpret_cast<ASurfaceTransaction*>(transaction));
}
void SurfaceControlNativeSetExtendedRangeBrightness(JNIEnv*, jclass, jlong,
                                                    jlong, jfloat, jfloat);
void SurfaceControlNativeSetExtendedRangeBrightness(JNIEnv*, jclass,
                                                    jlong transaction,
                                                    jlong control,
                                                    jfloat current_ratio,
                                                    jfloat desired_ratio) {
  // Standard-dynamic-range windows require no headroom transform. The Metal
  // composer will consume these values when HDR layer state is plumbed; the
  // optional hint must not abort an otherwise valid HWUI transaction.
  ASurfaceTransaction_setExtendedRangeBrightness(
      SurfaceTransaction(transaction), SurfaceControl(control), current_ratio,
      desired_ratio);
}
void SurfaceControlNativeSetColor(JNIEnv* env, jclass, jlong transaction,
                                  jlong control, jfloatArray color) {
  jfloat values[3]{0.0f, 0.0f, 0.0f};
  if (color != nullptr && env->GetArrayLength(color) >= 3) {
    env->GetFloatArrayRegion(color, 0, 3, values);
  }
  ASurfaceTransaction_setColor(
      SurfaceTransaction(transaction), SurfaceControl(control), values[0],
      values[1], values[2], 1.0f, ADATASPACE_UNKNOWN);
}
void SurfaceControlNativeSetDesiredHdrHeadroom(JNIEnv*, jclass,
                                               jlong transaction,
                                               jlong control, jfloat ratio) {
  // The SDR Metal swapchain has a fixed headroom of 1.0. Keep the framework
  // transaction valid; HDR negotiation belongs to the display backend.
  ASurfaceTransaction_setDesiredHdrHeadroom(
      SurfaceTransaction(transaction), SurfaceControl(control), ratio);
}

struct DarwinBlastBufferQueue {
  void* native_window = nullptr;
  ASurfaceControl* surface_control = nullptr;
  jint width = 0;
  jint height = 0;
  jint format = 1;
};

jlong BlastBufferQueueNativeCreate(JNIEnv*, jclass, jstring, jboolean) {
  return reinterpret_cast<jlong>(new (std::nothrow) DarwinBlastBufferQueue());
}
void BlastBufferQueueNativeNoop(JNIEnv*, jclass, jlong) {}
void BlastBufferQueueNativeNoop2(JNIEnv*, jclass, jlong, jlong) {}
void BlastBufferQueueNativeNoop3(JNIEnv*, jclass, jlong, jlong, jlong) {}
void BlastBufferQueueNativeDestroy(JNIEnv*, jclass, jlong handle) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr) return;
  if (queue->native_window != nullptr) {
    darwin_art_android_ANativeWindow_release(queue->native_window);
  }
  delete queue;
}
void BlastBufferQueueNativeUpdate(JNIEnv*, jclass, jlong handle,
                                  jlong surface_control, jlong width,
                                  jlong height, jint format) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr || width <= 0 || height <= 0 || width > INT32_MAX ||
      height > INT32_MAX) {
    return;
  }
  queue->width = static_cast<jint>(width);
  queue->height = static_cast<jint>(height);
  queue->format = format == 0 ? 1 : format;
  queue->surface_control = reinterpret_cast<ASurfaceControl*>(surface_control);
  if (queue->native_window == nullptr) {
    queue->native_window = darwin_art_android_ANativeWindow_create(
        queue->width, queue->height, queue->format);
  } else {
    (void)darwin_art_android_ANativeWindow_setBuffersGeometry(
        queue->native_window, queue->width, queue->height, queue->format);
  }
  darwin_art_android_ANativeWindow_set_surface_control(
      queue->native_window, queue->surface_control);
}
jlong BlastBufferQueueNativeGetLastAcquiredFrameNum(JNIEnv*, jclass, jlong) {
  return 0;
}
jboolean BlastBufferQueueNativeIsSameSurfaceControl(JNIEnv*, jclass,
                                                    jlong handle,
                                                    jlong surface_control) {
  const auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  return queue != nullptr &&
                 queue->surface_control ==
                     reinterpret_cast<ASurfaceControl*>(surface_control)
             ? JNI_TRUE
             : JNI_FALSE;
}
jobject BlastBufferQueueNativeGetSurface(JNIEnv* env, jclass, jlong handle,
                                         jboolean) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  jclass surface_class = env->FindClass("android/view/Surface");
  jmethodID constructor = surface_class == nullptr
                              ? nullptr
                              : env->GetMethodID(surface_class, "<init>", "()V");
  jobject surface = constructor == nullptr
                        ? nullptr
                        : env->NewObject(surface_class, constructor);
  jfieldID native_object = surface_class == nullptr
                               ? nullptr
                               : env->GetFieldID(surface_class, "mNativeObject", "J");
  if (surface != nullptr && native_object != nullptr && queue != nullptr &&
      queue->native_window != nullptr && !env->ExceptionCheck()) {
    darwin_art_android_ANativeWindow_acquire(queue->native_window);
    env->SetLongField(
        surface, native_object,
        reinterpret_cast<jlong>(queue->native_window));
  }
  env->DeleteLocalRef(surface_class);
  return surface;
}
jobject BlastBufferQueueNativeGatherPendingTransactions(JNIEnv* env, jclass,
                                                         jlong, jlong) {
  jclass transaction_class =
      env->FindClass("android/view/SurfaceControl$Transaction");
  jmethodID constructor =
      transaction_class == nullptr
          ? nullptr
          : env->GetMethodID(transaction_class, "<init>", "()V");
  jobject transaction = constructor == nullptr
                            ? nullptr
                            : env->NewObject(transaction_class, constructor);
  env->DeleteLocalRef(transaction_class);
  return transaction;
}
void BlastBufferQueueNativeSetApplyToken(JNIEnv*, jclass, jlong, jobject) {}
void BlastBufferQueueNativeSetHangCallback(JNIEnv*, jclass, jlong, jobject) {}
jboolean BlastBufferQueueNativeSyncNextTransaction(JNIEnv*, jclass, jlong,
                                                   jobject, jboolean) {
  return JNI_TRUE;
}

std::atomic<jlong> g_egl_handle{1};
std::atomic<jint> g_audio_session_id{1};

jfieldID AudioTrackNativeField(JNIEnv* env, jobject track) {
  jclass clazz = env->GetObjectClass(track);
  if (clazz == nullptr) return nullptr;
  jfieldID field = env->GetFieldID(clazz, "mNativeTrackInJavaObj", "J");
  env->DeleteLocalRef(clazz);
  return field;
}

DarwinAudioTrack* GetAudioTrack(JNIEnv* env, jobject track) {
  jfieldID field = AudioTrackNativeField(env, track);
  if (field == nullptr) return nullptr;
  return reinterpret_cast<DarwinAudioTrack*>(
      static_cast<uintptr_t>(env->GetLongField(track, field)));
}

// A detached host Surface is a process-local producer endpoint backed by the
// active IOSurface/CAMetalLayer bridge. Java owns the token lifetime; native
// video libraries receive the ordinary android.view.Surface object.
jboolean SurfaceNativeIsValid(JNIEnv*, jclass, jlong handle) {
  return handle != 0 ? JNI_TRUE : JNI_FALSE;
}
void SurfaceNativeRelease(JNIEnv*, jclass, jlong handle) {
  (void)darwin_art_android_ANativeWindow_release_if_managed(
      reinterpret_cast<void*>(static_cast<uintptr_t>(handle)));
}
jlong SurfaceNativeCreateFromSurfaceTexture(JNIEnv* env, jclass,
                                            jobject surface_texture) {
  return darwin_art_android_surface_texture_acquire_producer(env,
                                                             surface_texture);
}
jint SurfaceNativeGetWidth(JNIEnv*, jclass, jlong handle) {
  return handle == 0 ? 0 : darwin_art::DarwinAngleHostSurfaceWidth();
}
jint SurfaceNativeGetHeight(JNIEnv*, jclass, jlong handle) {
  return handle == 0 ? 0 : darwin_art::DarwinAngleHostSurfaceHeight();
}
jlong SurfaceNativeGetNextFrameNumber(JNIEnv*, jclass, jlong) { return 0; }
jboolean SurfaceNativeFalse(JNIEnv*, jclass, jlong) { return JNI_FALSE; }
void SurfaceNativeAllocateBuffers(JNIEnv*, jclass, jlong) {}
jint SurfaceNativeStatus(JNIEnv*, jclass, jlong, jint) { return 0; }
jint SurfaceNativeForceDisconnect(JNIEnv*, jclass, jlong) { return 0; }
jint SurfaceNativeSetBoolean(JNIEnv*, jclass, jlong, jboolean) { return 0; }
jint SurfaceNativeSetFrameRate(JNIEnv*, jclass, jlong, jfloat, jint, jint) {
  return 0;
}
jlong SurfaceNativeGetFromBlastBufferQueue(JNIEnv*, jclass, jlong old_surface,
                                           jlong blast_queue) {
  // BLASTBufferQueue is the producer identity for this Java Surface. Reuse it
  // across Surface.copyFrom() calls so producer replacement remains atomic.
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(blast_queue);
  if (queue == nullptr || queue->native_window == nullptr) return old_surface;
  darwin_art_android_ANativeWindow_acquire(queue->native_window);
  return reinterpret_cast<jlong>(queue->native_window);
}

constexpr uint64_t kDarwinSurfaceParcelMagic = 0x4441534600000000ull;

jlong SurfaceNativeReadFromParcel(JNIEnv* env, jclass, jlong old_handle,
                                  jobject parcel) {
  if (env == nullptr || parcel == nullptr) return 0;
  jclass parcel_class = env->GetObjectClass(parcel);
  jmethodID read_long = parcel_class == nullptr
                            ? nullptr
                            : env->GetMethodID(parcel_class, "readLong", "()J");
  const jlong token = read_long == nullptr
                          ? 0
                          : env->CallLongMethod(parcel, read_long);
  if (env->ExceptionCheck() ||
      (static_cast<uint64_t>(token) & 0xffffffff00000000ull) !=
          kDarwinSurfaceParcelMagic) {
    env->DeleteLocalRef(parcel_class);
    return 0;
  }
  jmethodID read_int = env->GetMethodID(parcel_class, "readInt", "()I");
  const jint width =
      read_int == nullptr ? 0 : env->CallIntMethod(parcel, read_int);
  const jint height =
      read_int == nullptr ? 0 : env->CallIntMethod(parcel, read_int);
  const jint owner_process_id =
      read_int == nullptr ? 0 : env->CallIntMethod(parcel, read_int);
  const jint layer_id =
      read_int == nullptr ? 0 : env->CallIntMethod(parcel, read_int);
  env->DeleteLocalRef(parcel_class);
  if (width > 0 && height > 0 && !env->ExceptionCheck()) {
    darwin_art::ConfigureDarwinAngleHostSurface(0, 0, width, height);
  }
  if (owner_process_id > 0 && layer_id > 0 && !env->ExceptionCheck()) {
    darwin_art_android_ANativeWindow_register_imported_surface_identity(
        token, static_cast<uint32_t>(owner_process_id),
        static_cast<uint32_t>(layer_id));
  }
  if (std::getenv("DARWIN_ART_DEBUG_ANATIVEWINDOW") != nullptr) {
    std::cerr << "ART Android Surface parcel: read pid=" << getpid()
              << " token=0x" << std::hex << static_cast<uint64_t>(token)
              << std::dec << " size=" << width << "x" << height << "\n";
  }
  static_cast<void>(old_handle);
  // This is a process-independent surface identity, not a native pointer.
  // The GPU process resolves the matching IOSurface through the host surface
  // broker before ANGLE creates its EGL window surface.
  return token;
}

void SurfaceNativeWriteToParcel(JNIEnv* env, jclass, jlong handle,
                                jobject parcel) {
  if (env == nullptr || parcel == nullptr) return;
  jclass parcel_class = env->GetObjectClass(parcel);
  jmethodID write_long =
      parcel_class == nullptr
          ? nullptr
          : env->GetMethodID(parcel_class, "writeLong", "(J)V");
  if (write_long != nullptr) {
    const uint64_t identity =
        kDarwinSurfaceParcelMagic |
        (handle == 0 ? 0ull : static_cast<uint64_t>(handle) & 0xffffffffull);
    env->CallVoidMethod(parcel, write_long, static_cast<jlong>(identity));
    jmethodID write_int =
        env->GetMethodID(parcel_class, "writeInt", "(I)V");
    if (write_int != nullptr) {
      const jint width = darwin_art::DarwinAngleHostSurfaceWidth();
      const jint height = darwin_art::DarwinAngleHostSurfaceHeight();
      env->CallVoidMethod(parcel, write_int, width);
      env->CallVoidMethod(parcel, write_int, height);
      uint32_t owner_process_id = 0;
      uint32_t layer_id = 0;
      (void)darwin_art_android_ANativeWindow_get_surface_control_identity(
          reinterpret_cast<void*>(static_cast<uintptr_t>(handle)),
          &owner_process_id, &layer_id);
      env->CallVoidMethod(parcel, write_int,
                          static_cast<jint>(owner_process_id));
      env->CallVoidMethod(parcel, write_int, static_cast<jint>(layer_id));
      if (std::getenv("DARWIN_ART_DEBUG_ANATIVEWINDOW") != nullptr) {
        std::cerr << "ART Android Surface parcel: write pid=" << getpid()
                  << " token=0x" << std::hex << identity << std::dec
                  << " size=" << width << "x" << height
                  << " owner=" << owner_process_id
                  << " layer=" << layer_id << "\n";
      }
    }
  }
  env->DeleteLocalRef(parcel_class);
}

jint AudioSystemGetMaxChannelCount(JNIEnv*, jclass) { return 24; }
jint AudioSystemGetMaxSampleRate(JNIEnv*, jclass) { return 192000; }
jint AudioSystemGetMinSampleRate(JNIEnv*, jclass) { return 4000; }
jint AudioSystemGetPrimaryOutputSampleRate(JNIEnv*, jclass) {
  return darwin_audio_primary_output_sample_rate();
}
jint AudioSystemGetPrimaryOutputFrameCount(JNIEnv*, jclass) {
  return darwin_audio_primary_output_frame_count();
}
jint AudioSystemNewAudioSessionId(JNIEnv*, jclass) {
  return g_audio_session_id.fetch_add(1, std::memory_order_relaxed);
}
jint AudioTrackGetOutputSampleRate(JNIEnv*, jclass, jint) { return 48000; }
jint AudioTrackGetMinBufferSize(JNIEnv*, jclass, jint sample_rate,
                                jint channel_count, jint audio_format) {
  if (sample_rate <= 0 || channel_count <= 0 || audio_format <= 0) return -2;
  // Twenty milliseconds of stereo PCM16, rounded up to a practical queue
  // quantum. AudioTrack.native_setup owns the actual Darwin output stream.
  const jint bytes = (sample_rate / 50) * channel_count * 2;
  return std::max<jint>(4096, bytes);
}
jint AudioTrackSetup(JNIEnv* env, jobject track, jobject, jobject,
                     jintArray sample_rates, jint channel_mask, jint,
                     jint audio_format, jint buffer_bytes, jint,
                     jintArray session_ids, jobject, jlong, jboolean, jint,
                     jobject, jstring) {
  jint sample_rate = 48000;
  if (sample_rates != nullptr && env->GetArrayLength(sample_rates) > 0) {
    env->GetIntArrayRegion(sample_rates, 0, 1, &sample_rate);
  }
  buffer_bytes = std::max<jint>(4096, buffer_bytes);
  if (session_ids != nullptr && env->GetArrayLength(session_ids) > 0) {
    jint session = 0;
    env->GetIntArrayRegion(session_ids, 0, 1, &session);
    if (session == 0) {
      session = AudioSystemNewAudioSessionId(env, nullptr);
      env->SetIntArrayRegion(session_ids, 0, 1, &session);
    }
  }
  jfieldID field = AudioTrackNativeField(env, track);
  if (field == nullptr) return -20;
  DarwinAudioTrack* state = darwin_audio_track_create(
      sample_rate, channel_mask, audio_format, buffer_bytes);
  if (state == nullptr) return -20;
  env->SetLongField(track, field,
                    static_cast<jlong>(reinterpret_cast<uintptr_t>(state)));
  return 0;
}
void AudioTrackRelease(JNIEnv* env, jobject track) {
  jfieldID field = AudioTrackNativeField(env, track);
  if (field == nullptr) return;
  auto* state = reinterpret_cast<DarwinAudioTrack*>(
      static_cast<uintptr_t>(env->GetLongField(track, field)));
  env->SetLongField(track, field, 0);
  darwin_audio_track_destroy(state);
}
void AudioTrackVoid(JNIEnv*, jobject) {}
void AudioTrackStart(JNIEnv* env, jobject track) {
  darwin_audio_track_start(GetAudioTrack(env, track));
}
void AudioTrackStop(JNIEnv* env, jobject track) {
  darwin_audio_track_stop(GetAudioTrack(env, track));
}
void AudioTrackPause(JNIEnv* env, jobject track) {
  darwin_audio_track_pause(GetAudioTrack(env, track));
}
void AudioTrackFlush(JNIEnv* env, jobject track) {
  darwin_audio_track_flush(GetAudioTrack(env, track));
}
void AudioTrackSetPlayerId(JNIEnv*, jobject, jint) {}
void AudioTrackSetVolume(JNIEnv* env, jobject track, jfloat left, jfloat right) {
  darwin_audio_track_set_volume(GetAudioTrack(env, track), left, right);
}
jint AudioTrackStatusInt(JNIEnv*, jobject, jint) { return 0; }
jint AudioTrackGetInt(JNIEnv* env, jobject track) {
  return darwin_audio_track_buffer_capacity_frames(GetAudioTrack(env, track));
}
jint AudioTrackGetPosition(JNIEnv* env, jobject track) {
  return static_cast<jint>(
      darwin_audio_track_position(GetAudioTrack(env, track)));
}
jint AudioTrackWriteByte(JNIEnv* env, jobject track, jbyteArray array,
                         jint offset, jint size, jint, jboolean blocking) {
  DarwinAudioTrack* state = GetAudioTrack(env, track);
  if (state == nullptr) return -3;
  if (array == nullptr || offset < 0 || size < 0 ||
      offset > env->GetArrayLength(array) - size) {
    return -2;
  }
  std::vector<jbyte> bytes(static_cast<size_t>(size));
  if (size != 0) env->GetByteArrayRegion(array, offset, size, bytes.data());
  if (env->ExceptionCheck()) return -3;
  return static_cast<jint>(darwin_audio_track_write(
      state, bytes.data(), bytes.size(), blocking == JNI_TRUE));
}
jint AudioTrackWriteShort(JNIEnv* env, jobject track, jshortArray array,
                          jint offset, jint size, jint, jboolean blocking) {
  DarwinAudioTrack* state = GetAudioTrack(env, track);
  if (state == nullptr) return -3;
  if (array == nullptr || offset < 0 || size < 0 ||
      offset > env->GetArrayLength(array) - size) {
    return -2;
  }
  std::vector<jshort> samples(static_cast<size_t>(size));
  if (size != 0) env->GetShortArrayRegion(array, offset, size, samples.data());
  if (env->ExceptionCheck()) return -3;
  const size_t bytes = darwin_audio_track_write(
      state, samples.data(), samples.size() * sizeof(jshort),
      blocking == JNI_TRUE);
  return static_cast<jint>(bytes / sizeof(jshort));
}
jint AudioTrackWriteFloat(JNIEnv* env, jobject track, jfloatArray array,
                          jint offset, jint size, jint, jboolean blocking) {
  DarwinAudioTrack* state = GetAudioTrack(env, track);
  if (state == nullptr) return -3;
  if (array == nullptr || offset < 0 || size < 0 ||
      offset > env->GetArrayLength(array) - size) {
    return -2;
  }
  std::vector<jfloat> samples(static_cast<size_t>(size));
  if (size != 0) env->GetFloatArrayRegion(array, offset, size, samples.data());
  if (env->ExceptionCheck()) return -3;
  const size_t bytes = darwin_audio_track_write(
      state, samples.data(), samples.size() * sizeof(jfloat),
      blocking == JNI_TRUE);
  return static_cast<jint>(bytes / sizeof(jfloat));
}
jint AudioTrackWriteBuffer(JNIEnv* env, jobject track, jobject buffer,
                           jint offset, jint size, jint, jboolean blocking) {
  DarwinAudioTrack* state = GetAudioTrack(env, track);
  if (state == nullptr) return -3;
  auto* data = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  const jlong capacity = env->GetDirectBufferCapacity(buffer);
  if (data == nullptr || offset < 0 || size < 0 || capacity < 0 ||
      static_cast<jlong>(offset) > capacity - size) {
    return -2;
  }
  return static_cast<jint>(darwin_audio_track_write(
      state, data + offset, static_cast<size_t>(size), blocking == JNI_TRUE));
}
jint AudioSystemListPorts(JNIEnv* env, jclass, jobject, jintArray generation) {
  if (generation != nullptr && env->GetArrayLength(generation) > 0) {
    const jint value = 1;
    env->SetIntArrayRegion(generation, 0, 1, &value);
  }
  // AUDIO_STATUS_OK with an empty list is the valid detached-host topology;
  // AudioTrack itself is backed by the Darwin audio implementation.
  return 0;
}
jint AudioSystemSetParameters(JNIEnv*, jclass, jstring) { return 0; }
jint AudioProductStrategyList(JNIEnv*, jclass, jobject) { return 0; }
void AudioPortEventNativeSetup(JNIEnv*, jobject, jobject) {}
void AudioPortEventNativeFinalize(JNIEnv*, jobject) {}

struct DarwinVelocitySample {
  jlong time_ms = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct DarwinVelocityTracker {
  std::unordered_map<jint, std::vector<DarwinVelocitySample>> samples;
  std::unordered_map<jint, std::array<float, 2>> computed;
  jint active_pointer_id = -1;
};

void AddVelocitySample(DarwinVelocityTracker* tracker, jint pointer_id,
                       jlong time_ms, float x, float y) {
  if (tracker == nullptr || time_ms < 0 || !std::isfinite(x) ||
      !std::isfinite(y)) {
    return;
  }
  auto& samples = tracker->samples[pointer_id];
  if (!samples.empty() && samples.back().time_ms == time_ms) {
    samples.back() = {time_ms, x, y};
  } else {
    samples.push_back({time_ms, x, y});
  }
  const jlong horizon = time_ms - 200;
  samples.erase(std::remove_if(samples.begin(), samples.end(),
                               [horizon](const DarwinVelocitySample& sample) {
                                 return sample.time_ms < horizon;
                               }),
                samples.end());
  if (samples.size() > 20) {
    samples.erase(samples.begin(), samples.end() - 20);
  }
  if (tracker->active_pointer_id < 0) tracker->active_pointer_id = pointer_id;
}

float EstimateVelocity(const std::vector<DarwinVelocitySample>& samples,
                       jint axis, jint units, float max_velocity) {
  if (samples.size() < 2 || units <= 0) return 0.0f;
  const jlong newest = samples.back().time_ms;
  size_t first = 0;
  while (first + 1 < samples.size() && samples[first].time_ms < newest - 100) {
    ++first;
  }
  const size_t count = samples.size() - first;
  if (count < 2) return 0.0f;
  double mean_time = 0.0;
  double mean_position = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    mean_time += static_cast<double>(samples[index].time_ms - newest);
    mean_position += axis == 0 ? samples[index].x : samples[index].y;
  }
  mean_time /= static_cast<double>(count);
  mean_position /= static_cast<double>(count);
  double numerator = 0.0;
  double denominator = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    const double time = static_cast<double>(samples[index].time_ms - newest) -
                        mean_time;
    const double position =
        static_cast<double>(axis == 0 ? samples[index].x : samples[index].y) -
        mean_position;
    numerator += time * position;
    denominator += time * time;
  }
  if (denominator <= 0.0) return 0.0f;
  const float velocity =
      static_cast<float>((numerator / denominator) * units);
  return std::clamp(velocity, -max_velocity, max_velocity);
}

jlong VelocityTrackerInitialize(JNIEnv*, jclass, jint) {
  auto* tracker = new (std::nothrow) DarwinVelocityTracker();
  return reinterpret_cast<jlong>(tracker);
}

void VelocityTrackerDispose(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<DarwinVelocityTracker*>(handle);
}

void VelocityTrackerAddMovement(JNIEnv* env, jclass, jlong handle, jobject event) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr || event == nullptr) return;
  jclass event_class = env->GetObjectClass(event);
  jmethodID get_pointer_count =
      env->GetMethodID(event_class, "getPointerCount", "()I");
  jmethodID get_pointer_id = env->GetMethodID(event_class, "getPointerId", "(I)I");
  jmethodID get_history_size =
      env->GetMethodID(event_class, "getHistorySize", "()I");
  jmethodID get_historical_time =
      env->GetMethodID(event_class, "getHistoricalEventTime", "(I)J");
  jmethodID get_historical_x =
      env->GetMethodID(event_class, "getHistoricalX", "(II)F");
  jmethodID get_historical_y =
      env->GetMethodID(event_class, "getHistoricalY", "(II)F");
  jmethodID get_event_time = env->GetMethodID(event_class, "getEventTime", "()J");
  jmethodID get_x = env->GetMethodID(event_class, "getX", "(I)F");
  jmethodID get_y = env->GetMethodID(event_class, "getY", "(I)F");
  if (env->ExceptionCheck() || get_pointer_count == nullptr ||
      get_pointer_id == nullptr || get_history_size == nullptr ||
      get_historical_time == nullptr || get_historical_x == nullptr ||
      get_historical_y == nullptr || get_event_time == nullptr ||
      get_x == nullptr || get_y == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(event_class);
    return;
  }
  const jint pointer_count = env->CallIntMethod(event, get_pointer_count);
  const jint history_size = env->CallIntMethod(event, get_history_size);
  for (jint pointer_index = 0; pointer_index < pointer_count; ++pointer_index) {
    const jint pointer_id =
        env->CallIntMethod(event, get_pointer_id, pointer_index);
    for (jint history_index = 0; history_index < history_size; ++history_index) {
      AddVelocitySample(
          tracker, pointer_id,
          env->CallLongMethod(event, get_historical_time, history_index),
          env->CallFloatMethod(event, get_historical_x, pointer_index,
                               history_index),
          env->CallFloatMethod(event, get_historical_y, pointer_index,
                               history_index));
    }
    AddVelocitySample(tracker, pointer_id,
                      env->CallLongMethod(event, get_event_time),
                      env->CallFloatMethod(event, get_x, pointer_index),
                      env->CallFloatMethod(event, get_y, pointer_index));
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(event_class);
}

void VelocityTrackerClear(JNIEnv*, jclass, jlong handle) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr) return;
  tracker->samples.clear();
  tracker->computed.clear();
  tracker->active_pointer_id = -1;
}

void VelocityTrackerComputeCurrentVelocity(JNIEnv*, jclass, jlong handle,
                                           jint units, jfloat max_velocity) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr) return;
  tracker->computed.clear();
  for (const auto& [pointer_id, samples] : tracker->samples) {
    tracker->computed[pointer_id] = {
        EstimateVelocity(samples, 0, units, max_velocity),
        EstimateVelocity(samples, 1, units, max_velocity)};
    if (std::getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != nullptr) {
      std::cerr << "ART Android VelocityTracker pointer=" << pointer_id
                << " samples=" << samples.size()
                << " vx=" << tracker->computed[pointer_id][0]
                << " vy=" << tracker->computed[pointer_id][1] << "\n";
    }
  }
}

jfloat VelocityTrackerGetVelocity(JNIEnv*, jclass, jlong handle, jint axis,
                                  jint pointer_id) {
  auto* tracker = reinterpret_cast<DarwinVelocityTracker*>(handle);
  if (tracker == nullptr || (axis != 0 && axis != 1)) return 0.0f;
  if (pointer_id == -1) pointer_id = tracker->active_pointer_id;
  const auto velocity = tracker->computed.find(pointer_id);
  return velocity == tracker->computed.end() ? 0.0f : velocity->second[axis];
}
jboolean VelocityTrackerIsAxisSupported(JNIEnv*, jclass, jint axis) {
  return axis == 0 || axis == 1 ? JNI_TRUE : JNI_FALSE;
}

jlong NextEglHandle() { return g_egl_handle.fetch_add(1, std::memory_order_relaxed); }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
struct DarwinPaint {
  jint flags = 0;
  jint color = 0xff000000;
};
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
void PaintFinalizer(void* paint) { delete static_cast<DarwinPaint*>(paint); }

jlong PaintInit() {
  return reinterpret_cast<std::uintptr_t>(new DarwinPaint());
}

jlong PaintGetNativeFinalizer() {
  return reinterpret_cast<std::uintptr_t>(&PaintFinalizer);
}

void PaintSetFlags(jlong handle, jint flags) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->flags = flags;
  }
}

void PaintSetElegantTextHeight(jlong, jint) {}

jint PaintSetTextLocales(JNIEnv*, jclass, jlong, jstring) { return 0; }

void PaintSetColor(jlong handle, jint color) {
  auto* paint = reinterpret_cast<DarwinPaint*>(
      static_cast<std::uintptr_t>(handle));
  if (paint != nullptr) {
    paint->color = color;
  }
}
#endif

bool Register(JNIEnv* env, const char* class_name, JNINativeMethod* methods,
              jint method_count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) {
    return false;
  }
  const bool registered =
      env->RegisterNatives(klass, methods, method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}

void MediaDrmNativeInit(JNIEnv*, jclass) {}

jboolean MediaDrmIsCryptoSchemeSupported(JNIEnv*, jclass, jbyteArray,
                                         jstring, jint) {
  return JNI_FALSE;
}

jbyteArray MediaDrmGetSupportedCryptoSchemes(JNIEnv* env, jclass) {
  return env->NewByteArray(0);
}

jint CharsetUtilsToModifiedUtf8Bytes(JNIEnv* env, jclass, jstring source,
                                     jint source_length, jlong destination,
                                     jint destination_offset,
                                     jint destination_length) {
  auto* bytes = reinterpret_cast<char*>(static_cast<std::uintptr_t>(destination));
  const jint worst_length = source_length * 4;
  if (destination_offset >= 0 &&
      destination_offset + worst_length < destination_length) {
    env->GetStringUTFRegion(source, 0, source_length,
                            bytes + destination_offset);
    return static_cast<jint>(
        std::strlen(bytes + destination_offset + source_length) +
        source_length);
  }

  const jint encoded_length = env->GetStringUTFLength(source);
  if (destination_offset >= 0 &&
      destination_offset + encoded_length < destination_length) {
    env->GetStringUTFRegion(source, 0, source_length,
                            bytes + destination_offset);
    return encoded_length;
  }
  return -encoded_length;
}

jstring CharsetUtilsFromModifiedUtf8Bytes(JNIEnv* env, jclass, jlong source,
                                          jint source_offset,
                                          jint source_length) {
  auto* bytes = reinterpret_cast<char*>(static_cast<std::uintptr_t>(source));
  const char saved = bytes[source_offset + source_length];
  bytes[source_offset + source_length] = '\0';
  jstring result = env->NewStringUTF(bytes + source_offset);
  bytes[source_offset + source_length] = saved;
  return result;
}

}  // namespace

// Keep the dynamic JNI fallback as well as the explicit table registration.
// Some framework threads resolve Process natives before the framework table is
// visible through their boot-class loader.
extern "C" JNIEXPORT void Java_android_os_Process_setThreadPriority(
    JNIEnv*, jclass, jint) {}
extern "C" JNIEXPORT void Java_android_os_Process_setThreadPriority__II(
    JNIEnv*, jclass, jint, jint) {}
extern "C" JNIEXPORT jint Java_android_os_Process_getThreadPriority(
    JNIEnv*, jclass, jint) {
  return 0;
}
extern "C" JNIEXPORT jlong Java_android_os_Process_getElapsedCpuTime(
    JNIEnv* env, jclass clazz) {
  return darwin_art::framework_system::process_get_elapsed_cpu_time(env, clazz);
}

namespace darwin_art {

bool RegisterFrameworkSupportNatives(JNIEnv* env) {
  JNINativeMethod native_allocation_methods[] = {
      {const_cast<char*>("applyFreeFunction"), const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&NativeAllocationRegistryApplyFreeFunction)},
  };
  return Register(env, "libcore/util/NativeAllocationRegistry",
                  native_allocation_methods,
                  static_cast<jint>(std::size(native_allocation_methods))) &&
         RegisterDarwinSecurityTrustNatives(env);
}

bool RegisterFrameworkNatives(JNIEnv* env) {
  JNINativeMethod charset_utils_methods[] = {
      {const_cast<char*>("toModifiedUtf8Bytes"),
       const_cast<char*>("(Ljava/lang/String;IJII)I"),
       reinterpret_cast<void*>(&CharsetUtilsToModifiedUtf8Bytes)},
      {const_cast<char*>("fromModifiedUtf8Bytes"),
       const_cast<char*>("(JII)Ljava/lang/String;"),
       reinterpret_cast<void*>(&CharsetUtilsFromModifiedUtf8Bytes)},
  };
  if (!Register(env, "android/util/CharsetUtils", charset_utils_methods,
                static_cast<jint>(std::size(charset_utils_methods)))) {
    return false;
  }

  JNINativeMethod media_drm_methods[] = {
      {const_cast<char*>("native_init"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaDrmNativeInit)},
      {const_cast<char*>("isCryptoSchemeSupportedNative"),
       const_cast<char*>("([BLjava/lang/String;I)Z"),
       reinterpret_cast<void*>(&MediaDrmIsCryptoSchemeSupported)},
      {const_cast<char*>("getSupportedCryptoSchemesNative"),
       const_cast<char*>("()[B"),
       reinterpret_cast<void*>(&MediaDrmGetSupportedCryptoSchemes)},
  };
  if (!Register(env, "android/media/MediaDrm", media_drm_methods,
                static_cast<jint>(std::size(media_drm_methods)))) {
    return false;
  }
  if (!darwin_art::RegisterDarwinMediaCodecNatives(env)) {
    return false;
  }
  if (!RegisterMotionEventNatives(env)) {
    return false;
  }
  JNINativeMethod velocity_tracker_methods[] = {
      {const_cast<char*>("nativeInitialize"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&VelocityTrackerInitialize)},
      {const_cast<char*>("nativeDispose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&VelocityTrackerDispose)},
      {const_cast<char*>("nativeAddMovement"),
       const_cast<char*>("(JLandroid/view/MotionEvent;)V"),
       reinterpret_cast<void*>(&VelocityTrackerAddMovement)},
      {const_cast<char*>("nativeClear"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&VelocityTrackerClear)},
      {const_cast<char*>("nativeComputeCurrentVelocity"), const_cast<char*>("(JIF)V"),
       reinterpret_cast<void*>(&VelocityTrackerComputeCurrentVelocity)},
      {const_cast<char*>("nativeGetVelocity"), const_cast<char*>("(JII)F"),
       reinterpret_cast<void*>(&VelocityTrackerGetVelocity)},
      {const_cast<char*>("nativeIsAxisSupported"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&VelocityTrackerIsAxisSupported)},
  };
  if (!Register(env, "android/view/VelocityTracker", velocity_tracker_methods,
                static_cast<jint>(std::size(velocity_tracker_methods)))) {
    return false;
  }
  using namespace framework_system;
  JNINativeMethod process_methods[] = {
      {const_cast<char*>("setThreadPriority"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&ProcessSetThreadPriority)},
      {const_cast<char*>("setThreadPriority"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&ProcessSetThreadPriorityForTid)},
      {const_cast<char*>("getThreadPriority"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&ProcessGetThreadPriority)},
      {const_cast<char*>("getElapsedCpuTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&process_get_elapsed_cpu_time)},
  };
  if (!Register(env, "android/os/Process", process_methods,
                static_cast<jint>(std::size(process_methods)))) {
    return false;
  }

  // Match the framework capability constants returned by AOSP's
  // android_media_AudioSystem.cpp. Session IDs normally come from AudioFlinger;
  // the detached single-process runtime owns the equivalent monotonic scope.
  JNINativeMethod audio_system_methods[] = {
      {const_cast<char*>("native_getMaxChannelCount"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemGetMaxChannelCount)},
      {const_cast<char*>("native_getMaxSampleRate"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemGetMaxSampleRate)},
      {const_cast<char*>("native_getMinSampleRate"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemGetMinSampleRate)},
      {const_cast<char*>("getPrimaryOutputSamplingRate"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemGetPrimaryOutputSampleRate)},
      {const_cast<char*>("getPrimaryOutputFrameCount"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemGetPrimaryOutputFrameCount)},
      {const_cast<char*>("newAudioSessionId"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioSystemNewAudioSessionId)},
      {const_cast<char*>("listAudioPorts"),
       const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
       reinterpret_cast<void*>(&AudioSystemListPorts)},
      {const_cast<char*>("listAudioPatches"),
       const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
       reinterpret_cast<void*>(&AudioSystemListPorts)},
      {const_cast<char*>("setParameters"),
       const_cast<char*>("(Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&AudioSystemSetParameters)},
  };
  if (!Register(env, "android/media/AudioSystem", audio_system_methods,
                static_cast<jint>(std::size(audio_system_methods)))) {
    return false;
  }

  JNINativeMethod audio_track_methods[] = {
      {const_cast<char*>("native_get_output_sample_rate"),
       const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&AudioTrackGetOutputSampleRate)},
      {const_cast<char*>("native_get_min_buff_size"),
       const_cast<char*>("(III)I"),
       reinterpret_cast<void*>(&AudioTrackGetMinBufferSize)},
      {const_cast<char*>("native_setup"),
       const_cast<char*>("(Ljava/lang/Object;Ljava/lang/Object;[IIIIII[ILandroid/os/Parcel;JZILjava/lang/Object;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&AudioTrackSetup)},
      {const_cast<char*>("native_start"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackStart)},
      {const_cast<char*>("native_stop"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackStop)},
      {const_cast<char*>("native_pause"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackPause)},
      {const_cast<char*>("native_flush"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackFlush)},
      {const_cast<char*>("native_enableDeviceCallback"),
       const_cast<char*>("()V"), reinterpret_cast<void*>(&AudioTrackVoid)},
      {const_cast<char*>("native_disableDeviceCallback"),
       const_cast<char*>("()V"), reinterpret_cast<void*>(&AudioTrackVoid)},
      {const_cast<char*>("native_release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackRelease)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioTrackRelease)},
      {const_cast<char*>("native_setPlayerIId"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&AudioTrackSetPlayerId)},
      {const_cast<char*>("native_setVolume"), const_cast<char*>("(FF)V"),
       reinterpret_cast<void*>(&AudioTrackSetVolume)},
      {const_cast<char*>("native_set_playback_rate"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&AudioTrackStatusInt)},
      {const_cast<char*>("native_get_buffer_capacity_frames"),
       const_cast<char*>("()I"), reinterpret_cast<void*>(&AudioTrackGetInt)},
      {const_cast<char*>("native_get_buffer_size_frames"),
       const_cast<char*>("()I"), reinterpret_cast<void*>(&AudioTrackGetInt)},
      {const_cast<char*>("native_get_position"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&AudioTrackGetPosition)},
      {const_cast<char*>("native_write_byte"), const_cast<char*>("([BIIIZ)I"),
       reinterpret_cast<void*>(&AudioTrackWriteByte)},
      {const_cast<char*>("native_write_short"), const_cast<char*>("([SIIIZ)I"),
       reinterpret_cast<void*>(&AudioTrackWriteShort)},
      {const_cast<char*>("native_write_float"), const_cast<char*>("([FIIIZ)I"),
       reinterpret_cast<void*>(&AudioTrackWriteFloat)},
      {const_cast<char*>("native_write_native_bytes"),
       const_cast<char*>("(Ljava/nio/ByteBuffer;IIIZ)I"),
       reinterpret_cast<void*>(&AudioTrackWriteBuffer)},
  };
  if (!Register(env, "android/media/AudioTrack", audio_track_methods,
                static_cast<jint>(std::size(audio_track_methods)))) {
    return false;
  }

  JNINativeMethod audio_product_strategy_methods[] = {
      {const_cast<char*>("native_list_audio_product_strategies"),
       const_cast<char*>("(Ljava/util/ArrayList;)I"),
       reinterpret_cast<void*>(&AudioProductStrategyList)},
  };
  if (!Register(env, "android/media/audiopolicy/AudioProductStrategy",
                audio_product_strategy_methods,
                static_cast<jint>(std::size(audio_product_strategy_methods)))) {
    return false;
  }

  JNINativeMethod audio_port_event_methods[] = {
      {const_cast<char*>("native_setup"),
       const_cast<char*>("(Ljava/lang/Object;)V"),
       reinterpret_cast<void*>(&AudioPortEventNativeSetup)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&AudioPortEventNativeFinalize)},
  };
  if (!Register(env, "android/media/AudioPortEventHandler",
                audio_port_event_methods,
                static_cast<jint>(std::size(audio_port_event_methods)))) {
    return false;
  }

  JNINativeMethod surface_control_methods[] = {
      {const_cast<char*>("nativeCreate"),
       const_cast<char*>(
           "(Landroid/view/SurfaceSession;Ljava/lang/String;IIIIJ"
           "Landroid/os/Parcel;)J"),
       reinterpret_cast<void*>(&SurfaceControlNativeCreate)},
      {const_cast<char*>("nativeGetHandle"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&SurfaceControlNativeGetHandle)},
      {const_cast<char*>("nativeCopyFromSurfaceControl"),
       const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&SurfaceControlNativeCopy)},
      {const_cast<char*>("nativeDisconnect"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeDisconnect)},
      {const_cast<char*>("nativeGetNativeSurfaceControlFinalizer"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceControlNativeGetFinalizer)},
      {const_cast<char*>("nativeGetNativeTransactionFinalizer"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceTransactionNativeGetFinalizer)},
      {const_cast<char*>("nativeCreateTransaction"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SurfaceControlNativeCreateTransaction)},
      {const_cast<char*>("nativeApplyTransaction"),
       const_cast<char*>("(JZZ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeApplyTransaction)},
      {const_cast<char*>("nativeSetTransformHint"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetTransformHint)},
      {const_cast<char*>("nativeSetFrameRateCategory"),
       const_cast<char*>("(JJIZ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetFrameRateCategory)},
      {const_cast<char*>("nativeClearTransaction"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeClearTransaction)},
      {const_cast<char*>("nativeMergeTransaction"), const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeMergeTransaction)},
      {const_cast<char*>("nativeSetAnimationTransaction"),
       const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop1)},
      {const_cast<char*>("nativeSetEarlyWakeupStart"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop1)},
      {const_cast<char*>("nativeSetEarlyWakeupEnd"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop1)},
      {const_cast<char*>("nativeSetFlags"), const_cast<char*>("(JJII)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetFlags)},
      {const_cast<char*>("nativeSetPosition"), const_cast<char*>("(JJFF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetPosition)},
      {const_cast<char*>("nativeSetScale"), const_cast<char*>("(JJFF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetScale)},
      {const_cast<char*>("nativeSetLayer"), const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetLayer)},
      {const_cast<char*>("nativeSetLayerStack"), const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetRelativeLayer"), const_cast<char*>("(JJJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetRelativeLayer)},
      {const_cast<char*>("nativeReparent"), const_cast<char*>("(JJJ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeReparent)},
      {const_cast<char*>("nativeSetAlpha"), const_cast<char*>("(JJF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetAlpha)},
      {const_cast<char*>("nativeSetColor"), const_cast<char*>("(JJ[F)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetColor)},
      {const_cast<char*>("nativeSetDesiredHdrHeadroom"),
       const_cast<char*>("(JJF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetDesiredHdrHeadroom)},
      {const_cast<char*>("nativeSetMatrix"), const_cast<char*>("(JJFFFF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetMatrix)},
      {const_cast<char*>("nativeSetWindowCrop"),
       const_cast<char*>("(JJIIII)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetWindowCrop)},
      {const_cast<char*>("nativeSetCrop"), const_cast<char*>("(JJFFFF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetDestinationFrame"),
       const_cast<char*>("(JJIIII)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetCornerRadius"), const_cast<char*>("(JJF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetShadowRadius"), const_cast<char*>("(JJF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetColorSpaceAgnostic"),
       const_cast<char*>("(JJZ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetBufferTransform"),
       const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetBufferTransform)},
      {const_cast<char*>("nativeSetDamageRegion"),
       const_cast<char*>("(JJLandroid/graphics/Region;)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetTransparentRegionHint"),
       const_cast<char*>("(JJLandroid/graphics/Region;)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetDataSpace"), const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetFrameTimelineVsync"),
       const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetDesiredPresentTimeNanos"),
       const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetFixedTransformHint"),
       const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetFrameRate"),
       const_cast<char*>("(JJFII)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetFrameRateSelectionPriority"),
       const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetFrameRateSelectionStrategy"),
       const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetDimmingEnabled"),
       const_cast<char*>("(JJZ)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetExtendedRangeBrightness"),
       const_cast<char*>("(JJFF)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeSetExtendedRangeBrightness)},
      {const_cast<char*>("nativeSetTrustedOverlay"), const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
      {const_cast<char*>("nativeSetDropInputMode"), const_cast<char*>("(JJI)V"),
       reinterpret_cast<void*>(&SurfaceControlNativeTransactionNoop2)},
  };
  if (!Register(env, "android/view/SurfaceControl", surface_control_methods,
                static_cast<jint>(std::size(surface_control_methods)))) {
    return false;
  }

  JNINativeMethod blast_buffer_queue_methods[] = {
      {const_cast<char*>("nativeApplyPendingTransactions"),
       const_cast<char*>("(JJ)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeNoop2)},
      {const_cast<char*>("nativeClearSyncTransaction"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeNoop)},
      {const_cast<char*>("nativeCreate"),
       const_cast<char*>("(Ljava/lang/String;Z)J"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeCreate)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeDestroy)},
      {const_cast<char*>("nativeGatherPendingTransactions"),
       const_cast<char*>("(JJ)Landroid/view/SurfaceControl$Transaction;"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeGatherPendingTransactions)},
      {const_cast<char*>("nativeGetLastAcquiredFrameNum"),
       const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeGetLastAcquiredFrameNum)},
      {const_cast<char*>("nativeGetSurface"),
       const_cast<char*>("(JZ)Landroid/view/Surface;"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeGetSurface)},
      {const_cast<char*>("nativeIsSameSurfaceControl"),
       const_cast<char*>("(JJ)Z"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeIsSameSurfaceControl)},
      {const_cast<char*>("nativeMergeWithNextTransaction"),
       const_cast<char*>("(JJJ)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeNoop3)},
      {const_cast<char*>("nativeSetApplyToken"),
       const_cast<char*>("(JLandroid/os/IBinder;)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeSetApplyToken)},
      {const_cast<char*>("nativeSetTransactionHangCallback"),
       const_cast<char*>(
           "(JLandroid/graphics/BLASTBufferQueue$TransactionHangCallback;)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeSetHangCallback)},
      {const_cast<char*>("nativeSetWaitForBufferReleaseCallback"),
       const_cast<char*>(
           "(JLandroid/graphics/BLASTBufferQueue$WaitForBufferReleaseCallback;)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeSetHangCallback)},
      {const_cast<char*>("nativeStopContinuousSyncTransaction"),
       const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeNoop)},
      {const_cast<char*>("nativeSyncNextTransaction"),
       const_cast<char*>("(JLjava/util/function/Consumer;Z)Z"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeSyncNextTransaction)},
      {const_cast<char*>("nativeUpdate"), const_cast<char*>("(JJJJI)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeUpdate)},
  };
  if (!Register(env, "android/graphics/BLASTBufferQueue",
                blast_buffer_queue_methods,
                static_cast<jint>(std::size(blast_buffer_queue_methods)))) {
    return false;
  }

  JNINativeMethod surface_methods[] = {
      {const_cast<char*>("nativeCreateFromSurfaceTexture"),
       const_cast<char*>("(Landroid/graphics/SurfaceTexture;)J"),
       reinterpret_cast<void*>(&SurfaceNativeCreateFromSurfaceTexture)},
      {const_cast<char*>("nativeGetFromBlastBufferQueue"),
       const_cast<char*>("(JJ)J"),
       reinterpret_cast<void*>(&SurfaceNativeGetFromBlastBufferQueue)},
      {const_cast<char*>("nativeIsValid"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&SurfaceNativeIsValid)},
      {const_cast<char*>("nativeRelease"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceNativeRelease)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceNativeRelease)},
      {const_cast<char*>("nativeGetWidth"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&SurfaceNativeGetWidth)},
      {const_cast<char*>("nativeGetHeight"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&SurfaceNativeGetHeight)},
      {const_cast<char*>("nativeGetNextFrameNumber"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&SurfaceNativeGetNextFrameNumber)},
      {const_cast<char*>("nativeIsConsumerRunningBehind"),
       const_cast<char*>("(J)Z"), reinterpret_cast<void*>(&SurfaceNativeFalse)},
      {const_cast<char*>("nativeAllocateBuffers"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SurfaceNativeAllocateBuffers)},
      {const_cast<char*>("nativeSetScalingMode"), const_cast<char*>("(JI)I"),
       reinterpret_cast<void*>(&SurfaceNativeStatus)},
      {const_cast<char*>("nativeForceScopedDisconnect"),
       const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&SurfaceNativeForceDisconnect)},
      {const_cast<char*>("nativeSetSharedBufferModeEnabled"),
       const_cast<char*>("(JZ)I"),
       reinterpret_cast<void*>(&SurfaceNativeSetBoolean)},
      {const_cast<char*>("nativeSetAutoRefreshEnabled"),
       const_cast<char*>("(JZ)I"),
       reinterpret_cast<void*>(&SurfaceNativeSetBoolean)},
      {const_cast<char*>("nativeSetFrameRate"),
       const_cast<char*>("(JFII)I"),
       reinterpret_cast<void*>(&SurfaceNativeSetFrameRate)},
      {const_cast<char*>("nativeReadFromParcel"),
       const_cast<char*>("(JLandroid/os/Parcel;)J"),
       reinterpret_cast<void*>(&SurfaceNativeReadFromParcel)},
      {const_cast<char*>("nativeWriteToParcel"),
       const_cast<char*>("(JLandroid/os/Parcel;)V"),
       reinterpret_cast<void*>(&SurfaceNativeWriteToParcel)},
  };
  if (!Register(env, "android/view/Surface", surface_methods,
                static_cast<jint>(std::size(surface_methods)))) {
    return false;
  }
  if (!RegisterDarwinSurfaceTextureNatives(env)) {
    return false;
  }

  if (!darwin_art::RegisterDarwinAngleEglNatives(env)) {
    return false;
  }

  JNINativeMethod message_queue_methods[] = {
      {const_cast<char*>("nativeInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&message_queue_native_init)},
      {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_destroy)},
      {const_cast<char*>("nativePollOnce"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&message_queue_native_poll_once)},
      {const_cast<char*>("nativeWake"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&message_queue_native_wake)},
      {const_cast<char*>("nativeIsPolling"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&message_queue_native_is_polling)},
      {const_cast<char*>("nativeSetFileDescriptorEvents"),
       const_cast<char*>("(JII)V"),
       reinterpret_cast<void*>(&message_queue_native_set_file_descriptor_events)},
  };
  if (!Register(env, "android/os/MessageQueue", message_queue_methods,
                static_cast<jint>(std::size(message_queue_methods)))) {
    return false;
  }

  if (!RegisterFrameworkAnimationNatives(env)) {
    return false;
  }

  JNINativeMethod event_log_methods[] = {
      {const_cast<char*>("writeEvent"),
       const_cast<char*>("(I[Ljava/lang/Object;)I"),
       reinterpret_cast<void*>(&event_log_write_event)},
  };
  if (!Register(env, "android/util/EventLog", event_log_methods,
                static_cast<jint>(std::size(event_log_methods)))) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  JNINativeMethod log_methods[] = {
      {const_cast<char*>("isLoggable"),
       const_cast<char*>("(Ljava/lang/String;I)Z"),
       reinterpret_cast<void*>(&log_is_loggable)},
      {const_cast<char*>("println_native"),
       const_cast<char*>(
           "(IILjava/lang/String;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&log_println)},
  };
  if (!Register(env, "android/util/Log", log_methods,
                static_cast<jint>(std::size(log_methods)))) {
    return false;
  }
#endif

  JNINativeMethod trace_methods[] = {
      {const_cast<char*>("nativeIsTagEnabled"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&trace_is_tag_enabled)},
  };
  if (!Register(env, "android/os/Trace", trace_methods,
                static_cast<jint>(std::size(trace_methods)))) {
    return false;
  }

  JNINativeMethod system_clock_methods[] = {
      {const_cast<char*>("currentThreadTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_current_thread_time_millis)},
      {const_cast<char*>("elapsedRealtime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime)},
      {const_cast<char*>("elapsedRealtimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_elapsed_realtime_nanos)},
      {const_cast<char*>("uptimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_millis)},
      {const_cast<char*>("uptimeNanos"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&system_clock_uptime_nanos)},
  };
  if (!Register(env, "android/os/SystemClock", system_clock_methods,
                static_cast<jint>(std::size(system_clock_methods)))) {
    return false;
  }

  if (!RegisterFrameworkBinderNatives(env)) {
    return false;
  }

  if (!RegisterFrameworkSqliteNatives(env)) {
    return false;
  }

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  if (!RegisterFrameworkRenderNodeNatives(env)) {
    return false;
  }

  JNINativeMethod paint_methods[] = {
      {const_cast<char*>("nInit"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintInit)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&PaintGetNativeFinalizer)},
      {const_cast<char*>("nSetFlags"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetFlags)},
      {const_cast<char*>("nSetElegantTextHeight"),
       const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetElegantTextHeight)},
      {const_cast<char*>("nSetTextLocales"),
       const_cast<char*>("(JLjava/lang/String;)I"),
       reinterpret_cast<void*>(&PaintSetTextLocales)},
      {const_cast<char*>("nSetColor"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&PaintSetColor)},
  };
  if (!Register(env, "android/graphics/Paint", paint_methods,
                static_cast<jint>(std::size(paint_methods)))) {
    return false;
  }
#endif

#if !defined(DARWIN_ART_REAL_GRAPHICS)
  if (!RegisterFrameworkAssetManagerNatives(env)) {
    return false;
  }
#endif

  if (!RegisterFrameworkSystemPropertyNatives(env)) {
    return false;
  }

  return true;
}

}  // namespace darwin_art
