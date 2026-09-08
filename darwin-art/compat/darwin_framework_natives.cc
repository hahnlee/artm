#include "darwin_framework_natives.h"
#include "darwin_android_platform.h"
#include "darwin_android_surface_texture.h"
#include "darwin_audio_track.h"
#include "darwin_angle_egl.h"
#include "darwin_framework_system_natives.h"
#include "darwin_motion_event_natives.h"
#include "darwin_media_codec.h"
#include "darwin_media_extractor.h"
#include "darwin_security_trust.h"

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include <android/surface_control.h>
#include <android/hardware_buffer.h>
#include "../_aosp/system/libziparchive/include/ziparchive/zip_archive.h"

#include <fcntl.h>
#include <unistd.h>

extern "C" intptr_t darwin_art_bionic_pread(int, void*, size_t, int64_t);
extern "C" int darwin_art_bionic_socket_broker_close(int);
extern "C" int sync_wait(int, int);

namespace {

jbyteArray IncrementalFileSignature(JNIEnv*, jclass, jstring) {
  // Installed APKs live on ordinary macOS filesystems, not Linux IncFS.
  // Absence of an IncFS v4 signature is normal: the AOSP verifier then checks
  // an optional .idsig and proceeds to the APK's authenticated v3/v2 blocks.
  return nullptr;
}
jboolean IncrementalEnabled(JNIEnv*, jclass) { return JNI_FALSE; }
jboolean IncrementalFileDescriptor(JNIEnv*, jclass, jint) { return JNI_FALSE; }
jboolean IncrementalPath(JNIEnv*, jclass, jstring) { return JNI_FALSE; }

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

void SurfaceControlNativeSetTransparentRegionHint(JNIEnv* env, jclass,
                                                  jlong transaction,
                                                  jlong control,
                                                  jobject region) {
  if (env == nullptr) return;
  // RegionIterator exposes the exact SkRegion rectangles used by ViewRoot's
  // transparent-region hint. Keep the producer ABI bounded to eight rects;
  // an empty/null Region explicitly clears the prior hint.
  std::array<int32_t, 32> flattened{};
  size_t count = 0;
  if (region != nullptr) {
    jclass iterator_class =
        env->FindClass("android/graphics/RegionIterator");
    jclass rect_class = env->FindClass("android/graphics/Rect");
    jmethodID iterator_constructor =
        iterator_class == nullptr
            ? nullptr
            : env->GetMethodID(iterator_class, "<init>",
                               "(Landroid/graphics/Region;)V");
    jmethodID next =
        iterator_class == nullptr
            ? nullptr
            : env->GetMethodID(iterator_class, "next",
                               "(Landroid/graphics/Rect;)Z");
    jmethodID rect_constructor =
        rect_class == nullptr
            ? nullptr
            : env->GetMethodID(rect_class, "<init>", "()V");
    jfieldID left = rect_class == nullptr
                        ? nullptr
                        : env->GetFieldID(rect_class, "left", "I");
    jfieldID top = rect_class == nullptr
                       ? nullptr
                       : env->GetFieldID(rect_class, "top", "I");
    jfieldID right = rect_class == nullptr
                         ? nullptr
                         : env->GetFieldID(rect_class, "right", "I");
    jfieldID bottom = rect_class == nullptr
                          ? nullptr
                          : env->GetFieldID(rect_class, "bottom", "I");
    jobject iterator =
        iterator_constructor == nullptr
            ? nullptr
            : env->NewObject(iterator_class, iterator_constructor, region);
    jobject rect = rect_constructor == nullptr
                       ? nullptr
                       : env->NewObject(rect_class, rect_constructor);
    const bool valid = iterator != nullptr && rect != nullptr && next != nullptr &&
                       left != nullptr && top != nullptr && right != nullptr &&
                       bottom != nullptr && !env->ExceptionCheck();
    if (valid) {
      while (count < 8 && env->CallBooleanMethod(iterator, next, rect) == JNI_TRUE) {
        const int32_t rect_left = env->GetIntField(rect, left);
        const int32_t rect_top = env->GetIntField(rect, top);
        const int32_t rect_right = env->GetIntField(rect, right);
        const int32_t rect_bottom = env->GetIntField(rect, bottom);
        if (rect_right > rect_left && rect_bottom > rect_top) {
          flattened[count * 4 + 0] = rect_left;
          flattened[count * 4 + 1] = rect_top;
          flattened[count * 4 + 2] = rect_right;
          flattened[count * 4 + 3] = rect_bottom;
          ++count;
        }
      }
    }
    const bool failed = env->ExceptionCheck();
    if (iterator != nullptr) env->DeleteLocalRef(iterator);
    if (rect != nullptr) env->DeleteLocalRef(rect);
    if (iterator_class != nullptr) env->DeleteLocalRef(iterator_class);
    if (rect_class != nullptr) env->DeleteLocalRef(rect_class);
    if (failed) {
      env->ExceptionClear();
      return;
    }
  }
  darwin_art_android_surface_transaction_set_transparent_region_hint(
      reinterpret_cast<void*>(transaction), reinterpret_cast<void*>(control),
      flattened.data(), count);
}

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
  struct State {
    struct PendingTransaction {
      ASurfaceTransaction* transaction = nullptr;
      uint64_t frame = 0;
    };

    JavaVM* vm = nullptr;
    std::mutex mutex;
    void* native_window = nullptr;
    jobject sync_consumer = nullptr;
    bool continuous_sync = false;
    bool destroyed = false;
    // The continuous-sync accumulator is deliberately separate from the
    // frame-indexed merge list.  A mergeWithNextTransaction call is valid
    // even when no sync consumer is currently registered.
    ASurfaceTransaction* continuous_transaction = nullptr;
    uint64_t continuous_frame = 0;
    std::deque<PendingTransaction> future_transactions;
    // Once a transaction has been handed to the Java sync consumer, the
    // producer must not apply a later transaction ahead of its commit.  The
    // queue callback therefore takes ownership into this bounded FIFO until
    // the consumer transaction's commit/discard callback releases the gate.
    bool outstanding_sync = false;
    enum class SyncPhase : uint8_t { kIdle, kReserved, kGated, kDraining };
    SyncPhase sync_phase = SyncPhase::kIdle;
    uint64_t sync_generation = 0;
    std::deque<ASurfaceTransaction*> held_transactions;
    uint64_t last_acquired_frame = 0;
  };

  std::shared_ptr<State> state;
  void* native_window = nullptr;
  ASurfaceControl* surface_control = nullptr;
  jint width = 0;
  jint height = 0;
  jint format = 1;
};

struct BlastTransactionObserverContext {
  std::shared_ptr<DarwinBlastBufferQueue::State> state;
};

struct BlastTransactionGateContext {
  std::shared_ptr<DarwinBlastBufferQueue::State> state;
  uint64_t generation = 0;
};

constexpr size_t kMaxHeldBlastTransactions = 8;
std::atomic<uint32_t> g_blast_debug_events{0};

void BlastDebugTrace(const char* event, const void* state, const void* window,
                     const void* transaction, uint64_t value) {
  if (std::getenv("DARWIN_ART_DEBUG_BLAST") == nullptr &&
      std::getenv("DEBUG_BLAST") == nullptr) {
    return;
  }
  const uint32_t sequence =
      g_blast_debug_events.fetch_add(1, std::memory_order_relaxed);
  if (sequence >= 128) return;
  std::fprintf(stderr,
               "ART BLAST[%u] %s state=%p window=%p tx=%p value=%llu\n",
               sequence, event == nullptr ? "?" : event, state, window,
               transaction, static_cast<unsigned long long>(value));
}

void ReleaseBlastTransactionObserverContext(void* opaque) {
  delete static_cast<BlastTransactionObserverContext*>(opaque);
}

JNIEnv* AttachBlastThread(JavaVM* vm, bool* attached) {
  if (attached != nullptr) *attached = false;
  if (vm == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  if (attached != nullptr) *attached = true;
  return env;
}

void DeleteBlastConsumer(JNIEnv* env, jobject consumer) {
  if (env != nullptr && consumer != nullptr) env->DeleteGlobalRef(consumer);
}

void ApplyAndDeleteBlastTransaction(ASurfaceTransaction* transaction) {
  if (transaction == nullptr) return;
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
}

jobject NewBlastJavaTransaction(JNIEnv* env,
                                ASurfaceTransaction* transaction) {
  if (env == nullptr || transaction == nullptr || env->ExceptionCheck()) {
    return nullptr;
  }
  jclass transaction_class =
      env->FindClass("android/view/SurfaceControl$Transaction");
  jmethodID constructor =
      transaction_class == nullptr
          ? nullptr
          : env->GetMethodID(transaction_class, "<init>", "(J)V");
  jobject result =
      constructor == nullptr
          ? nullptr
          : env->NewObject(transaction_class, constructor,
                           reinterpret_cast<jlong>(transaction));
  if (transaction_class != nullptr) env->DeleteLocalRef(transaction_class);
  return result;
}

void DiscardHeldBlastTransactions(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    bool apply) {
  if (state == nullptr) return;
  std::deque<ASurfaceTransaction*> held;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    held.swap(state->held_transactions);
  }
  for (ASurfaceTransaction* transaction : held) {
    if (apply) {
      ApplyAndDeleteBlastTransaction(transaction);
    } else if (transaction != nullptr) {
      // Deletion invokes the platform discard callbacks, returning any
      // producer-held buffer without presenting a stale transaction.
      ASurfaceTransaction_delete(transaction);
    }
  }
}

void ReleaseReservedBlastGate(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    uint64_t generation, bool apply) {
  if (state == nullptr) return;
  std::deque<ASurfaceTransaction*> held;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->sync_generation != generation ||
        state->sync_phase == DarwinBlastBufferQueue::State::SyncPhase::kIdle) {
      return;
    }
    state->outstanding_sync = false;
    state->sync_phase = DarwinBlastBufferQueue::State::SyncPhase::kIdle;
    held.swap(state->held_transactions);
  }
  for (ASurfaceTransaction* transaction : held) {
    if (apply) {
      ApplyAndDeleteBlastTransaction(transaction);
    } else if (transaction != nullptr) {
      ASurfaceTransaction_delete(transaction);
    }
  }
}

