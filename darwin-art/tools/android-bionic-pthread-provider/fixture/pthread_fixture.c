#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
  kThreadCount = 8,
  kIterations = 2000,
  kCondWaiters = 4,
  kAndroidEbusy = 16,
  kAndroidEinval = 22,
  kAndroidEnotsup = 95,
  kAndroidEtimedout = 110,
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_static_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_destroy_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_explicit_mutex;
static pthread_key_t g_key;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_monotonic_cond = PTHREAD_COND_INITIALIZER_MONOTONIC_NP;
static pthread_cond_t g_pshared_cond = {{1}};
static pthread_mutex_t g_cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_monotonic_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t g_rwlock_pshared = {{0, 0, 1}};
static _Atomic int g_once_calls;
static _Atomic int g_worker_slots;
static _Atomic int g_destructor_calls;
static _Atomic int g_destructor_second_passes;
static _Atomic int g_destroy_race_complete;
static _Atomic long g_thread_tokens[kThreadCount];
static _Atomic int g_cond_waiting;
static _Atomic int g_cond_completed;
static int g_cond_tickets;
static int g_cond_broadcast;
static _Atomic int g_rw_reader_entries;
static _Atomic int g_rw_active_readers;
static _Atomic int g_rw_max_readers;
static _Atomic int g_rw_release_readers;
static _Atomic int g_rw_writer_entered;
static int g_rw_value;
static int g_counter;

static void OnceRoutine(void) {
  atomic_fetch_add_explicit(&g_once_calls, 1, memory_order_relaxed);
}

static void TlsDestructor(void* value) {
  atomic_fetch_add_explicit(&g_destructor_calls, 1, memory_order_relaxed);
  if ((uintptr_t)value == 1) {
    if (pthread_setspecific(g_key, (void*)(uintptr_t)2) != 0) {
      atomic_store_explicit(&g_destructor_second_passes, -1000,
                            memory_order_relaxed);
    }
  } else if ((uintptr_t)value == 2) {
    atomic_fetch_add_explicit(&g_destructor_second_passes, 1,
                              memory_order_relaxed);
  }
}

