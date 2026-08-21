#pragma once

#include <jni.h>

#include <memory>
#include <string>
#include <vector>

namespace art {
class DexFile;
class Thread;
}  // namespace art

namespace darwin_art_graphics {
struct GraphicsState;
}

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
  darwin_art_graphics::GraphicsState* graphics_state = nullptr;
};

// Value-only acceptance state shared by the orchestration and shutdown
// phases.  Keeping this beside the process lifecycle mutex prevents the
// 1600-line probe from growing another set of unsynchronized globals.
struct AcceptanceSnapshot {
  bool network_elf_loaded = false;
  bool apk_elf_loaded = false;
  bool direct_apk_loaded = false;
  bool provider_hooks_installed = false;
  std::string apk_sha256;
  std::string apk_root_sha256;
};

bool begin_run();
void record_created_runtime(art::Thread* art_thread);
void record_graphics_state(darwin_art_graphics::GraphicsState* state);
void record_resource_runtime_installed();
void finish_run();
void record_app_dex_file(std::unique_ptr<const art::DexFile> dex_file);
std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files();
void clear_app_dex_files();

art::Thread* owner_thread_for_callback();
ShutdownBeginResult begin_shutdown(ShutdownSnapshot* snapshot);
void mark_shutdown_failed();
void mark_shutdown_complete();

void record_network_elf_loaded();
void record_apk_elf_loaded(std::string apk_sha256, std::string apk_root_sha256);
void record_direct_apk_loaded();
void record_provider_hooks_installed();
void clear_provider_hooks_state();
AcceptanceSnapshot acceptance_snapshot();

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
