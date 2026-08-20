#pragma once

#include <cstdint>
#include <string>

namespace darwin_art_process {

// Immutable, owner-thread configuration assembled once at the C ABI boundary.
// Keeping environment parsing out of the ART orchestration TU means changing a
// probe mode no longer recompiles the 1,700-line runtime implementation.
struct ProcessOptions final {
  std::string elf_fixture_path;
  std::string generic_elf_path;
  std::string apk_elf_path;
  std::string apk_sha256;
  std::string apk_root_sha256;
  std::string direct_apk_path;
  std::string direct_apk_root;
  std::string libcxx_collections_path;
  std::string libcxx_exception_path;
  std::string tls_fixture_path;
  std::string network_fixture_path;
  std::string apk_app_package;
  std::string apk_app_activity;
  std::string apk_app_descriptor;
  std::string apk_app_support_dex;
  std::string framework_res_apk;

  bool run_elf_jni_fixture = false;
  bool run_generic_elf = false;
  bool run_apk_elf = false;
  bool run_direct_apk = false;
  bool run_libcxx_acceptance = false;
  bool run_tls_acceptance = false;
  bool run_network_acceptance = false;
  bool has_apk_app_identity_environment = false;
  bool has_framework_res_apk = false;
  bool has_window_scale = false;
  bool run_apk_app = false;
  bool run_framework_button = false;
  bool use_framework_resources = false;
  bool expect_apk_widgets = false;
  int32_t window_scale = 1;
};

// Returns 0 on success, 47 for an invalid mixed network mode, and 48 for any
// malformed APK/framework/window environment. `error` is diagnostic text only.
int LoadProcessOptions(ProcessOptions* options, std::string* error);

}  // namespace darwin_art_process
