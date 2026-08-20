#pragma once

#include <jni.h>

#include <memory>
#include <vector>

namespace art {
class DexFile;
class Thread;
}  // namespace art

namespace darwin_art_process {

enum class ShutdownBeginResult {
  kReady,
  kAlreadyComplete,
  kFailed,
  kNotReady,
  kWrongThread,
};

struct ShutdownSnapshot {
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
};

bool begin_run();
void record_created_runtime(art::Thread* art_thread);
void record_resource_runtime_installed();
void finish_run();
void record_app_dex_file(std::unique_ptr<const art::DexFile> dex_file);
std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files();
void clear_app_dex_files();

art::Thread* owner_thread_for_callback();
ShutdownBeginResult begin_shutdown(ShutdownSnapshot* snapshot);
void mark_shutdown_failed();
void mark_shutdown_complete();

// Keeps ART's mutator transition and process phase transition paired even
// when the managed-work lambda returns through an error path.
class ScopedRunBoundary final {
 public:
  ScopedRunBoundary() = default;
  ~ScopedRunBoundary();

  void set_art_thread(art::Thread* art_thread);

 private:
  art::Thread* art_thread_ = nullptr;
};

}  // namespace darwin_art_process
