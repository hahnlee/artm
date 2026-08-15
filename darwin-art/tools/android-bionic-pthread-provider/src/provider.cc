#include "darwin_art_bionic_pthread.h"

#include <pthread.h>

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

static_assert(sizeof(DarwinArtAndroidPthread) == 8);
static_assert(sizeof(DarwinArtAndroidPthreadKey) == 4);
static_assert(sizeof(DarwinArtAndroidPthreadOnce) == 4);
static_assert(sizeof(DarwinArtAndroidPthreadMutex) == 40);
static_assert(sizeof(DarwinArtAndroidPthreadCond) == 48);
static_assert(sizeof(DarwinArtAndroidPthreadRwlock) == 56);

constexpr int kAndroidEagain = 11;
constexpr int kAndroidEnomem = 12;
constexpr int kAndroidEbusy = 16;
constexpr int kAndroidEinval = 22;
constexpr int kAndroidEdeadlk = 35;
constexpr int kAndroidEnotsup = 95;
constexpr int kAndroidEtimedout = 110;
constexpr uint32_t kAndroidKeyValid = UINT32_C(0x80000000);
constexpr uint32_t kAndroidKeySlots = 128;
constexpr int kAndroidDestructorIterations = 4;
constexpr uint16_t kAndroidMutexDestroyed = UINT16_C(0xffff);
constexpr uint32_t kAndroidCondSharedMask = UINT32_C(0x0001);
constexpr uint32_t kAndroidCondClockMask = UINT32_C(0x0002);
constexpr uint32_t kAndroidCondCounterStep = UINT32_C(0x0004);
constexpr uint32_t kAndroidCondDestroyed = UINT32_C(0xdeadc04d);
constexpr uint32_t kAndroidRwlockPendingReaders = UINT32_C(0x00000001);
constexpr uint32_t kAndroidRwlockPendingWriters = UINT32_C(0x00000002);
constexpr uint32_t kAndroidRwlockReaderStep = UINT32_C(0x00000004);
constexpr uint32_t kAndroidRwlockWriterOwned = UINT32_C(0x80000000);
constexpr size_t kAndroidRwlockMaxReaders = (UINT32_C(1) << 29) - 1;
constexpr int32_t kOnceNotStarted = 0;
constexpr int32_t kOnceUnderway = 1;
constexpr int32_t kOnceComplete = 2;

int AndroidError(int error) {
  if (error == 0) return 0;
  if (error == EBUSY) return kAndroidEbusy;
  if (error == EINVAL) return kAndroidEinval;
  if (error == ENOMEM) return kAndroidEnomem;
  if (error == EAGAIN) return kAndroidEagain;
  if (error == EDEADLK) return kAndroidEdeadlk;
  if (error == ETIMEDOUT) return kAndroidEtimedout;
  if (error == EPERM) return 1;
  if (error == ESRCH) return 3;
  if (error == ENOTSUP) return kAndroidEnotsup;
  return kAndroidEinval;
}

struct KeyEntry {
  uint32_t slot{};
  uint64_t generation{};
  DarwinArtAndroidTlsDestructor destructor{};
};

struct KeySlot {
  uint64_t generation{};
  std::shared_ptr<KeyEntry> active;
};

struct ThreadTlsValue {
  uint64_t generation{};
  void* value{};
};

struct ThreadTlsState {
  ThreadTlsValue values[kAndroidKeySlots];
};

struct MutexEntry {
  pthread_mutex_t host{};
  std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_condition;
  enum class Lifecycle { kAlive, kDestroying, kDestroyed };
  Lifecycle lifecycle{Lifecycle::kAlive};
  size_t active_operations{};
  bool host_initialized{};

  ~MutexEntry() {
    if (host_initialized) {
      pthread_mutex_destroy(&host);
    }
  }
};

struct OnceEntry {
  std::mutex mutex;
  std::condition_variable condition;
  int state{kOnceNotStarted};
};

struct CondEntry {
  pthread_cond_t host{};
  std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_condition;
  enum class Lifecycle { kAlive, kDestroying, kDestroyed };
  Lifecycle lifecycle{Lifecycle::kAlive};
  size_t active_operations{};
  size_t waiters{};
  bool monotonic{};
  bool host_initialized{};

  ~CondEntry() {
    if (host_initialized) pthread_cond_destroy(&host);
  }
};

struct RwlockEntry {
  std::mutex mutex;
  std::condition_variable readers_condition;
  std::condition_variable writers_condition;
  enum class Lifecycle { kAlive, kDestroyed };
  Lifecycle lifecycle{Lifecycle::kAlive};
  size_t active_readers{};
  size_t pending_readers{};
  size_t pending_writers{};
  DarwinArtAndroidPthread writer_owner{};
};

