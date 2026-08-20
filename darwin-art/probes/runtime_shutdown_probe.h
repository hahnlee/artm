#pragma once

#include <cstdint>
#include <string>

namespace darwin_art_process {

// Snapshot of process-global acceptance state needed after DestroyJavaVM.
// The shutdown TU receives values, not aliases into the orchestration object,
// so teardown has no hidden ownership dependency on runtime_link_probe.cc.
struct ShutdownState final {
  bool network_elf_loaded = false;
  bool apk_elf_loaded = false;
  bool direct_apk_loaded = false;
  bool provider_hooks_installed = false;
  std::string apk_sha256;
  std::string apk_root_sha256;
};

int32_t run_shutdown(const ShutdownState& state);

}  // namespace darwin_art_process

extern "C" int32_t darwin_art_shutdown_process();
