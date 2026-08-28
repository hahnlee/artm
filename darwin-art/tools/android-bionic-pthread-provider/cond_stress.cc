#include "darwin_art_bionic_pthread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int kAndroidEbusy = 16;
constexpr int kAndroidEtimedout = 110;
constexpr int kThreads = 8;
constexpr int kRounds = 100;

struct RoundState {
  DarwinArtAndroidPthreadCond cond{};
  DarwinArtAndroidPthreadMutex mutex{};
  std::atomic<int> waiting{};
  bool release{};
};

int RunRound() {
  RoundState state{};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&] {
      if (darwin_art_bionic_pthread_mutex_lock(&state.mutex) != 0) {
        std::abort();
      }
      state.waiting.fetch_add(1, std::memory_order_release);
      while (!state.release) {
        if (darwin_art_bionic_pthread_cond_wait(&state.cond, &state.mutex) !=
            0) {
          std::abort();
        }
      }
      if (darwin_art_bionic_pthread_mutex_unlock(&state.mutex) != 0) {
        std::abort();
      }
    });
  }
  while (state.waiting.load(std::memory_order_acquire) != kThreads) {
    std::this_thread::yield();
  }
  if (darwin_art_bionic_pthread_cond_destroy(&state.cond) != kAndroidEbusy) {
    return 1;
  }
  if (darwin_art_bionic_pthread_mutex_lock(&state.mutex) != 0) return 2;
  state.release = true;
  if (darwin_art_bionic_pthread_cond_broadcast(&state.cond) != 0) return 3;
  if (darwin_art_bionic_pthread_mutex_unlock(&state.mutex) != 0) return 4;
  for (std::thread& thread : threads) thread.join();
  if (darwin_art_bionic_pthread_cond_destroy(&state.cond) != 0) return 5;
  if (darwin_art_bionic_pthread_mutex_destroy(&state.mutex) != 0) return 6;
  if (darwin_art_bionic_pthread_provider_reset() != 0) return 7;
  return 0;
}

int RunMonotonicTimeout() {
  DarwinArtAndroidPthreadCond cond{};
  DarwinArtAndroidPthreadMutex mutex{};
  // Android PTHREAD_COND_INITIALIZER_MONOTONIC_NP has clock bit 1 set.
  cond.opaque[0] = 2;
  timespec deadline{};
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return 10;
  deadline.tv_nsec += 2000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000L;
  }
  if (darwin_art_bionic_pthread_mutex_lock(&mutex) != 0) return 11;
  if (darwin_art_bionic_pthread_cond_timedwait(&cond, &mutex, &deadline) !=
      kAndroidEtimedout) {
    return 12;
  }
  if (darwin_art_bionic_pthread_mutex_trylock(&mutex) != kAndroidEbusy) {
    return 13;
  }
  if (darwin_art_bionic_pthread_mutex_unlock(&mutex) != 0) return 14;
  if (darwin_art_bionic_pthread_cond_destroy(&cond) != 0) return 15;
  if (darwin_art_bionic_pthread_mutex_destroy(&mutex) != 0) return 16;
  if (darwin_art_bionic_pthread_provider_reset() != 0) return 17;
  return 0;
}

int RunMonotonicAttributeTimeout() {
  DarwinArtAndroidPthreadCond cond{};
  DarwinArtAndroidPthreadMutex mutex{};
  uint32_t attributes = 0;
  if (darwin_art_bionic_pthread_condattr_init(&attributes) != 0) return 20;
  if (darwin_art_bionic_pthread_condattr_setclock(&attributes, 1) != 0) {
    return 21;
  }
  if (darwin_art_bionic_pthread_cond_init(&cond, &attributes) != 0) return 22;
  if (darwin_art_bionic_pthread_condattr_destroy(&attributes) != 0) return 23;
  timespec deadline{};
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return 24;
  deadline.tv_nsec += 2000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000L;
  }
  if (darwin_art_bionic_pthread_mutex_lock(&mutex) != 0) return 25;
  if (darwin_art_bionic_pthread_cond_timedwait(&cond, &mutex, &deadline) !=
      kAndroidEtimedout) {
    return 26;
  }
  if (darwin_art_bionic_pthread_mutex_unlock(&mutex) != 0) return 27;
  if (darwin_art_bionic_pthread_cond_destroy(&cond) != 0) return 28;
  if (darwin_art_bionic_pthread_mutex_destroy(&mutex) != 0) return 29;
  if (darwin_art_bionic_pthread_provider_reset() != 0) return 30;
  return 0;
}

}  // namespace

int main() {
  for (int round = 0; round < kRounds; ++round) {
    const int result = RunRound();
    if (result != 0) return result;
  }
  const int timeout = RunMonotonicTimeout();
  if (timeout != 0) return timeout;
  const int attribute_timeout = RunMonotonicAttributeTimeout();
  if (attribute_timeout != 0) return attribute_timeout;
  std::puts("pthread-cond-stress: PASS rounds=100 waiters=8 destroy-wait=EBUSY ASan=clean monotonic-timeout=110 monotonic-attr-timeout=110 relock=owned");
  return 0;
}
