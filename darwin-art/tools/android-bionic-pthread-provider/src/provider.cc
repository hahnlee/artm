#include "darwin_art_bionic_pthread.h"

#include <pthread.h>

#include <atomic>
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

constexpr int kAndroidEagain = 11;
constexpr int kAndroidEnomem = 12;
constexpr int kAndroidEbusy = 16;
constexpr int kAndroidEinval = 22;
constexpr int kAndroidEdeadlk = 35;
constexpr int kAndroidEnotsup = 95;
constexpr uint32_t kAndroidKeyValid = UINT32_C(0x80000000);
constexpr uint32_t kAndroidKeySlots = 128;
constexpr int kAndroidDestructorIterations = 4;
constexpr uint16_t kAndroidMutexDestroyed = UINT16_C(0xffff);
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

struct ProviderState {
  std::mutex mutex;
  KeySlot key_slots[kAndroidKeySlots];
  std::unordered_set<ThreadTlsState*> thread_tls_states;
  std::unordered_map<DarwinArtAndroidPthreadMutex*, std::shared_ptr<MutexEntry>>
      mutexes;
  std::unordered_map<DarwinArtAndroidPthreadOnce*, std::shared_ptr<OnceEntry>>
      once_controls;
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
         name == "once-private" || name == "mutex-normal-private";
}
