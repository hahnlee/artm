#include <mach-o/dyld.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cstddef>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_art/darwin_art.h"
#include "debugger.h"
#include "darwin_framework_natives.h"
#include "darwin_binder_wire.h"
#include "darwin_provider_owners.h"
#include "runtime_network_probe.h"
#include "runtime_hwui_probe.h"
#include "runtime_elf_probe.h"
#include "runtime_abi_probe.h"
#include "runtime_process_state.h"
#include "runtime_process_options.h"
#include "runtime_acceptance_phases.h"
#include "runtime_jni_scope.h"
#include "runtime_frame_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_graphics_gpu.h"
#include "runtime_graphics_session.h"
#include "runtime_graphics_phase.h"
#include "runtime_jni_acceptance_probe.h"
#include "runtime_registration_phase.h"
#include "runtime_upstream_test.h"
#include "surfaceflinger/service_darwin.h"
#include "runtime_app_bootstrap.h"
#include "runtime_app_presentation.h"
#include "darwin_media_codec.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jni/java_vm_ext.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/throwable.h"
#include "plugin.h"
#include "runtime.h"
#include "jni/java_vm_ext.h"
#include "parsed_options.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "ti/agent.h"
#include "well_known_classes.h"

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#include "runtime_apk_graph.h"
#endif

extern "C" int darwin_art_install_context_loader(JNIEnv* env,
                                                   jobject app_loader);
extern "C" bool darwin_art_register_upstream_arttest(JNIEnv* env,
                                                       jclass harness);

namespace {

bool ConfigureAndroidLogTags() {
  const char* tags = std::getenv("ANDROID_LOG_TAGS");
  if (tags == nullptr) return true;
  std::string value(tags);
  size_t start = 0;
  while (start < value.size()) {
    while (start < value.size() && std::isspace(
                                        static_cast<unsigned char>(value[start]))) {
      ++start;
    }
    if (start == value.size()) break;
    const size_t end = value.find_first_of(" \t\r\n", start);
    const std::string spec = value.substr(start, end - start);
    if (spec.size() == 3 && spec[0] == '*' && spec[1] == ':') {
      using android::base::LogSeverity;
      LogSeverity severity;
      switch (spec[2]) {
        case 'v': severity = android::base::VERBOSE; break;
        case 'd': severity = android::base::DEBUG; break;
        case 'i': severity = android::base::INFO; break;
        case 'w': severity = android::base::WARNING; break;
        case 'e': severity = android::base::ERROR; break;
        case 'f':
        case 's': severity = android::base::FATAL_WITHOUT_ABORT; break;
        default:
          std::cerr << "unsupported '" << spec
                    << "' in ANDROID_LOG_TAGS (" << tags << ")\n";
          return false;
      }
      android::base::SetMinimumLogSeverity(severity);
    }
    start = end == std::string::npos ? value.size() : end + 1;
  }
  return true;
}

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written <= 0) return false;
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size != 0) {
    const ssize_t read_count = read(fd, bytes, size);
    if (read_count <= 0) return false;
    bytes += read_count;
    size -= static_cast<size_t>(read_count);
  }
  return true;
}

