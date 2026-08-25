#include "darwin_art_bionic_syscall.h"

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>

extern "C" void darwin_art_bionic_errno_store(int32_t android_errno);

namespace {

constexpr uint64_t kFutex = 98;
constexpr uint64_t kRtSigprocmask = 135;
constexpr uint64_t kGettid = 178;
constexpr uint64_t kGetrandom = 278;
constexpr uint32_t kFutexWaitPrivate = 128;
constexpr uint32_t kFutexWakePrivate = 129;
constexpr int32_t kWakeOne = 1;
constexpr int32_t kWakeAll = std::numeric_limits<int32_t>::max();
constexpr int32_t kAndroidEagain = 11;
constexpr int32_t kAndroidEfault = 14;
constexpr int32_t kAndroidEinval = 22;
constexpr int32_t kAndroidEnosys = 38;
constexpr int32_t kAndroidEtimedout = 110;
constexpr uint32_t kGrndNonblock = 0x1;
constexpr uint32_t kGrndRandom = 0x2;
constexpr uint32_t kGrndInsecure = 0x4;
constexpr size_t kWaitEntryCount = 257;

struct AndroidTimespec {
  int64_t seconds;
  int64_t nanoseconds;
};

struct WaitEntry {
  const int32_t* address = nullptr;
  size_t waiters = 0;
  size_t wake_tokens = 0;
  pthread_cond_t condition{};
};

pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t g_wait_once = PTHREAD_ONCE_INIT;
WaitEntry g_wait_entries[kWaitEntryCount];
std::atomic<uint32_t> g_next_tid{1};
thread_local uint32_t g_thread_tid;

void InitializeWaitEntries() {
  for (WaitEntry& entry : g_wait_entries) {
    (void)pthread_cond_init(&entry.condition, nullptr);
  }
}

bool HasRangeProtection(const void* pointer, size_t length,
                        vm_prot_t required) {
  const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
  if (begin == 0 || length == 0 || begin > UINTPTR_MAX - length) return false;
  const uintptr_t end = begin + length;
  mach_vm_address_t region = static_cast<mach_vm_address_t>(begin);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  const kern_return_t status = mach_vm_region(
      mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &count, &object);
  if (object != MACH_PORT_NULL) {
    (void)mach_port_deallocate(mach_task_self(), object);
  }
  if (status != KERN_SUCCESS || (info.protection & required) != required ||
      region > begin || region_size > UINT64_MAX - region) {
    return false;
  }
  return end <= region + region_size;
}

bool IsReadableRange(const void* pointer, size_t length) {
  return HasRangeProtection(pointer, length, VM_PROT_READ);
}

bool IsWritableRange(const void* pointer, size_t length) {
  return HasRangeProtection(pointer, length, VM_PROT_WRITE);
}

WaitEntry* FindWaitEntry(const int32_t* address, bool create) {
  WaitEntry* empty = nullptr;
  for (WaitEntry& entry : g_wait_entries) {
    if (entry.address == address) return &entry;
    if (empty == nullptr && entry.address == nullptr && entry.waiters == 0 &&
        entry.wake_tokens == 0) {
      empty = &entry;
    }
  }
  if (create && empty != nullptr) empty->address = address;
  return create ? empty : nullptr;
}

void ReleaseEmptyEntry(WaitEntry* entry) {
  if (entry->waiters == 0 && entry->wake_tokens == 0) entry->address = nullptr;
}

long Fail(int32_t android_errno) {
  darwin_art_bionic_errno_store(android_errno);
  return -1;
}

long GetTid() {
  if (g_thread_tid == 0) {
    const uint32_t candidate = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    if (candidate == 0 || candidate > INT32_MAX) return Fail(kAndroidEnosys);
    g_thread_tid = candidate;
  }
  return static_cast<long>(g_thread_tid);
}

long GetRandom(const uint64_t* arguments) {
  void* buffer = reinterpret_cast<void*>(arguments[1]);
  const uint64_t length = arguments[2];
  const uint32_t flags = static_cast<uint32_t>(arguments[3]);
  constexpr uint32_t kKnownFlags =
      kGrndNonblock | kGrndRandom | kGrndInsecure;
  if ((flags & ~kKnownFlags) != 0 ||
      (flags & (kGrndRandom | kGrndInsecure)) ==
          (kGrndRandom | kGrndInsecure) ||
      length > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
    return Fail(kAndroidEinval);
  }
  if (length == 0) return 0;
  if (!IsWritableRange(buffer, static_cast<size_t>(length))) {
    return Fail(kAndroidEfault);
  }
  // Darwin's arc4random_buf is backed by the host CSPRNG and cannot expose a
  // Linux entropy-pool-not-ready state. Consequently GRND_NONBLOCK has the
  // same successful result as a blocking request while preserving secure
  // randomness and never forwarding an Android syscall number to Darwin.
  arc4random_buf(buffer, static_cast<size_t>(length));
  return static_cast<long>(length);
}

bool MonotonicDeadline(const AndroidTimespec& relative, timespec* deadline) {
  if (relative.seconds < 0 || relative.nanoseconds < 0 ||
      relative.nanoseconds >= 1000000000LL ||
      clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
    return false;
  }
  if (relative.seconds > std::numeric_limits<time_t>::max() - deadline->tv_sec) {
    return false;
  }
  deadline->tv_sec += static_cast<time_t>(relative.seconds);
  deadline->tv_nsec += static_cast<long>(relative.nanoseconds);
  if (deadline->tv_nsec >= 1000000000L) {
    if (deadline->tv_sec == std::numeric_limits<time_t>::max()) return false;
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000L;
  }
  return true;
}

bool RemainingUntil(const timespec& deadline, timespec* remaining) {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
  if (now.tv_sec > deadline.tv_sec ||
      (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
    remaining->tv_sec = 0;
    remaining->tv_nsec = 0;
    return false;
  }
  remaining->tv_sec = deadline.tv_sec - now.tv_sec;
  remaining->tv_nsec = deadline.tv_nsec - now.tv_nsec;
  if (remaining->tv_nsec < 0) {
    --remaining->tv_sec;
    remaining->tv_nsec += 1000000000L;
  }
  return true;
}

long FutexWait(const uint64_t* arguments) {
  auto* address = reinterpret_cast<int32_t*>(arguments[1]);
  const int32_t expected = static_cast<int32_t>(arguments[3]);
  const void* timeout_pointer = reinterpret_cast<const void*>(arguments[4]);
  if (address == nullptr || (reinterpret_cast<uintptr_t>(address) & 3) != 0) {
    return Fail(kAndroidEinval);
  }
  if (arguments[5] != 0 || arguments[6] != 0 || timeout_pointer == nullptr) {
    return Fail(kAndroidEinval);
  }
  if (!IsReadableRange(address, sizeof(*address)) ||
      !IsReadableRange(timeout_pointer, sizeof(AndroidTimespec))) {
    return Fail(kAndroidEfault);
  }
  AndroidTimespec relative{};
  memcpy(&relative, timeout_pointer, sizeof(relative));
  timespec deadline{};
  if (!MonotonicDeadline(relative, &deadline)) return Fail(kAndroidEinval);

  (void)pthread_once(&g_wait_once, InitializeWaitEntries);
  (void)pthread_mutex_lock(&g_wait_lock);
  if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != expected) {
    (void)pthread_mutex_unlock(&g_wait_lock);
    return Fail(kAndroidEagain);
  }
  WaitEntry* entry = FindWaitEntry(address, true);
  if (entry == nullptr) {
    (void)pthread_mutex_unlock(&g_wait_lock);
    return Fail(kAndroidEagain);
  }
  ++entry->waiters;
  int wait_status = 0;
  while (entry->wake_tokens == 0 && wait_status == 0) {
    timespec remaining{};
    if (!RemainingUntil(deadline, &remaining)) {
      wait_status = ETIMEDOUT;
      break;
    }
    wait_status = pthread_cond_timedwait_relative_np(
        &entry->condition, &g_wait_lock, &remaining);
    if (wait_status == ETIMEDOUT && RemainingUntil(deadline, &remaining)) {
      wait_status = 0;
    }
  }
  long result = 0;
  if (entry->wake_tokens != 0) {
    --entry->wake_tokens;
  } else if (wait_status != 0) {
    result = Fail(wait_status == ETIMEDOUT ? kAndroidEtimedout
                                          : kAndroidEinval);
  }
  --entry->waiters;
  ReleaseEmptyEntry(entry);
  (void)pthread_mutex_unlock(&g_wait_lock);
  return result;
}

long FutexWake(const uint64_t* arguments) {
  auto* address = reinterpret_cast<int32_t*>(arguments[1]);
  const int32_t requested = static_cast<int32_t>(arguments[3]);
  if (address == nullptr || (reinterpret_cast<uintptr_t>(address) & 3) != 0 ||
      (requested != kWakeOne && requested != kWakeAll) || arguments[4] != 0 ||
      arguments[5] != 0 || arguments[6] != 0) {
    return Fail(kAndroidEinval);
  }
  if (!IsReadableRange(address, sizeof(*address))) {
    return Fail(kAndroidEfault);
  }
  (void)pthread_once(&g_wait_once, InitializeWaitEntries);
  (void)pthread_mutex_lock(&g_wait_lock);
  WaitEntry* entry = FindWaitEntry(address, false);
  if (entry == nullptr) {
    (void)pthread_mutex_unlock(&g_wait_lock);
    return 0;
  }
  if (entry->wake_tokens > entry->waiters) std::abort();
  const size_t eligible = entry->waiters - entry->wake_tokens;
  const size_t selected = requested == kWakeOne ? std::min<size_t>(1, eligible)
                                                 : eligible;
  entry->wake_tokens += selected;
  if (selected == 1) {
    (void)pthread_cond_signal(&entry->condition);
  } else if (selected > 1) {
    (void)pthread_cond_broadcast(&entry->condition);
  }
  (void)pthread_mutex_unlock(&g_wait_lock);
  return static_cast<long>(selected);
}

long Futex(const uint64_t* arguments) {
  const uint32_t operation = static_cast<uint32_t>(arguments[2]);
  if (operation == kFutexWaitPrivate) return FutexWait(arguments);
  if (operation == kFutexWakePrivate) return FutexWake(arguments);
  return Fail(kAndroidEinval);
}

long ReadabilityProbe(const uint64_t* arguments) {
  if (static_cast<int32_t>(arguments[1]) != -1 || arguments[2] == 0 ||
      arguments[3] != 0 || arguments[4] != 8) {
    return Fail(kAndroidEinval);
  }
  return Fail(IsReadableRange(reinterpret_cast<const void*>(arguments[2]), 8)
                  ? kAndroidEinval
                  : kAndroidEfault);
}

}  // namespace