void BlastTransactionGateFinished(void* opaque, bool apply) {
  auto* gate = static_cast<BlastTransactionGateContext*>(opaque);
  if (gate == nullptr) return;
  auto state = std::move(gate->state);
  const uint64_t generation = gate->generation;
  delete gate;
  if (state == nullptr) return;
  // Keep the generation gated while taking and applying each FIFO batch.
  // Queue callbacks may arrive during ApplyAndDelete and are admitted to the
  // same FIFO; only the locked empty check transitions back to idle.
  for (;;) {
    std::deque<ASurfaceTransaction*> batch;
    bool should_apply = apply;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->sync_generation != generation ||
          !state->outstanding_sync ||
          state->sync_phase ==
              DarwinBlastBufferQueue::State::SyncPhase::kIdle) {
        return;
      }
      if (state->destroyed) should_apply = false;
      if (state->held_transactions.empty()) {
        state->outstanding_sync = false;
        state->sync_phase =
            DarwinBlastBufferQueue::State::SyncPhase::kIdle;
        return;
      }
      state->sync_phase =
          DarwinBlastBufferQueue::State::SyncPhase::kDraining;
      batch.swap(state->held_transactions);
    }
    for (ASurfaceTransaction* transaction : batch) {
      if (should_apply) {
        ApplyAndDeleteBlastTransaction(transaction);
      } else if (transaction != nullptr) {
        ASurfaceTransaction_delete(transaction);
      }
    }
  }
}

void BlastTransactionGateOnCommit(void* opaque,
                                  ASurfaceTransactionStats*) {
  auto* gate = static_cast<BlastTransactionGateContext*>(opaque);
  BlastDebugTrace("gate-commit", gate == nullptr ? nullptr : gate->state.get(),
                  nullptr, nullptr,
                  gate == nullptr ? 0 : gate->generation);
  BlastTransactionGateFinished(opaque, true);
}

void BlastTransactionGateOnDiscard(void* opaque) {
  auto* gate = static_cast<BlastTransactionGateContext*>(opaque);
  BlastDebugTrace("gate-discard", gate == nullptr ? nullptr : gate->state.get(),
                  nullptr, nullptr,
                  gate == nullptr ? 0 : gate->generation);
  BlastTransactionGateFinished(opaque, false);
}

bool ArmBlastTransactionGate(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    ASurfaceTransaction* transaction, uint64_t generation) {
  if (state == nullptr || transaction == nullptr) return false;
  auto* gate = new (std::nothrow)
      BlastTransactionGateContext{state, generation};
  if (gate == nullptr) return false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->destroyed || !state->outstanding_sync ||
        state->sync_generation != generation ||
        state->sync_phase !=
            DarwinBlastBufferQueue::State::SyncPhase::kReserved) {
      delete gate;
      return false;
    }
    state->sync_phase = DarwinBlastBufferQueue::State::SyncPhase::kGated;
  }
  // The same context is installed on mutually-exclusive paths: the platform
  // clears discard callbacks before invoking commit, so exactly one callback
  // owns and destroys the gate context.
  ASurfaceTransaction_setOnCommit(transaction, gate,
                                  &BlastTransactionGateOnCommit);
  darwin_art_android_surface_transaction_set_on_discard(
      transaction, gate, &BlastTransactionGateOnDiscard);
  BlastDebugTrace("gate-arm", state.get(), nullptr, transaction, generation);
  return true;
}

std::vector<ASurfaceTransaction*> TakeDueBlastTransactions(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    uint64_t frame, bool all) {
  std::vector<ASurfaceTransaction*> result;
  if (state == nullptr) return result;
  std::lock_guard<std::mutex> lock(state->mutex);
  for (auto it = state->future_transactions.begin();
       it != state->future_transactions.end();) {
    if (all || it->frame == 0 || (frame != 0 && it->frame <= frame)) {
      result.push_back(it->transaction);
      it = state->future_transactions.erase(it);
    } else {
      ++it;
    }
  }
  return result;
}

void MergeBlastTransactions(ASurfaceTransaction* destination,
                            const std::vector<ASurfaceTransaction*>& sources) {
  if (destination == nullptr) {
    for (ASurfaceTransaction* source : sources) {
      if (source != nullptr) ASurfaceTransaction_delete(source);
    }
    return;
  }
  for (ASurfaceTransaction* source : sources) {
    if (source == nullptr) continue;
    darwin_art_android_surface_transaction_merge(destination, source);
    ASurfaceTransaction_delete(source);
  }
}

bool DeliverBlastTransaction(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    jobject consumer, ASurfaceTransaction* transaction,
    uint64_t generation) {
  if (transaction == nullptr) return false;
  if (state == nullptr) {
    // There is no callback owner left to consume this transaction. Returning
    // false leaves it to the native-window caller, which still owns it.
    return false;
  }
  if (consumer == nullptr) {
    ApplyAndDeleteBlastTransaction(transaction);
    return true;
  }
  bool attached = false;
  JNIEnv* env = AttachBlastThread(state->vm, &attached);
  if (env == nullptr) {
    // The sync reservation was made before JNI attach. Consume this callback's
    // transaction locally, then release any FIFO entries in order.
    DeleteBlastConsumer(nullptr, consumer);
    ApplyAndDeleteBlastTransaction(transaction);
    ReleaseReservedBlastGate(state, generation, true);
    return true;
  }
  BlastDebugTrace("consumer-invoke", state.get(), nullptr, transaction,
                  generation);
  jobject java_transaction = NewBlastJavaTransaction(env, transaction);
  jclass consumer_class =
      consumer == nullptr ? nullptr : env->GetObjectClass(consumer);
  jmethodID accept =
      consumer_class == nullptr
          ? nullptr
          : env->GetMethodID(consumer_class, "accept", "(Ljava/lang/Object;)V");
  const bool callable = java_transaction != nullptr && accept != nullptr &&
                        !env->ExceptionCheck();
  bool handed_to_java = false;
  if (callable) {
    // Arm before invoking Consumer.accept: accept may synchronously call
    // Transaction.apply(), and queue callbacks may arrive as soon as that
    // re-entrant call returns.
    if (ArmBlastTransactionGate(state, transaction, generation)) {
      handed_to_java = true;
      env->CallVoidMethod(consumer, accept, java_transaction);
    } else {
      // The Java wrapper owns the pointer, but no consumer saw it. Apply it
      // while the wrapper is still local and release the reservation; never
      // hand an ungated transaction to Java where later frames could pass it.
      ASurfaceTransaction_apply(transaction);
      ReleaseReservedBlastGate(state, generation, true);
    }
  }
  const bool exception = env->ExceptionCheck();
  BlastDebugTrace(exception ? "consumer-exception" : "consumer-return",
                  state.get(), nullptr, transaction, generation);
  if (exception) {
    // Once the Java Transaction has been passed to Consumer, Java owns the
    // native pointer even if Consumer closes it and then throws.  Never touch
    // the raw pointer on that path: it may already have been deleted.
    env->ExceptionClear();
    if (!handed_to_java && java_transaction == nullptr) {
      ApplyAndDeleteBlastTransaction(transaction);
    } else if (!handed_to_java) {
      // The wrapper exists but was not handed off (for example, accept could
      // not be resolved). It still owns the pointer, so apply only and let
      // its finalizer perform deletion.
      ASurfaceTransaction_apply(transaction);
    }
    if (!handed_to_java) ReleaseReservedBlastGate(state, generation, true);
  } else if (!callable) {
    if (java_transaction != nullptr) {
      ASurfaceTransaction_apply(transaction);
    } else {
      ApplyAndDeleteBlastTransaction(transaction);
    }
    ReleaseReservedBlastGate(state, generation, true);
  }
  if (consumer_class != nullptr) env->DeleteLocalRef(consumer_class);
  if (java_transaction != nullptr) env->DeleteLocalRef(java_transaction);
  DeleteBlastConsumer(env, consumer);
  if (attached) state->vm->DetachCurrentThread();
  // A Java Transaction constructed with the private native-pointer constructor
  // owns the transaction after construction. If it reached Consumer, its
  // finalizer (or Consumer.apply/close) is the only valid native owner; this
  // function deliberately never reuses the raw pointer on that path.
  return true;
}

bool BlastBufferQueueTransactionCallback(void* opaque, void* transaction,
                                         uint64_t frame_number) {
  auto* observer = static_cast<BlastTransactionObserverContext*>(opaque);
  if (observer == nullptr || observer->state == nullptr || transaction == nullptr)
    return false;
  const auto& state = observer->state;
  BlastDebugTrace("queue-callback", state.get(), nullptr, transaction,
                  frame_number);
  jobject consumer = nullptr;
  ASurfaceTransaction* deliver = nullptr;
  uint64_t generation = 0;
  std::vector<ASurfaceTransaction*> due;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_acquired_frame = std::max(state->last_acquired_frame,
                                          frame_number);
    if (state->destroyed) return false;
    for (auto it = state->future_transactions.begin();
         it != state->future_transactions.end();) {
      if (it->frame == 0 ||
          (frame_number != 0 && it->frame <= frame_number)) {
        due.push_back(it->transaction);
        it = state->future_transactions.erase(it);
      } else {
        ++it;
      }
    }
    auto* incoming = static_cast<ASurfaceTransaction*>(transaction);
    // Merge frame-indexed metadata into the actual queued buffer transaction
    // before any sync/continuous ownership decision.
    for (ASurfaceTransaction* source : due) {
      if (source != nullptr) {
        darwin_art_android_surface_transaction_merge(incoming, source);
        ASurfaceTransaction_delete(source);
      }
    }
    if (state->continuous_sync) {
      if (state->continuous_transaction == nullptr) {
        state->continuous_transaction = incoming;
        state->continuous_frame = frame_number;
      } else {
        darwin_art_android_surface_transaction_merge(
            state->continuous_transaction, incoming);
        ASurfaceTransaction_delete(incoming);
        state->continuous_frame =
            std::max(state->continuous_frame, frame_number);
      }
      return true;
    }
    // A reserved single-sync consumer must claim the first queued transaction
    // even though outstanding_sync is already true. That reservation closes
    // the race between SyncNextTransaction and JNI delivery.
    if (state->sync_consumer != nullptr) {
      consumer = state->sync_consumer;
      state->sync_consumer = nullptr;
      state->continuous_sync = false;
      generation = state->sync_generation;
      deliver = incoming;
    } else if (state->outstanding_sync) {
      // A commit gate is active even though the Java consumer slot has been
      // consumed. Preserve queue order until that transaction commits.
      if (state->held_transactions.size() < kMaxHeldBlastTransactions) {
        state->held_transactions.push_back(incoming);
        return true;
      }
      // Do not let a bounded-queue overflow violate the commit gate. The
      // incoming transaction is still owned by this callback, so discard it
      // (and return its producer buffer through the platform lifecycle hook)
      // rather than applying it ahead of the outstanding Java transaction.
      ASurfaceTransaction_delete(incoming);
      return true;
    }
  }
  // Consumer invocation may re-enter framework code and must never run under
  // the state mutex. The callback has claimed transaction ownership here.
  return DeliverBlastTransaction(state, consumer, deliver, generation);
}