struct ProviderState {
  std::mutex mutex;
  KeySlot key_slots[kAndroidKeySlots];
  std::unordered_set<ThreadTlsState*> thread_tls_states;
  std::unordered_map<DarwinArtAndroidPthreadMutex*, std::shared_ptr<MutexEntry>>
      mutexes;
  std::unordered_map<DarwinArtAndroidPthreadOnce*, std::shared_ptr<OnceEntry>>
      once_controls;
  std::unordered_map<DarwinArtAndroidPthreadCond*, std::shared_ptr<CondEntry>>
      conditions;
  std::unordered_map<DarwinArtAndroidPthreadRwlock*,
                     std::shared_ptr<RwlockEntry>>
      rwlocks;
};

ProviderState& State() {
  // Intentionally process-lifetime: Darwin TLS destructors can run after
  // ordinary C++ static destruction has begun.
  static ProviderState* state = new ProviderState();
  return *state;
}

std::atomic<uint64_t>& NextThreadToken() {
  static std::atomic<uint64_t> value{1};
  return value;
}

pthread_key_t& HostThreadTlsKey() {
  static pthread_key_t key{};
  return key;
}

pthread_once_t& HostThreadTlsKeyOnce() {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  return once;
}

thread_local ThreadTlsState* tls_destructor_state = nullptr;

void HostThreadTlsDestructor(void* opaque);

void InitializeHostThreadTlsKey() {
  const int result =
      pthread_key_create(&HostThreadTlsKey(), &HostThreadTlsDestructor);
  if (result != 0) std::abort();
}

ThreadTlsState* GetThreadTlsState(bool create) {
  if (tls_destructor_state != nullptr) return tls_destructor_state;
  pthread_once(&HostThreadTlsKeyOnce(), &InitializeHostThreadTlsKey);
  auto* thread_state =
      static_cast<ThreadTlsState*>(pthread_getspecific(HostThreadTlsKey()));
  if (thread_state != nullptr || !create) return thread_state;
  thread_state = new (std::nothrow) ThreadTlsState{};
  if (thread_state == nullptr) return nullptr;
  if (pthread_setspecific(HostThreadTlsKey(), thread_state) != 0) {
    delete thread_state;
    return nullptr;
  }
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.thread_tls_states.insert(thread_state);
  return thread_state;
}

DarwinArtAndroidPthread CurrentThreadToken() {
  static thread_local const uint64_t token =
      NextThreadToken().fetch_add(1, std::memory_order_relaxed);
  return static_cast<DarwinArtAndroidPthread>(token);
}

bool DecodeKey(DarwinArtAndroidPthreadKey key, uint32_t* slot_out) {
  const uint32_t raw = static_cast<uint32_t>(key);
  if ((raw & kAndroidKeyValid) == 0) return false;
  const uint32_t slot = raw & ~kAndroidKeyValid;
  if (slot >= kAndroidKeySlots) return false;
  *slot_out = slot;
  return true;
}

void HostThreadTlsDestructor(void* opaque) {
  auto* thread_state = static_cast<ThreadTlsState*>(opaque);
  tls_destructor_state = thread_state;
  // Bionic runs at most PTHREAD_DESTRUCTOR_ITERATIONS passes. Clear each value
  // before invoking its callback so reinsertion schedules a later pass.
  for (int iteration = 0; iteration < kAndroidDestructorIterations;
       ++iteration) {
    struct Pending {
      DarwinArtAndroidTlsDestructor destructor;
      void* value;
    };
    Pending pending[kAndroidKeySlots];
    size_t count = 0;
    {
      ProviderState& state = State();
      std::lock_guard<std::mutex> lock(state.mutex);
      for (uint32_t slot = 0; slot < kAndroidKeySlots; ++slot) {
        const std::shared_ptr<KeyEntry>& entry = state.key_slots[slot].active;
        ThreadTlsValue& value = thread_state->values[slot];
        if (entry != nullptr && value.value != nullptr &&
            value.generation == entry->generation &&
            entry->destructor != nullptr) {
          pending[count++] = Pending{entry->destructor, value.value};
          value.value = nullptr;
        }
      }
    }
    if (count == 0) break;
    for (size_t index = 0; index < count; ++index) {
      // Android arm64 and Darwin arm64 agree for this fixed one-pointer
      // callback. Arbitrary callback signatures remain out of scope.
      pending[index].destructor(pending[index].value);
    }
  }
  tls_destructor_state = nullptr;
  ProviderState& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.thread_tls_states.erase(thread_state);
  }
  delete thread_state;
}

uint16_t AndroidMutexState(const DarwinArtAndroidPthreadMutex* mutex) {
  uint16_t value = 0;
  std::memcpy(&value, mutex, sizeof(value));
  return value;
}

void SetAndroidMutexState(DarwinArtAndroidPthreadMutex* mutex, uint16_t value) {
  std::memcpy(mutex, &value, sizeof(value));
}

