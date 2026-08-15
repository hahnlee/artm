#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
  kThreadCount = 8,
  kIterations = 2000,
  kAndroidEbusy = 16,
  kAndroidEinval = 22,
  kAndroidEnotsup = 95,
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_static_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_destroy_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_explicit_mutex;
static pthread_key_t g_key;
static _Atomic int g_once_calls;
static _Atomic int g_worker_slots;
static _Atomic int g_destructor_calls;
static _Atomic int g_destructor_second_passes;
static _Atomic int g_destroy_race_complete;
static _Atomic long g_thread_tokens[kThreadCount];
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
  return 0;
}