void DiscardPendingBlastTransaction(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state,
    JNIEnv* env) {
  if (state == nullptr) return;
  jobject consumer = nullptr;
  ASurfaceTransaction* continuous = nullptr;
  std::deque<DarwinBlastBufferQueue::State::PendingTransaction> future;
  std::deque<ASurfaceTransaction*> held;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    consumer = state->sync_consumer;
    state->sync_consumer = nullptr;
    state->continuous_sync = false;
    // A consumer still present here has not been claimed by the queue
    // callback, so clear can safely cancel its reservation. If it is already
    // null, a delivery is in flight and its generation owns the reservation.
    if (consumer != nullptr &&
        state->sync_phase ==
            DarwinBlastBufferQueue::State::SyncPhase::kReserved) {
      state->outstanding_sync = false;
      state->sync_phase =
          DarwinBlastBufferQueue::State::SyncPhase::kIdle;
      ++state->sync_generation;
      held.swap(state->held_transactions);
    }
    continuous = state->continuous_transaction;
    state->continuous_transaction = nullptr;
    state->continuous_frame = 0;
    future.swap(state->future_transactions);
  }
  DeleteBlastConsumer(env, consumer);
  if (continuous != nullptr) ASurfaceTransaction_delete(continuous);
  for (const auto& pending : future) {
    if (pending.transaction != nullptr) {
      ASurfaceTransaction_delete(pending.transaction);
    }
  }
  for (ASurfaceTransaction* transaction : held) {
    if (transaction != nullptr) ASurfaceTransaction_delete(transaction);
  }
}

void DispatchContinuousBlastTransaction(
    const std::shared_ptr<DarwinBlastBufferQueue::State>& state, JNIEnv* env) {
  if (state == nullptr) return;
  jobject consumer = nullptr;
  ASurfaceTransaction* pending = nullptr;
  uint64_t generation = 0;
  std::vector<ASurfaceTransaction*> future;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->sync_consumer == nullptr || !state->continuous_sync) return;
    consumer = state->sync_consumer;
    generation = state->sync_generation;
    state->sync_consumer = nullptr;
    state->continuous_sync = false;
    pending = state->continuous_transaction;
    state->continuous_transaction = nullptr;
    state->continuous_frame = 0;
    for (const auto& entry : state->future_transactions) {
      if (entry.transaction != nullptr) future.push_back(entry.transaction);
    }
    state->future_transactions.clear();
  }
  if (pending == nullptr) pending = ASurfaceTransaction_create();
  // The native stop call is made by the Java/UI thread, but delivery is kept
  // outside the queue mutex for the same re-entry guarantee as frame delivery.
  if (pending == nullptr) {
    DeleteBlastConsumer(env, consumer);
    for (ASurfaceTransaction* transaction : future) {
      if (transaction != nullptr) ASurfaceTransaction_delete(transaction);
    }
    return;
  }
  MergeBlastTransactions(pending, future);
  if (!DeliverBlastTransaction(state, consumer, pending, generation)) {
    // Stop is not itself an ANativeWindow callback, so there is no caller to
    // perform the normal-apply fallback when JNI delivery cannot attach.
    ApplyAndDeleteBlastTransaction(pending);
  }
}

jlong BlastBufferQueueNativeCreate(JNIEnv* env, jclass, jstring, jboolean) {
  auto* queue = new (std::nothrow) DarwinBlastBufferQueue();
  if (queue == nullptr) return 0;
  try {
    queue->state = std::make_shared<DarwinBlastBufferQueue::State>();
  } catch (const std::bad_alloc&) {
    delete queue;
    return 0;
  }
  if (queue->state == nullptr || env == nullptr ||
      env->GetJavaVM(&queue->state->vm) != JNI_OK) {
    delete queue;
    return 0;
  }
  return reinterpret_cast<jlong>(queue);
}
void BlastBufferQueueNativeNoop(JNIEnv*, jclass, jlong) {}
void BlastBufferQueueNativeNoop2(JNIEnv*, jclass, jlong, jlong) {}
void BlastBufferQueueNativeNoop3(JNIEnv*, jclass, jlong, jlong, jlong) {}

struct ImageReaderFields {
  jfieldID context = nullptr;
  jfieldID image_buffer = nullptr;
  jfieldID image_timestamp = nullptr;
  jfieldID image_dataspace = nullptr;
  jfieldID image_transform = nullptr;
  jfieldID image_scaling_mode = nullptr;
  jclass reader_class = nullptr;
  jmethodID post_event = nullptr;
};
ImageReaderFields g_image_reader_fields;

struct ImageReaderPendingBuffer {
  AHardwareBuffer* buffer = nullptr;
  int32_t slot = -1;
  int fence = -1;
  int32_t dataspace = 0;
  int64_t timestamp_ns = 0;
};

struct DarwinImageReader {
  std::atomic<uint32_t> references{1};
  JavaVM* vm = nullptr;
  std::mutex mutex;
  void* producer = nullptr;
  jobject weak_self = nullptr;
  std::deque<ImageReaderPendingBuffer> pending;
  uint32_t acquired = 0;
  uint32_t max_images = 1;
  bool closed = false;
};

struct DarwinSurfaceImage {
  AHardwareBuffer* buffer = nullptr;
  void* producer = nullptr;
  int32_t slot = -1;
  int fence = -1;
  int32_t dataspace = 0;
  int64_t timestamp_ns = 0;
  DarwinImageReader* reader = nullptr;
};

void ReleaseImageReaderPending(void* producer,
                               ImageReaderPendingBuffer* pending) {
  if (pending == nullptr) return;
  if (producer != nullptr && pending->slot >= 0) {
    darwin_art_android_ANativeWindow_release_consumer_slot(
        producer, pending->slot, -1);
  }
  if (pending->fence >= 0)
    (void)darwin_art_bionic_socket_broker_close(pending->fence);
  if (pending->buffer != nullptr) AHardwareBuffer_release(pending->buffer);
  *pending = ImageReaderPendingBuffer{};
}

void ReleaseImageReader(DarwinImageReader* reader) {
  if (reader == nullptr ||
      reader->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  for (auto& pending : reader->pending)
    ReleaseImageReaderPending(reader->producer, &pending);
  if (reader->weak_self != nullptr) {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (reader->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) !=
        JNI_OK) {
      attached = reader->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    }
    if (env != nullptr) env->DeleteGlobalRef(reader->weak_self);
    if (attached) reader->vm->DetachCurrentThread();
  }
  if (reader->producer != nullptr)
    darwin_art_android_ANativeWindow_release(reader->producer);
  delete reader;
}

void ReleaseImageReaderCallbackContext(void* context) {
  ReleaseImageReader(static_cast<DarwinImageReader*>(context));
}

void ImageReaderQueueBuffer(void* context, AHardwareBuffer* buffer,
                            int32_t slot, int fence, int32_t dataspace) {
  auto* reader = static_cast<DarwinImageReader*>(context);
  if (reader == nullptr || buffer == nullptr) {
    if (fence >= 0) (void)darwin_art_bionic_socket_broker_close(fence);
    return;
  }
  AHardwareBuffer_acquire(buffer);
  ImageReaderPendingBuffer dropped;
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(reader->mutex);
    if (reader->closed) {
      dropped = {.buffer = buffer, .slot = slot, .fence = fence};
    } else {
      if (reader->pending.size() >= reader->max_images) {
        dropped = reader->pending.front();
        reader->pending.pop_front();
      }
      reader->pending.push_back({
          .buffer = buffer,
          .slot = slot,
          .fence = fence,
          .dataspace = dataspace,
          .timestamp_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count(),
      });
      notify = true;
    }
  }
  ReleaseImageReaderPending(reader->producer, &dropped);
  if (!notify || reader->weak_self == nullptr ||
      g_image_reader_fields.reader_class == nullptr ||
      g_image_reader_fields.post_event == nullptr) {
    return;
  }
  JNIEnv* env = nullptr;
  bool attached = false;
  if (reader->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) !=
      JNI_OK) {
    attached = reader->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
  }
  if (env != nullptr) {
    env->CallStaticVoidMethod(g_image_reader_fields.reader_class,
                              g_image_reader_fields.post_event,
                              reader->weak_self);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) reader->vm->DetachCurrentThread();
}

DarwinImageReader* GetImageReader(JNIEnv* env, jobject object) {
  return g_image_reader_fields.context == nullptr
             ? nullptr
             : reinterpret_cast<DarwinImageReader*>(static_cast<uintptr_t>(
                   env->GetLongField(object, g_image_reader_fields.context)));
}