uint32_t AndroidCondState(const DarwinArtAndroidPthreadCond* cond) {
  uint32_t value = 0;
  std::memcpy(&value, cond, sizeof(value));
  return value;
}

void SetAndroidCondState(DarwinArtAndroidPthreadCond* cond, uint32_t value) {
  std::memcpy(cond, &value, sizeof(value));
}

void SetAndroidCondWaiters(DarwinArtAndroidPthreadCond* cond, uint32_t value) {
  std::memcpy(reinterpret_cast<unsigned char*>(cond) + sizeof(uint32_t),
              &value,
              sizeof(value));
}

void SetAndroidRwlockVisible(DarwinArtAndroidPthreadRwlock* visible,
                             const RwlockEntry& entry) {
  uint32_t state = 0;
  if (entry.writer_owner != 0) {
    state |= kAndroidRwlockWriterOwned;
  } else {
    state |= static_cast<uint32_t>(entry.active_readers) *
             kAndroidRwlockReaderStep;
  }
  if (entry.pending_readers != 0) state |= kAndroidRwlockPendingReaders;
  if (entry.pending_writers != 0) state |= kAndroidRwlockPendingWriters;
  const int32_t writer_tid = static_cast<int32_t>(entry.writer_owner);
  std::memcpy(visible, &state, sizeof(state));
  std::memcpy(reinterpret_cast<unsigned char*>(visible) + sizeof(state),
              &writer_tid,
              sizeof(writer_tid));
}

std::shared_ptr<MutexEntry> FindOrCreateMutex(
    DarwinArtAndroidPthreadMutex* mutex,
    int* error_out) {
  if (mutex == nullptr) {
    *error_out = kAndroidEinval;
    return nullptr;
  }
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.mutexes.find(mutex);
  if (found != state.mutexes.end()) {
    *error_out = 0;
    return found->second;
  }
  // The only lazy initializer in this first slice is Bionic's normal static
  // initializer, whose complete 40-byte representation is zero.
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(mutex);
  for (size_t index = 0; index < sizeof(*mutex); ++index) {
    if (bytes[index] != 0) {
      *error_out = AndroidMutexState(mutex) == kAndroidMutexDestroyed
                       ? kAndroidEbusy
                       : kAndroidEnotsup;
      return nullptr;
    }
  }
  auto entry = std::make_shared<MutexEntry>();
  const int result = pthread_mutex_init(&entry->host, nullptr);
  if (result != 0) {
    *error_out = AndroidError(result);
    return nullptr;
  }
  entry->host_initialized = true;
  state.mutexes.emplace(mutex, entry);
  *error_out = 0;
  return entry;
}

std::shared_ptr<CondEntry> FindOrCreateCond(
    DarwinArtAndroidPthreadCond* cond,
    int* error_out) {
  if (cond == nullptr) {
    *error_out = kAndroidEinval;
    return nullptr;
  }
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.conditions.find(cond);
  if (found != state.conditions.end()) {
    *error_out = 0;
    return found->second;
  }
  const uint32_t visible = AndroidCondState(cond);
  if (visible == kAndroidCondDestroyed) {
    *error_out = kAndroidEbusy;
    return nullptr;
  }
  if ((visible & kAndroidCondSharedMask) != 0) {
    *error_out = kAndroidEnotsup;
    return nullptr;
  }
  if ((visible & ~(kAndroidCondSharedMask | kAndroidCondClockMask)) != 0) {
    *error_out = kAndroidEinval;
    return nullptr;
  }
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(cond);
  for (size_t index = sizeof(uint32_t); index < sizeof(*cond); ++index) {
    if (bytes[index] != 0) {
      *error_out = kAndroidEinval;
      return nullptr;
    }
  }
  auto entry = std::make_shared<CondEntry>();
  const int result = pthread_cond_init(&entry->host, nullptr);
  if (result != 0) {
    *error_out = AndroidError(result);
    return nullptr;
  }
  entry->host_initialized = true;
  entry->monotonic = (visible & kAndroidCondClockMask) != 0;
  state.conditions.emplace(cond, entry);
  *error_out = 0;
  return entry;
}

int BeginCondWait(const std::shared_ptr<CondEntry>& cond,
                  const std::shared_ptr<MutexEntry>& mutex) {
  std::scoped_lock lock(cond->lifecycle_mutex, mutex->lifecycle_mutex);
  if (cond->lifecycle != CondEntry::Lifecycle::kAlive ||
      mutex->lifecycle != MutexEntry::Lifecycle::kAlive) {
    return kAndroidEbusy;
  }
  ++cond->active_operations;
  ++cond->waiters;
  ++mutex->active_operations;
  return 0;
}