__attribute__((visibility("default"))) int pthread_fixture_setup(void) {
  pthread_mutexattr_t unsupported_attributes = 0;
  if (pthread_mutex_init(&g_explicit_mutex, &unsupported_attributes) !=
      kAndroidEnotsup) {
    return -1;
  }
  if (pthread_mutex_init(&g_explicit_mutex, 0) != 0) return -2;
  if (pthread_mutex_trylock(&g_explicit_mutex) != 0) return -3;
  if (pthread_mutex_unlock(&g_explicit_mutex) != 0) return -4;
  if (pthread_key_create(&g_key, &TlsDestructor) != 0) return -5;
  if (pthread_self() == 0 || pthread_self() != pthread_self()) return -6;
  if (pthread_cond_signal(&g_pshared_cond) != kAndroidEnotsup) return -7;
  if (pthread_rwlock_rdlock(&g_rwlock_pshared) != kAndroidEnotsup) return -8;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_worker(void) {
  const int slot = atomic_fetch_add_explicit(&g_worker_slots, 1,
                                              memory_order_relaxed);
  if (slot < 0 || slot >= kThreadCount) return -10;
  const pthread_t first = pthread_self();
  if (first == 0 || first != pthread_self()) return -11;
  atomic_store_explicit(&g_thread_tokens[slot], first, memory_order_relaxed);

  if (pthread_once(&g_once, &OnceRoutine) != 0) return -12;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    if (pthread_mutex_lock(&g_static_mutex) != 0) return -13;
    if (iteration == 0 &&
        pthread_mutex_trylock(&g_static_mutex) != kAndroidEbusy) {
      return -14;
    }
    ++g_counter;
    if (pthread_mutex_unlock(&g_static_mutex) != 0) return -15;
  }
  if (pthread_setspecific(g_key, (void*)(uintptr_t)1) != 0) return -16;
  if ((uintptr_t)pthread_getspecific(g_key) != 1) return -17;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_mutex_lookup_race(void) {
  for (int iteration = 0; iteration < 10000; ++iteration) {
    const int locked = pthread_mutex_lock(&g_destroy_race_mutex);
    if (locked == kAndroidEbusy) return 0;
    if (locked != 0) return -40;
    const int unlocked = pthread_mutex_unlock(&g_destroy_race_mutex);
    if (unlocked != 0) return -41;
  }
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_mutex_destroy_race(void) {
  for (int attempt = 0; attempt < 100000; ++attempt) {
    const int result = pthread_mutex_destroy(&g_destroy_race_mutex);
    if (result == 0) {
      atomic_store_explicit(&g_destroy_race_complete, 1, memory_order_relaxed);
      return 0;
    }
    if (result != kAndroidEbusy) return -42;
  }
  return -43;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_waiter(void) {
  if (pthread_mutex_lock(&g_cond_mutex) != 0) return -50;
  atomic_fetch_add_explicit(&g_cond_waiting, 1, memory_order_release);
  while (g_cond_tickets == 0 && !g_cond_broadcast) {
    if (pthread_cond_wait(&g_cond, &g_cond_mutex) != 0) return -51;
  }
  if (g_cond_tickets != 0) --g_cond_tickets;
  atomic_fetch_add_explicit(&g_cond_completed, 1, memory_order_release);
  if (pthread_mutex_unlock(&g_cond_mutex) != 0) return -52;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_waiting(void) {
  return atomic_load_explicit(&g_cond_waiting, memory_order_acquire);
}

__attribute__((visibility("default"))) int pthread_fixture_cond_completed(void) {
  return atomic_load_explicit(&g_cond_completed, memory_order_acquire);
}

__attribute__((visibility("default"))) int pthread_fixture_cond_spurious_signal(void) {
  if (pthread_mutex_lock(&g_cond_mutex) != 0) return -53;
  const int result = pthread_cond_signal(&g_cond);
  if (pthread_mutex_unlock(&g_cond_mutex) != 0) return -54;
  return result == 0 ? 0 : -55;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_signal_one(void) {
  if (pthread_mutex_lock(&g_cond_mutex) != 0) return -56;
  ++g_cond_tickets;
  const int result = pthread_cond_signal(&g_cond);
  if (pthread_mutex_unlock(&g_cond_mutex) != 0) return -57;
  return result == 0 ? 0 : -58;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_broadcast(void) {
  if (pthread_mutex_lock(&g_cond_mutex) != 0) return -59;
  g_cond_broadcast = 1;
  const int result = pthread_cond_broadcast(&g_cond);
  if (pthread_mutex_unlock(&g_cond_mutex) != 0) return -60;
  return result == 0 ? 0 : -61;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_destroy_busy(void) {
  return pthread_cond_destroy(&g_cond) == kAndroidEbusy ? 0 : -62;
}

__attribute__((visibility("default"))) int pthread_fixture_cond_timedwait(void) {
  const struct timespec absolute_monotonic_past = {0, 0};
  if (pthread_mutex_lock(&g_monotonic_mutex) != 0) return -63;
  if (pthread_cond_timedwait(&g_monotonic_cond, &g_monotonic_mutex,
                             &absolute_monotonic_past) != kAndroidEtimedout) {
    return -64;
  }
  if (pthread_mutex_trylock(&g_monotonic_mutex) != kAndroidEbusy) return -65;
  if (pthread_mutex_unlock(&g_monotonic_mutex) != 0) return -66;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_recursive_read(void) {
  if (pthread_rwlock_rdlock(&g_rwlock) != 0) return -80;
  if (pthread_rwlock_rdlock(&g_rwlock) != 0) return -81;
  if (pthread_rwlock_unlock(&g_rwlock) != 0) return -82;
  if (pthread_rwlock_unlock(&g_rwlock) != 0) return -83;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_reader(void) {
  if (pthread_rwlock_rdlock(&g_rwlock) != 0) return -84;
  const int active =
      atomic_fetch_add_explicit(&g_rw_active_readers, 1, memory_order_acq_rel) + 1;
  int maximum = atomic_load_explicit(&g_rw_max_readers, memory_order_relaxed);
  while (active > maximum &&
         !atomic_compare_exchange_weak_explicit(
             &g_rw_max_readers, &maximum, active, memory_order_relaxed,
             memory_order_relaxed)) {
  }
  atomic_fetch_add_explicit(&g_rw_reader_entries, 1, memory_order_release);
  while (atomic_load_explicit(&g_rw_release_readers, memory_order_acquire) == 0) {
  }
  atomic_fetch_sub_explicit(&g_rw_active_readers, 1, memory_order_acq_rel);
  if (pthread_rwlock_unlock(&g_rwlock) != 0) return -85;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_reader_entries(void) {
  return atomic_load_explicit(&g_rw_reader_entries, memory_order_acquire);
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_writer(void) {
  if (pthread_rwlock_wrlock(&g_rwlock) != 0) return -86;
  ++g_rw_value;
  atomic_store_explicit(&g_rw_writer_entered, 1, memory_order_release);
  if (pthread_rwlock_unlock(&g_rwlock) != 0) return -87;
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_writer_entered(void) {
  return atomic_load_explicit(&g_rw_writer_entered, memory_order_acquire);
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_release_readers(void) {
  atomic_store_explicit(&g_rw_release_readers, 1, memory_order_release);
  return 0;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_writer_hold(void) {
  return pthread_rwlock_wrlock(&g_rwlock) == 0 ? 0 : -88;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_wrong_unlock(void) {
  return pthread_rwlock_unlock(&g_rwlock) == 1 ? 0 : -89;
}

__attribute__((visibility("default"))) int pthread_fixture_rwlock_writer_release(void) {
  return pthread_rwlock_unlock(&g_rwlock) == 0 ? 0 : -90;
}

__attribute__((visibility("default"))) int pthread_fixture_finish(void) {
  if (atomic_load_explicit(&g_worker_slots, memory_order_relaxed) !=
      kThreadCount) {
    return -20;
  }
  if (atomic_load_explicit(&g_once_calls, memory_order_relaxed) != 1) return -21;
  if (g_counter != kThreadCount * kIterations) return -22;
  for (int left = 0; left < kThreadCount; ++left) {
    const long token = atomic_load_explicit(&g_thread_tokens[left],
                                            memory_order_relaxed);
    if (token == 0) return -23;
    for (int right = left + 1; right < kThreadCount; ++right) {
      if (token == atomic_load_explicit(&g_thread_tokens[right],
                                        memory_order_relaxed)) {
        return -24;
      }
    }
  }
  if (atomic_load_explicit(&g_destructor_calls, memory_order_relaxed) !=
      kThreadCount * 2) {
    return -25;
  }
  if (atomic_load_explicit(&g_destructor_second_passes, memory_order_relaxed) !=
      kThreadCount) {
    return -26;
  }
  if (atomic_load_explicit(&g_destroy_race_complete, memory_order_relaxed) != 1) {
    return -34;
  }
  if (pthread_mutex_trylock(&g_static_mutex) != 0) return -27;
  if (pthread_mutex_unlock(&g_static_mutex) != 0) return -28;
  if (pthread_mutex_destroy(&g_static_mutex) != 0) return -29;
  if (pthread_mutex_destroy(&g_explicit_mutex) != 0) return -30;
  if (pthread_key_delete(g_key) != 0) return -31;
  if (pthread_getspecific(g_key) != 0) return -32;
  if (pthread_key_delete(g_key) != kAndroidEinval) return -33;
  if (atomic_load_explicit(&g_cond_waiting, memory_order_acquire) !=
          kCondWaiters ||
      atomic_load_explicit(&g_cond_completed, memory_order_acquire) !=
          kCondWaiters) {
    return -67;
  }
  if (pthread_cond_destroy(&g_cond) != 0) return -68;
  if (pthread_cond_destroy(&g_cond) != kAndroidEbusy) return -69;
  if (pthread_cond_destroy(&g_monotonic_cond) != 0) return -70;
  if (pthread_mutex_destroy(&g_cond_mutex) != 0) return -71;
  if (pthread_mutex_destroy(&g_monotonic_mutex) != 0) return -72;
  if (atomic_load_explicit(&g_rw_max_readers, memory_order_relaxed) < 2)
    return -91;
  if (atomic_load_explicit(&g_rw_writer_entered, memory_order_acquire) != 1 ||
      g_rw_value != 1)
    return -92;
  if (pthread_rwlock_unlock(&g_rwlock) != 1) return -93;
  return 0;
}