void ImageReaderNativeClassInit(JNIEnv* env, jclass reader_class) {
  g_image_reader_fields.context =
      env->GetFieldID(reader_class, "mNativeContext", "J");
  g_image_reader_fields.reader_class =
      static_cast<jclass>(env->NewGlobalRef(reader_class));
  g_image_reader_fields.post_event = env->GetStaticMethodID(
      reader_class, "postEventFromNative", "(Ljava/lang/Object;)V");
  jclass image_class =
      env->FindClass("android/media/ImageReader$SurfaceImage");
  if (image_class == nullptr) return;
  g_image_reader_fields.image_buffer =
      env->GetFieldID(image_class, "mNativeBuffer", "J");
  g_image_reader_fields.image_timestamp =
      env->GetFieldID(image_class, "mTimestamp", "J");
  g_image_reader_fields.image_dataspace =
      env->GetFieldID(image_class, "mDataSpace", "I");
  g_image_reader_fields.image_transform =
      env->GetFieldID(image_class, "mTransform", "I");
  g_image_reader_fields.image_scaling_mode =
      env->GetFieldID(image_class, "mScalingMode", "I");
  env->DeleteLocalRef(image_class);
}

void ImageReaderNativeInit(JNIEnv* env, jobject object, jobject weak_self,
                           jint width, jint height, jint max_images, jlong,
                           jint format, jint) {
  auto* reader = new (std::nothrow) DarwinImageReader();
  if (reader == nullptr) return;
  env->GetJavaVM(&reader->vm);
  reader->max_images = static_cast<uint32_t>(std::max(1, max_images));
  reader->weak_self = env->NewGlobalRef(weak_self);
  reader->producer =
      darwin_art_android_ANativeWindow_create(width, height, format);
  if (reader->producer == nullptr || reader->weak_self == nullptr) {
    ReleaseImageReader(reader);
    return;
  }
  reader->references.fetch_add(1, std::memory_order_relaxed);
  if (!darwin_art_android_ANativeWindow_set_owned_queue_callback(
          reader->producer, &ImageReaderQueueBuffer, reader,
          &ReleaseImageReaderCallbackContext)) {
    reader->references.fetch_sub(1, std::memory_order_relaxed);
    ReleaseImageReader(reader);
    return;
  }
  env->SetLongField(object, g_image_reader_fields.context,
                    reinterpret_cast<jlong>(reader));
}

void ImageReaderNativeClose(JNIEnv* env, jobject object) {
  DarwinImageReader* reader = GetImageReader(env, object);
  if (reader == nullptr) return;
  env->SetLongField(object, g_image_reader_fields.context, 0);
  std::deque<ImageReaderPendingBuffer> pending;
  {
    std::lock_guard<std::mutex> lock(reader->mutex);
    reader->closed = true;
    pending.swap(reader->pending);
  }
  darwin_art_android_ANativeWindow_set_owned_queue_callback(
      reader->producer, nullptr, nullptr, nullptr);
  for (auto& item : pending)
    ReleaseImageReaderPending(reader->producer, &item);
  ReleaseImageReader(reader);
}

jobject ImageReaderNativeGetSurface(JNIEnv* env, jobject object) {
  DarwinImageReader* reader = GetImageReader(env, object);
  if (reader == nullptr) return nullptr;
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
  if (surface != nullptr && native_object != nullptr) {
    darwin_art_android_ANativeWindow_acquire(reader->producer);
    env->SetLongField(surface, native_object,
                      reinterpret_cast<jlong>(reader->producer));
  }
  if (surface_class != nullptr) env->DeleteLocalRef(surface_class);
  return surface;
}

jint ImageReaderNativeImageSetup(JNIEnv* env, jobject object, jobject image) {
  DarwinImageReader* reader = GetImageReader(env, object);
  if (reader == nullptr) return 1;
  ImageReaderPendingBuffer pending;
  {
    std::lock_guard<std::mutex> lock(reader->mutex);
    if (reader->acquired >= reader->max_images) return 2;
    if (reader->pending.empty()) return 1;
    pending = reader->pending.front();
    reader->pending.pop_front();
    ++reader->acquired;
  }
  auto* native_image = new (std::nothrow) DarwinSurfaceImage{
      .buffer = pending.buffer,
      .producer = reader->producer,
      .slot = pending.slot,
      .fence = pending.fence,
      .dataspace = pending.dataspace,
      .timestamp_ns = pending.timestamp_ns,
      .reader = reader,
  };
  if (native_image == nullptr) {
    ReleaseImageReaderPending(reader->producer, &pending);
    std::lock_guard<std::mutex> lock(reader->mutex);
    --reader->acquired;
    return 1;
  }
  reader->references.fetch_add(1, std::memory_order_relaxed);
  darwin_art_android_ANativeWindow_acquire(reader->producer);
  env->SetLongField(image, g_image_reader_fields.image_buffer,
                    reinterpret_cast<jlong>(native_image));
  env->SetLongField(image, g_image_reader_fields.image_timestamp,
                    pending.timestamp_ns);
  env->SetIntField(image, g_image_reader_fields.image_dataspace,
                   pending.dataspace);
  env->SetIntField(image, g_image_reader_fields.image_transform, 0);
  env->SetIntField(image, g_image_reader_fields.image_scaling_mode, 0);
  return 0;
}

DarwinSurfaceImage* GetSurfaceImage(JNIEnv* env, jobject image) {
  return g_image_reader_fields.image_buffer == nullptr
             ? nullptr
             : reinterpret_cast<DarwinSurfaceImage*>(static_cast<uintptr_t>(
                   env->GetLongField(image,
                                     g_image_reader_fields.image_buffer)));
}

void ImageReaderNativeReleaseImage(JNIEnv* env, jobject, jobject image) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  if (native_image == nullptr) return;
  env->SetLongField(image, g_image_reader_fields.image_buffer, 0);
  darwin_art_android_ANativeWindow_release_consumer_slot(
      native_image->producer, native_image->slot, -1);
  if (native_image->fence >= 0)
    (void)darwin_art_bionic_socket_broker_close(native_image->fence);
  AHardwareBuffer_release(native_image->buffer);
  {
    std::lock_guard<std::mutex> lock(native_image->reader->mutex);
    if (native_image->reader->acquired > 0) --native_image->reader->acquired;
  }
  darwin_art_android_ANativeWindow_release(native_image->producer);
  ReleaseImageReader(native_image->reader);
  delete native_image;
}

void ImageReaderNativeDiscardFreeBuffers(JNIEnv*, jobject object) {
  // The producer's fixed three-slot pool is reclaimed as each queued consumer
  // slot is released. There is no separate gralloc cache to discard on Darwin.
  (void)object;
}

jint ImageReaderNativeDetachImage(JNIEnv*, jobject, jobject, jboolean) {
  // Detaching transfers GraphicBuffer ownership outside the reader. The Java
  // HardwareBuffer path used by Chromium does not detach; report unsupported
  // without corrupting the acquired slot's ownership.
  return -1;
}

jobjectArray ImageReaderNativeCreateImagePlanes(JNIEnv* env, jclass,
                                                 jint count, jobject, jint,
                                                 jint, jint, jint, jint,
                                                 jint) {
  jclass plane = env->FindClass("android/media/ImageReader$ImagePlane");
  jobjectArray result = plane == nullptr
                            ? nullptr
                            : env->NewObjectArray(std::max(0, count), plane,
                                                  nullptr);
  if (plane != nullptr) env->DeleteLocalRef(plane);
  return result;
}

void ImageReaderNativeUnlockGraphicBuffer(JNIEnv*, jclass, jobject) {}

jobjectArray SurfaceImageNativeCreatePlanes(JNIEnv* env, jobject, jint count,
                                             jint, jlong) {
  jclass plane = env->FindClass(
      "android/media/ImageReader$SurfaceImage$SurfacePlane");
  jobjectArray result = plane == nullptr
                            ? nullptr
                            : env->NewObjectArray(std::max(0, count), plane,
                                                  nullptr);
  if (plane != nullptr) env->DeleteLocalRef(plane);
  return result;
}

jint SurfaceImageNativeGetWidth(JNIEnv* env, jobject image) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  if (native_image == nullptr) return 0;
  AHardwareBuffer_Desc desc{};
  AHardwareBuffer_describe(native_image->buffer, &desc);
  return static_cast<jint>(desc.width);
}
jint SurfaceImageNativeGetHeight(JNIEnv* env, jobject image) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  if (native_image == nullptr) return 0;
  AHardwareBuffer_Desc desc{};
  AHardwareBuffer_describe(native_image->buffer, &desc);
  return static_cast<jint>(desc.height);
}
jint SurfaceImageNativeGetFormat(JNIEnv* env, jobject image, jint reader_format) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  if (native_image == nullptr) return reader_format;
  AHardwareBuffer_Desc desc{};
  AHardwareBuffer_describe(native_image->buffer, &desc);
  return static_cast<jint>(desc.format);
}
jint SurfaceImageNativeGetFenceFd(JNIEnv* env, jobject image) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  return native_image == nullptr ? -1 : native_image->fence;
}
void HardwareBufferFinalizer(void* opaque) {
  AHardwareBuffer_release(static_cast<AHardwareBuffer*>(opaque));
}
jlong HardwareBufferNativeCreate(JNIEnv*, jclass, jint width, jint height,
                                 jint format, jint layers, jlong usage) {
  AHardwareBuffer_Desc desc{
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .layers = static_cast<uint32_t>(layers),
      .format = static_cast<uint32_t>(format),
      .usage = static_cast<uint64_t>(usage),
      .stride = 0,
      .rfu0 = 0,
      .rfu1 = 0,
  };
  AHardwareBuffer* buffer = nullptr;
  return AHardwareBuffer_allocate(&desc, &buffer) == 0
             ? reinterpret_cast<jlong>(buffer)
             : 0;
}
jlong HardwareBufferNativeCreateFromGraphicBuffer(JNIEnv*, jclass, jobject) {
  return 0;
}
jlong HardwareBufferNativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&HardwareBufferFinalizer);
}
void HardwareBufferNativeWriteToParcel(JNIEnv*, jclass, jlong, jobject) {}
jlong HardwareBufferNativeReadFromParcel(JNIEnv*, jclass, jobject) { return 0; }
jboolean HardwareBufferNativeIsSupported(JNIEnv*, jclass, jint width,
                                         jint height, jint format,
                                         jint layers, jlong usage) {
  AHardwareBuffer_Desc desc{
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .layers = static_cast<uint32_t>(layers),
      .format = static_cast<uint32_t>(format),
      .usage = static_cast<uint64_t>(usage),
  };
  return AHardwareBuffer_isSupported(&desc) ? JNI_TRUE : JNI_FALSE;
}
AHardwareBuffer_Desc DescribeHardwareBuffer(jlong handle) {
  AHardwareBuffer_Desc desc{};
  if (handle != 0) AHardwareBuffer_describe(
      reinterpret_cast<AHardwareBuffer*>(static_cast<uintptr_t>(handle)),
      &desc);
  return desc;
}
jint HardwareBufferNativeGetWidth(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(DescribeHardwareBuffer(handle).width);
}
jint HardwareBufferNativeGetHeight(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(DescribeHardwareBuffer(handle).height);
}
jint HardwareBufferNativeGetFormat(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(DescribeHardwareBuffer(handle).format);
}
jint HardwareBufferNativeGetLayers(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(DescribeHardwareBuffer(handle).layers);
}
jlong HardwareBufferNativeGetUsage(JNIEnv*, jclass, jlong handle) {
  return static_cast<jlong>(DescribeHardwareBuffer(handle).usage);
}
jlong HardwareBufferNativeEstimateSize(jlong handle) {
  const AHardwareBuffer_Desc desc = DescribeHardwareBuffer(handle);
  uint32_t bytes_per_pixel = 4;
  if (desc.format == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM) bytes_per_pixel = 2;
  return static_cast<jlong>(desc.height) *
         static_cast<jlong>(desc.stride == 0 ? desc.width : desc.stride) *
         bytes_per_pixel * std::max<uint32_t>(1, desc.layers);
}
jlong HardwareBufferNativeGetId(jlong handle) { return handle; }