void EndCondWait(DarwinArtAndroidPthreadCond* visible_cond,
                 const std::shared_ptr<CondEntry>& cond,
                 const std::shared_ptr<MutexEntry>& mutex) {
  std::scoped_lock lock(cond->lifecycle_mutex, mutex->lifecycle_mutex);
  --cond->active_operations;
  --cond->waiters;
  --mutex->active_operations;
  SetAndroidCondWaiters(visible_cond, static_cast<uint32_t>(cond->waiters));
  cond->lifecycle_condition.notify_all();
  mutex->lifecycle_condition.notify_all();
}

timespec MonotonicRelativeTimeout(const timespec& absolute) {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  timespec relative{};
  relative.tv_sec = absolute.tv_sec - now.tv_sec;
  relative.tv_nsec = absolute.tv_nsec - now.tv_nsec;
  if (relative.tv_nsec < 0) {
    --relative.tv_sec;
    relative.tv_nsec += 1000000000L;
  }
  if (relative.tv_sec < 0) return timespec{0, 0};
  return relative;
}

int CondWait(DarwinArtAndroidPthreadCond* visible_cond,
             DarwinArtAndroidPthreadMutex* visible_mutex,
             const timespec* absolute_timeout) {
  if (absolute_timeout != nullptr &&
      (absolute_timeout->tv_sec < 0 || absolute_timeout->tv_nsec < 0 ||
       absolute_timeout->tv_nsec >= 1000000000L)) {
    return kAndroidEinval;
  }
  int error = 0;
  std::shared_ptr<CondEntry> cond = FindOrCreateCond(visible_cond, &error);
  if (cond == nullptr) return error;
  std::shared_ptr<MutexEntry> mutex = FindOrCreateMutex(visible_mutex, &error);
  if (mutex == nullptr) return error;
  error = BeginCondWait(cond, mutex);
  if (error != 0) return error;
  {
    std::lock_guard<std::mutex> lock(cond->lifecycle_mutex);
    SetAndroidCondWaiters(visible_cond, static_cast<uint32_t>(cond->waiters));
  }
  int result = 0;
  if (absolute_timeout == nullptr) {
    result = pthread_cond_wait(&cond->host, &mutex->host);
  } else if (cond->monotonic) {
    const timespec relative = MonotonicRelativeTimeout(*absolute_timeout);
    result = pthread_cond_timedwait_relative_np(
        &cond->host, &mutex->host, &relative);
  } else {
    result = pthread_cond_timedwait(
        &cond->host, &mutex->host, absolute_timeout);
  }
  EndCondWait(visible_cond, cond, mutex);
  return AndroidError(result);
}

int CondPulse(DarwinArtAndroidPthreadCond* visible_cond, bool broadcast) {
  int error = 0;
  std::shared_ptr<CondEntry> cond = FindOrCreateCond(visible_cond, &error);
  if (cond == nullptr) return error;
  {
    std::lock_guard<std::mutex> lock(cond->lifecycle_mutex);
    if (cond->lifecycle != CondEntry::Lifecycle::kAlive) return kAndroidEbusy;
    ++cond->active_operations;
    if (cond->waiters != 0) {
      SetAndroidCondState(
          visible_cond,
          AndroidCondState(visible_cond) + kAndroidCondCounterStep);
    }
  }
  const int result = broadcast ? pthread_cond_broadcast(&cond->host)
                               : pthread_cond_signal(&cond->host);
  {
    std::lock_guard<std::mutex> lock(cond->lifecycle_mutex);
    --cond->active_operations;
    if (cond->active_operations == 0) cond->lifecycle_condition.notify_all();
  }
  return AndroidError(result);
}

std::shared_ptr<RwlockEntry> FindOrCreateRwlock(
    DarwinArtAndroidPthreadRwlock* rwlock,
    int* error_out) {
  if (rwlock == nullptr) {
    *error_out = kAndroidEinval;
    return nullptr;
  }
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.rwlocks.find(rwlock);
  if (found != state.rwlocks.end()) {
    *error_out = 0;
    return found->second;
  }
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(rwlock);
  // Bionic's pshared byte is offset 8. This slice accepts only the all-zero
  // PTHREAD_RWLOCK_INITIALIZER and never fabricates process-shared behavior.
  if (bytes[8] != 0) {
    *error_out = kAndroidEnotsup;
    return nullptr;
  }
  for (size_t index = 0; index < sizeof(*rwlock); ++index) {
    if (bytes[index] != 0) {
      *error_out = kAndroidEinval;
      return nullptr;
    }
  }
  auto entry = std::make_shared<RwlockEntry>();
  state.rwlocks.emplace(rwlock, entry);
  *error_out = 0;
  return entry;
}