extern "C" long darwin_art_bionic_syscall_captured(const uint64_t* registers,
                                                     const uint8_t* stack) {
  const int saved_errno = errno;
  long result = -1;
  if (registers == nullptr || stack == nullptr) {
    result = Fail(kAndroidEinval);
  } else if (registers[0] == kGettid) {
    result = GetTid();
  } else if (registers[0] == kFutex) {
    result = Futex(registers);
  } else if (registers[0] == kRtSigprocmask) {
    result = ReadabilityProbe(registers);
  } else if (registers[0] == kGetrandom) {
    result = GetRandom(registers);
  } else {
    result = Fail(kAndroidEnosys);
  }
  errno = saved_errno;
  return result;
}

extern "C" size_t darwin_art_bionic_syscall_waiter_count(
    const int32_t* address) {
  const int saved_errno = errno;
  (void)pthread_once(&g_wait_once, InitializeWaitEntries);
  (void)pthread_mutex_lock(&g_wait_lock);
  const WaitEntry* entry = FindWaitEntry(address, false);
  const size_t result = entry == nullptr ? 0 : entry->waiters;
  (void)pthread_mutex_unlock(&g_wait_lock);
  errno = saved_errno;
  return result;
}

extern "C" void darwin_art_bionic_syscall_spurious_wake(
    const int32_t* address) {
  const int saved_errno = errno;
  (void)pthread_once(&g_wait_once, InitializeWaitEntries);
  (void)pthread_mutex_lock(&g_wait_lock);
  WaitEntry* entry = FindWaitEntry(address, false);
  if (entry != nullptr) (void)pthread_cond_broadcast(&entry->condition);
  (void)pthread_mutex_unlock(&g_wait_lock);
  errno = saved_errno;
}

extern "C" DarwinArtBionicSyscallFunction darwin_art_bionic_syscall_resolve(
    const char* soname, const char* symbol, const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      strcmp(soname, "libc.so") != 0 || strcmp(symbol, "syscall") != 0 ||
      strcmp(version, "LIBC") != 0) {
    return nullptr;
  }
  return reinterpret_cast<DarwinArtBionicSyscallFunction>(
      darwin_art_bionic_syscall);
}

extern "C" const char* darwin_art_bionic_syscall_capability(
    const char* capability) {
  if (capability == nullptr) return "invalid-capability";
  if (strcmp(capability, "android-aapcs64-varargs-capture") == 0 ||
      strcmp(capability, "provider-owned-stable-tid") == 0 ||
      strcmp(capability, "host-csprng-getrandom") == 0 ||
      strcmp(capability, "futex-private-wait-wake") == 0 ||
      strcmp(capability, "libunwind-readability-probe") == 0) {
    return "supported";
  }
  return "unsupported";
}