jobject SurfaceImageNativeGetHardwareBuffer(JNIEnv* env, jobject image) {
  DarwinSurfaceImage* native_image = GetSurfaceImage(env, image);
  if (native_image == nullptr || native_image->buffer == nullptr) return nullptr;
  jclass clazz = env->FindClass("android/hardware/HardwareBuffer");
  jmethodID constructor = clazz == nullptr
                              ? nullptr
                              : env->GetMethodID(clazz, "<init>", "(J)V");
  if (constructor == nullptr) {
    if (clazz != nullptr) env->DeleteLocalRef(clazz);
    return nullptr;
  }
  AHardwareBuffer_acquire(native_image->buffer);
  jobject result = env->NewObject(
      clazz, constructor, reinterpret_cast<jlong>(native_image->buffer));
  if (result == nullptr) AHardwareBuffer_release(native_image->buffer);
  env->DeleteLocalRef(clazz);
  return result;
}

struct DarwinSyncFence {
  std::atomic<uint32_t> references{1};
  int fd = -1;
};
void SyncFenceFinalizer(void* opaque) {
  auto* fence = static_cast<DarwinSyncFence*>(opaque);
  if (fence == nullptr ||
      fence->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  if (fence->fd >= 0) (void)darwin_art_bionic_socket_broker_close(fence->fd);
  delete fence;
}
jlong SyncFenceNativeGetDestructor(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&SyncFenceFinalizer);
}
jlong SyncFenceNativeCreate(JNIEnv*, jclass, jint fd) {
  if (fd < 0) return 0;
  auto* fence = new (std::nothrow) DarwinSyncFence();
  if (fence == nullptr) {
    (void)darwin_art_bionic_socket_broker_close(fd);
    return 0;
  }
  fence->fd = fd;
  return reinterpret_cast<jlong>(fence);
}
jboolean SyncFenceNativeIsValid(JNIEnv*, jclass, jlong handle) {
  const auto* fence = reinterpret_cast<const DarwinSyncFence*>(
      static_cast<uintptr_t>(handle));
  return fence != nullptr && fence->fd >= 0 ? JNI_TRUE : JNI_FALSE;
}
jint SyncFenceNativeGetFd(JNIEnv*, jclass, jlong handle) {
  const auto* fence = reinterpret_cast<const DarwinSyncFence*>(
      static_cast<uintptr_t>(handle));
  return fence == nullptr ? -1 : fence->fd;
}
jboolean SyncFenceNativeWait(JNIEnv*, jclass, jlong handle,
                             jlong timeout_nanos) {
  const auto* fence = reinterpret_cast<const DarwinSyncFence*>(
      static_cast<uintptr_t>(handle));
  if (fence == nullptr || fence->fd < 0) return JNI_TRUE;
  const int timeout_millis =
      timeout_nanos < 0
          ? -1
          : static_cast<int>(std::min<jlong>(
                std::numeric_limits<int>::max(),
                (timeout_nanos + 999999) / 1000000));
  return sync_wait(fence->fd, timeout_millis) == 0 ? JNI_TRUE : JNI_FALSE;
}
jlong SyncFenceNativeGetSignalTime(JNIEnv*, jclass, jlong handle) {
  const auto* fence = reinterpret_cast<const DarwinSyncFence*>(
      static_cast<uintptr_t>(handle));
  if (fence == nullptr || fence->fd < 0) return -1;
  if (sync_wait(fence->fd, 0) != 0) return std::numeric_limits<jlong>::max();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void SyncFenceNativeIncRef(JNIEnv*, jclass, jlong handle) {
  auto* fence = reinterpret_cast<DarwinSyncFence*>(
      static_cast<uintptr_t>(handle));
  if (fence != nullptr)
    fence->references.fetch_add(1, std::memory_order_relaxed);
}
jint PublicFormatNativeGetHalFormat(JNIEnv*, jclass, jint format) {
  // frameworks/native/libs/ui/PublicFormat.cpp maps the encoded formats to
  // their transport formats. Most public/HAL enums are intentionally 1:1.
  switch (format) {
    case 0x100:       // JPEG
    case 0x101:       // DEPTH_POINT_CLOUD
    case 0x69656963:  // DEPTH_JPEG
    case 0x48454946:  // HEIC
    case 0x1005:      // JPEG_R
    case 0x1006:      // HEIC_ULTRAHDR
      return 0x21;    // HAL_PIXEL_FORMAT_BLOB
    case 0x44363159:  // DEPTH16
      return 0x20363159;  // HAL_PIXEL_FORMAT_Y16
    case 0x20:        // RAW_SENSOR
    case 0x1002:      // RAW_DEPTH
      return 0x20;    // HAL_PIXEL_FORMAT_RAW16
    case 0x1003:      // RAW_DEPTH10
      return 0x25;    // HAL_PIXEL_FORMAT_RAW10
    default:
      return format;
  }
}
jint PublicFormatNativeGetHalDataspace(JNIEnv*, jclass, jint format) {
  // Exact Android dataspace constants from system/graphics.h. Chrome's RGBA
  // snapshot path uses UNKNOWN; retain the complete non-default mappings so
  // other framework ImageReader clients observe AOSP's contract as well.
  switch (format) {
    case 0x100:       // JPEG
    case 0x23:        // YUV_420_888
    case 0x11:        // NV21
    case 0x32315659:  // YV12
      return 0x101;   // HAL_DATASPACE_V0_JFIF
    case 0x101:       // DEPTH_POINT_CLOUD
    case 0x44363159:  // DEPTH16
    case 0x1002:      // RAW_DEPTH
    case 0x1003:      // RAW_DEPTH10
      return 0x1000;  // HAL_DATASPACE_DEPTH
    case 0x69656963:  // DEPTH_JPEG
      return 0x1002;  // HAL_DATASPACE_DYNAMIC_DEPTH
    default:
      return 0;
  }
}
jint PublicFormatNativeGetPublicFormat(JNIEnv*, jclass, jint format,
                                       jint dataspace) {
  if (format == 0x21) {  // HAL_PIXEL_FORMAT_BLOB
    if (dataspace == 0x1000) return 0x101;
    if (dataspace == 0x1002) return 0x69656963;
    return 0x100;
  }
  if (format == 0x20) return dataspace == 0x1000 ? 0x1002 : 0x20;
  if (format == 0x25) return dataspace == 0x1000 ? 0x1003 : 0x25;
  if (format == 0x20363159)
    return dataspace == 0x1000 ? 0x44363159 : 0x20363159;
  return format;
}
void BlastBufferQueueNativeDestroy(JNIEnv* env, jclass, jlong handle) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr) return;
  auto state = queue->state;
  void* native_window = queue->native_window;
  if (state != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->destroyed = true;
      state->native_window = nullptr;
      state->outstanding_sync = false;
      state->sync_phase =
          DarwinBlastBufferQueue::State::SyncPhase::kIdle;
      ++state->sync_generation;
    }
    if (native_window != nullptr) {
      darwin_art_android_ANativeWindow_set_transaction_callback(
          native_window, nullptr, nullptr, nullptr);
    }
    DiscardPendingBlastTransaction(state, env);
    // Transactions held behind a Java commit gate cannot be presented after
    // queue destruction. Delete them after unregistering the observer so the
    // platform discard callbacks return their producer slots safely.
    DiscardHeldBlastTransactions(state, false);
  }
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
  if (queue->state != nullptr) {
    std::lock_guard<std::mutex> lock(queue->state->mutex);
    if (queue->state->native_window == nullptr) {
      auto* context = new (std::nothrow) BlastTransactionObserverContext{
          queue->state};
      if (context != nullptr &&
          darwin_art_android_ANativeWindow_set_transaction_callback(
              queue->native_window, &BlastBufferQueueTransactionCallback,
              context, &ReleaseBlastTransactionObserverContext)) {
        queue->state->native_window = queue->native_window;
        BlastDebugTrace("observer-registered", queue->state.get(),
                        queue->native_window, nullptr, 1);
      } else {
        BlastDebugTrace("observer-register-failed", queue->state.get(),
                        queue->native_window, nullptr, 0);
        delete context;
      }
    }
  }
}
jlong BlastBufferQueueNativeGetLastAcquiredFrameNum(JNIEnv*, jclass,
                                                   jlong handle) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr || queue->state == nullptr) return 0;
  std::lock_guard<std::mutex> lock(queue->state->mutex);
  return static_cast<jlong>(queue->state->last_acquired_frame);
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
                                                         jlong handle,
                                                         jlong frame) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  std::shared_ptr<DarwinBlastBufferQueue::State> state =
      queue == nullptr ? nullptr : queue->state;
  const auto future = TakeDueBlastTransactions(
      state, frame <= 0 ? 0 : static_cast<uint64_t>(frame), frame <= 0);
  ASurfaceTransaction* pending = ASurfaceTransaction_create();
  MergeBlastTransactions(pending, future);
  if (pending == nullptr) pending = ASurfaceTransaction_create();
  jobject result = NewBlastJavaTransaction(env, pending);
  if (result == nullptr) ApplyAndDeleteBlastTransaction(pending);
  return result;
}
void BlastBufferQueueNativeSetApplyToken(JNIEnv*, jclass, jlong, jobject) {}
void BlastBufferQueueNativeSetHangCallback(JNIEnv*, jclass, jlong, jobject) {}