template <typename Operation>
int WithLiveMutex(const std::shared_ptr<MutexEntry>& entry,
                  Operation operation,
                  bool permit_while_destroying = false) {
  {
    std::unique_lock<std::mutex> lock(entry->lifecycle_mutex);
    while (!permit_while_destroying &&
           entry->lifecycle == MutexEntry::Lifecycle::kDestroying) {
      entry->lifecycle_condition.wait(lock);
    }
    if (entry->lifecycle == MutexEntry::Lifecycle::kDestroyed) {
      return kAndroidEbusy;
    }
    ++entry->active_operations;
  }
  const int result = operation(&entry->host);
  {
    std::lock_guard<std::mutex> lock(entry->lifecycle_mutex);
    --entry->active_operations;
    if (entry->active_operations == 0) {
      entry->lifecycle_condition.notify_all();
    }
  }
  return AndroidError(result);
}

}  // namespace

extern "C" DarwinArtAndroidPthread darwin_art_bionic_pthread_self(void) {
  return CurrentThreadToken();
}

extern "C" int darwin_art_bionic_pthread_key_create(
    DarwinArtAndroidPthreadKey* key,
    DarwinArtAndroidTlsDestructor destructor) {
  if (key == nullptr) return kAndroidEinval;
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (uint32_t slot = 0; slot < kAndroidKeySlots; ++slot) {
    KeySlot& key_slot = state.key_slots[slot];
    if (key_slot.active != nullptr) continue;
    auto entry = std::make_shared<KeyEntry>();
    entry->slot = slot;
    entry->generation = ++key_slot.generation;
    entry->destructor = destructor;
    key_slot.active = entry;
    *key = static_cast<int32_t>(kAndroidKeyValid | slot);
    return 0;
  }
  return kAndroidEagain;
}

extern "C" int darwin_art_bionic_pthread_key_delete(
    DarwinArtAndroidPthreadKey key) {
  uint32_t slot = 0;
  if (!DecodeKey(key, &slot)) return kAndroidEinval;
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.key_slots[slot].active == nullptr) return kAndroidEinval;
  // POSIX deletion does not call destructors. Per-thread values retain only a
  // generation number and raw application value, never an owning KeyEntry.
  // Slot reuse therefore cannot create a cycle or a stale host-key callback.
  state.key_slots[slot].active.reset();
  return 0;
}

extern "C" void* darwin_art_bionic_pthread_getspecific(
    DarwinArtAndroidPthreadKey key) {
  uint32_t slot = 0;
  if (!DecodeKey(key, &slot)) return nullptr;
  ThreadTlsState* thread_state = GetThreadTlsState(false);
  if (thread_state == nullptr) return nullptr;
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const std::shared_ptr<KeyEntry>& entry = state.key_slots[slot].active;
  if (entry == nullptr ||
      thread_state->values[slot].generation != entry->generation) {
    return nullptr;
  }
  return thread_state->values[slot].value;
}

extern "C" int darwin_art_bionic_pthread_setspecific(
    DarwinArtAndroidPthreadKey key,
    const void* value) {
  uint32_t slot = 0;
  if (!DecodeKey(key, &slot)) return kAndroidEinval;
  ThreadTlsState* thread_state = GetThreadTlsState(value != nullptr);
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const std::shared_ptr<KeyEntry>& entry = state.key_slots[slot].active;
  if (entry == nullptr) return kAndroidEinval;
  if (thread_state == nullptr) {
    // Clearing a valid key with no state is already satisfied.
    return value == nullptr ? 0 : kAndroidEnomem;
  }
  thread_state->values[slot].generation = entry->generation;
  thread_state->values[slot].value = const_cast<void*>(value);
  return 0;
}

extern "C" int darwin_art_bionic_pthread_once(
    DarwinArtAndroidPthreadOnce* once,
    DarwinArtAndroidOnceRoutine routine) {
  if (once == nullptr || routine == nullptr) return kAndroidEinval;
  const int32_t visible = __atomic_load_n(once, __ATOMIC_ACQUIRE);
  if (visible == kOnceComplete) return 0;
  if (visible != kOnceNotStarted && visible != kOnceUnderway) {
    return kAndroidEinval;
  }
  ProviderState& state = State();
  std::shared_ptr<OnceEntry> entry;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.once_controls.find(once);
    if (found == state.once_controls.end()) {
      if (visible != kOnceNotStarted) return kAndroidEinval;
      entry = std::make_shared<OnceEntry>();
      state.once_controls.emplace(once, entry);
    } else {
      entry = found->second;
    }
  }
  std::unique_lock<std::mutex> lock(entry->mutex);
  while (entry->state == kOnceUnderway) entry->condition.wait(lock);
  if (entry->state == kOnceComplete) return 0;
  entry->state = kOnceUnderway;
  __atomic_store_n(once, kOnceUnderway, __ATOMIC_RELEASE);
  lock.unlock();
  routine();
  lock.lock();
  entry->state = kOnceComplete;
  __atomic_store_n(once, kOnceComplete, __ATOMIC_RELEASE);
  lock.unlock();
  entry->condition.notify_all();
  return 0;
}

