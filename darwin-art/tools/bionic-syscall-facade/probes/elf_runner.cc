#include "darwin_art_bionic_errno.h"
#include "darwin_art_bionic_syscall.h"
#include "darwin_art_elf_loader.h"

#include <errno.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <vector>

namespace {

DarwinArtElfResolveStatus Resolve(void*,
                                  const DarwinArtElfSymbolRequest* request,
                                  uintptr_t* output,
                                  DarwinArtElfErrorBuffer*) {
  if (request == nullptr || output == nullptr) {
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  const auto function = darwin_art_bionic_syscall_resolve(
      request->version_soname, request->symbol, request->version_name);
  if (function == nullptr) return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
  *output = reinterpret_cast<uintptr_t>(function);
  return DARWIN_ART_ELF_RESOLVE_FOUND;
}

template <typename Function>
Function Lookup(DarwinArtElfHandle* image, const char* name) {
  uintptr_t address = 0;
  char message[256]{};
  DarwinArtElfErrorBuffer error{message, sizeof(message), 0};
  if (darwin_art_elf_lookup(image, name, &address, &error) !=
          DARWIN_ART_ELF_OK ||
      address == 0) {
    std::fprintf(stderr, "lookup %s: %s\n", name, message);
    std::abort();
  }
  return reinterpret_cast<Function>(address);
}

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "bionic-syscall-facade: FAIL %s\n", message);
    std::abort();
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 10;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  if (bytes.empty() || input.bad()) return 11;

  DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION, Resolve, nullptr};
  DarwinArtElfHandle* image = nullptr;
  char message[512]{};
  DarwinArtElfErrorBuffer error{message, sizeof(message), 0};
  Check(darwin_art_elf_load_bytes(bytes.data(), bytes.size(), &options, &image,
                                  &error) == DARWIN_ART_ELF_OK,
        "load Android ELF");
  Check(darwin_art_elf_run_initializers(image, &error) == DARWIN_ART_ELF_OK,
        "run initializers");

  using Gettid = long (*)();
  using Wait = long (*)(int32_t*, int32_t, int64_t);
  using Wake = long (*)(int32_t*);
  using Readable = long (*)(const void*);
  Gettid gettid = Lookup<Gettid>(image, "SyscallFixtureGettid");
  Wait wait = Lookup<Wait>(image, "SyscallFixtureWait");
  Wake wake_one = Lookup<Wake>(image, "SyscallFixtureWakeOne");
  Wake wake_all = Lookup<Wake>(image, "SyscallFixtureWakeAll");
  Readable readable = Lookup<Readable>(image, "SyscallFixtureReadable");
  Gettid unknown = Lookup<Gettid>(image, "SyscallFixtureUnknown");
  Wake bad_futex = Lookup<Wake>(image, "SyscallFixtureBadFutex");

  errno = EDOM;
  const long main_tid = gettid();
  Check(main_tid > 0 && gettid() == main_tid && errno == EDOM,
        "stable main gettid and host errno");
  std::vector<long> tids(8);
  std::vector<std::thread> tid_threads;
  for (size_t index = 0; index < tids.size(); ++index) {
    tid_threads.emplace_back([&, index] {
      const long first = gettid();
      const long second = gettid();
      Check(first > 0 && first == second, "stable worker gettid");
      tids[index] = first;
    });
  }
  for (std::thread& thread : tid_threads) thread.join();
  std::set<long> distinct(tids.begin(), tids.end());
  Check(distinct.size() == tids.size() && distinct.count(main_tid) == 0,
        "unique per-thread gettid");

  alignas(4) int32_t word = 0;
  constexpr size_t kWaiters = 4;
  std::vector<long> wait_results(kWaiters, -2);
  std::vector<int32_t> wait_errnos(kWaiters, -1);
  std::vector<std::thread> wait_threads;
  for (size_t index = 0; index < kWaiters; ++index) {
    wait_threads.emplace_back([&, index] {
      darwin_art_bionic_errno_store(0);
      wait_results[index] = wait(&word, 0, 2000000000LL);
      wait_errnos[index] = darwin_art_bionic_errno_load();
    });
  }
  const auto waiter_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (darwin_art_bionic_syscall_waiter_count(&word) != kWaiters &&
         std::chrono::steady_clock::now() < waiter_deadline) {
    std::this_thread::yield();
  }
  Check(darwin_art_bionic_syscall_waiter_count(&word) == kWaiters,
        "contention admission");
  __atomic_store_n(&word, 1, __ATOMIC_SEQ_CST);
  Check(wake_all(&word) == static_cast<long>(kWaiters), "wake all count");
  for (std::thread& thread : wait_threads) thread.join();
  for (size_t index = 0; index < kWaiters; ++index) {
    Check(wait_results[index] == 0 && wait_errnos[index] == 0,
          "wake all result");
  }

  word = 0;
  std::atomic<int> completed{0};
  std::thread first([&] {
    Check(wait(&word, 0, 2000000000LL) == 0, "wake-one first waiter");
    completed.fetch_add(1, std::memory_order_relaxed);
  });
  std::thread second([&] {
    Check(wait(&word, 0, 2000000000LL) == 0, "wake-one second waiter");
    completed.fetch_add(1, std::memory_order_relaxed);
  });
  while (darwin_art_bionic_syscall_waiter_count(&word) != 2) {
    std::this_thread::yield();
  }
  Check(wake_one(&word) == 1, "wake one count");
  while (completed.load(std::memory_order_relaxed) != 1) {
    std::this_thread::yield();
  }
  Check(darwin_art_bionic_syscall_waiter_count(&word) == 1,
        "wake one leaves one waiter");
  Check(wake_all(&word) == 1, "final wake count");
  first.join();
  second.join();

  word = 7;
  darwin_art_bionic_errno_store(0);
  Check(wait(&word, 0, 1000000LL) == -1 &&
            darwin_art_bionic_errno_load() == 11,
        "futex compare mismatch EAGAIN");
  word = 0;
  darwin_art_bionic_errno_store(0);
  Check(wait(&word, 0, 20000000LL) == -1 &&
            darwin_art_bionic_errno_load() == 110,
        "futex relative timeout ETIMEDOUT");
  Check(wake_one(&word) == 0, "timeout leaves no wake token");

  word = 0;
  std::atomic<bool> spurious_done{false};
  long spurious_result = 0;
  int32_t spurious_errno = 0;
  const auto spurious_start = std::chrono::steady_clock::now();
  std::thread spurious_waiter([&] {
    darwin_art_bionic_errno_store(0);
    spurious_result = wait(&word, 0, 80000000LL);
    spurious_errno = darwin_art_bionic_errno_load();
    spurious_done.store(true, std::memory_order_release);
  });
  while (darwin_art_bionic_syscall_waiter_count(&word) != 1) {
    std::this_thread::yield();
  }
  while (!spurious_done.load(std::memory_order_acquire)) {
    darwin_art_bionic_syscall_spurious_wake(&word);
    std::this_thread::yield();
  }
  spurious_waiter.join();
  const auto spurious_elapsed = std::chrono::steady_clock::now() - spurious_start;
  Check(spurious_result == -1 && spurious_errno == 110 &&
            spurious_elapsed >= std::chrono::milliseconds(40) &&
            spurious_elapsed < std::chrono::milliseconds(500),
        "spurious wake does not extend monotonic deadline");
  Check(wake_one(&word) == 0, "spurious timeout leaves no token");

  constexpr size_t kWaitTableCapacity = 257;
  std::vector<int32_t> capacity_words(kWaitTableCapacity, 0);
  std::vector<long> capacity_results(kWaitTableCapacity, -2);
  std::vector<std::thread> capacity_threads;
  capacity_threads.reserve(kWaitTableCapacity);
  for (size_t index = 0; index < kWaitTableCapacity; ++index) {
    capacity_threads.emplace_back([&, index] {
      capacity_results[index] =
          wait(&capacity_words[index], 0, 5000000000LL);
    });
  }
  const auto capacity_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    size_t admitted = 0;
    for (size_t index = 0; index < kWaitTableCapacity; ++index) {
      admitted += darwin_art_bionic_syscall_waiter_count(
          &capacity_words[index]) == 1;
    }
    if (admitted == kWaitTableCapacity) break;
    Check(std::chrono::steady_clock::now() < capacity_deadline,
          "wait-table admission deadline");
    std::this_thread::yield();
  }
  alignas(4) int32_t overflow_word = 0;
  darwin_art_bionic_errno_store(0);
  Check(wait(&overflow_word, 0, 1000000LL) == -1 &&
            darwin_art_bionic_errno_load() == 11,
        "257-address wait-table exhaustion EAGAIN");
  for (size_t index = 0; index < kWaitTableCapacity; ++index) {
    Check(wake_one(&capacity_words[index]) == 1,
          "capacity waiter wake count");
  }
  for (std::thread& thread : capacity_threads) thread.join();
  for (long result : capacity_results) {
    Check(result == 0, "capacity waiter result");
  }

  darwin_art_bionic_errno_store(0);
  Check(readable(reinterpret_cast<const void*>(gettid)) == -1 &&
            darwin_art_bionic_errno_load() == 22,
        "libunwind readable probe EINVAL");
  void* guard = mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  Check(guard != MAP_FAILED, "guard map");
  darwin_art_bionic_errno_store(0);
  Check(wake_one(static_cast<int32_t*>(guard)) == -1 &&
            darwin_art_bionic_errno_load() == 14,
        "futex wake guard EFAULT");
  darwin_art_bionic_errno_store(0);
  Check(readable(guard) == -1 && darwin_art_bionic_errno_load() == 14,
        "libunwind guard probe EFAULT");
  Check(munmap(guard, 16384) == 0, "guard unmap");
  darwin_art_bionic_errno_store(0);
  Check(wake_one(static_cast<int32_t*>(guard)) == -1 &&
            darwin_art_bionic_errno_load() == 14,
        "futex wake unmapped EFAULT");
  darwin_art_bionic_errno_store(0);
  Check(readable(guard) == -1 && darwin_art_bionic_errno_load() == 14,
        "libunwind unmapped probe EFAULT");
  darwin_art_bionic_errno_store(0);
  Check(readable(reinterpret_cast<const void*>(UINTPTR_MAX - 3)) == -1 &&
            darwin_art_bionic_errno_load() == 14,
        "libunwind overflow probe EFAULT");

  darwin_art_bionic_errno_store(0);
  Check(unknown() == -1 && darwin_art_bionic_errno_load() == 38,
        "unknown syscall ENOSYS");
  darwin_art_bionic_errno_store(0);
  Check(bad_futex(&word) == -1 && darwin_art_bionic_errno_load() == 22,
        "unknown futex operation EINVAL");
  Check(errno == EDOM, "host errno preserved");

  Check(darwin_art_elf_unload(&image, &error) == DARWIN_ART_ELF_OK &&
            image == nullptr,
        "unload Android ELF");
  std::fprintf(stderr,
               "bionic-syscall-facade: ELF PASS gettid=threads futex="
               "wait+wake-one+wake-all timeout=monotonic-spurious "
               "capacity=257 invalid-wake=EFAULT rt_sigprocmask=readability "
               "unknown=closed\n");
  return 0;
}
