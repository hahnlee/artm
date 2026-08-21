#include "runtime_process_state.h"

#include <pthread.h>

#include <mutex>
#include <utility>
#include <vector>

#include "../include/darwin_art/darwin_art.h"

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
  darwin_art_graphics::GraphicsState* graphics_state = nullptr;
  const darwin_art_lifecycle_hooks_t* lifecycle_hooks = nullptr;
  AcceptanceSnapshot acceptance;
  std::vector<std::unique_ptr<const art::DexFile>> app_dex_files;
};

State g_state;

}  // namespace

bool begin_run(const struct darwin_art_lifecycle_hooks* lifecycle_hooks) {
  if (lifecycle_hooks != nullptr) {
    if (lifecycle_hooks->struct_size < sizeof(*lifecycle_hooks) ||
        lifecycle_hooks->abi_version != DARWIN_ART_ABI_VERSION ||
        lifecycle_hooks->context == nullptr ||
        lifecycle_hooks->begin_run == nullptr ||
        lifecycle_hooks->finish_run == nullptr ||
        lifecycle_hooks->begin_shutdown == nullptr ||
        lifecycle_hooks->mark_failed == nullptr ||
        lifecycle_hooks->begin_run(lifecycle_hooks->context) != 0) {
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.phase != Phase::kNeverStarted) return false;
  g_state.phase = Phase::kRunning;
  g_state.owner_thread = pthread_self();
  g_state.owner_thread_valid = true;
  g_state.lifecycle_hooks = lifecycle_hooks;
  return true;
}

void record_created_runtime(art::Thread* art_thread) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  CHECK(art::Runtime::Current() != nullptr);
  g_state.java_vm = reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
  g_state.art_thread = art_thread;
}

void record_graphics_state(darwin_art_graphics::GraphicsState* state) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.graphics_state = state;
}

void record_resource_runtime_installed() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  CHECK(!g_state.resource_runtime_installed);
  g_state.resource_runtime_installed = true;
}

void finish_run(bool runtime_created) {
  const struct darwin_art_lifecycle_hooks* lifecycle_hooks = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    CHECK(g_state.phase == Phase::kRunning);
    g_state.phase = runtime_created ? Phase::kAwaitingShutdown
                                    : Phase::kCreateFailed;
    lifecycle_hooks = g_state.lifecycle_hooks;
  }
  if (lifecycle_hooks != nullptr &&
      lifecycle_hooks->finish_run(lifecycle_hooks->context,
                                  runtime_created ? 1 : 0) != 0) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.phase = Phase::kShutdownFailed;
  }
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
  const struct darwin_art_lifecycle_hooks* lifecycle_hooks = nullptr;
  {
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
    snapshot->graphics_state = g_state.graphics_state;
    lifecycle_hooks = g_state.lifecycle_hooks;
  }
  if (lifecycle_hooks != nullptr &&
      lifecycle_hooks->begin_shutdown(lifecycle_hooks->context) != 0) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.phase = Phase::kShutdownFailed;
    return ShutdownBeginResult::kFailed;
  }
  return ShutdownBeginResult::kReady;
}

void mark_shutdown_failed() {
  const darwin_art_lifecycle_hooks_t* lifecycle_hooks = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.phase = Phase::kShutdownFailed;
    lifecycle_hooks = g_state.lifecycle_hooks;
  }
  if (lifecycle_hooks != nullptr) {
    lifecycle_hooks->mark_failed(lifecycle_hooks->context,
                                 DARWIN_ART_STATUS_SHUTDOWN_FAILED);
  }
}

void mark_shutdown_complete() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.java_vm = nullptr;
  g_state.art_thread = nullptr;
  g_state.resource_runtime_installed = false;
  g_state.graphics_state = nullptr;
  g_state.lifecycle_hooks = nullptr;
  g_state.phase = Phase::kShutdownComplete;
}

void record_network_elf_loaded() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.acceptance.network_elf_loaded = true;
}

void record_apk_elf_loaded(std::string apk_sha256, std::string apk_root_sha256) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.acceptance.apk_elf_loaded = true;
  g_state.acceptance.apk_sha256 = std::move(apk_sha256);
  g_state.acceptance.apk_root_sha256 = std::move(apk_root_sha256);
}

void record_direct_apk_loaded() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.acceptance.direct_apk_loaded = true;
}

void record_provider_hooks_installed() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CHECK(g_state.phase == Phase::kRunning);
  g_state.acceptance.provider_hooks_installed = true;
}

void clear_provider_hooks_state() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.acceptance.provider_hooks_installed = false;
}

AcceptanceSnapshot acceptance_snapshot() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  return g_state.acceptance;
}

ScopedRunBoundary::~ScopedRunBoundary() {
  if (art_thread_ == nullptr) {
    finish_run(false);
    return;
  }
  CHECK_EQ(art::Thread::Current(), art_thread_);
  const art::ThreadState state = art_thread_->GetState();
  if (state == art::ThreadState::kRunnable) {
    art_thread_->TransitionFromRunnableToSuspended(art::ThreadState::kNative);
  } else {
    CHECK_EQ(state, art::ThreadState::kNative);
  }
  finish_run(true);
}

void ScopedRunBoundary::set_art_thread(art::Thread* art_thread) {
  DCHECK(art_thread_ == nullptr);
  DCHECK(art_thread != nullptr);
  art_thread_ = art_thread;
}

}  // namespace darwin_art_process