extern "C" int darwin_art_bionic_pthread_mutex_init(
    DarwinArtAndroidPthreadMutex* mutex,
    const void* android_attributes) {
  if (mutex == nullptr) return kAndroidEinval;
  if (android_attributes != nullptr) return kAndroidEnotsup;
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.mutexes.find(mutex);
  if (found != state.mutexes.end()) {
    std::lock_guard<std::mutex> lifecycle_lock(
        found->second->lifecycle_mutex);
    if (found->second->lifecycle != MutexEntry::Lifecycle::kDestroyed) {
      return kAndroidEbusy;
    }
  }
  auto entry = std::make_shared<MutexEntry>();
  const int result = pthread_mutex_init(&entry->host, nullptr);
  if (result != 0) return AndroidError(result);
  entry->host_initialized = true;
  std::memset(mutex, 0, sizeof(*mutex));
  state.mutexes[mutex] = entry;
  return 0;
}

extern "C" int darwin_art_bionic_pthread_mutex_lock(
    DarwinArtAndroidPthreadMutex* mutex) {
  int error = 0;
  std::shared_ptr<MutexEntry> entry = FindOrCreateMutex(mutex, &error);
  return entry == nullptr ? error : WithLiveMutex(entry, pthread_mutex_lock);
}

extern "C" int darwin_art_bionic_pthread_mutex_trylock(
    DarwinArtAndroidPthreadMutex* mutex) {
  int error = 0;
  std::shared_ptr<MutexEntry> entry = FindOrCreateMutex(mutex, &error);
  return entry == nullptr ? error : WithLiveMutex(entry, pthread_mutex_trylock);
}

extern "C" int darwin_art_bionic_pthread_mutex_unlock(
    DarwinArtAndroidPthreadMutex* mutex) {
  int error = 0;
  std::shared_ptr<MutexEntry> entry = FindOrCreateMutex(mutex, &error);
  // Unlock is the drain path for a lock operation that completed before a
  // concurrent destroy entered kDestroying. Blocking it would deadlock the
  // destroy waiter behind a still-held host mutex.
  return entry == nullptr ? error
                          : WithLiveMutex(entry, pthread_mutex_unlock, true);
}

extern "C" int darwin_art_bionic_pthread_mutex_destroy(
    DarwinArtAndroidPthreadMutex* mutex) {
  int error = 0;
  std::shared_ptr<MutexEntry> entry = FindOrCreateMutex(mutex, &error);
  if (entry == nullptr) return error;
  std::unique_lock<std::mutex> lock(entry->lifecycle_mutex);
  while (entry->lifecycle == MutexEntry::Lifecycle::kDestroying) {
    entry->lifecycle_condition.wait(lock);
  }
  if (entry->lifecycle == MutexEntry::Lifecycle::kDestroyed) {
    return kAndroidEbusy;
  }
  entry->lifecycle = MutexEntry::Lifecycle::kDestroying;
  while (entry->active_operations != 0) {
    entry->lifecycle_condition.wait(lock);
  }
  lock.unlock();
  const int result = pthread_mutex_destroy(&entry->host);
  lock.lock();
  if (result == 0) {
    entry->host_initialized = false;
    entry->lifecycle = MutexEntry::Lifecycle::kDestroyed;
    SetAndroidMutexState(mutex, kAndroidMutexDestroyed);
  } else {
    entry->lifecycle = MutexEntry::Lifecycle::kAlive;
  }
  lock.unlock();
  entry->lifecycle_condition.notify_all();
  return AndroidError(result);
}

extern "C" int darwin_art_bionic_pthread_cond_wait(
    DarwinArtAndroidPthreadCond* cond,
    DarwinArtAndroidPthreadMutex* mutex) {
  return CondWait(cond, mutex, nullptr);
}

extern "C" int darwin_art_bionic_pthread_cond_timedwait(
    DarwinArtAndroidPthreadCond* cond,
    DarwinArtAndroidPthreadMutex* mutex,
    const timespec* absolute_timeout) {
  return CondWait(cond, mutex, absolute_timeout);
}

extern "C" int darwin_art_bionic_pthread_cond_signal(
    DarwinArtAndroidPthreadCond* cond) {
  return CondPulse(cond, false);
}

extern "C" int darwin_art_bionic_pthread_cond_broadcast(
    DarwinArtAndroidPthreadCond* cond) {
  return CondPulse(cond, true);
}

