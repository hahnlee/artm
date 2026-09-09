#pragma once

#if defined(__APPLE__)

#include <mach/mach.h>

#include <mutex>
#include <unordered_map>
#include <unistd.h>
namespace darwin_art::remote_task {

// Keep one validated task send right per remote process.  macOS may block on
// repeated task_for_pid calls while a target is in a fork/exec transition;
// callers receive a temporary send-right copy and remain responsible for
// deallocating that copy.
inline mach_port_t Acquire(pid_t pid) {
  if (pid == getpid()) return mach_task_self();
  static std::mutex mutex;
  static std::unordered_map<pid_t, mach_port_t> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto found = cache.find(pid);
  if (found == cache.end()) {
    mach_port_t task = MACH_PORT_NULL;
    if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS ||
        task == MACH_PORT_NULL) {
      return MACH_PORT_NULL;
    }
    cache.emplace(pid, task);
    found = cache.find(pid);
  }
  if (mach_port_mod_refs(mach_task_self(), found->second, MACH_PORT_RIGHT_SEND, 1) !=
      KERN_SUCCESS) {
    return MACH_PORT_NULL;
  }
  return found->second;
}

}  // namespace darwin_art::remote_task

#endif