void BlastBufferQueueNativeMergeWithNextTransaction(JNIEnv*, jclass,
                                                    jlong handle,
                                                    jlong transaction,
                                                    jlong frame) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  auto* source = reinterpret_cast<ASurfaceTransaction*>(transaction);
  if (queue == nullptr || queue->state == nullptr || source == nullptr) return;
  ASurfaceTransaction* apply_now = nullptr;
  const uint64_t target_frame = frame <= 0 ? 0 : static_cast<uint64_t>(frame);
  {
    std::lock_guard<std::mutex> lock(queue->state->mutex);
    if (queue->state->destroyed) {
      return;
    } else if (frame > 0 && static_cast<uint64_t>(frame) <=
                         queue->state->last_acquired_frame) {
      apply_now = source;
    } else {
      ASurfaceTransaction* owned = ASurfaceTransaction_create();
      if (owned != nullptr) {
        darwin_art_android_surface_transaction_merge(owned, source);
        queue->state->future_transactions.push_back(
            {owned, target_frame});
      }
    }
  }
  if (apply_now != nullptr) ASurfaceTransaction_apply(apply_now);
}

void BlastBufferQueueNativeClearSyncTransaction(JNIEnv* env, jclass,
                                                jlong handle) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr) return;
  DiscardPendingBlastTransaction(queue->state, env);
}

void BlastBufferQueueNativeStopContinuousSyncTransaction(JNIEnv* env, jclass,
                                                         jlong handle) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr) return;
  DispatchContinuousBlastTransaction(queue->state, env);
}

void BlastBufferQueueNativeApplyPendingTransactions(JNIEnv*, jclass,
                                                    jlong handle, jlong frame) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr || queue->state == nullptr) return;
  const auto future = TakeDueBlastTransactions(
      queue->state, frame <= 0 ? 0 : static_cast<uint64_t>(frame), frame <= 0);
  ASurfaceTransaction* pending = ASurfaceTransaction_create();
  MergeBlastTransactions(pending, future);
  ApplyAndDeleteBlastTransaction(pending);
}

jboolean BlastBufferQueueNativeSyncNextTransaction(JNIEnv* env, jclass,
                                                   jlong handle,
                                                   jobject consumer,
                                                   jboolean acquire_single) {
  auto* queue = reinterpret_cast<DarwinBlastBufferQueue*>(handle);
  if (queue == nullptr || queue->state == nullptr || env == nullptr ||
      consumer == nullptr) {
    return JNI_FALSE;
  }
  jobject global = env->NewGlobalRef(consumer);
  if (global == nullptr) return JNI_FALSE;
  auto state = queue->state;
  bool accepted = false;
  uint64_t generation = 0;
  void* observed_window = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    observed_window = state->native_window;
    if (!state->destroyed && state->sync_consumer == nullptr &&
        !state->outstanding_sync) {
      state->sync_consumer = global;
      state->continuous_sync = acquire_single == JNI_FALSE;
      // Reserve the generation before JNI attach/wrapper construction. A
      // queue callback racing this call will either claim this consumer or be
      // held behind this reservation, never applied as an ordinary frame.
      state->outstanding_sync = true;
      state->sync_phase =
          DarwinBlastBufferQueue::State::SyncPhase::kReserved;
      ++state->sync_generation;
      generation = state->sync_generation;
      accepted = true;
    }
  }
  if (!accepted) env->DeleteGlobalRef(global);
  BlastDebugTrace(accepted ? "sync-reserved" : "sync-rejected", state.get(),
                  observed_window, nullptr,
                  accepted ? generation : 0);
  return accepted ? JNI_TRUE : JNI_FALSE;
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
void SurfaceNativeDestroy(JNIEnv*, jclass, jlong) {
  // Surface.destroy() is a producer disconnect, not a strong-reference
  // release. Surface.java immediately calls release() afterward, which owns
  // the single nativeRelease for this Java Surface. Mapping both natives to
  // SurfaceNativeRelease consumed BLASTBufferQueue's producer reference and
  // left nativeDestroy() locking a freed ANativeWindow during compositor
  // detach. The Darwin queue currently has no separate connected-API state,
  // so disconnect is intentionally a no-op while reference teardown remains
  // exclusively in SurfaceNativeRelease.
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
jlong SurfaceNativeGetNextFrameNumber(JNIEnv*, jclass, jlong handle) {
  return static_cast<jlong>(darwin_art_android_ANativeWindow_next_frame_number(
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle))));
}
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

// MediaExtractor's Java class keeps its native handle in mNativeContext. The
// demux state and bounded WebM parser are shared with the NDK facade.

bool ExtractZipAsset(const std::string& source, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) return false;
  std::string apk = source;
  std::string entry;
  const size_t bang = source.find("!/");
  if (bang != std::string::npos) {
    apk = source.substr(0, bang);
    entry = source.substr(bang + 2);
    if (apk.rfind("jar:", 0) == 0) apk.erase(0, 4);
    if (apk.rfind("file://", 0) == 0) apk.erase(0, 7);
    else if (apk.rfind("file:", 0) == 0) apk.erase(0, 5);
  }
  if (entry.empty()) return false;
  ZipArchiveHandle archive = nullptr;
  if (OpenArchive(apk.c_str(), &archive) != 0) return false;
  ZipEntry64 zip_entry;
  const bool found = FindEntry(archive, entry, &zip_entry) == 0;
  if (found && zip_entry.uncompressed_length <= (64u * 1024u * 1024u)) {
    bytes->resize(static_cast<size_t>(zip_entry.uncompressed_length));
    if (ExtractToMemory(archive, &zip_entry, bytes->data(), bytes->size()) != 0)
      bytes->clear();
  }
  CloseArchive(archive);
  return !bytes->empty();
}

MediaExtractorState* GetExtractorState(JNIEnv* env, jobject self) {
  if (env == nullptr || self == nullptr) return nullptr;
  jclass type = env->GetObjectClass(self);
  jfieldID field = type == nullptr
                       ? nullptr
                       : env->GetFieldID(type, "mNativeContext", "J");
  jlong value = field == nullptr ? 0 : env->GetLongField(self, field);
  env->DeleteLocalRef(type);
  return reinterpret_cast<MediaExtractorState*>(static_cast<uintptr_t>(value));
}

void SetExtractorState(JNIEnv* env, jobject self, MediaExtractorState* state) {
  if (env == nullptr || self == nullptr) return;
  jclass type = env->GetObjectClass(self);
  jfieldID field = type == nullptr
                       ? nullptr
                       : env->GetFieldID(type, "mNativeContext", "J");
  if (field != nullptr) {
    env->SetLongField(self, field, reinterpret_cast<jlong>(state));
  }
  env->DeleteLocalRef(type);
}