std::string ResolveDaemonPackage(const char* package_name) {
  // Android's package manager publishes immutable system packages in addition
  // to user-installed packages. Keep the first compatibility services in the
  // system-server registry rather than teaching individual applications about
  // them. The records intentionally advertise only package identity; Binder
  // services are added separately when an app actually requests one.
  static constexpr const char* kGoogleAndroidReleaseCertificate =
      "MIIEQzCCAyugAwIBAgIJAMLgh0ZkSjCNMA0GCSqGSIb3DQEBBAUAMHQxCzAJBgNVBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlh"
      "MRYwFAYDVQQHEw1Nb3VudGFpbiBWaWV3MRQwEgYDVQQKEwtHb29nbGUgSW5jLjEQMA4GA1UECxMHQW5kcm9pZDEQMA4GA1UEAxMH"
      "QW5kcm9pZDAeFw0wODA4MjEyMzEzMzRaFw0zNjAxMDcyMzEzMzRaMHQxCzAJBgNVBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlh"
      "MRYwFAYDVQQHEw1Nb3VudGFpbiBWaWV3MRQwEgYDVQQKEwtHb29nbGUgSW5jLjEQMA4GA1UECxMHQW5kcm9pZDEQMA4GA1UEAxMH"
      "QW5kcm9pZDCCASAwDQYJKoZIhvcNAQEBBQADggENADCCAQgCggEBAKtWLgDYO6IIrgqWbxJOKdoR8qtW0I9Y4sypEwPpt1TTcvZA"
      "pxsdyxMJZ2JORland2qSGT2y5b+3JKkedxiLDmpHpDsz2WCbdxgxRczfey5YZnTJ4VZbH0xqWVW/8lGmPav5xVwnIiJS6HXk+BVK"
      "ZF+JcWjAsb/GEuq/eFdpuzSqeYTcfi6idkyugwfYwXFU1+5fZKUaRKYCwkkFQVfcAs1fXA5V+++FGfvjJ/CxURaSxaBvGdGDhfXE"
      "28LWuT9ozCl5xw4Yq5OGazvV24mZVSoOO0yZ31j7kYvtwYK6NeADwbSxDdJEqO4k//0zOHKrUiGYXtqw/A0LFFtqoZKFjnkCAQOj"
      "gdkwgdYwHQYDVR0OBBYEFMd9jMIhF1Ylmn/Tgt9r45jk14alMIGmBgNVHSMEgZ4wgZuAFMd9jMIhF1Ylmn/Tgt9r45jk14aloXik"
      "djB0MQswCQYDVQQGEwJVUzETMBEGA1UECBMKQ2FsaWZvcm5pYTEWMBQGA1UEBxMNTW91bnRhaW4gVmlldzEUMBIGA1UEChMLR29v"
      "Z2xlIEluYy4xEDAOBgNVBAsTB0FuZHJvaWQxEDAOBgNVBAMTB0FuZHJvaWSCCQDC4IdGZEowjTAMBgNVHRMEBTADAQH/MA0GCSqG"
      "SIb3DQEBBAUAA4IBAQBt0lLO74UwLDYKqs6Tm8/yzKkEu116FmH4rkaymUIE0P9KaMftGlMexFlaYjzmB2OxZyl6euNXEsQH8gjw"
      "yxCUKRJNexBiGcCEyj6z+a1fuHHvkiaai+KL8W1EyNmgjmyy8AW7P+LLlkR+ho5zEHatRbM/YAnqGcFh5iZBqpknHf1SKMXFh4dd"
      "239FJ1jWYfbMDMy3NS5CTMQ2XFI1MvcyUTdZPErjQfTbQe3aDQsQcafEQPD+nqActifKZ0Np0IS9L9kR/wbNvyz6ENwPiTrjV2KR"
      "kEjH78ZMcUQXg0L3BYHJ3lc69Vs5Ddf9uUGGMYldX3WfMBEmh/9iFBDAaTCK";
  const std::string package(package_name == nullptr ? "" : package_name);
  const char* guest_apk = nullptr;
  const char* version_name = nullptr;
  const char* label = nullptr;
  if (package == "com.google.android.gms") {
    guest_apk = "/system/priv-app/GmsCore/GmsCore.apk";
    version_name = "25.12.00";
    label = "Google Play services";
  } else if (package == "com.android.vending") {
    guest_apk = "/system/priv-app/Phonesky/Phonesky.apk";
    version_name = "45.2.19";
    label = "Google Play Store";
  }
  if (guest_apk != nullptr) {
    return std::string("darwin-art-launch-v1\napk=") + guest_apk +
           "\ndex=" + guest_apk +
           "\nsha256=darwin-art-system-package\nmetadata=darwin-art-system:" +
           " package=" + package +
           " system=true enabled=true version_code=251200000 version_name=" +
           version_name + " target_sdk=36 label=" + label +
           " signature_base64=" + kGoogleAndroidReleaseCertificate +
           " permissions=com.google.android.c2dm.permission.SEND\n";
  }
  const char* socket_path = std::getenv("DARWIN_ART_PROFILE_SOCKET");
  if (socket_path == nullptr || package_name == nullptr) return {};
  if (package.empty() || package.size() > 255 ||
      !std::all_of(package.begin(), package.end(), [](unsigned char byte) {
        return std::isalnum(byte) || byte == '.' || byte == '_' || byte == '-';
      })) {
    return {};
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (std::strlen(socket_path) >= sizeof(address.sun_path)) {
    close(fd);
    return {};
  }
  std::memcpy(address.sun_path, socket_path, std::strlen(socket_path) + 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return {};
  }
  uint8_t header[16]{};
  std::memcpy(header, "DARTD001", 8);
  const uint16_t version = 1;
  const uint16_t operation = 6;
  std::memcpy(header + 8, &version, sizeof(version));
  std::memcpy(header + 10, &operation, sizeof(operation));
  const uint32_t length = static_cast<uint32_t>(package.size());
  std::memcpy(header + 12, &length, sizeof(length));
  if (!WriteAll(fd, header, sizeof(header)) ||
      !WriteAll(fd, package.data(), package.size()) ||
      !ReadAll(fd, header, sizeof(header)) ||
      std::memcmp(header, "DARTD001", 8) != 0) {
    close(fd);
    return {};
  }
  uint16_t response_version = 0;
  uint16_t response_operation = 0;
  std::memcpy(&response_version, header + 8, sizeof(response_version));
  std::memcpy(&response_operation, header + 10, sizeof(response_operation));
  if (response_version != version || response_operation != (operation | 0x8000)) {
    close(fd);
    return {};
  }
  uint32_t response_length = 0;
  std::memcpy(&response_length, header + 12, sizeof(response_length));
  if (response_length < 4 || response_length > 64 * 1024) {
    close(fd);
    return {};
  }
  std::vector<uint8_t> response(response_length);
  const bool received = ReadAll(fd, response.data(), response.size());
  close(fd);
  uint32_t status = 1;
  if (received) std::memcpy(&status, response.data(), sizeof(status));
  return received && status == 0
             ? std::string(response.begin() + 4, response.end())
             : std::string();
}

jstring NativeResolveDaemonPackage(JNIEnv* env, jclass, jstring package_name) {
  if (package_name == nullptr) return nullptr;
  const char* utf = env->GetStringUTFChars(package_name, nullptr);
  if (utf == nullptr) return nullptr;
  const std::string record = ResolveDaemonPackage(utf);
  env->ReleaseStringUTFChars(package_name, utf);
  return record.empty() ? nullptr : env->NewStringUTF(record.c_str());
}

jclass LoadApplicationClass(JNIEnv* env, jobject loader, const char* name) {
  jclass loader_class = env->GetObjectClass(loader);
  jmethodID load = loader_class == nullptr
                       ? nullptr
                       : env->GetMethodID(loader_class, "loadClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring class_name = env->NewStringUTF(name);
  jobject loaded = load == nullptr
                       ? nullptr
                       : env->CallObjectMethod(loader, load, class_name);
  env->DeleteLocalRef(class_name);
  env->DeleteLocalRef(loader_class);
  return static_cast<jclass>(loaded);
}

int RunSystemServerLite(JNIEnv* env, jobject app_loader) {
  const char* socket_path = std::getenv("DARWIN_ART_SYSTEM_SERVER_SOCKET");
  if (socket_path == nullptr || *socket_path == '\0') {
    std::cerr << "ART system_server-lite: socket capability missing\n";
    return 70;
  }
  jclass server = LoadApplicationClass(
      env, app_loader, "dev.darwinart.system.DarwinSystemServer");
  JNINativeMethod methods[] = {
      {const_cast<char*>("nativeResolvePackage"),
       const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&NativeResolveDaemonPackage)},
  };
  if (server == nullptr || env->ExceptionCheck() ||
      env->RegisterNatives(server, methods, 1) != JNI_OK) {
    std::cerr << "ART system_server-lite: could not load/register DarwinSystemServer"
              << " class=" << server << " exception=" << env->ExceptionCheck()
              << "\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 70;
  }
  jmethodID create = env->GetStaticMethodID(
      server, "createPackageRegistry", "()Landroid/os/Binder;");
  jobject registry = create == nullptr
                         ? nullptr
                         : env->CallStaticObjectMethod(server, create);
  if (registry == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART system_server-lite: could not create package registry"
              << " method=" << create << " registry=" << registry
              << " exception=" << env->ExceptionCheck() << "\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 70;
  }

  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    std::cerr << "ART system_server-lite: socket() failed: "
              << std::strerror(errno) << "\n";
    return 70;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (std::strlen(socket_path) >= sizeof(address.sun_path)) {
    std::cerr << "ART system_server-lite: socket path exceeds Darwin limit: "
              << socket_path << "\n";
    close(listener);
    return 70;
  }
  std::memcpy(address.sun_path, socket_path, std::strlen(socket_path) + 1);
  const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (probe >= 0 &&
      connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    close(probe);
    close(listener);
    return 0;
  }
  if (probe >= 0) close(probe);
  unlink(socket_path);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      chmod(socket_path, 0600) != 0 || listen(listener, 16) != 0) {
    std::cerr << "ART system_server-lite: publish failed for " << socket_path
              << ": " << std::strerror(errno) << "\n";
    close(listener);
    return 70;
  }
  std::cerr << "ART system_server-lite: package registry ready socket="
            << socket_path << "\n";
  for (;;) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) continue;
    if (!darwin_art::StartServingRemoteBinder(env, client, registry)) {
      close(client);
    }
  }
}

}  // namespace

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_run_process(
    const darwin_art_process_config_t* config,
    darwin_art_process_result_t* run_result) {
  darwin_art_process::ProcessConfigBounds config_bounds;
  std::string config_error;
  const int config_status = darwin_art_process::ValidateProcessConfig(
      config, run_result, &config_bounds, &config_error);
  if (config_status != 0) {
    std::cerr << "darwin_art_run_process: " << config_error << "\n";
    return config_status;
  }
  const uint64_t heap_initial = config_bounds.heap_initial_bytes;
  const uint64_t heap_maximum = config_bounds.heap_maximum_bytes;

  const darwin_art_lifecycle_hooks_t* lifecycle_hooks =
      config->struct_size >=
                  offsetof(darwin_art_process_config_t, lifecycle_hooks) +
                      sizeof(config->lifecycle_hooks)
          ? config->lifecycle_hooks
          : nullptr;
  if (!darwin_art_process::begin_run(lifecycle_hooks)) {
    std::cerr << "darwin_art_run_process: process already started\n";
    return DARWIN_ART_STATUS_PROCESS_ALREADY_STARTED;
  }
  const darwin_art_host_services_t* host_services =
      config->struct_size >=
              offsetof(darwin_art_process_config_t, host_services) +
                  sizeof(config->host_services)
          ? config->host_services
          : nullptr;
  if (!darwin_art_process::record_host_services(host_services)) {
    std::cerr << "darwin_art_run_process: invalid host-services sidecar\n";
    return 64;
  }
  // The graphics sidecar is an additive tail of the ABI. Never read it from
  // a legacy prefix, and never overload host_context with graphics state.
  if (config->struct_size >=
          offsetof(darwin_art_process_config_t, graphics_session_context) +
              sizeof(config->graphics_session_context) &&
      config->graphics_session_context != nullptr &&
      darwin_art_graphics::bind_session_for_process(
          config->graphics_session_context) != 0) {
    std::cerr << "darwin_art_run_process: graphics session binding failed\n";
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  auto* graphics_state = darwin_art_graphics::state_for_context(
      config->graphics_session_context);
  darwin_art_process::record_graphics_state(graphics_state);
  darwin_art_process::ScopedRunBoundary process_boundary;
  darwin_art_process::ProcessOptions process_options;
  std::string options_error;
  const int options_status =
      darwin_art_process::LoadProcessOptions(&process_options, &options_error);
  if (options_status != 0) {
    std::cerr << options_error << "\n";
    return options_status;
  }
  const char* elf_fixture_path = process_options.elf_fixture_path.c_str();
  const char* generic_elf_path = process_options.generic_elf_path.c_str();
  const char* apk_elf_path = process_options.apk_elf_path.c_str();
  const char* apk_sha256 = process_options.apk_sha256.c_str();
  const char* apk_root_sha256 = process_options.apk_root_sha256.c_str();
  const char* direct_apk_path = process_options.direct_apk_path.c_str();
  const char* direct_apk_root = process_options.direct_apk_root.c_str();
  const char* libcxx_collections_path =
      process_options.libcxx_collections_path.c_str();
  const char* libcxx_exception_path = process_options.libcxx_exception_path.c_str();
  const char* tls_fixture_path = process_options.tls_fixture_path.c_str();
  const char* network_fixture_path = process_options.network_fixture_path.c_str();
  const char* apk_app_package = process_options.apk_app_package.c_str();
  const char* apk_app_activity = process_options.apk_app_activity.c_str();
  const char* apk_app_descriptor = process_options.apk_app_descriptor.c_str();
  const char* apk_app_support_dex = process_options.apk_app_support_dex.c_str();
  const char* apk_app_native_path = process_options.apk_app_native_path.c_str();
  const char* framework_res_apk = process_options.framework_res_apk.c_str();
  const bool run_elf_jni_fixture = process_options.run_elf_jni_fixture;
  const bool run_generic_elf = process_options.run_generic_elf;
  const bool run_apk_elf = process_options.run_apk_elf;
  const bool run_direct_apk = process_options.run_direct_apk;
  const bool run_libcxx_acceptance = process_options.run_libcxx_acceptance;
  const bool run_tls_acceptance = process_options.run_tls_acceptance;
  const bool run_network_acceptance = process_options.run_network_acceptance;
  const bool has_apk_app_identity_environment =
      process_options.has_apk_app_identity_environment;
  const bool run_apk_app = process_options.run_apk_app;
  const bool run_system_server =
      run_apk_app && std::getenv("DARWIN_ART_SYSTEM_SERVER_MODE") != nullptr;
  const char* service_component =
      run_apk_app ? std::getenv("DARWIN_ART_APK_SERVICE_COMPONENT") : nullptr;
  const char* service_control =
      run_apk_app ? std::getenv("DARWIN_ART_SERVICE_CONTROL_FD") : nullptr;
  const bool run_service_process = service_component != nullptr &&
                                   *service_component != '\0' &&
                                   service_control != nullptr;
  std::string service_class_name;
  std::string service_descriptor;
  if (run_service_process) {
    const char* slash = std::strchr(service_component, '/');
    service_class_name = slash == nullptr ? service_component : slash + 1;
    if (!service_class_name.empty() && service_class_name.front() == '.') {
      service_class_name = process_options.apk_app_package + service_class_name;
    }
    service_descriptor = "L" + service_class_name + ";";
    std::replace(service_descriptor.begin(), service_descriptor.end(), '.', '/');
  }
  const bool run_framework_button = process_options.run_framework_button;
  const bool use_framework_resources = process_options.use_framework_resources;
  const jint window_scale = process_options.window_scale;
  constexpr jint kApkFrameWidth = 360;
  constexpr jint kApkFrameHeight = 640;
  const bool expect_apk_widgets = process_options.expect_apk_widgets;

  // Android's native launchers initialize libbase logging before creating the
  // VM. Honor the same global ANDROID_LOG_TAGS contract so ART DEBUG lifecycle
  // diagnostics (including metrics reporter startup/shutdown) are observable.
  if (!ConfigureAndroidLogTags()) return 54;

  // Darwin's malloc zones can claim the fixed compressed-reference window
  // while RuntimeArgumentMap is being assembled. Reserve ART's bounded arena
  // after the one-shot process gate, but before the launcher performs its first
  // heap allocation. MemMap::Init itself is process-global and not safe for
  // concurrent callers.
  art::MemMap::Init();

  darwin_art_frame_probe::configure(config->host_context, config->frame_callback);

  if (!darwin_art::InitializeFrameworkGraphicsRuntime()) {
    std::cerr << "ART Darwin graphics: runtime initialization failed\n";
    return 36;
  }

  std::string boot_class_path =
      std::string(config->core_oj_jar) + ":" + config->core_libart_jar + ":" +
      config->framework_jar + ":" + config->core_icu4j_jar;
  if (const char* configured_boot_class_path =
          std::getenv("DARWIN_ART_BOOT_CLASSPATH");
      configured_boot_class_path != nullptr && configured_boot_class_path[0] != '\0') {
    boot_class_path = configured_boot_class_path;
  }
  std::cerr << "Mach-O slide: 0x" << std::hex << _dyld_get_image_vmaddr_slide(0)
            << std::dec << "\n";
  art::Locks::Init();
  if (std::getenv("DARWIN_ART_UPSTREAM_METRICS") != nullptr) {
    // These are ART flags rather than RuntimeArgumentMap keys. Parse the same
    // options dalvikvm receives so gFlags records their command-line origin;
    // the detached launcher continues to assemble the remaining typed runtime
    // arguments below.
    const art::RuntimeOptions metrics_options{
        {"-Xmetrics-force-enable:true", nullptr},
        {"-Xmetrics-write-to-logcat:true", nullptr},
        {"-Xmetrics-reporting-mods:100", nullptr},
    };
    art::RuntimeArgumentMap parsed_metrics;
    if (!art::ParsedOptions::Parse(
            metrics_options, /*ignore_unrecognized=*/false, &parsed_metrics)) {
      return 53;
    }
  }
  art::RuntimeArgumentMap options;
  if (const char* serialized_options =
          std::getenv("DARWIN_ART_RUNTIME_OPTIONS");
      serialized_options != nullptr && serialized_options[0] != '\0') {
    std::vector<std::string> option_storage;
    std::istringstream option_stream(serialized_options);
    for (std::string option; std::getline(option_stream, option);) {
      if (!option.empty()) option_storage.push_back(std::move(option));
    }
    art::RuntimeOptions parsed_options;
    for (const std::string& option : option_storage) {
      parsed_options.emplace_back(option, nullptr);
    }
    if (!art::ParsedOptions::Parse(
            parsed_options, /*ignore_unrecognized=*/false, &options)) {
      std::cerr << "ART runtime: invalid serialized runtime options\n";
      return 55;
    }
  }
  options.Set(art::RuntimeArgumentMap::BootClassPath,
              art::ParseStringList<':'>::Split(boot_class_path));
  std::string boot_class_path_locations = boot_class_path;
  if (const char* configured_locations =
          std::getenv("DARWIN_ART_BOOT_CLASSPATH_LOCATIONS");
      configured_locations != nullptr && configured_locations[0] != '\0') {
    boot_class_path_locations = configured_locations;
  }
  options.Set(art::RuntimeArgumentMap::BootClassPathLocations,
              art::ParseStringList<':'>::Split(boot_class_path_locations));
  // Android's zygote trusts boot oat files by their logical /system location,
  // while this detached host stores the same components in a build directory.
  // Keep those identities separate: pass exact backing files through ART's
  // standard BCP FD contract and make only the image's symbolic location
  // logical. The vector is indexed by the complete BCP, with -1 reserved for
  // components that have no image artifact (for example the unsafe probe DEX).
  if (const char* boot_image_root =
          std::getenv("DARWIN_ART_BOOT_IMAGE_FD_ROOT");
      boot_image_root != nullptr && boot_image_root[0] != '\0') {
    const std::vector<std::string> bcp =
        art::ParseStringList<':'>::Split(boot_class_path);
    std::vector<int> image_fds(bcp.size(), -1);
    std::vector<int> vdex_fds(bcp.size(), -1);
    std::vector<int> oat_fds(bcp.size(), -1);
    auto open_component = [&](size_t index, const char* suffix) {
      std::string name = "boot";
      if (index != 0u) {
        const size_t slash = bcp[index].rfind('/');
        std::string base = bcp[index].substr(slash == std::string::npos ? 0u : slash + 1u);
        const size_t dot = base.rfind('.');
        if (dot != std::string::npos) base.resize(dot);
        name += "-" + base;
      }
      name += suffix;
      const std::string path = std::string(boot_image_root) + "/" + name;
      const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        std::cerr << "ART runtime: missing boot image component " << path << "\n";
      }
      return fd;
    };
    // The final BCP entry is an unsafe test DEX and intentionally has no
    // boot image component. Every generated Android 16 boot component before
    // it must have all three files.
    const size_t component_count = bcp.size() == 0u ? 0u : bcp.size() - 1u;
    for (size_t i = 0; i < component_count; ++i) {
      image_fds[i] = open_component(i, ".art");
      vdex_fds[i] = open_component(i, ".vdex");
      oat_fds[i] = open_component(i, ".oat");
      if (image_fds[i] < 0 || vdex_fds[i] < 0 || oat_fds[i] < 0) return 56;
    }
    options.Set(art::RuntimeArgumentMap::Image,
                art::ParseStringList<':'>::Split("/system/framework/boot.art"));
    options.Set(art::RuntimeArgumentMap::BootClassPathImageFds,
                art::ParseIntList<':'> (std::move(image_fds)));
    options.Set(art::RuntimeArgumentMap::BootClassPathVdexFds,
                art::ParseIntList<':'> (std::move(vdex_fds)));
    options.Set(art::RuntimeArgumentMap::BootClassPathOatFds,
                art::ParseIntList<':'> (std::move(oat_fds)));
  }
  std::string application_class_path = config->app_dex;
  if (run_system_server) {
    application_class_path = apk_app_support_dex;
  } else if (run_apk_app && apk_app_support_dex[0] != '\0') {
    application_class_path.insert(0, std::string(apk_app_support_dex) + ":");
  }
  options.Set(art::RuntimeArgumentMap::ClassPath,
              application_class_path);
  // ClassLoader.SystemClassLoader is initialized lazily from this property.
  // Android's app_process/dalvikvm launcher supplies the process class path at
  // VM creation; setting only the thread context loader later leaves custom
  // ClassLoader parent delegation attached to the boot loader.
  std::vector<std::string> runtime_properties{
      std::string("java.class.path=") + application_class_path};
  if (const char* java_tmpdir = std::getenv("DARWIN_ART_JAVA_IO_TMPDIR");
      java_tmpdir != nullptr && java_tmpdir[0] == '/') {
    runtime_properties.emplace_back(std::string("java.io.tmpdir=") +
                                    java_tmpdir);
  }
  options.Set(art::RuntimeArgumentMap::PropertiesList,
              std::move(runtime_properties));
  // Android application processes use ART's JIT unless the launcher requests
  // otherwise. `-Xusejit:false` disables JIT compilation but does not mean
  // `-Xint`: AOSP's ordinary `--interpreter` run-test configuration still
  // permits executable AOT code and otherwise enters Nterp. ParsedOptions owns
  // the independent `-Xint`/Interpret setting, so do not overwrite it while
  // selecting JIT policy here. The environment override exists only for
  // differential tests and diagnostics; applications do not need a
  // Darwin-specific hardware/JIT flag.
  const char* jit_mode = std::getenv("DARWIN_ART_JIT");
  bool enable_jit = true;
  if (jit_mode != nullptr) {
    if (std::strcmp(jit_mode, "1") == 0) {
      enable_jit = true;
    } else if (std::strcmp(jit_mode, "0") == 0) {
      enable_jit = false;
    } else {
      std::cerr << "ART runtime: invalid JIT mode " << jit_mode << "\n";
      return 55;
    }
  }
  if (enable_jit && std::getenv("DARWIN_ART_JIT_TRACE") != nullptr) {
    art::gLogVerbosity.jit = true;
    art::gLogVerbosity.deopt = true;
  }
  options.Set(art::RuntimeArgumentMap::UseJitCompilation, enable_jit);
  if (std::getenv("DARWIN_ART_UPSTREAM_ZYGOTE") != nullptr) {
    options.Set(art::RuntimeArgumentMap::Zygote, art::Unit{});
    options.Set(art::RuntimeArgumentMap::JITCodeCacheInitialCapacity,
                art::MemoryKiB(64 * art::MB));
  }
  if (std::getenv("DARWIN_ART_UPSTREAM_MAIN") != nullptr) {
    // AOSP test/default_run.py always supplies this option for run-tests so a
    // SIGQUIT assertion tests ART callbacks rather than stressing libunwind.
    options.Set(art::RuntimeArgumentMap::DumpNativeStackOnSigQuit, false);
  }
  // AOSP dalvikvm/run-test expresses startup instrumentation as an
  // OpenJDK-JVMTI plugin plus -agentpath. Keep the option in ART's native
  // RuntimeArgumentMap so the plugin enters ONLOAD/LIVE phases at the same
  // lifecycle points as Android, rather than invoking agent callbacks from
  // the host harness.
  const char* startup_agent =
      std::getenv("DARWIN_ART_UPSTREAM_JVMTI_AGENT");
  const bool has_startup_agent =
      startup_agent != nullptr && startup_agent[0] != '\0';
  const bool has_deferred_agent =
      std::getenv("DARWIN_ART_UPSTREAM_DEFERRED_JVMTI_AGENT") != nullptr;
  // AOSP run-test can provide -agentpath directly from run.py without an
  // extra Darwin environment variable. Preserve those parsed agent specs and
  // still load the OpenJDK JVMTI plugin that owns their callbacks.
  const bool has_parsed_agents = options.Exists(art::RuntimeArgumentMap::AgentPath);
  if (has_startup_agent || has_deferred_agent || has_parsed_agents) {
    // AOSP plugin tests pass their own libartagent path. The runner rewrites
    // that Android alias to the staged test DSO, which must remain the sole
    // plugin so its ArtPlugin lifecycle hooks run exactly once. Ordinary
    // agent-only launches still need Darwin's generic OpenJDK JVMTI plugin.
    if (!options.Exists(art::RuntimeArgumentMap::Plugins)) {
      std::vector<art::Plugin> plugins;
      plugins.push_back(art::Plugin::Create("libopenjdkjvmti.so"));
      options.Set(art::RuntimeArgumentMap::Plugins, std::move(plugins));
    }
    // Android's debuggable run-test configuration supplies
    // -Xopaque-jni-ids:true even when -agentpath is attached after zygote
    // specialization. Structural JVMTI extensions and stable IDs must start
    // in indexed mode before any class exposes a jmethodID.
    options.Set(art::RuntimeArgumentMap::OpaqueJniIds,
                art::JniIdType::kIndices);
  }
  if (has_startup_agent) {
    std::list<art::ti::AgentSpec> agents;
    agents.emplace_back(startup_agent);
    options.Set(art::RuntimeArgumentMap::AgentPath, std::move(agents));
  }
  if (std::getenv("DARWIN_ART_UPSTREAM_SWAPPABLE_JNI_IDS") != nullptr) {
    // AOSP's 1972/1973 launch contract replaces the ordinary debuggable
    // indexed-ID option with -Xopaque-jni-ids:swapable and disables automatic
    // promotion. The tests then exercise both legal runtime transitions.
    options.Set(art::RuntimeArgumentMap::OpaqueJniIds,
                art::JniIdType::kSwapablePointer);
    options.Set(art::RuntimeArgumentMap::AutoPromoteOpaqueJniIds, false);
  }
  // ParsedOptions already collected every literal `-Xcompiler-option` pair
  // supplied by app_process/run-test. Extend that Android-owned vector rather
  // than replacing it with launcher defaults; otherwise options such as
  // `--debuggable` silently disappear before JitCompiler reads them.
  std::vector<std::string> compiler_options =
      options.GetOrDefault(art::RuntimeArgumentMap::CompilerOptions);
  // AOSP's run-test launcher always supplies --compile-art-test.  Besides
  // checker assertions, this makes the optimizing compiler honor the
  // $noinline$/$inline$ method-name contracts used by the unmodified test
  // corpus.  Keep it scoped to that launcher contract; production APKs use
  // normal Android inlining policy.
  if (std::getenv("DARWIN_ART_UPSTREAM_MAIN") != nullptr) {
    compiler_options.emplace_back("--compile-art-test");
  }
  // app_process passes the package's ApplicationInfo.FLAG_DEBUGGABLE state
  // into ART as a compiler option before Runtime::Create().  This is not a
  // test switch: debuggable JIT code retains the dex-register environments
  // required by JDWP/JVMTI and asynchronous full-stack deoptimization, while
  // release applications keep the normal optimizing compiler policy.
  bool java_debuggable = false;
  if (const char* debuggable =
          std::getenv("DARWIN_ART_RUNTIME_JAVA_DEBUGGABLE");
      debuggable != nullptr) {
    if (std::strcmp(debuggable, "1") == 0) {
      java_debuggable = true;
      compiler_options.emplace_back("--debuggable");
    } else if (std::strcmp(debuggable, "0") != 0) {
      std::cerr << "ART runtime: invalid Java debuggable state "
                << debuggable << "\n";
      return 50;
    }
  }
  if (!compiler_options.empty()) {
    options.Set(art::RuntimeArgumentMap::CompilerOptions,
                std::move(compiler_options));
  }
  // app_process passes -Xtarget-sdk-version before Runtime::Create so class
  // verification observes the application's compatibility behavior. Keep the
  // detached host contract equally early; setting VMRuntime after classes are
  // loaded is too late for verifier policy.
  if (const char* target_sdk =
          std::getenv("DARWIN_ART_RUNTIME_TARGET_SDK_VERSION");
      target_sdk != nullptr && target_sdk[0] != '\0') {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(target_sdk, &end, 10);
    if (end == target_sdk || *end != '\0' || parsed > UINT32_MAX) {
      std::cerr << "ART runtime: invalid target SDK " << target_sdk << "\n";
      return 49;
    }
    options.Set(art::RuntimeArgumentMap::TargetSdkVersion,
                static_cast<unsigned int>(parsed));
  }
  if (const char* finalizer_timeout =
          std::getenv("DARWIN_ART_RUNTIME_FINALIZER_TIMEOUT_MS");
      finalizer_timeout != nullptr && finalizer_timeout[0] != '\0') {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(finalizer_timeout, &end, 10);
    if (end == finalizer_timeout || *end != '\0' || parsed > UINT32_MAX) {
      std::cerr << "ART runtime: invalid finalizer timeout "
                << finalizer_timeout << "\n";
      return 52;
    }
    options.Set(art::RuntimeArgumentMap::FinalizerTimeoutMs,
                static_cast<unsigned int>(parsed));
  }
  // app_process supplies device defaults, while dalvikvm/run-test may supply
  // an explicit -Xms/-Xmx policy. Preserve ParsedOptions ownership whenever
  // the Android command line selected a value; the host config is only the
  // detached launcher's default.
  if (!options.Exists(art::RuntimeArgumentMap::MemoryInitialSize)) {
    options.Set(art::RuntimeArgumentMap::MemoryInitialSize,
                art::MemoryKiB(heap_initial));
  }
  // Android's normal launcher supplies Xms, HeapGrowthLimit, and Xmx as
  // separate values. The detached runtime previously used Xms as the growth
  // limit, permanently capping apps at 64 MiB even when Xmx was 256 MiB.
  if (!options.Exists(art::RuntimeArgumentMap::HeapGrowthLimit)) {
    options.Set(art::RuntimeArgumentMap::HeapGrowthLimit,
                art::MemoryKiB(heap_maximum));
  }
  if (!options.Exists(art::RuntimeArgumentMap::MemoryMaximumSize)) {
    options.Set(art::RuntimeArgumentMap::MemoryMaximumSize,
                art::MemoryKiB(heap_maximum));
  }
  art::LogVerbosity verbosity{};
  verbosity.heap = true;
  options.Set(art::RuntimeArgumentMap::Verbose, verbosity);

  // Startup-agent output belongs to ART run-test's native process stream.
  // Darwin otherwise block-buffers redirected stdout and reorders ONLOAD text
  // behind later Java descriptor writes.
  if (std::getenv("DARWIN_ART_UPSTREAM_JVMTI_AGENT") != nullptr &&
      std::setvbuf(stdout, nullptr, _IONBF, 0) != 0) {
    return 51;
  }

  if (!art::Runtime::Create(std::move(options))) {
    return 1;
  }
  // Android's zygote specialization publishes DEBUG_ENABLE_JDWP separately
  // from ApplicationInfo.FLAG_DEBUGGABLE. ART run-tests that attach a limited
  // JVMTI environment use that process capability while deliberately keeping
  // Java compilation non-debuggable. Preserve the same split contract in the
  // detached launcher instead of weakening Runtime::AttachAgent().
  if (std::getenv("DARWIN_ART_UPSTREAM_JVMTI") != nullptr) {
    art::Dbg::SetJdwpAllowed(true);
  }
  // Zygote children normally publish this state while specializing the app
  // process. This detached process has no zygote, so establish the equivalent
  // Runtime state immediately after creation and before loading app classes.
  // The compiler option above independently controls emitted CodeInfo.
  if (java_debuggable) {
    art::Runtime::Current()->SetRuntimeDebugState(
        art::Runtime::RuntimeDebugState::kJavaDebuggableAtInit);
  }
  if (enable_jit && std::getenv("DARWIN_ART_JIT_TRACE") != nullptr) {
    std::cerr << "ART runtime: java-debuggable="
              << (art::Runtime::Current()->IsJavaDebuggable() ? 1 : 0)
              << " at-init="
              << (art::Runtime::Current()->IsJavaDebuggableAtInit() ? 1 : 0)
              << "\n";
  }

  art::Thread* self = art::Thread::Current();
  darwin_art_process::record_created_runtime(self);
  if (self == nullptr) {
    std::cerr << "ART Darwin DEX: no current thread\n";
    return 2;
  }
  if (config->struct_size >=
          offsetof(darwin_art_process_config_t, graphics_session_context) +
              sizeof(config->graphics_session_context) &&
      config->graphics_session_context != nullptr &&
      darwin_art_graphics::bind_session_art_thread(self) != 0) {
    std::cerr << "ART graphics: session ART-thread binding failed\n";
    return 33;
  }
  JNIEnv* env = self->GetJniEnv();

  // Android loads libopenjdk's boot JNI owner before boot classes execute.
  // Do the same for the composed Darwin owner so core-oj named methods are
  // discoverable during the first class initializers, not only after app load.
  if (const char* owner = std::getenv("DARWIN_ART_OPENJDK_NAMED_JNI_OWNER");
      owner != nullptr && *owner != '\0') {
    std::string load_error;
    if (!art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
            env, owner, nullptr, nullptr, &load_error)) {
      std::cerr << "ART OpenJDK boot JNI owner load failed: " << load_error
                << "\n";
      return 18;
    }
  }

  process_boundary.set_art_thread(self);
  if (config->provider_acquire != nullptr) {
    darwin_art::providers::darwin_art_provider_install_hooks(
        config->provider_context, config->provider_acquire,
        config->provider_release);
    darwin_art_process::record_provider_hooks_installed();
  }
  return [&]() -> int32_t {

  art::interpreter::UnstartedRuntime::Initialize();
  art::ScopedObjectAccess soa(self);
  darwin_art_jni_scope::ScopedLocalFrame local_frame(self->GetJniEnv());
  if (!local_frame.valid()) {
    std::cerr << "ART Darwin JNI: local frame allocation failed\n";
    return 34;
  }
  art::WellKnownClasses::Init(self->GetJniEnv());

  art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();
  art::StackHandleScope<32> hs(self);
  const int runtime_start_status =
      darwin_art_registration_phase::start(env, self);
  if (runtime_start_status != 0) {
    return runtime_start_status;
  }
  const char* activity_descriptor =
      run_service_process
          ? service_descriptor.c_str()
          : (run_apk_app && !run_system_server
                 ? apk_app_descriptor
                 : "Ldev/darwinart/probe/ProbeActivity;");
  const char* process_dex =
      run_system_server ? apk_app_support_dex : config->app_dex;
  darwin_art_app::ClassSet app_classes;
  const int app_status = darwin_art_app::load_classes(
      self->GetJniEnv(), self, class_linker, soa, hs,
      run_apk_app && !run_system_server, process_dex,
      apk_app_support_dex, apk_app_native_path, activity_descriptor,
      run_direct_apk, direct_apk_path,
      run_elf_jni_fixture, run_network_acceptance,
      darwin_art::GetFrameworkGraphicsBackend() ==
          darwin_art::FrameworkGraphicsBackend::kProbeCanvas,
      &app_classes);
  if (app_status != 0) {
    std::cerr << "ART application class load failed status=" << app_status
              << " process_dex=" << process_dex
              << " support_dex=" << apk_app_support_dex << "\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return app_status;
  }
  jobject app_loader_ref = app_classes.app_loader;
  jclass hello_class = app_classes.hello;
  jclass probe_activity_class = app_classes.activity;
  jclass probe_context_class = app_classes.context;
  jclass probe_resources_class = app_classes.resources;
  jclass probe_view_class = app_classes.view;
  jclass probe_canvas_class = app_classes.canvas;
  jclass content_root_class = app_classes.content_root;
  jclass package_manager_class = app_classes.package_manager;
  jclass native_fixture_class = app_classes.native_fixture;
  jclass network_fixture_class = app_classes.network_fixture;
  art::Handle<art::mirror::Class> hello =
      hs.NewHandle(soa.Decode<art::mirror::Class>(hello_class));
  if (std::getenv("DARWIN_ART_TEST_SURFACE_LOCK_CANVAS") != nullptr &&
      !darwin_art_presentation::verify_software_surface_canvas(env)) {
    std::cerr << "ART Android Surface: Java software Canvas acceptance failed\n";
    return 39;
  }
  if (std::getenv("DARWIN_ART_TEST_MEDIA_CODEC_SURFACE") != nullptr &&
      !darwin_art::VerifyDarwinMediaCodecSurfaceLifecycle(env)) {
    std::cerr << "ART Android MediaCodec: output Surface acceptance failed\n";
    return 40;
  }
  if (std::getenv("DARWIN_ART_UPSTREAM_MAIN") != nullptr) {
    // This harness is the Darwin equivalent of AOSP's dalvikvm command, not
    // an Activity process. Its shutdown must follow AndroidRuntime exactly.
    darwin_art_process::record_dalvikvm_process();
    if (darwin_art_install_context_loader(env, app_loader_ref) != 0) {
      std::cerr << "ART upstream test: context ClassLoader installation failed\n";
      return 120;
    }
    if (!darwin_art_upstream_test::Prepare(
            env, app_classes.upstream_test_harness)) {
      std::cerr << "ART upstream test: output capture preparation failed\n";
      return 121;
    }
    if (!darwin_art_register_upstream_arttest(
            env, app_classes.upstream_test_harness)) {
      std::cerr << "ART upstream test: libarttest registration failed\n";
      return 125;
    }
    if (!darwin_art_upstream_test::ResolveMainDexStrings(
            env, soa, hs, app_classes.upstream_test_harness)) {
      std::cerr << "ART upstream test: application Dex string resolution failed\n";
      return 124;
    }
    // VDEX-backed run-test invocations must observe the compiler-produced oat
    // entrypoint as AOT.  Forcing a JIT CompileMain here replaces the
    // instrumentation entrypoint and makes isAotCompiled() report false even
    // when the adjacent oat/vdex contract was loaded successfully.
    const bool vdex_backed = std::getenv("DARWIN_ART_UPSTREAM_VDEX") != nullptr;
    if (vdex_backed) {
      std::cerr << "ART upstream test: AOT application method selected void Main.main(java.lang.String[])\n";
    }
    if (enable_jit && !vdex_backed && !darwin_art_upstream_test::CompileMain(
            env, self, soa, hs, app_classes.upstream_test_harness)) {
      std::cerr << "ART upstream test: optimized Main compilation failed\n";
      return 123;
    }
    return darwin_art_upstream_test::Run(env, app_classes.upstream_test_harness);
  }
  // Run the compiler/JNI gate without depending on UI resource setup.
  if (std::getenv("DARWIN_ART_JIT_ACCEPTANCE_ONLY") != nullptr) {
    darwin_art_jni_acceptance_phase::Results acceptance{};
    const int status = darwin_art_jni_acceptance_phase::run(
        env, self, class_linker, hello, hello_class, &acceptance);
    run_result->hello_answer = acceptance.hello_answer;
    run_result->native_round_trip = acceptance.native_round_trip;
    run_result->arraycopy_result = acceptance.arraycopy_result;
    return status;
  }
  art::Handle<art::mirror::Class> probe_activity =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_activity_class));
  art::Handle<art::mirror::Class> probe_context_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_context_class));
  art::Handle<art::mirror::Class> probe_resources_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_resources_class));
  art::Handle<art::mirror::Class> probe_view_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(probe_view_class));
  art::Handle<art::mirror::Class> content_root_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(content_root_class));
  art::Handle<art::mirror::Class> package_manager_handle =
      hs.NewHandle(soa.Decode<art::mirror::Class>(package_manager_class));
  art::MutableHandle<art::mirror::Class> native_fixture_handle(
      hs.NewHandle(soa.Decode<art::mirror::Class>(native_fixture_class)));
  art::MutableHandle<art::mirror::Class> network_fixture_handle(
      hs.NewHandle(soa.Decode<art::mirror::Class>(network_fixture_class)));
  art::Handle<art::mirror::Class> framework_activity = hs.NewHandle(
      class_linker->FindSystemClass(self, "Landroid/app/Activity;"));
  if (framework_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity class lookup failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 21;
  }
  if (content_root_handle == nullptr || package_manager_handle == nullptr ||
      (run_elf_jni_fixture && native_fixture_handle == nullptr) ||
      (run_network_acceptance && network_fixture_handle == nullptr) ||
      self->IsExceptionPending()) {
    std::cerr << "ART Android window: Darwin Canvas/Window lookup failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 21;
  }

  const int registration_status = darwin_art_registration_phase::finish(
      {.env = env,
       .self = self,
       .app_loader_ref = app_loader_ref,
       .probe_canvas_class = probe_canvas_class,
       .graphics_state = graphics_state});
  if (registration_status != 0) {
    return registration_status;
  }

  if (run_system_server) {
    if (!darwin_art_surfaceflinger_service_start()) {
      std::cerr << "ART system_server-lite: SurfaceFlinger service setup failed\n";
      return 70;
    }
    if (darwin_art_install_context_loader(env, app_loader_ref) != 0) {
      std::cerr << "ART system_server-lite: ClassLoader setup failed\n";
      return 70;
    }
    const int status = RunSystemServerLite(env, app_loader_ref);
    run_result->hello_answer = 0;
    run_result->native_round_trip = 0;
    run_result->arraycopy_result = 0;
    run_result->activity_probe_result = 0;
    run_result->lifecycle_result = 0;
    run_result->frame_width = 0;
    run_result->frame_height = 0;
    return status;
  }

  // Android's managed System.load/Runtime.nativeLoad path reaches
  // JavaVMExt only after the app PathClassLoader and thread context loader
  // have been installed.  Loading earlier makes JNI_OnLoad observe the boot
  // loader and breaks RegisterNatives for app-owned classes.
  const bool managed_process_native_path =
      run_apk_app || std::getenv("DARWIN_ART_UPSTREAM_MAIN") != nullptr;
  if (managed_process_native_path &&
      !process_options.apk_app_native_path.empty()) {
    if (darwin_art_app::install_native_library_path(
            env, app_loader_ref, apk_app_native_path) != 0) {
      std::cerr << "ART Android APK: PathClassLoader native path setup failed\n";
      return 46;
    }
    // Real Android APKs load JNI libraries from the managed
    // System.load/Runtime.nativeLoad path during class initialization.  Do
    // not eagerly call JavaVMExt here as that would make the subsequent
    // System.loadLibrary a recursive second load of the same image.  The
    // explicit loader remains available for the isolated fixture/direct APK
    // gates, while the normal app path follows the platform ordering.
    const char* managed_native_load =
        std::getenv("DARWIN_ART_APK_MANAGED_NATIVE_LOAD");
    if (run_apk_app &&
        (managed_native_load == nullptr ||
         std::strcmp(managed_native_load, "1") != 0)) {
      const int native_status = darwin_art_app::load_native_library(
          env, self, app_loader_ref, apk_app_native_path);
      if (native_status != 0) {
        return native_status;
      }
    }
  }

  if (!class_linker->EnsureInitialized(self, probe_activity, true, true)) {
    std::cerr << "ART Android framework: launcher Activity initialization failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return 22;
  }
  jmethodID activity_constructor =
      env->GetMethodID(probe_activity_class, "<init>", "()V");
  // APK Activity construction is deferred to the attached Java UI thread.
  // Activity/Fragment hosts capture thread identity during construction.
  jobject activity_instance =
      run_apk_app || activity_constructor == nullptr
          ? nullptr
          : env->NewObject(probe_activity_class, activity_constructor);
  jmethodID probe_value =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_activity_class, "probeValue", "()I");
  jint activity_result =
      run_apk_app
          ? 42
          : (activity_instance == nullptr || probe_value == nullptr
                 ? -1
                 : env->CallIntMethod(activity_instance, probe_value));
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android framework: ProbeActivity constructor threw\n";
    env->ExceptionDescribe();
    env->ExceptionClear();
    return 23;
  }
  if (activity_result != 42) {
    std::cerr << "ART Android framework: expected 42, got " << activity_result
              << "\n";
    return 24;
  }

  jobject package_manager =
      package_manager_class == nullptr
          ? nullptr
          : soa.AddLocalReference<jobject>(
                package_manager_handle->AllocObject(self));
  if (package_manager == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: package feature stub failed\n";
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    return 27;
  }
  jint lifecycle_result = 43;
  int presentation_status = 0;
  if (run_service_process) {
    if (darwin_art_install_context_loader(env, app_loader_ref) != 0) {
      std::cerr << "ART Android service: context ClassLoader setup failed\n";
      return 27;
    }
    const jint control_fd = static_cast<jint>(std::strtol(
        service_control, nullptr, 10));
    presentation_status = darwin_art_presentation::run_service(
        env, self, probe_activity_class, probe_context_class,
        probe_resources_class, package_manager, use_framework_resources,
        window_scale, framework_res_apk, apk_app_package,
        service_class_name.c_str(),
        process_options.apk_app_resource_apk.c_str(), control_fd);
    if (presentation_status != 0) return presentation_status;
    run_result->hello_answer = 0;
    run_result->native_round_trip = 0;
    run_result->arraycopy_result = 0;
    run_result->activity_probe_result = 0;
    run_result->lifecycle_result = 0;
    run_result->frame_width = 0;
    run_result->frame_height = 0;
    return 0;
  }
  // The ELF/JNI acceptance fixture is a headless execution test.  It does
  // not represent an Android application process and deliberately has no
  // support DEX containing DarwinServiceBridge, so entering the window
  // presentation bootstrap here would exercise an unrelated service-manager
  // dependency and turn a JNI test into an IServiceManager NPE.  Real APK
  // processes and the framework-button probe still take the complete
  // presentation path below.
  const bool run_headless_elf_fixture =
      run_elf_jni_fixture && !run_apk_app && !run_framework_button &&
      !run_service_process;
  if (run_headless_elf_fixture) {
    presentation_status = 0;
  } else if (run_apk_app) {
    // The ART process owner remains the Android main/UI Looper thread recorded
    // by begin_run(). NSWindow/CAMetalLayer creation is marshalled to the
    // AppKit actor, while graphics/input callbacks stay on this owner thread;
    // no JNI or GraphicsSession state crosses into AppKit.
    if (darwin_art_graphics::prepare_gpu_surface(
            graphics_state, kApkFrameWidth * window_scale,
            kApkFrameHeight * window_scale) != 0) {
      std::cerr << "ART Android GPU: main-thread surface preparation failed\n";
      return 33;
    }
    // registration_phase already prepared the main Looper and installed the
    // APK PathClassLoader on this exact ART owner thread. Reassert the
    // context-loader binding here as an explicit precondition for Activity
    // construction, then bootstrap the service bridge before any framework
    // object can consult it.
    jclass looper_class = env->FindClass("android/os/Looper");
    jmethodID my_looper =
        looper_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(looper_class, "myLooper",
                                     "()Landroid/os/Looper;");
    jmethodID main_looper =
        looper_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(looper_class, "getMainLooper",
                                     "()Landroid/os/Looper;");
    jobject owner_looper =
        my_looper == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(looper_class, my_looper);
    jobject process_main_looper =
        main_looper == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(looper_class, main_looper);
    if (looper_class == nullptr || my_looper == nullptr ||
        main_looper == nullptr || owner_looper == nullptr ||
        process_main_looper == nullptr ||
        !env->IsSameObject(owner_looper, process_main_looper) ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android owner: main Looper is not prepared on owner\n";
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      env->DeleteLocalRef(process_main_looper);
      env->DeleteLocalRef(owner_looper);
      env->DeleteLocalRef(looper_class);
      return 25;
    }
    env->DeleteLocalRef(process_main_looper);
    env->DeleteLocalRef(owner_looper);
    env->DeleteLocalRef(looper_class);
    if (darwin_art_install_context_loader(env, app_loader_ref) != 0) {
      std::cerr << "ART Android owner: context ClassLoader setup failed\n";
      return 27;
    }
    jclass binder_internal =
        env->FindClass("com/android/internal/os/BinderInternal");
    jmethodID get_context_object =
        binder_internal == nullptr
            ? nullptr
            : env->GetStaticMethodID(binder_internal, "getContextObject",
                                     "()Landroid/os/IBinder;");
    jobject context_binder =
        get_context_object == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(binder_internal, get_context_object);
    jmethodID handle_binder_gc =
        binder_internal == nullptr
            ? nullptr
            : env->GetStaticMethodID(binder_internal, "handleGc", "()V");
    if (handle_binder_gc != nullptr) {
      env->CallStaticVoidMethod(binder_internal, handle_binder_gc);
    }
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    if (handle_binder_gc == nullptr) {
      std::cerr << "ART Android owner: BinderInternal.handleGc is not registered\n";
      env->DeleteLocalRef(context_binder);
      env->DeleteLocalRef(binder_internal);
      return 28;
    }
    uint64_t owner_thread_id = 0;
    (void)pthread_threadid_np(nullptr, &owner_thread_id);
    std::cerr << "ART Android owner: thread="
              << reinterpret_cast<uintptr_t>(pthread_self())
              << " tid=" << owner_thread_id
              << " art=" << self << " looper=main context_loader=1"
              << " service_bridge=" << (context_binder != nullptr) << "\n";
    env->DeleteLocalRef(context_binder);
    env->DeleteLocalRef(binder_internal);

    presentation_status = darwin_art_presentation::run(
        env, self, activity_instance, probe_activity_class, probe_context_class,
        probe_resources_class, probe_view_class, probe_canvas_class,
        content_root_class, package_manager, true, use_framework_resources,
        expect_apk_widgets, !process_options.apk_app_native_path.empty(),
        run_framework_button, window_scale, framework_res_apk, apk_app_package,
        apk_app_activity, process_options.apk_app_resource_apk.c_str(),
        graphics_state);
  } else {
    presentation_status = darwin_art_presentation::run(
        env, self, activity_instance, probe_activity_class, probe_context_class,
        probe_resources_class, probe_view_class, probe_canvas_class,
        content_root_class, package_manager, false, use_framework_resources,
        expect_apk_widgets, !process_options.apk_app_native_path.empty(),
        run_framework_button, window_scale, framework_res_apk, apk_app_package,
        apk_app_activity, process_options.apk_app_resource_apk.c_str(),
        graphics_state);
  }
  if (presentation_status != 0) {
    return presentation_status;
  }
  if (run_apk_app &&
      darwin_art_graphics::refresh_gpu_surface_identity(graphics_state) != 0) {
    std::cerr << "ART Android GPU: application identity update failed\n";
    return 33;
  }

  darwin_art_jni_acceptance_phase::Results jni_results;
  const int jni_status = darwin_art_jni_acceptance_phase::run(
      env, self, class_linker, hello, hello_class, &jni_results);
  if (jni_status != 0) {
    return jni_status;
  }

  if (run_network_acceptance &&
      darwin_art_network_phase::run(env, network_fixture_path,
                                    app_loader_ref, network_fixture_class) != 0) {
    return 47;
  }

  if (run_elf_jni_fixture) {
    const darwin_art_elf_probe::FixtureGraphAcceptance fixture_input{
        .env = env,
        .self = self,
        .app_loader_ref = app_loader_ref,
        .native_fixture_class = native_fixture_class,
        .elf_fixture_path = elf_fixture_path,
        .generic_elf_path = generic_elf_path,
        .libcxx_collections_path = libcxx_collections_path,
        .libcxx_exception_path = libcxx_exception_path,
        .tls_fixture_path = tls_fixture_path,
        .apk_sha256 = apk_sha256,
        .apk_root_sha256 = apk_root_sha256,
        .run_generic_elf = run_generic_elf,
        .run_apk_elf = run_apk_elf,
        .run_libcxx_acceptance = run_libcxx_acceptance,
        .run_tls_acceptance = run_tls_acceptance,
    };
    const int fixture_status =
        darwin_art_elf_probe::run_fixture_graph_acceptance(fixture_input);
    if (fixture_status != 0) {
      return fixture_status;
    }
  }

  run_result->hello_answer = jni_results.hello_answer;
  run_result->native_round_trip = jni_results.native_round_trip;
  run_result->arraycopy_result = jni_results.arraycopy_result;
  run_result->activity_probe_result = activity_result;
  run_result->lifecycle_result = lifecycle_result;
  const auto frame_dimensions = darwin_art_frame_probe::dimensions();
  run_result->frame_width = static_cast<uint32_t>(frame_dimensions.width);
  run_result->frame_height = static_cast<uint32_t>(frame_dimensions.height);
  return 0;
  }();
}
