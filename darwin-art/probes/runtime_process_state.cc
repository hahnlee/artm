#include "runtime_process_state.h"

#include <pthread.h>

#include <mutex>
#include <vector>

#include "base/locks.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

namespace darwin_art_process {
namespace {

enum class Phase {
  kNeverStarted,
  kRunning,
  kAwaitingShutdown,
  kShuttingDown,
  kShutdownComplete,
  kCreateFailed,
  kShutdownFailed,
};

struct State {
  std::mutex mutex;
  Phase phase = Phase::kNeverStarted;
  pthread_t owner_thread{};
  bool owner_thread_valid = false;
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
  std::vector<std::unique_ptr<const art::DexFile>> app_dex_files;
};

State g_state;

}  // namespace

bool begin_run() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.phase != Phase::kNeverStarted) return false;
  g_state.phase = Phase::kRunning;
  g_state.owner_thread = pthread_self();
  g_state.owner_thread_valid = true;
  return true;
}

void record_created_runtime(art::Thread* art_thread) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  CHECK(art::Runtime::Current() != nullptr);
  g_state.java_vm = reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
  g_state.art_thread = art_thread;
}

void record_resource_runtime_installed() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  CHECK(!g_state.resource_runtime_installed);
  g_state.resource_runtime_installed = true;
}

void finish_run() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.phase = g_state.java_vm == nullptr ? Phase::kCreateFailed
                                              : Phase::kAwaitingShutdown;
}

void record_app_dex_file(std::unique_ptr<const art::DexFile> dex_file) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.app_dex_files.emplace_back(std::move(dex_file));
}

std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files() {
  return g_state.app_dex_files;
}

void clear_app_dex_files() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.app_dex_files.clear();
}

art::Thread* owner_thread_for_callback() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.phase != Phase::kAwaitingShutdown ||
      !g_state.owner_thread_valid ||
      pthread_equal(g_state.owner_thread, pthread_self()) == 0 ||
      g_state.art_thread == nullptr) {
    return nullptr;
  }
  return g_state.art_thread;
}

ShutdownBeginResult begin_shutdown(ShutdownSnapshot* snapshot) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  switch (g_state.phase) {
    case Phase::kShutdownComplete:
      return ShutdownBeginResult::kAlreadyComplete;
    case Phase::kShutdownFailed:
      return ShutdownBeginResult::kFailed;
    case Phase::kNeverStarted:
    case Phase::kCreateFailed:
    case Phase::kRunning:
    case Phase::kShuttingDown:
      return ShutdownBeginResult::kNotReady;
    case Phase::kAwaitingShutdown:
      break;
  }
  if (!g_state.owner_thread_valid ||
      pthread_equal(g_state.owner_thread, pthread_self()) == 0 ||
      (g_state.art_thread != nullptr &&
       art::Thread::Current() != g_state.art_thread)) {
    return ShutdownBeginResult::kWrongThread;
  }
  g_state.phase = Phase::kShuttingDown;
  snapshot->java_vm = g_state.java_vm;
  snapshot->art_thread = g_state.art_thread;
  snapshot->resource_runtime_installed = g_state.resource_runtime_installed;
  return ShutdownBeginResult::kReady;
}

void mark_shutdown_failed() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.phase = Phase::kShutdownFailed;
}

void mark_shutdown_complete() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.java_vm = nullptr;
  g_state.art_thread = nullptr;
  g_state.resource_runtime_installed = false;
  g_state.phase = Phase::kShutdownComplete;
}

ScopedRunBoundary::~ScopedRunBoundary() {
  if (art_thread_ == nullptr) {
    finish_run();
    return;
  }
  CHECK_EQ(art::Thread::Current(), art_thread_);
  const art::ThreadState state = art_thread_->GetState();
  if (state == art::ThreadState::kRunnable) {
    art_thread_->TransitionFromRunnableToSuspended(art::ThreadState::kNative);
  } else {
    CHECK_EQ(state, art::ThreadState::kNative);
  }
  finish_run();
}

void ScopedRunBoundary::set_art_thread(art::Thread* art_thread) {
  DCHECK(art_thread_ == nullptr);
  DCHECK(art_thread != nullptr);
  art_thread_ = art_thread;
}

}  // namespace darwin_art_process
