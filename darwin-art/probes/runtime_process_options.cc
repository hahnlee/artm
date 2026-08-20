#include "runtime_process_options.h"

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>

namespace darwin_art_process {
namespace {

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

bool HasEnv(const char* name) { return std::getenv(name) != nullptr; }

bool Present(const std::string& value) { return !value.empty(); }

bool IsSha256(const std::string& value) {
  if (value.size() != 64) return false;
  for (unsigned char byte : value) {
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool IsPrivateExtractedRoot(const std::string& path) {
  if (path.empty()) return false;
  struct stat path_stat {};
  struct stat followed_stat {};
  if (lstat(path.c_str(), &path_stat) != 0 ||
      stat(path.c_str(), &followed_stat) != 0 ||
      !S_ISREG(path_stat.st_mode) || path_stat.st_dev != followed_stat.st_dev ||
      path_stat.st_ino != followed_stat.st_ino ||
      (path_stat.st_mode & 0777) != 0400) {
    return false;
  }
  const std::size_t separator = path.rfind('/');
  if (separator == std::string::npos || separator == 0) return false;
  struct stat directory_stat {};
  const std::string directory = path.substr(0, separator);
  return lstat(directory.c_str(), &directory_stat) == 0 &&
         S_ISDIR(directory_stat.st_mode) &&
         (directory_stat.st_mode & 0777) == 0500;
}

}  // namespace

int LoadProcessOptions(ProcessOptions* options, std::string* error) {
  if (options == nullptr || error == nullptr) return 48;
  *options = ProcessOptions{};
  options->elf_fixture_path = Env("DARWIN_ART_ANDROID_ELF_JNI_FIXTURE");
  options->generic_elf_path = Env("DARWIN_ART_ANDROID_ELF_GENERIC_FIXTURE");
  options->apk_elf_path = Env("DARWIN_ART_ANDROID_APK_ELF_FIXTURE");
  options->apk_sha256 = Env("DARWIN_ART_ANDROID_APK_SHA256");
  options->apk_root_sha256 = Env("DARWIN_ART_ANDROID_APK_ROOT_SHA256");
  options->direct_apk_path = Env("DARWIN_ART_DIRECT_APK_FIXTURE");
  options->direct_apk_root = Env("DARWIN_ART_DIRECT_APK_ROOT");
  options->libcxx_collections_path =
      Env("DARWIN_ART_ANDROID_LIBCXX_COLLECTIONS_FIXTURE");
  options->libcxx_exception_path =
      Env("DARWIN_ART_ANDROID_LIBCXX_EXCEPTION_FIXTURE");
  options->tls_fixture_path = Env("DARWIN_ART_ANDROID_TLS_FIXTURE");
  options->network_fixture_path = Env("DARWIN_ART_ANDROID_NETWORK_FIXTURE");
  options->apk_app_package = Env("DARWIN_ART_APK_APP_PACKAGE");
  options->apk_app_activity = Env("DARWIN_ART_APK_APP_ACTIVITY");
  options->apk_app_descriptor = Env("DARWIN_ART_APK_APP_DESCRIPTOR");
  options->apk_app_support_dex = Env("DARWIN_ART_APK_APP_SUPPORT_DEX");
  options->framework_res_apk = Env("DARWIN_ART_FRAMEWORK_RES_APK");
  const std::string window_scale = Env("DARWIN_ART_WINDOW_SCALE");
  const bool has_test_fonts_xml = HasEnv("DARWIN_ART_TEST_FONTS_XML");
  options->has_framework_res_apk = HasEnv("DARWIN_ART_FRAMEWORK_RES_APK");
  options->has_window_scale = HasEnv("DARWIN_ART_WINDOW_SCALE");

  options->run_elf_jni_fixture = Present(options->elf_fixture_path);
  options->run_generic_elf = Present(options->generic_elf_path);
  options->run_apk_elf =
      Present(options->apk_elf_path) && IsSha256(options->apk_sha256) &&
      IsSha256(options->apk_root_sha256) &&
      IsPrivateExtractedRoot(options->apk_elf_path) && options->run_generic_elf &&
      options->apk_elf_path == options->generic_elf_path;
  options->run_direct_apk = Present(options->direct_apk_path) &&
                            Present(options->direct_apk_root);
  options->run_libcxx_acceptance = Present(options->libcxx_collections_path) &&
                                   Present(options->libcxx_exception_path);
  options->run_tls_acceptance = Present(options->tls_fixture_path);
  options->run_network_acceptance = Present(options->network_fixture_path);
  options->has_apk_app_identity_environment =
      HasEnv("DARWIN_ART_APK_APP_PACKAGE") ||
      HasEnv("DARWIN_ART_APK_APP_ACTIVITY") ||
      HasEnv("DARWIN_ART_APK_APP_DESCRIPTOR") ||
      HasEnv("DARWIN_ART_APK_APP_SUPPORT_DEX");
  options->run_apk_app =
      Present(options->apk_app_package) && Present(options->apk_app_activity) &&
      options->apk_app_descriptor.size() >= 3 &&
      options->apk_app_descriptor.size() <= 513 &&
      options->apk_app_descriptor.front() == 'L' &&
      options->apk_app_descriptor.back() == ';' &&
      Present(options->apk_app_support_dex) &&
      options->framework_res_apk.starts_with('/');
  options->run_framework_button = !options->has_apk_app_identity_environment &&
                                  has_test_fonts_xml &&
                                  options->framework_res_apk.starts_with('/');
  options->use_framework_resources = options->run_apk_app || options->run_framework_button;
  if (!options->has_window_scale || window_scale == "1") {
    options->window_scale = 1;
  } else if (window_scale == "2") {
    options->window_scale = 2;
  } else {
    *error = "ART Android APK app environment is incomplete or invalid";
    return 48;
  }
  options->expect_apk_widgets = options->run_apk_app &&
                                Env("DARWIN_ART_APK_APP_EXPECT_WIDGETS") == "1";

  if ((options->has_apk_app_identity_environment && !options->run_apk_app) ||
      (options->has_framework_res_apk && !options->use_framework_resources)) {
    *error = "ART Android APK app environment is incomplete or invalid";
    return 48;
  }
  if (options->run_network_acceptance &&
      (options->run_elf_jni_fixture || options->run_generic_elf ||
       options->run_apk_elf || options->run_libcxx_acceptance ||
       options->run_tls_acceptance || options->run_direct_apk)) {
    *error = "ART Android network fixture requires an isolated process";
    return 47;
  }
  return 0;
}

}  // namespace darwin_art_process
