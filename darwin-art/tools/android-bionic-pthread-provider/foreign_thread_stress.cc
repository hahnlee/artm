#include "darwin_art_bionic_pthread.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include <unistd.h>

// Standalone provider probe: semaphore entry points require the errno sink.
extern "C" void darwin_art_bionic_errno_store(int32_t value) { errno = value; }

namespace {
std::atomic<unsigned> signals{0};
std::atomic<bool> wrong_recipient{false};
pthread_t expected_recipient;
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void SignalHandler(int) {
  if (!pthread_equal(pthread_self(), expected_recipient))
    wrong_recipient.store(true, std::memory_order_relaxed);
  signals.fetch_add(1, std::memory_order_release);
}

template <class Predicate> bool Wait(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::yield();
  }
  return true;
}

bool Round() {
  std::atomic<DarwinArtAndroidPthread> published{0};
  std::atomic<bool> release{false};
  uintptr_t stack_address = 0;
  std::thread foreign([&] {
    int local = 0;
    expected_recipient = pthread_self();
    stack_address = reinterpret_cast<uintptr_t>(&local);
    published.store(darwin_art_bionic_pthread_self(), std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
  });
  bool ok = Wait([&] { return published.load(std::memory_order_acquire) != 0; });
  const auto token = published.load(std::memory_order_acquire);
  const auto caller = darwin_art_bionic_pthread_self();
  const int live = darwin_art_bionic_pthread_kill(token, 0);
  DarwinArtAndroidPthreadAttr attributes{};
  const int attr = darwin_art_bionic_pthread_getattr_np(token, &attributes);
  void* base = attributes.stack_base;
  size_t size = attributes.stack_size;
  sched_param parameters{};
  int policy = 0;
  const int scheduling = darwin_art_bionic_pthread_getschedparam(token, &policy, &parameters);
  const int join = darwin_art_bionic_pthread_join(token, nullptr);
  const int detach = darwin_art_bionic_pthread_detach(token);
  const int busy = darwin_art_bionic_pthread_provider_reset();
  ok &= live == 0 && attr == 0 && scheduling == 0 &&
        join == 22 && detach == 22 && busy == 16 &&
        darwin_art_bionic_pthread_self() == caller &&
        darwin_art_bionic_pthread_kill(token, 0) == 0;
  const uintptr_t low = reinterpret_cast<uintptr_t>(base);
  ok &= stack_address >= low && stack_address - low < size;
  const unsigned before = signals.load(std::memory_order_acquire);
  const int delivery = darwin_art_bionic_pthread_kill(token, 10);  // Android SIGUSR1.
  ok &= delivery == 0;
  if (delivery == 0)
    ok &= Wait([&] { return signals.load(std::memory_order_acquire) == before + 1; });
  release.store(true, std::memory_order_release);
  foreign.join();
  const int stale = darwin_art_bionic_pthread_kill(token, 0);
  const int stale_attr = darwin_art_bionic_pthread_getattr_np(token, &attributes);
  const int stale_sched = darwin_art_bionic_pthread_getschedparam(token, &policy, &parameters);
  const int reset = darwin_art_bionic_pthread_provider_reset();
  ok &= stale == 3 && stale_attr == 3 && stale_sched == 3 && reset == 0 &&
        !wrong_recipient.load(std::memory_order_acquire);
  if (!ok)
    std::fprintf(stderr, "foreign-thread: live=%d attr=%d sched=%d join=%d detach=%d busy=%d signal=%d stale=%d/%d/%d reset=%d\n",
                 live, attr, scheduling, join, detach, busy, delivery,
                 stale, stale_attr, stale_sched, reset);
  return ok;
}
}  // namespace

int main() {
  alarm(20);
  struct sigaction action{}, previous{};
  action.sa_handler = SignalHandler;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, &previous) != 0) return 2;
  bool ok = true;
  for (unsigned round = 0; round < 64 && ok; ++round) ok = Round();
  sigaction(SIGUSR1, &previous, nullptr);
  alarm(0);
  if (!ok) return 1;
  std::puts("foreign-thread: PASS rounds=64 signal=actual stack=valid ownership=preserved stale=ESRCH reset=transactional");
  return 0;
}