extern "C" int darwin_art_bionic_pthread_cond_destroy(
    DarwinArtAndroidPthreadCond* visible_cond) {
  int error = 0;
  std::shared_ptr<CondEntry> cond = FindOrCreateCond(visible_cond, &error);
  if (cond == nullptr) return error;
  std::unique_lock<std::mutex> lock(cond->lifecycle_mutex);
  if (cond->lifecycle != CondEntry::Lifecycle::kAlive) return kAndroidEbusy;
  // POSIX makes destruction with waiters undefined and Bionic does not detect
  // it. The facade defines a safe Android-errno failure instead of destroying
  // host storage beneath a blocked Android thread.
  if (cond->active_operations != 0 || cond->waiters != 0) return kAndroidEbusy;
  cond->lifecycle = CondEntry::Lifecycle::kDestroying;
  lock.unlock();
  const int result = pthread_cond_destroy(&cond->host);
  lock.lock();
  if (result == 0) {
    cond->host_initialized = false;
    cond->lifecycle = CondEntry::Lifecycle::kDestroyed;
    SetAndroidCondState(visible_cond, kAndroidCondDestroyed);
    SetAndroidCondWaiters(visible_cond, 0);
  } else {
    cond->lifecycle = CondEntry::Lifecycle::kAlive;
  }
  lock.unlock();
  cond->lifecycle_condition.notify_all();
  return AndroidError(result);
}

extern "C" int darwin_art_bionic_pthread_rwlock_rdlock(
    DarwinArtAndroidPthreadRwlock* visible) {
  int error = 0;
  std::shared_ptr<RwlockEntry> rwlock = FindOrCreateRwlock(visible, &error);
  if (rwlock == nullptr) return error;
  const DarwinArtAndroidPthread self = CurrentThreadToken();
  std::unique_lock<std::mutex> lock(rwlock->mutex);
  if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
    return kAndroidEbusy;
  }
  if (rwlock->writer_owner == self) return kAndroidEdeadlk;
  if (rwlock->writer_owner != 0) {
    ++rwlock->pending_readers;
    SetAndroidRwlockVisible(visible, *rwlock);
    rwlock->readers_condition.wait(lock, [&] {
      return rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive ||
             rwlock->writer_owner == 0;
    });
    --rwlock->pending_readers;
    if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
      SetAndroidRwlockVisible(visible, *rwlock);
      return kAndroidEbusy;
    }
  }
  if (rwlock->active_readers == kAndroidRwlockMaxReaders) {
    SetAndroidRwlockVisible(visible, *rwlock);
    return kAndroidEagain;
  }
  ++rwlock->active_readers;
  SetAndroidRwlockVisible(visible, *rwlock);
  return 0;
}

extern "C" int darwin_art_bionic_pthread_rwlock_wrlock(
    DarwinArtAndroidPthreadRwlock* visible) {
  int error = 0;
  std::shared_ptr<RwlockEntry> rwlock = FindOrCreateRwlock(visible, &error);
  if (rwlock == nullptr) return error;
  const DarwinArtAndroidPthread self = CurrentThreadToken();
  std::unique_lock<std::mutex> lock(rwlock->mutex);
  if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
    return kAndroidEbusy;
  }
  if (rwlock->writer_owner == self) return kAndroidEdeadlk;
  if (rwlock->writer_owner != 0 || rwlock->active_readers != 0) {
    ++rwlock->pending_writers;
    SetAndroidRwlockVisible(visible, *rwlock);
    rwlock->writers_condition.wait(lock, [&] {
      return rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive ||
             (rwlock->writer_owner == 0 && rwlock->active_readers == 0);
    });
    --rwlock->pending_writers;
    if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
      SetAndroidRwlockVisible(visible, *rwlock);
      return kAndroidEbusy;
    }
  }
  rwlock->writer_owner = self;
  SetAndroidRwlockVisible(visible, *rwlock);
  return 0;
}

extern "C" int darwin_art_bionic_pthread_rwlock_unlock(
    DarwinArtAndroidPthreadRwlock* visible) {
  int error = 0;
  std::shared_ptr<RwlockEntry> rwlock = FindOrCreateRwlock(visible, &error);
  if (rwlock == nullptr) return error;
  const DarwinArtAndroidPthread self = CurrentThreadToken();
  std::unique_lock<std::mutex> lock(rwlock->mutex);
  if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
    return kAndroidEbusy;
  }
  if (rwlock->writer_owner != 0) {
    if (rwlock->writer_owner != self) return 1;
    rwlock->writer_owner = 0;
  } else if (rwlock->active_readers != 0) {
    // Pinned Bionic tracks only a global reader count, not reader thread ids.
    --rwlock->active_readers;
  } else {
    return 1;
  }
  SetAndroidRwlockVisible(visible, *rwlock);
  const bool wake_writer =
      rwlock->writer_owner == 0 && rwlock->active_readers == 0 &&
      rwlock->pending_writers != 0;
  const bool wake_readers = rwlock->writer_owner == 0 &&
                            rwlock->pending_writers == 0 &&
                            rwlock->pending_readers != 0;
  lock.unlock();
  if (wake_writer) {
    rwlock->writers_condition.notify_one();
  } else if (wake_readers) {
    rwlock->readers_condition.notify_all();
  }
  return 0;
}