jobject NewExtractorFormat(JNIEnv* env, const MediaExtractorState* state) {
  if (env == nullptr || state == nullptr) return nullptr;
  jclass map_class = env->FindClass("java/util/HashMap");
  if (map_class == nullptr) return nullptr;
  jmethodID constructor = env->GetMethodID(map_class, "<init>", "()V");
  jmethodID put = env->GetMethodID(
      map_class, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  jobject map = constructor == nullptr ? nullptr : env->NewObject(map_class, constructor);
  if (map == nullptr || put == nullptr) {
    env->DeleteLocalRef(map_class);
    return map;
  }
  jclass integer_class = env->FindClass("java/lang/Integer");
  jclass long_class = env->FindClass("java/lang/Long");
  jclass float_class = env->FindClass("java/lang/Float");
  jmethodID integer_value = integer_class == nullptr
                                ? nullptr
                                : env->GetMethodID(integer_class, "<init>", "(I)V");
  jmethodID long_value = long_class == nullptr
                             ? nullptr
                             : env->GetMethodID(long_class, "<init>", "(J)V");
  auto add_string = [&](const char* key, const char* value) {
    jstring k = env->NewStringUTF(key);
    jstring v = env->NewStringUTF(value);
    env->CallObjectMethod(map, put, k, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  };
  auto add_int = [&](const char* key, jint value) {
    jstring k = env->NewStringUTF(key);
    jobject v = integer_value == nullptr ? nullptr : env->NewObject(integer_class, integer_value, value);
    env->CallObjectMethod(map, put, k, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  };
  auto add_long = [&](const char* key, jlong value) {
    jstring k = env->NewStringUTF(key);
    jobject v = long_value == nullptr ? nullptr : env->NewObject(long_class, long_value, value);
    env->CallObjectMethod(map, put, k, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  };
  auto add_float = [&](const char* key, jfloat value) {
    jstring k = env->NewStringUTF(key);
    jmethodID ctor = float_class == nullptr ? nullptr : env->GetMethodID(float_class, "<init>", "(F)V");
    jobject v = ctor == nullptr ? nullptr : env->NewObject(float_class, ctor, value);
    env->CallObjectMethod(map, put, k, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  };
  add_string("mime", "video/x-vnd.on2.vp9");
  add_int("width", state->width);
  add_int("height", state->height);
  add_float("frame-rate", 30.0f);
  add_long("durationUs", state->duration_us);
  env->DeleteLocalRef(integer_class);
  env->DeleteLocalRef(long_class);
  env->DeleteLocalRef(float_class);
  env->DeleteLocalRef(map_class);
  return map;
}

void MediaExtractorNativeInit(JNIEnv*, jclass) {}
void MediaExtractorNativeSetup(JNIEnv* env, jobject self) {
  SetExtractorState(env, self, new (std::nothrow) MediaExtractorState());
}
void MediaExtractorNativeSetDataSource(JNIEnv* env, jobject self, jobject, jstring path,
                                       jobjectArray, jobjectArray) {
  auto* state = GetExtractorState(env, self);
  if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC") != nullptr) {
    std::cerr << "ART Android MediaExtractor: nativeSetDataSource state=" << state
              << " path_obj=" << path << "\n";
  }
  if (state == nullptr || path == nullptr) return;
  *state = MediaExtractorState{};
  const char* chars = env->GetStringUTFChars(path, nullptr);
  if (chars == nullptr) return;
  const std::string source(chars);
  std::vector<uint8_t> bytes;
  const bool parsed = ExtractZipAsset(chars, &bytes);
  env->ReleaseStringUTFChars(path, chars);
  if (parsed) ParseWebm(bytes, state);
  // Keep a normal filesystem path useful for non-APK callers.
  if (!state->has_source) {
    FILE* file = std::fopen(source.c_str(), "rb");
    if (file != nullptr) {
      std::fseek(file, 0, SEEK_END); const long length = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (length > 0 && length <= 64 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(length));
        std::fread(bytes.data(), 1, bytes.size(), file);
        ParseWebm(bytes, state);
      }
      std::fclose(file);
    }
  }
  if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC") != nullptr) {
    std::cerr << "ART Android MediaExtractor: source=" << source
              << " parsed=" << parsed << " samples=" << state->samples.size()
              << " size=" << bytes.size() << " duration_us=" << state->duration_us
              << "\n";
  }
}
void MediaExtractorSetDataSourceFd(JNIEnv* env, jobject self, jobject descriptor,
                                   jlong offset, jlong length) {
  auto* state = GetExtractorState(env, self);
  if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC") != nullptr) {
    std::cerr << "ART Android MediaExtractor: setDataSourceFd state=" << state
              << " descriptor=" << descriptor << " offset=" << offset
              << " length=" << length << "\n";
  }
  if (state == nullptr || descriptor == nullptr) return;
  if (offset < 0 || length <= 0 ||
      static_cast<uint64_t>(length) > kMediaExtractorMaxBytes) {
    return;
  }
  jclass type = env->GetObjectClass(descriptor);
  jfieldID field = type == nullptr ? nullptr : env->GetFieldID(type, "descriptor", "I");
  if (field == nullptr && type != nullptr) {
    env->ExceptionClear();
    field = env->GetFieldID(type, "fd", "I");
  }
  const jint fd = field == nullptr ? -1 : env->GetIntField(descriptor, field);
  env->DeleteLocalRef(type);
  if (fd < 0) return;
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  size_t total = 0;
  while (total < bytes.size()) {
    if (offset > std::numeric_limits<jlong>::max() -
                    static_cast<jlong>(total)) {
      bytes.clear();
      break;
    }
    const intptr_t n = darwin_art_bionic_pread(
        fd, bytes.data() + total, bytes.size() - total,
        offset + static_cast<jlong>(total));
    if (n <= 0 || static_cast<uint64_t>(n) > bytes.size() - total) break;
    total += static_cast<size_t>(n);
  }
  bytes.resize(total);
  ParseWebm(bytes, state);
  if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC") != nullptr) {
    std::cerr << "ART Android MediaExtractor: fd=" << fd << " samples="
              << state->samples.size() << " bytes=" << bytes.size() << "\n";
  }
}
void MediaExtractorSetDataSourceMedia(JNIEnv* env, jobject self, jobject) {
  // Java MediaDataSource callbacks are not a filesystem source. Do not claim
  // a track unless the source was actually read and parsed.
  if (auto* state = GetExtractorState(env, self); state != nullptr)
    *state = MediaExtractorState{};
}
jobject MediaExtractorGetFileFormat(JNIEnv* env, jobject self) {
  return NewExtractorFormat(env, GetExtractorState(env, self));
}
jobject MediaExtractorGetTrackFormat(JNIEnv* env, jobject self, jint) {
  return NewExtractorFormat(env, GetExtractorState(env, self));
}
jint MediaExtractorGetTrackCount(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state != nullptr && state->has_source ? 1 : 0;
}
void MediaExtractorRelease(JNIEnv* env, jobject self) {
  delete GetExtractorState(env, self);
  SetExtractorState(env, self, nullptr);
}
void MediaExtractorFinalize(JNIEnv* env, jobject self) { MediaExtractorRelease(env, self); }
void MediaExtractorSelectTrack(JNIEnv*, jobject, jint) {}
void MediaExtractorUnselectTrack(JNIEnv*, jobject, jint) {}
jboolean MediaExtractorAdvance(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  if (state == nullptr || !state->has_source) return JNI_FALSE;
  if (state->sample_index + 1 >= state->samples.size()) {
    state->sample_index = state->samples.size();
    return JNI_FALSE;
  }
  ++state->sample_index;
  return JNI_TRUE;
}
jlong MediaExtractorGetSampleTime(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state != nullptr &&
                 state->sample_index < state->samples.size()
             ? state->samples[state->sample_index].pts_us : -1;
}
jlong MediaExtractorGetSampleSize(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state != nullptr &&
                 state->sample_index < state->samples.size()
             ? static_cast<jlong>(state->samples[state->sample_index].data.size()) : -1;
}
jint MediaExtractorGetSampleFlags(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state != nullptr &&
                 state->sample_index < state->samples.size()
             ? state->samples[state->sample_index].flags : 0;
}
jint MediaExtractorGetSampleTrackIndex(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state != nullptr && state->sample_index < state->samples.size() ? 0 : -1;
}
jint MediaExtractorReadSampleData(JNIEnv* env, jobject self, jobject buffer, jint offset) {
  auto* state = GetExtractorState(env, self);
  if (state == nullptr || buffer == nullptr ||
      state->sample_index >= state->samples.size() || offset < 0)
    return -1;
  void* destination = env->GetDirectBufferAddress(buffer);
  const jlong capacity = env->GetDirectBufferCapacity(buffer);
  const auto& sample = state->samples[state->sample_index].data;
  if (destination == nullptr ||
      sample.size() > static_cast<size_t>(std::numeric_limits<jint>::max()) ||
      static_cast<jlong>(offset) > capacity ||
      sample.size() > static_cast<size_t>(capacity - offset)) return -1;
  std::memcpy(static_cast<uint8_t*>(destination) + offset, sample.data(), sample.size());
  return static_cast<jint>(sample.size());
}
void MediaExtractorSeekTo(JNIEnv* env, jobject self, jlong time_us, jint mode) {
  auto* state = GetExtractorState(env, self);
  if (state == nullptr) return;
  size_t index = 0;
  while (index < state->samples.size() &&
         state->samples[index].pts_us < time_us) ++index;
  if (mode == 0 && index > 0) --index;
  state->sample_index = std::min(index, state->samples.size());
}
jlong MediaExtractorGetCachedDuration(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state == nullptr ? 0 : state->duration_us;
}
jboolean MediaExtractorHasCacheReachedEnd(JNIEnv* env, jobject self) {
  auto* state = GetExtractorState(env, self);
  return state == nullptr || state->sample_index + 1 >= state->samples.size();
}
jboolean MediaExtractorGetSampleCryptoInfo(JNIEnv*, jobject, jobject) { return JNI_FALSE; }
jobject MediaExtractorGetAudioPresentations(JNIEnv* env, jobject, jint) {
  jclass collections = env->FindClass("java/util/Collections");
  jmethodID empty = collections == nullptr ? nullptr : env->GetStaticMethodID(
      collections, "emptyList", "()Ljava/util/List;");
  jobject result = empty == nullptr ? nullptr : env->CallStaticObjectMethod(collections, empty);
  env->DeleteLocalRef(collections);
  return result;
}
jobject MediaExtractorGetMetrics(JNIEnv* env, jobject) {
  jclass type = env->FindClass("android/os/PersistableBundle");
  jmethodID ctor = type == nullptr ? nullptr : env->GetMethodID(type, "<init>", "()V");
  jobject result = ctor == nullptr ? nullptr : env->NewObject(type, ctor);
  env->DeleteLocalRef(type);
  return result;
}
void MediaExtractorSetLogSessionId(JNIEnv*, jobject, jstring) {}
void MediaExtractorSetMediaCas(JNIEnv*, jobject, jobject) {}

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
  JNINativeMethod media_extractor_methods[] = {
      {const_cast<char*>("getFileFormatNative"), const_cast<char*>("()Ljava/util/Map;"),
       reinterpret_cast<void*>(&MediaExtractorGetFileFormat)},
      {const_cast<char*>("getTrackFormatNative"), const_cast<char*>("(I)Ljava/util/Map;"),
       reinterpret_cast<void*>(&MediaExtractorGetTrackFormat)},
      {const_cast<char*>("nativeSetDataSource"),
       const_cast<char*>("(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&MediaExtractorNativeSetDataSource)},
      {const_cast<char*>("nativeSetMediaCas"), const_cast<char*>("(Landroid/os/IHwBinder;)V"),
       reinterpret_cast<void*>(&MediaExtractorSetMediaCas)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaExtractorFinalize)},
      {const_cast<char*>("native_getAudioPresentations"), const_cast<char*>("(I)Ljava/util/List;"),
       reinterpret_cast<void*>(&MediaExtractorGetAudioPresentations)},
      {const_cast<char*>("native_getMetrics"), const_cast<char*>("()Landroid/os/PersistableBundle;"),
       reinterpret_cast<void*>(&MediaExtractorGetMetrics)},
      {const_cast<char*>("native_init"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaExtractorNativeInit)},
      {const_cast<char*>("native_setLogSessionId"), const_cast<char*>("(Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&MediaExtractorSetLogSessionId)},
      {const_cast<char*>("native_setup"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaExtractorNativeSetup)},
      {const_cast<char*>("advance"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&MediaExtractorAdvance)},
      {const_cast<char*>("getCachedDuration"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&MediaExtractorGetCachedDuration)},
      {const_cast<char*>("getSampleCryptoInfo"), const_cast<char*>("(Landroid/media/MediaCodec$CryptoInfo;)Z"),
       reinterpret_cast<void*>(&MediaExtractorGetSampleCryptoInfo)},
      {const_cast<char*>("getSampleFlags"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&MediaExtractorGetSampleFlags)},
      {const_cast<char*>("getSampleSize"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&MediaExtractorGetSampleSize)},
      {const_cast<char*>("getSampleTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&MediaExtractorGetSampleTime)},
      {const_cast<char*>("getSampleTrackIndex"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&MediaExtractorGetSampleTrackIndex)},
      {const_cast<char*>("setDataSource"),
       const_cast<char*>("(Ljava/io/FileDescriptor;JJ)V"),
       reinterpret_cast<void*>(&MediaExtractorSetDataSourceFd)},
      {const_cast<char*>("setDataSource"), const_cast<char*>("(Landroid/media/MediaDataSource;)V"),
       reinterpret_cast<void*>(&MediaExtractorSetDataSourceMedia)},
      {const_cast<char*>("getTrackCount"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&MediaExtractorGetTrackCount)},
      {const_cast<char*>("hasCacheReachedEndOfStream"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&MediaExtractorHasCacheReachedEnd)},
      {const_cast<char*>("readSampleData"), const_cast<char*>("(Ljava/nio/ByteBuffer;I)I"),
       reinterpret_cast<void*>(&MediaExtractorReadSampleData)},
      {const_cast<char*>("release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaExtractorRelease)},
      {const_cast<char*>("seekTo"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&MediaExtractorSeekTo)},
      {const_cast<char*>("selectTrack"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&MediaExtractorSelectTrack)},
      {const_cast<char*>("unselectTrack"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&MediaExtractorUnselectTrack)},
  };
  if (!Register(env, "android/media/MediaExtractor", media_extractor_methods,
                static_cast<jint>(std::size(media_extractor_methods)))) {
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
  JNINativeMethod incremental_methods[] = {
      {const_cast<char*>("nativeIsEnabled"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&IncrementalEnabled)},
      {const_cast<char*>("nativeIsV2Available"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&IncrementalEnabled)},
      {const_cast<char*>("nativeIsIncrementalFd"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&IncrementalFileDescriptor)},
      {const_cast<char*>("nativeIsIncrementalPath"), const_cast<char*>("(Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(&IncrementalPath)},
      {const_cast<char*>("nativeUnsafeGetFileSignature"),
       const_cast<char*>("(Ljava/lang/String;)[B"),
       reinterpret_cast<void*>(&IncrementalFileSignature)},
  };
  if (!Register(env, "android/os/incremental/IncrementalManager",
                incremental_methods, static_cast<jint>(std::size(incremental_methods)))) {
    return false;
  }
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
       reinterpret_cast<void*>(&SurfaceControlNativeSetTransparentRegionHint)},
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
       reinterpret_cast<void*>(&BlastBufferQueueNativeApplyPendingTransactions)},
      {const_cast<char*>("nativeClearSyncTransaction"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&BlastBufferQueueNativeClearSyncTransaction)},
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
       reinterpret_cast<void*>(&BlastBufferQueueNativeMergeWithNextTransaction)},
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
       reinterpret_cast<void*>(&BlastBufferQueueNativeStopContinuousSyncTransaction)},
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

  JNINativeMethod image_reader_methods[] = {
      {const_cast<char*>("nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&ImageReaderNativeClassInit)},
      {const_cast<char*>("nativeInit"),
       const_cast<char*>("(Ljava/lang/Object;IIIJII)V"),
       reinterpret_cast<void*>(&ImageReaderNativeInit)},
      {const_cast<char*>("nativeClose"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&ImageReaderNativeClose)},
      {const_cast<char*>("nativeReleaseImage"),
       const_cast<char*>("(Landroid/media/Image;)V"),
       reinterpret_cast<void*>(&ImageReaderNativeReleaseImage)},
      {const_cast<char*>("nativeImageSetup"),
       const_cast<char*>("(Landroid/media/Image;)I"),
       reinterpret_cast<void*>(&ImageReaderNativeImageSetup)},
      {const_cast<char*>("nativeGetSurface"),
       const_cast<char*>("()Landroid/view/Surface;"),
       reinterpret_cast<void*>(&ImageReaderNativeGetSurface)},
      {const_cast<char*>("nativeDetachImage"),
       const_cast<char*>("(Landroid/media/Image;Z)I"),
       reinterpret_cast<void*>(&ImageReaderNativeDetachImage)},
      {const_cast<char*>("nativeCreateImagePlanes"),
       const_cast<char*>(
           "(ILandroid/graphics/GraphicBuffer;IIIIII)[Landroid/media/ImageReader$ImagePlane;"),
       reinterpret_cast<void*>(&ImageReaderNativeCreateImagePlanes)},
      {const_cast<char*>("nativeUnlockGraphicBuffer"),
       const_cast<char*>("(Landroid/graphics/GraphicBuffer;)V"),
       reinterpret_cast<void*>(&ImageReaderNativeUnlockGraphicBuffer)},
      {const_cast<char*>("nativeDiscardFreeBuffers"),
       const_cast<char*>("()V"),
       reinterpret_cast<void*>(&ImageReaderNativeDiscardFreeBuffers)},
  };
  if (!Register(env, "android/media/ImageReader", image_reader_methods,
                static_cast<jint>(std::size(image_reader_methods)))) {
    return false;
  }

  JNINativeMethod surface_image_methods[] = {
      {const_cast<char*>("nativeCreatePlanes"), const_cast<char*>("(IIJ)[Landroid/media/ImageReader$SurfaceImage$SurfacePlane;"),
       reinterpret_cast<void*>(&SurfaceImageNativeCreatePlanes)},
      {const_cast<char*>("nativeGetWidth"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SurfaceImageNativeGetWidth)},
      {const_cast<char*>("nativeGetHeight"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SurfaceImageNativeGetHeight)},
      {const_cast<char*>("nativeGetFormat"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&SurfaceImageNativeGetFormat)},
      {const_cast<char*>("nativeGetFenceFd"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SurfaceImageNativeGetFenceFd)},
      {const_cast<char*>("nativeGetHardwareBuffer"),
       const_cast<char*>("()Landroid/hardware/HardwareBuffer;"),
       reinterpret_cast<void*>(&SurfaceImageNativeGetHardwareBuffer)},
  };
  if (!Register(env, "android/media/ImageReader$SurfaceImage",
                surface_image_methods,
                static_cast<jint>(std::size(surface_image_methods)))) {
    return false;
  }

  JNINativeMethod hardware_buffer_methods[] = {
      {const_cast<char*>("nCreateHardwareBuffer"),
       const_cast<char*>("(IIIIJ)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeCreate)},
      {const_cast<char*>("nCreateFromGraphicBuffer"),
       const_cast<char*>("(Landroid/graphics/GraphicBuffer;)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeCreateFromGraphicBuffer)},
      {const_cast<char*>("nGetNativeFinalizer"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetFinalizer)},
      {const_cast<char*>("nWriteHardwareBufferToParcel"),
       const_cast<char*>("(JLandroid/os/Parcel;)V"),
       reinterpret_cast<void*>(&HardwareBufferNativeWriteToParcel)},
      {const_cast<char*>("nReadHardwareBufferFromParcel"),
       const_cast<char*>("(Landroid/os/Parcel;)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeReadFromParcel)},
      {const_cast<char*>("nIsSupported"), const_cast<char*>("(IIIIJ)Z"),
       reinterpret_cast<void*>(&HardwareBufferNativeIsSupported)},
      {const_cast<char*>("nGetWidth"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetWidth)},
      {const_cast<char*>("nGetHeight"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetHeight)},
      {const_cast<char*>("nGetFormat"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetFormat)},
      {const_cast<char*>("nGetLayers"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetLayers)},
      {const_cast<char*>("nGetUsage"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetUsage)},
      {const_cast<char*>("nEstimateSize"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeEstimateSize)},
      {const_cast<char*>("nGetId"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&HardwareBufferNativeGetId)},
  };
  if (!Register(env, "android/hardware/HardwareBuffer",
                hardware_buffer_methods,
                static_cast<jint>(std::size(hardware_buffer_methods)))) {
    return false;
  }

  JNINativeMethod sync_fence_methods[] = {
      {const_cast<char*>("nGetDestructor"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SyncFenceNativeGetDestructor)},
      {const_cast<char*>("nCreate"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&SyncFenceNativeCreate)},
      {const_cast<char*>("nIsValid"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(&SyncFenceNativeIsValid)},
      {const_cast<char*>("nGetFd"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&SyncFenceNativeGetFd)},
      {const_cast<char*>("nWait"), const_cast<char*>("(JJ)Z"),
       reinterpret_cast<void*>(&SyncFenceNativeWait)},
      {const_cast<char*>("nGetSignalTime"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(&SyncFenceNativeGetSignalTime)},
      {const_cast<char*>("nIncRef"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&SyncFenceNativeIncRef)},
  };
  if (!Register(env, "android/hardware/SyncFence", sync_fence_methods,
                static_cast<jint>(std::size(sync_fence_methods)))) {
    return false;
  }

  JNINativeMethod public_format_methods[] = {
      {const_cast<char*>("nativeGetHalFormat"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&PublicFormatNativeGetHalFormat)},
      {const_cast<char*>("nativeGetHalDataspace"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&PublicFormatNativeGetHalDataspace)},
      {const_cast<char*>("nativeGetPublicFormat"), const_cast<char*>("(II)I"),
       reinterpret_cast<void*>(&PublicFormatNativeGetPublicFormat)},
  };
  if (!Register(env, "android/media/PublicFormatUtils",
                public_format_methods,
                static_cast<jint>(std::size(public_format_methods)))) {
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
       reinterpret_cast<void*>(&SurfaceNativeDestroy)},
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