extern "C" int darwin_art_bionic_pthread_rwlock_destroy(
    DarwinArtAndroidPthreadRwlock* visible) {
  int error = 0;
  std::shared_ptr<RwlockEntry> rwlock = FindOrCreateRwlock(visible, &error);
  if (rwlock == nullptr) return error;
  std::lock_guard<std::mutex> lock(rwlock->mutex);
  if (rwlock->lifecycle != RwlockEntry::Lifecycle::kAlive) {
    return kAndroidEbusy;
  }
  if (rwlock->writer_owner != 0 || rwlock->active_readers != 0 ||
      rwlock->pending_readers != 0 || rwlock->pending_writers != 0) {
    return kAndroidEbusy;
  }
  rwlock->lifecycle = RwlockEntry::Lifecycle::kDestroyed;
  // Bionic pthread_rwlock_destroy leaves an idle object's zero bytes intact.
  SetAndroidRwlockVisible(visible, *rwlock);
  return 0;
}

extern "C" void* darwin_art_bionic_pthread_resolve(const char* soname,
                                                    const char* symbol,
                                                    const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::string_view(soname) != "libc.so" ||
      std::string_view(version) != "LIBC") {
    return nullptr;
  }
#define RESOLVE(name)                                                        \
  if (std::string_view(symbol) == "pthread_" #name)                         \
    return reinterpret_cast<void*>(&darwin_art_bionic_pthread_##name)
  RESOLVE(self);
  RESOLVE(key_create);
  RESOLVE(key_delete);
  RESOLVE(getspecific);
  RESOLVE(setspecific);
  RESOLVE(once);
  RESOLVE(mutex_init);
  RESOLVE(mutex_lock);
  RESOLVE(mutex_trylock);
  RESOLVE(mutex_unlock);
  RESOLVE(mutex_destroy);
  RESOLVE(cond_wait);
  RESOLVE(cond_timedwait);
  RESOLVE(cond_signal);
  RESOLVE(cond_broadcast);
  RESOLVE(cond_destroy);
  RESOLVE(rwlock_rdlock);
  RESOLVE(rwlock_wrlock);
  RESOLVE(rwlock_unlock);
#undef RESOLVE
  return nullptr;
}

extern "C" int darwin_art_bionic_pthread_provider_reset(void) {
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (const KeySlot& slot : state.key_slots) {
    if (slot.active != nullptr) return kAndroidEbusy;
  }
  for (const auto& [address, entry] : state.mutexes) {
    (void)address;
    std::lock_guard<std::mutex> lifecycle_lock(entry->lifecycle_mutex);
    if (entry->lifecycle != MutexEntry::Lifecycle::kDestroyed) {
      return kAndroidEbusy;
    }
  }
  for (const auto& [address, entry] : state.conditions) {
    (void)address;
    std::lock_guard<std::mutex> lifecycle_lock(entry->lifecycle_mutex);
    if (entry->lifecycle != CondEntry::Lifecycle::kDestroyed) {
      return kAndroidEbusy;
    }
  }
  for (const auto& [address, entry] : state.rwlocks) {
    (void)address;
    std::lock_guard<std::mutex> lifecycle_lock(entry->mutex);
    if (entry->writer_owner != 0 || entry->active_readers != 0 ||
        entry->pending_readers != 0 || entry->pending_writers != 0) {
      return kAndroidEbusy;
    }
  }
  ThreadTlsState* current = tls_destructor_state;
  if (current == nullptr) {
    pthread_once(&HostThreadTlsKeyOnce(), &InitializeHostThreadTlsKey);
    current = static_cast<ThreadTlsState*>(
        pthread_getspecific(HostThreadTlsKey()));
  }
  for (ThreadTlsState* thread_state : state.thread_tls_states) {
    if (thread_state != current) return kAndroidEbusy;
  }
  if (current != nullptr) {
    if (tls_destructor_state != nullptr) return kAndroidEbusy;
    if (pthread_setspecific(HostThreadTlsKey(), nullptr) != 0) {
      return kAndroidEbusy;
    }
    state.thread_tls_states.erase(current);
    delete current;
  }
  state.mutexes.clear();
  state.conditions.clear();
  state.rwlocks.clear();
  state.once_controls.clear();
  return 0;
}

extern "C" size_t darwin_art_bionic_pthread_provider_retired_cell_count(void) {
  ProviderState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  size_t count = 0;
  for (const ThreadTlsState* thread_state : state.thread_tls_states) {
    for (const ThreadTlsValue& value : thread_state->values) {
      if (value.value != nullptr) ++count;
    }
  }
  return count;
}

extern "C" int darwin_art_bionic_pthread_capability(const char* capability) {
  if (capability == nullptr) return 0;
  const std::string_view name(capability);
  return name == "thread-identity-token" || name == "tls-key-destructor" ||
         name == "once-private" || name == "mutex-normal-private" ||
         name == "cond-private" || name == "cond-monotonic-clock" ||
         name == "rwlock-private-reader-preferred";
}
