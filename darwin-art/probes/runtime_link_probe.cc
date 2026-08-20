#include <arpa/inet.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "art_method-inl.h"
#include "base/locks.h"
#include "base/mem_map.h"
#include "base/logging.h"
#include "class_linker.h"
#include "cmdline_types.h"
#include "darwin_art/darwin_art.h"
#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_framework_natives.h"
#include "darwin_hwui_gpu_mode.h"
#include "darwin_surface_bridge.h"
#include "darwin_icu_natives.h"
#include "darwin_libcore_natives.h"
#include "darwin_openjdk_natives.h"
#include "dex/art_dex_file_loader.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jni/java_vm_ext.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/throwable.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "well_known_classes.h"

bool InstallProbeAndroidSystemRoot() {
  const char* root = std::getenv("DARWIN_ART_ANDROID_SYSTEM_ROOT");
  if (root == nullptr || root[0] == '\0') return true;
  const int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    std::cerr << "ART Android filesystem: cannot open test root " << root << "\n";
    return false;
  }
  constexpr uint8_t kMount[] = {'/', 's', 'y', 's', 't', 'e', 'm'};
  const auto status = darwin_art_bionic_fs_process_install(
      fd, kMount, sizeof(kMount), kMount, sizeof(kMount));
  close(fd);
  std::cerr << "ART Android filesystem: test root status="
            << static_cast<int>(status) << " root=" << root << "\n";
  return status == DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK ||
         status == DARWIN_ART_BIONIC_FS_PROCESS_OWNER_ALREADY_INSTALLED;
}

#if defined(DARWIN_ART_REAL_GRAPHICS)
#ifdef HIDDEN
#undef HIDDEN
#endif
#include "hwui/Canvas.h"
#define private public
#include "RenderNode.h"
#undef private
#include "pipeline/skia/RenderNodeDrawable.h"
#include "pipeline/skia/SkiaRecordingCanvas.h"
#include "include/core/SkSurface.h"
#endif

#if defined(DARWIN_ART_DIRECT_APK_RUNTIME)
#define main darwin_art_direct_apk_standalone_main
#include "../tools/android-apk-native-direct-load/direct_load.cc"
#undef main

#include <limits.h>
#include <unordered_set>

namespace {

struct DirectApkDiscoveredGraph {
  Mapping mapping;
  std::vector<Entry> entries;
  std::vector<std::string> names;
  std::vector<DarwinArtElfGraphSource> sources;
  std::string root;
};

std::mutex g_direct_apk_graphs_mutex;
std::unordered_set<DarwinArtElfDiscoveredGraph*> g_direct_apk_graphs;

void SetDirectApkError(DarwinArtElfErrorBuffer* error,
                       const std::string& message) {
  if (error == nullptr) return;
  error->required = message.size() + 1;
  if (error->data == nullptr || error->capacity == 0) return;
  const size_t count = std::min(message.size(), error->capacity - 1);
  std::memcpy(error->data, message.data(), count);
  error->data[count] = '\0';
}

bool IsDirectApkGraph(DarwinArtElfDiscoveredGraph* graph) {
  std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
  return g_direct_apk_graphs.contains(graph);
}

DarwinArtElfStatus BuildDirectApkGraph(
    const char* apk_path, const char* root,
    const char* const* provider_sonames, size_t provider_count,
    DarwinArtElfDiscoveredGraph** out_graph, DarwinArtElfErrorBuffer* error) {
  auto graph = std::make_unique<DirectApkDiscoveredGraph>();
  std::string failure;
  if (!graph->mapping.Open(apk_path, &failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  size_t mutation_offset = std::numeric_limits<size_t>::max();
  if (!Parse(graph->mapping, &graph->entries, &mutation_offset, &failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_FORMAT;
  }
  std::unordered_map<std::string, const Entry*> native;
  for (const Entry& entry : graph->entries) {
    if (entry.native) native.emplace(entry.leaf, &entry);
  }
  if (!native.contains(root)) {
    SetDirectApkError(error, "direct APK root entry is absent");
    return DARWIN_ART_ELF_FORMAT;
  }
  std::unordered_set<std::string> providers;
  for (size_t index = 0; index < provider_count; ++index) {
    if (provider_sonames[index] != nullptr) {
      providers.emplace(provider_sonames[index]);
    }
  }
  std::deque<std::string> queue{root};
  std::unordered_set<std::string> queued{root};
  while (!queue.empty()) {
    std::string name = std::move(queue.front());
    queue.pop_front();
    const Entry& entry = *native.at(name);
    char storage[512] = {};
    DarwinArtElfErrorBuffer inspect_error{storage, sizeof(storage), 0};
    DarwinArtElfInspection* inspection = nullptr;
    DarwinArtElfStatus status = darwin_art_elf_inspect_bytes(
        graph->mapping.data() + entry.data_offset, entry.uncompressed_size,
        &inspection, &inspect_error);
    if (status != DARWIN_ART_ELF_OK || inspection == nullptr) {
      SetDirectApkError(error, std::string("direct APK ELF inspection failed: ") + storage);
      return status;
    }
    const char* soname = nullptr;
    size_t needed_count = 0;
    status = darwin_art_elf_inspection_soname(inspection, &soname, &inspect_error);
    if (status != DARWIN_ART_ELF_OK || soname == nullptr || name != soname) {
      darwin_art_elf_inspection_destroy(&inspection);
      SetDirectApkError(error, "direct APK leaf and DT_SONAME differ");
      return DARWIN_ART_ELF_FORMAT;
    }
    status = darwin_art_elf_inspection_needed_count(
        inspection, &needed_count, &inspect_error);
    if (status != DARWIN_ART_ELF_OK) {
      darwin_art_elf_inspection_destroy(&inspection);
      SetDirectApkError(error, "direct APK DT_NEEDED count failed");
      return status;
    }
    for (size_t index = 0; index < needed_count; ++index) {
      const char* needed = nullptr;
      status = darwin_art_elf_inspection_needed_at(
          inspection, index, &needed, &inspect_error);
      if (status != DARWIN_ART_ELF_OK || needed == nullptr) {
        darwin_art_elf_inspection_destroy(&inspection);
        SetDirectApkError(error, "direct APK DT_NEEDED inspection failed");
        return status;
      }
      if (providers.contains(needed)) continue;
      if (!native.contains(needed)) {
        darwin_art_elf_inspection_destroy(&inspection);
        SetDirectApkError(error, "direct APK DT_NEEDED escaped the closed namespace");
        return DARWIN_ART_ELF_UNRESOLVED_SYMBOL;
      }
      if (queued.insert(needed).second) queue.emplace_back(needed);
    }
    darwin_art_elf_inspection_destroy(&inspection);
    graph->names.push_back(std::move(name));
  }
  graph->root = root;
  graph->sources.reserve(graph->names.size());
  for (const std::string& name : graph->names) {
    const Entry& entry = *native.at(name);
    graph->sources.push_back(DarwinArtElfGraphSource{
        name.c_str(), graph->mapping.data() + entry.data_offset,
        entry.uncompressed_size});
  }
  if (!graph->mapping.Unchanged(&failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  auto* published = reinterpret_cast<DarwinArtElfDiscoveredGraph*>(graph.release());
  {
    std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
    g_direct_apk_graphs.insert(published);
  }
  *out_graph = published;
  return DARWIN_ART_ELF_OK;
}

}  // namespace

extern "C" DarwinArtElfStatus darwin_art_direct_discover_sibling_graph(
    int directory_fd, const uint8_t* root_component,
    size_t root_component_length, const char* const* provider_sonames,
    size_t provider_count, int* out_root_is_elf,
    DarwinArtElfDiscoveredGraph** out_graph, DarwinArtElfErrorBuffer* error) {
  const char* expected_apk = std::getenv("DARWIN_ART_DIRECT_APK_FIXTURE");
  const char* root = std::getenv("DARWIN_ART_DIRECT_APK_ROOT");
  if (expected_apk == nullptr || root == nullptr || root_component == nullptr ||
      root_component_length == 0 || out_graph == nullptr) {
    return darwin_art_elf_discover_sibling_graph(
        directory_fd, root_component, root_component_length, provider_sonames,
        provider_count, out_root_is_elf, out_graph, error);
  }
  char directory_path[PATH_MAX] = {};
  if (fcntl(directory_fd, F_GETPATH, directory_path) != 0) {
    SetDirectApkError(error, "direct APK directory F_GETPATH failed");
    return DARWIN_ART_ELF_IO;
  }
  std::string apk_path(directory_path);
  if (!apk_path.ends_with('/')) apk_path.push_back('/');
  apk_path.append(reinterpret_cast<const char*>(root_component),
                  root_component_length);
  if (apk_path != expected_apk) {
    return darwin_art_elf_discover_sibling_graph(
        directory_fd, root_component, root_component_length, provider_sonames,
        provider_count, out_root_is_elf, out_graph, error);
  }
  if (out_root_is_elf != nullptr) *out_root_is_elf = 1;
  return BuildDirectApkGraph(apk_path.c_str(), root, provider_sonames,
                             provider_count, out_graph, error);
}

extern "C" DarwinArtElfStatus darwin_art_direct_discovered_graph_root_soname(
    const DarwinArtElfDiscoveredGraph* graph, const char** out_soname,
    DarwinArtElfErrorBuffer* error) {
  auto* mutable_graph = const_cast<DarwinArtElfDiscoveredGraph*>(graph);
  if (!IsDirectApkGraph(mutable_graph)) {
    return darwin_art_elf_discovered_graph_root_soname(graph, out_soname, error);
  }
  if (out_soname == nullptr) return DARWIN_ART_ELF_INVALID_ARGUMENT;
  *out_soname = reinterpret_cast<const DirectApkDiscoveredGraph*>(graph)->root.c_str();
  return DARWIN_ART_ELF_OK;
}

extern "C" DarwinArtElfStatus darwin_art_direct_discovered_graph_sources(
    const DarwinArtElfDiscoveredGraph* graph,
    const DarwinArtElfGraphSource** out_sources, size_t* out_count,
    DarwinArtElfErrorBuffer* error) {
  auto* mutable_graph = const_cast<DarwinArtElfDiscoveredGraph*>(graph);
  if (!IsDirectApkGraph(mutable_graph)) {
    return darwin_art_elf_discovered_graph_sources(graph, out_sources, out_count,
                                                   error);
  }
  if (out_sources == nullptr || out_count == nullptr) {
    return DARWIN_ART_ELF_INVALID_ARGUMENT;
  }
  const auto* direct = reinterpret_cast<const DirectApkDiscoveredGraph*>(graph);
  std::string failure;
  if (!direct->mapping.Unchanged(&failure)) {
    SetDirectApkError(error, failure);
    return DARWIN_ART_ELF_IO;
  }
  *out_sources = direct->sources.data();
  *out_count = direct->sources.size();
  return DARWIN_ART_ELF_OK;
}

extern "C" void darwin_art_direct_discovered_graph_destroy(
    DarwinArtElfDiscoveredGraph** graph) {
  if (graph == nullptr || *graph == nullptr) return;
  bool direct = false;
  {
    std::lock_guard<std::mutex> lock(g_direct_apk_graphs_mutex);
    direct = g_direct_apk_graphs.erase(*graph) == 1;
  }
  if (direct) {
    delete reinterpret_cast<DirectApkDiscoveredGraph*>(*graph);
    *graph = nullptr;
  } else {
    darwin_art_elf_discovered_graph_destroy(graph);
  }
}
#endif

extern "C" int darwin_art_elf_jni_fixture_registration_status();
extern "C" int darwin_art_elf_jni_fixture_lifecycle_status();
extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status();

namespace android {
enum JNICallType {
  kJNICallTypeRegular = 1,
};
extern "C" void* NativeBridgeGetTrampoline2(void* handle, const char* name,
                                             const char* shorty, uint32_t len,
                                             JNICallType jni_call_type);
extern "C" void* OpenNativeLibrary(JNIEnv* env, int32_t target_sdk_version,
                                    const char* path, jobject class_loader,
                                    const char* caller_location,
                                    jstring library_path,
                                    bool* needs_native_bridge,
                                    char** error_msg);
extern "C" bool CloseNativeLibrary(void* handle, bool needs_native_bridge,
                                    char** error_msg);
extern "C" void NativeLoaderFreeErrorMessage(char* message);
}  // namespace android

static jint HostPageSize(JNIEnv*, jclass) { return getpagesize(); }

static bool RunAndroidElfSelfTest(JNIEnv* env, JavaVM* vm,
                                  jobject class_loader, const char* path,
                                  std::string* error) {
  bool needs_native_bridge = false;
  char* open_error = nullptr;
  void* handle = android::OpenNativeLibrary(
      env, 35, path, class_loader, nullptr, nullptr, &needs_native_bridge,
      &open_error);
  if (handle == nullptr || open_error != nullptr || !needs_native_bridge) {
    *error = open_error == nullptr ? "Android ELF open failed without detail"
                                   : open_error;
    android::NativeLoaderFreeErrorMessage(open_error);
    if (handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(handle, needs_native_bridge,
                                        &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    return false;
  }
  android::NativeLoaderFreeErrorMessage(open_error);

  void* entry = android::NativeBridgeGetTrampoline2(
      handle, "JNI_OnLoad", nullptr, 0, android::kJNICallTypeRegular);
  using JniOnLoad = jint (*)(JavaVM*, void*);
  const jint version = entry == nullptr
                           ? JNI_ERR
                           : reinterpret_cast<JniOnLoad>(entry)(vm, nullptr);

  char* close_error = nullptr;
  const bool closed =
      android::CloseNativeLibrary(handle, needs_native_bridge, &close_error);
  if (version != JNI_VERSION_1_6 || !closed || close_error != nullptr) {
    *error = close_error == nullptr
                 ? "self-testing JNI_OnLoad returned " +
                       std::to_string(static_cast<int>(version))
                 : close_error;
    android::NativeLoaderFreeErrorMessage(close_error);
    return false;
  }
  android::NativeLoaderFreeErrorMessage(close_error);
  return true;
}

class BoundedLoopbackHttpServer {
 public:
  ~BoundedLoopbackHttpServer() { Stop(); }

  bool Start() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return false;
    int enabled = 1;
    (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 1) != 0) {
      close(listener_);
      listener_ = -1;
      return false;
    }
    socklen_t length = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                    &length) != 0) {
      close(listener_);
      listener_ = -1;
      return false;
    }
    port_ = ntohs(address.sin_port);
    try {
      thread_ = std::thread([this] { Serve(); });
    } catch (...) {
      close(listener_);
      listener_ = -1;
      port_ = 0;
      return false;
    }
    return true;
  }

  int port() const { return port_; }

  bool Stop() {
    if (joined_) return success_.load(std::memory_order_acquire);
    cancelled_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(listener_mutex_);
      if (listener_ >= 0) (void)shutdown(listener_, SHUT_RDWR);
    }
    if (thread_.joinable()) thread_.join();
    joined_ = true;
    return success_.load(std::memory_order_acquire);
  }

 private:
  void Serve() {
    pollfd pending{listener_, POLLIN, 0};
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (cancelled_.load(std::memory_order_acquire)) break;
      const int ready = poll(&pending, 1, 100);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || (pending.revents & POLLIN) == 0) continue;
      const int client = accept(listener_, nullptr, nullptr);
      if (client < 0) break;
      timeval timeout{2, 0};
      (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout));
      (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout));
      std::array<char, 512> request{};
      size_t used = 0;
      while (used + 1 < request.size()) {
        const ssize_t count =
            recv(client, request.data() + used, request.size() - 1 - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += static_cast<size_t>(count);
        request[used] = '\0';
        if (std::strstr(request.data(), "\r\n\r\n") != nullptr) break;
      }
      static constexpr char kRequest[] =
          "GET /runtime HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
      static constexpr char kResponse[] =
          "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
      bool valid = used == sizeof(kRequest) - 1 &&
                   std::memcmp(request.data(), kRequest, sizeof(kRequest) - 1) ==
                       0;
      size_t sent = 0;
      while (valid && sent < sizeof(kResponse) - 1) {
        const ssize_t count = send(client, kResponse + sent,
                                   sizeof(kResponse) - 1 - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          valid = false;
          break;
        }
        sent += static_cast<size_t>(count);
      }
      (void)shutdown(client, SHUT_WR);
      close(client);
      success_.store(valid && sent == sizeof(kResponse) - 1,
                     std::memory_order_release);
      break;
    }
    {
      std::lock_guard<std::mutex> lock(listener_mutex_);
      close(listener_);
      listener_ = -1;
    }
  }

  int listener_ = -1;
  std::mutex listener_mutex_;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> success_{false};
  bool joined_ = false;
};

static jlong NativePackedIntegerStack(JNIEnv*, jclass, jint a0, jint a1,
                                      jint a2, jint a3, jint a4, jint a5,
                                      jint spilled, jlong wide, jint tail0,
                                      jint tail1) {
  return a0 == 10 && a1 == 11 && a2 == 12 && a3 == 13 && a4 == 14 &&
                 a5 == 15 && spilled == 0x10203040 &&
                 wide == INT64_C(0x1122334455667788) &&
                 tail0 == 0x50607080 && tail1 == 0x12345678
             ? static_cast<jlong>(UINT64_C(0x13579bdf2468ace0))
             : -1;
}

static jlong NativePackedFloatingStack(JNIEnv*, jclass, jfloat a0, jfloat a1,
                                       jfloat a2, jfloat a3, jfloat a4,
                                       jfloat a5, jfloat a6, jfloat a7,
                                       jfloat spilled, jdouble wide) {
  return a0 == 1.0f && a1 == 2.0f && a2 == 3.0f && a3 == 4.0f &&
                 a4 == 5.0f && a5 == 6.0f && a6 == 7.0f && a7 == 8.0f &&
                 spilled == 9.5f && wide == 10.25
             ? static_cast<jlong>(UINT64_C(0x02468ace13579bdf))
             : -1;
}

static jlong NativePackedReferenceStack(JNIEnv*, jclass, jint a0, jint a1,
                                        jint a2, jint a3, jint a4, jint a5,
                                        jint spilled, jobject reference,
                                        jint tail) {
  return a0 == 20 && a1 == 21 && a2 == 22 && a3 == 23 && a4 == 24 &&
                 a5 == 25 && spilled == 0x23456701 && reference != nullptr &&
                 tail == 0x34567812
             ? static_cast<jlong>(UINT64_C(0x55aa55aa33cc33cc))
             : -1;
}

static jlong NativePackedNarrowStack(JNIEnv*, jclass, jint a0, jint a1,
                                     jint a2, jint a3, jint a4, jint a5,
                                     jboolean bool_value, jbyte byte_value,
                                     jchar char_value, jshort short_value,
                                     jint int_value, jlong long_value) {
  const bool valid =
      a0 == 30 && a1 == 31 && a2 == 32 && a3 == 33 && a4 == 34 && a5 == 35 &&
      bool_value == JNI_TRUE && static_cast<std::uint8_t>(byte_value) == 0x81 &&
      char_value == 0xabcd &&
      static_cast<std::uint16_t>(short_value) == 0x8765 &&
      int_value == 0x45678923 &&
      long_value == INT64_C(0x2233445566778899);
  return valid ? static_cast<jlong>(UINT64_C(0x1122aabb3344ccdd)) : -1;
}

static std::size_t g_frame_width = 0;
static std::size_t g_frame_height = 0;
static jclass g_probe_canvas_class = nullptr;
static jobject g_interactive_root = nullptr;
static jobject g_pressed_view = nullptr;
static jint g_interactive_width = 0;
static jint g_interactive_height = 0;
static void* g_host_context = nullptr;
static darwin_art_frame_callback_t g_frame_callback = nullptr;
static DarwinArtSurface* g_gpu_surface = nullptr;
static bool g_apk_elf_loaded = false;
static std::string g_apk_sha256;
static std::string g_apk_root_sha256;
static bool g_direct_apk_loaded = false;
static bool g_network_elf_loaded = false;

static bool IsSha256(const char* value) {
  if (value == nullptr || std::strlen(value) != 64) {
    return false;
  }
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= '0' && *cursor <= '9') ||
          (*cursor >= 'a' && *cursor <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool IsPrivateExtractedRoot(const char* path) {
  if (path == nullptr) {
    return false;
  }
  struct stat path_stat {};
  struct stat followed_stat {};
  if (lstat(path, &path_stat) != 0 || stat(path, &followed_stat) != 0 ||
      !S_ISREG(path_stat.st_mode) || path_stat.st_dev != followed_stat.st_dev ||
      path_stat.st_ino != followed_stat.st_ino ||
      (path_stat.st_mode & 0777) != 0400) {
    return false;
  }
  const std::string root_path(path);
  const std::size_t separator = root_path.rfind('/');
  if (separator == std::string::npos || separator == 0) {
    return false;
  }
  const std::string directory = root_path.substr(0, separator);
  struct stat directory_stat {};
  return lstat(directory.c_str(), &directory_stat) == 0 &&
         S_ISDIR(directory_stat.st_mode) &&
         (directory_stat.st_mode & 0777) == 0500;
}

enum class ProcessPhase {
  kNeverStarted,
  kRunning,
  kAwaitingShutdown,
  kShuttingDown,
  kShutdownComplete,
  kCreateFailed,
  kShutdownFailed,
};

struct ProcessState {
  std::mutex mutex;
  ProcessPhase phase = ProcessPhase::kNeverStarted;
  pthread_t owner_thread{};
  bool owner_thread_valid = false;
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
  std::vector<std::unique_ptr<const art::DexFile>> app_dex_files;
};

static ProcessState g_process_state;

static bool BeginProcessRun() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  if (g_process_state.phase != ProcessPhase::kNeverStarted) {
    return false;
  }
  g_process_state.phase = ProcessPhase::kRunning;
  g_process_state.owner_thread = pthread_self();
  g_process_state.owner_thread_valid = true;
  return true;
}

static void RecordCreatedRuntime(art::Thread* art_thread) {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  CHECK(art::Runtime::Current() != nullptr);
  g_process_state.java_vm = reinterpret_cast<JavaVM*>(
      art::Runtime::Current()->GetJavaVM());
  g_process_state.art_thread = art_thread;
}

static void RecordResourceRuntimeInstalled() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  CHECK(!g_process_state.resource_runtime_installed);
  g_process_state.resource_runtime_installed = true;
}

static void FinishProcessRun() {
  std::lock_guard<std::mutex> lock(g_process_state.mutex);
  CHECK(g_process_state.phase == ProcessPhase::kRunning);
  g_process_state.phase = g_process_state.java_vm == nullptr
                              ? ProcessPhase::kCreateFailed
                              : ProcessPhase::kAwaitingShutdown;
}

// darwin_art_run_process is deliberately a one-shot process ABI for now. ART's
// Runtime::Create() leaves its main thread runnable, which is appropriate for
// the monolithic probe but not for returning to an arbitrary native host. Keep
// this guard outside the managed-work lambda so every normal exit first
// destroys ScopedObjectAccess/StackHandleScope and only then releases the
// mutator lock by returning the ART thread to kNative.
class ScopedProcessRunBoundary final {
 public:
  ScopedProcessRunBoundary() = default;

  ~ScopedProcessRunBoundary() {
    if (art_thread_ == nullptr) {
      FinishProcessRun();
      return;
    }
    CHECK_EQ(art::Thread::Current(), art_thread_);
    const art::ThreadState state = art_thread_->GetState();
    if (state == art::ThreadState::kRunnable) {
      art_thread_->TransitionFromRunnableToSuspended(
          art::ThreadState::kNative);
    } else {
      CHECK_EQ(state, art::ThreadState::kNative);
    }
    FinishProcessRun();
  }

  void SetArtThread(art::Thread* art_thread) {
    DCHECK(art_thread_ == nullptr);
    DCHECK(art_thread != nullptr);
    art_thread_ = art_thread;
  }

 private:
  art::Thread* art_thread_ = nullptr;
};

class ScopedJniLocalFrame final {
 public:
  explicit ScopedJniLocalFrame(JNIEnv* env)
      : env_(env), pushed_(env != nullptr && env->PushLocalFrame(256) == JNI_OK) {}

  ~ScopedJniLocalFrame() {
    if (pushed_) {
      env_->PopLocalFrame(nullptr);
    }
  }

  bool IsValid() const { return pushed_; }

 private:
  JNIEnv* env_;
  bool pushed_;
};

static jboolean PresentFrame(JNIEnv* env, jclass, jint width, jint height,
                             jintArray argb) {
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
      argb == nullptr) {
    return JNI_FALSE;
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (env->GetArrayLength(argb) != static_cast<jsize>(pixel_count)) {
    return JNI_FALSE;
  }
  std::vector<jint> pixels(pixel_count);
  env->GetIntArrayRegion(argb, 0, static_cast<jsize>(pixel_count), pixels.data());
  if (env->ExceptionCheck()) {
    return JNI_FALSE;
  }
  const bool presented =
      g_frame_callback == nullptr ||
      g_frame_callback(g_host_context,
                       reinterpret_cast<const std::uint32_t*>(pixels.data()),
                       static_cast<std::uint32_t>(width),
                       static_cast<std::uint32_t>(height),
                       static_cast<std::size_t>(width) * sizeof(std::uint32_t)) != 0;
  if (presented) {
    g_frame_width = static_cast<std::size_t>(width);
    g_frame_height = static_cast<std::size_t>(height);
  }
  return presented ? JNI_TRUE : JNI_FALSE;
}

#if defined(DARWIN_ART_REAL_GRAPHICS)
// Production GPU frame path: View.draw() records into AOSP's Skia
// RecordingCanvas, the resulting RenderNode is replayed directly into the
// CAMetalLayer drawable. No Bitmap, Java int[] or IOSurface CPU mapping is
// created in this path.
static jboolean PresentGpuContent(JNIEnv* env, jobject view, jint width,
                                  jint height) {
  if (!darwin_art::hwui_gpu_enabled()) {
    return JNI_FALSE;
  }
  if (g_gpu_surface == nullptr) {
    DarwinArtSurfaceCreateInfo info{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .title = "Darwin ART · HWUI Metal",
        .visible = true,
    };
    DarwinArtSurfaceResult result = DARWIN_ART_SURFACE_OK;
    g_gpu_surface = darwin_art_surface_create(&info, &result);
    if (g_gpu_surface == nullptr) {
      std::cerr << "ART HWUI GPU: surface initialization failed status="
                << result << "\n";
      return JNI_FALSE;
    }
    darwin_art_surface_set_active_gpu(g_gpu_surface);
  }

  std::unique_ptr<android::Canvas> recorder(
      android::Canvas::create_recording_canvas(width, height));
  if (recorder == nullptr) {
    std::cerr << "ART HWUI GPU: RecordingCanvas initialization failed\n";
    return JNI_FALSE;
  }
  jclass canvas_class = env->FindClass("android/graphics/Canvas");
  jmethodID canvas_ctor = canvas_class == nullptr
                              ? nullptr
                              : env->GetMethodID(canvas_class, "<init>", "()V");
  jfieldID native_canvas = canvas_class == nullptr
                               ? nullptr
                               : env->GetFieldID(canvas_class,
                                                 "mNativeCanvasWrapper", "J");
  jobject java_canvas = canvas_ctor == nullptr || native_canvas == nullptr
                            ? nullptr
                            : env->NewObject(canvas_class, canvas_ctor);
  if (java_canvas == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(canvas_class);
    std::cerr << "ART HWUI GPU: Canvas wrapper initialization failed\n";
    return JNI_FALSE;
  }
  const jlong original_canvas = env->GetLongField(java_canvas, native_canvas);
  env->SetLongField(java_canvas, native_canvas,
                    reinterpret_cast<jlong>(recorder.get()));
  jclass view_class = env->FindClass("android/view/View");
  jmethodID draw = view_class == nullptr
                       ? nullptr
                       : env->GetMethodID(view_class, "draw",
                                          "(Landroid/graphics/Canvas;)V");
  jmethodID measure = view_class == nullptr
                          ? nullptr
                          : env->GetMethodID(view_class, "measure", "(II)V");
  jmethodID layout = view_class == nullptr
                         ? nullptr
                         : env->GetMethodID(view_class, "layout", "(IIII)V");
  if (draw == nullptr || measure == nullptr || layout == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    env->SetLongField(java_canvas, native_canvas, original_canvas);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(java_canvas);
    env->DeleteLocalRef(canvas_class);
    return JNI_FALSE;
  }
  // The standalone probe has no ViewRoot/ThreadedRenderer to perform the
  // normal measure/layout pass. Give the real widget an exact portrait
  // viewport before recording so Button/TextView emits its display list.
  constexpr jint kMeasureExactly = 0x40000000;
  const jint width_spec = kMeasureExactly | (width & 0x3fffffff);
  const jint height_spec = kMeasureExactly | (height & 0x3fffffff);
  env->CallVoidMethod(view, measure, width_spec, height_spec);
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    env->SetLongField(java_canvas, native_canvas, original_canvas);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(java_canvas);
    env->DeleteLocalRef(canvas_class);
    std::cerr << "ART HWUI GPU: View measure/layout failed\n";
    return JNI_FALSE;
  }
  env->CallVoidMethod(view, draw, java_canvas);
  // Restore the Canvas() raster wrapper so its NativeAllocationRegistry can
  // safely finalize the object; the recording canvas remains C++ owned.
  env->SetLongField(java_canvas, native_canvas, original_canvas);
  const bool draw_ok = !env->ExceptionCheck();
  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(java_canvas);
  env->DeleteLocalRef(canvas_class);
  if (!draw_ok) {
    std::cerr << "ART HWUI GPU: View.draw failed\n";
    env->ExceptionClear();
    return JNI_FALSE;
  }

  auto display_list =
      static_cast<android::uirenderer::skiapipeline::SkiaRecordingCanvas*>(
          recorder.get())->finishRecording();
  if (display_list == nullptr || display_list->isEmpty()) {
    std::cerr << "ART HWUI GPU: empty display list\n";
    return JNI_FALSE;
  }
  android::sp<android::uirenderer::RenderNode> node =
      new android::uirenderer::RenderNode();
  node->mDisplayList = android::uirenderer::DisplayList(std::move(display_list));
  node->mValid = true;
  node->mProperties.setLeftTopRightBottom(0, 0, width, height);

  DarwinArtGpuFrame* frame = darwin_art_surface_gpu_begin(g_gpu_surface);
  if (frame == nullptr) {
    std::cerr << "ART HWUI GPU: drawable begin failed\n";
    return JNI_FALSE;
  }
  auto* canvas = static_cast<SkCanvas*>(darwin_art_surface_gpu_canvas(frame));
  if (canvas == nullptr) {
    darwin_art_surface_gpu_end(g_gpu_surface, frame);
    std::cerr << "ART HWUI GPU: drawable canvas unavailable\n";
    return JNI_FALSE;
  }
  android::uirenderer::skiapipeline::RenderNodeDrawable drawable(
      node.get(), canvas, false);
  drawable.forceDraw(canvas);
  const DarwinArtSurfaceResult result =
      darwin_art_surface_gpu_end(g_gpu_surface, frame);
  if (result != DARWIN_ART_SURFACE_OK) {
    std::cerr << "ART HWUI GPU: drawable submit failed status=" << result
              << "\n";
    return JNI_FALSE;
  }
  g_frame_width = static_cast<std::size_t>(width);
  g_frame_height = static_cast<std::size_t>(height);
  return JNI_TRUE;
}
#endif

static jboolean PresentContent(JNIEnv* env, jclass, jobject view, jint width,
                               jint height) {
  if (view == nullptr || width <= 0 || height <= 0 || width > 4096 ||
      height > 4096) {
    return JNI_FALSE;
  }
  const darwin_art::FrameworkGraphicsBackend graphics_backend =
      darwin_art::GetFrameworkGraphicsBackend();
  const bool use_real_graphics =
      graphics_backend ==
      darwin_art::FrameworkGraphicsBackend::kAndroidGraphics;
#if defined(DARWIN_ART_REAL_GRAPHICS)
  if (darwin_art::hwui_gpu_enabled()) {
    return PresentGpuContent(env, view, width, height);
  }
#endif
  jclass canvas_class = nullptr;
  jclass real_canvas_class = nullptr;
  jclass bitmap_class = nullptr;
  jclass bitmap_config_class = nullptr;
  jclass view_class = nullptr;
  jobject bitmap = nullptr;
  jobject canvas = nullptr;
  jintArray pixels = nullptr;
  jmethodID snapshot = nullptr;
  auto release_render_target = [&]() {
    env->DeleteLocalRef(pixels);
    env->DeleteLocalRef(view_class);
    env->DeleteLocalRef(canvas);
    env->DeleteLocalRef(bitmap);
    env->DeleteLocalRef(real_canvas_class);
    env->DeleteLocalRef(bitmap_config_class);
    env->DeleteLocalRef(bitmap_class);
  };
  if (!use_real_graphics) {
    canvas_class = g_probe_canvas_class;
    if (canvas_class == nullptr) {
      std::cerr << "ART Android view: ProbeCanvas class is not rooted\n";
      return JNI_FALSE;
    }
    art::ScopedObjectAccess soa(env);
    art::ObjPtr<art::mirror::Class> canvas_mirror =
        soa.Decode<art::mirror::Class>(canvas_class);
    canvas = soa.AddLocalReference<jobject>(canvas_mirror->AllocObject(soa.Self()));
    jmethodID initialize =
        canvas == nullptr || env->ExceptionCheck()
            ? nullptr
            : env->GetMethodID(canvas_class, "initialize", "(II)V");
    if (!env->ExceptionCheck()) {
      snapshot = env->GetMethodID(canvas_class, "snapshot", "()[I");
    }
    if (canvas != nullptr && initialize != nullptr && snapshot != nullptr &&
        !env->ExceptionCheck()) {
      env->CallVoidMethod(canvas, initialize, width, height);
    }
  } else {
    bitmap_class = env->FindClass("android/graphics/Bitmap");
    if (!env->ExceptionCheck()) {
      bitmap_config_class = env->FindClass("android/graphics/Bitmap$Config");
    }
    if (!env->ExceptionCheck()) {
      real_canvas_class = env->FindClass("android/graphics/Canvas");
    }
    canvas_class = real_canvas_class;
    jfieldID argb_8888 = nullptr;
    jobject bitmap_config = nullptr;
    jmethodID create_bitmap = nullptr;
    jmethodID canvas_constructor = nullptr;
    if (bitmap_class != nullptr && bitmap_config_class != nullptr &&
        canvas_class != nullptr && !env->ExceptionCheck()) {
      argb_8888 = env->GetStaticFieldID(
          bitmap_config_class, "ARGB_8888",
          "Landroid/graphics/Bitmap$Config;");
    }
    if (argb_8888 != nullptr && !env->ExceptionCheck()) {
      bitmap_config =
          env->GetStaticObjectField(bitmap_config_class, argb_8888);
    }
    if (bitmap_config != nullptr && !env->ExceptionCheck()) {
      create_bitmap = env->GetStaticMethodID(
          bitmap_class, "createBitmap",
          "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    }
    if (create_bitmap != nullptr && !env->ExceptionCheck()) {
      bitmap = env->CallStaticObjectMethod(bitmap_class, create_bitmap, width,
                                           height, bitmap_config);
    }
    if (bitmap != nullptr && !env->ExceptionCheck()) {
      canvas_constructor = env->GetMethodID(
          canvas_class, "<init>", "(Landroid/graphics/Bitmap;)V");
    }
    if (canvas_constructor != nullptr && !env->ExceptionCheck()) {
      canvas = env->NewObject(canvas_class, canvas_constructor, bitmap);
    }
    env->DeleteLocalRef(bitmap_config);
  }
  const bool real_target_missing = use_real_graphics && bitmap == nullptr;
  if (canvas == nullptr || real_target_missing || env->ExceptionCheck()) {
    std::cerr << "ART Android view: "
              << (use_real_graphics
                      ? "Bitmap/Canvas(Bitmap) setup failed\n"
                      : "ProbeCanvas setup failed\n");
    art::Thread* self = art::Thread::Current();
    if (self != nullptr && self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    release_render_target();
    return JNI_FALSE;
  }
  view_class = env->FindClass("android/view/View");
  auto get_view_method = [&](const char* name,
                             const char* signature) -> jmethodID {
    return view_class == nullptr || env->ExceptionCheck()
               ? nullptr
               : env->GetMethodID(view_class, name, signature);
  };
  auto get_view_field = [&](const char* name) -> jfieldID {
    return view_class == nullptr || env->ExceptionCheck()
               ? nullptr
               : env->GetFieldID(view_class, name, "I");
  };
  jmethodID layout =
      get_view_method("layout", "(IIII)V");
  jmethodID measure =
      get_view_method("measure", "(II)V");
  jmethodID draw =
      get_view_method("draw", "(Landroid/graphics/Canvas;)V");
  jfieldID view_left = get_view_field("mLeft");
  jfieldID view_top = get_view_field("mTop");
  jfieldID view_right = get_view_field("mRight");
  jfieldID view_bottom = get_view_field("mBottom");
  if (canvas == nullptr || measure == nullptr || layout == nullptr ||
      draw == nullptr || view_left == nullptr || view_top == nullptr ||
      view_right == nullptr || view_bottom == nullptr ||
      env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }

  constexpr jint kExactly = 0x40000000;
  env->CallVoidMethod(view, measure, kExactly | width, kExactly | height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  // ViewRoot normally installs the surface bounds before the first traversal.
  // The Darwin window policy owns that root, so seed the same bounds before
  // layout. This prevents a detached View from trying to notify Android's
  // accessibility/window services merely because its initial frame changed.
  env->SetIntField(view, view_left, 0);
  env->SetIntField(view, view_top, 0);
  env->SetIntField(view, view_right, width);
  env->SetIntField(view, view_bottom, height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  env->CallVoidMethod(view, layout, 0, 0, width, height);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  env->CallVoidMethod(view, draw, canvas);
  if (env->ExceptionCheck()) {
    release_render_target();
    return JNI_FALSE;
  }
  if (!use_real_graphics) {
    pixels = static_cast<jintArray>(env->CallObjectMethod(canvas, snapshot));
  } else {
    const jsize pixel_count = static_cast<jsize>(width * height);
    pixels = env->NewIntArray(pixel_count);
    jmethodID get_pixels =
        bitmap_class == nullptr || env->ExceptionCheck()
            ? nullptr
            : env->GetMethodID(bitmap_class, "getPixels", "([IIIIIII)V");
    if (pixels != nullptr && get_pixels != nullptr && !env->ExceptionCheck()) {
      env->CallVoidMethod(bitmap, get_pixels, pixels, 0, width, 0, 0, width,
                          height);
    }
  }
  if (env->ExceptionCheck() || pixels == nullptr) {
    release_render_target();
    return JNI_FALSE;
  }
  const jboolean presented = PresentFrame(env, nullptr, width, height, pixels);
  release_render_target();
  return presented;
}

static jobject FindClickableViewAt(JNIEnv* env, jobject view, jfloat x,
                                   jfloat y) {
  if (view == nullptr || x < 0.0f || y < 0.0f || env->ExceptionCheck()) {
    return nullptr;
  }
  jclass view_class = env->FindClass("android/view/View");
  jclass group_class = env->FindClass("android/view/ViewGroup");
  if (view_class == nullptr || group_class == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }
  jmethodID get_width = env->GetMethodID(view_class, "getWidth", "()I");
  jmethodID get_height = env->GetMethodID(view_class, "getHeight", "()I");
  jmethodID get_visibility =
      env->GetMethodID(view_class, "getVisibility", "()I");
  jmethodID is_enabled = env->GetMethodID(view_class, "isEnabled", "()Z");
  jmethodID is_clickable = env->GetMethodID(view_class, "isClickable", "()Z");
  if (get_width == nullptr || get_height == nullptr ||
      get_visibility == nullptr || is_enabled == nullptr ||
      is_clickable == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }
  const jint width = env->CallIntMethod(view, get_width);
  const jint height = env->CallIntMethod(view, get_height);
  const jint visibility = env->CallIntMethod(view, get_visibility);
  const jboolean enabled = env->CallBooleanMethod(view, is_enabled);
  if (env->ExceptionCheck() || visibility != 0 || enabled != JNI_TRUE ||
      x >= static_cast<jfloat>(width) || y >= static_cast<jfloat>(height)) {
    env->DeleteLocalRef(group_class);
    env->DeleteLocalRef(view_class);
    return nullptr;
  }

  if (env->IsInstanceOf(view, group_class)) {
    jmethodID get_child_count =
        env->GetMethodID(group_class, "getChildCount", "()I");
    jmethodID get_child_at = env->GetMethodID(
        group_class, "getChildAt", "(I)Landroid/view/View;");
    jmethodID get_left = env->GetMethodID(view_class, "getLeft", "()I");
    jmethodID get_top = env->GetMethodID(view_class, "getTop", "()I");
    if (get_child_count != nullptr && get_child_at != nullptr &&
        get_left != nullptr && get_top != nullptr && !env->ExceptionCheck()) {
      const jint child_count = env->CallIntMethod(view, get_child_count);
      for (jint index = child_count - 1; index >= 0 && !env->ExceptionCheck();
           --index) {
        jobject child = env->CallObjectMethod(view, get_child_at, index);
        if (child == nullptr || env->ExceptionCheck()) {
          env->DeleteLocalRef(child);
          continue;
        }
        const jint left = env->CallIntMethod(child, get_left);
        const jint top = env->CallIntMethod(child, get_top);
        jobject result =
            FindClickableViewAt(env, child, x - left, y - top);
        env->DeleteLocalRef(child);
        if (result != nullptr) {
          env->DeleteLocalRef(group_class);
          env->DeleteLocalRef(view_class);
          return result;
        }
      }
    }
  }
  jobject result =
      env->CallBooleanMethod(view, is_clickable) == JNI_TRUE &&
              !env->ExceptionCheck()
          ? env->NewLocalRef(view)
          : nullptr;
  env->DeleteLocalRef(group_class);
  env->DeleteLocalRef(view_class);
  return result;
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_dispatch_pointer(
    uint32_t action, float x, float y) {
  if (action > 2u || !std::isfinite(x) || !std::isfinite(y)) {
    return 71;
  }
  art::Thread* art_thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    if (g_process_state.phase != ProcessPhase::kAwaitingShutdown ||
        !g_process_state.owner_thread_valid ||
        pthread_equal(g_process_state.owner_thread, pthread_self()) == 0 ||
        g_process_state.art_thread == nullptr || g_interactive_root == nullptr) {
      return 72;
    }
    art_thread = g_process_state.art_thread;
  }
  if (art::Thread::Current() != art_thread ||
      art_thread->GetState() != art::ThreadState::kNative) {
    return 73;
  }

  art::ScopedObjectAccess soa(art_thread);
  JNIEnv* env = art_thread->GetJniEnv();
  jobject hit = FindClickableViewAt(env, g_interactive_root, x, y);
  jclass view_class = env->FindClass("android/view/View");
  jmethodID set_pressed =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "setPressed", "(Z)V");
  jmethodID perform_click =
      view_class == nullptr
          ? nullptr
          : env->GetMethodID(view_class, "performClick", "()Z");
  if (view_class == nullptr || set_pressed == nullptr ||
      perform_click == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(hit);
    env->DeleteLocalRef(view_class);
    return 74;
  }

  if (action == 0u) {
    if (g_pressed_view != nullptr) {
      env->CallVoidMethod(g_pressed_view, set_pressed, JNI_FALSE);
      env->DeleteGlobalRef(g_pressed_view);
      g_pressed_view = nullptr;
    }
    if (hit != nullptr && !env->ExceptionCheck()) {
      env->CallVoidMethod(hit, set_pressed, JNI_TRUE);
      g_pressed_view = env->NewGlobalRef(hit);
    }
  } else if (action == 1u) {
    if (g_pressed_view != nullptr) {
      env->CallVoidMethod(g_pressed_view, set_pressed, JNI_FALSE);
      if (hit != nullptr && env->IsSameObject(hit, g_pressed_view) &&
          !env->ExceptionCheck()) {
        env->CallBooleanMethod(g_pressed_view, perform_click);
      }
      env->DeleteGlobalRef(g_pressed_view);
      g_pressed_view = nullptr;
    }
  }
  env->DeleteLocalRef(hit);
  env->DeleteLocalRef(view_class);
  const bool rendered = !env->ExceptionCheck() &&
                        PresentContent(env, nullptr, g_interactive_root,
                                       g_interactive_width,
                                       g_interactive_height) == JNI_TRUE;
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android input: click dispatch threw\n"
              << art_thread->GetException()->Dump() << "\n";
    art_thread->ClearException();
  }
  return rendered ? 0 : 75;
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_run_process(
    const darwin_art_process_config_t* config,
    darwin_art_process_result_t* run_result) {
  if (config == nullptr || run_result == nullptr ||
      config->struct_size < sizeof(darwin_art_process_config_t) ||
      run_result->struct_size < sizeof(darwin_art_process_result_t) ||
      config->abi_version != DARWIN_ART_ABI_VERSION ||
      run_result->abi_version != DARWIN_ART_ABI_VERSION ||
      config->core_oj_jar == nullptr || config->core_libart_jar == nullptr ||
      config->framework_jar == nullptr || config->core_icu4j_jar == nullptr ||
      config->app_dex == nullptr) {
    std::cerr << "darwin_art_run_process: invalid ABI/configuration\n";
    return 64;
  }

  const uint64_t heap_initial = config->heap_initial_bytes == 0
                                    ? 64u * 1024u * 1024u
                                    : config->heap_initial_bytes;
  const uint64_t heap_maximum = config->heap_maximum_bytes == 0
                                    ? 64u * 1024u * 1024u
                                    : config->heap_maximum_bytes;
  if (heap_initial > heap_maximum || heap_maximum > 256u * 1024u * 1024u) {
    std::cerr << "darwin_art_run_process: invalid heap bounds\n";
    return 65;
  }

  if (!BeginProcessRun()) {
    std::cerr << "darwin_art_run_process: process already started\n";
    return DARWIN_ART_STATUS_PROCESS_ALREADY_STARTED;
  }
  ScopedProcessRunBoundary process_boundary;
  const char* elf_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_ELF_JNI_FIXTURE");
  const bool run_elf_jni_fixture =
      elf_fixture_path != nullptr && elf_fixture_path[0] != '\0';
  const char* generic_elf_path =
      std::getenv("DARWIN_ART_ANDROID_ELF_GENERIC_FIXTURE");
  const bool run_generic_elf =
      generic_elf_path != nullptr && generic_elf_path[0] != '\0';
  const char* apk_elf_path =
      std::getenv("DARWIN_ART_ANDROID_APK_ELF_FIXTURE");
  const char* apk_sha256 = std::getenv("DARWIN_ART_ANDROID_APK_SHA256");
  const char* apk_root_sha256 =
      std::getenv("DARWIN_ART_ANDROID_APK_ROOT_SHA256");
  const bool run_apk_elf =
      apk_elf_path != nullptr && apk_elf_path[0] != '\0' &&
      IsSha256(apk_sha256) && IsSha256(apk_root_sha256) &&
      IsPrivateExtractedRoot(apk_elf_path) &&
      run_generic_elf && std::strcmp(apk_elf_path, generic_elf_path) == 0;
  const char* direct_apk_path =
      std::getenv("DARWIN_ART_DIRECT_APK_FIXTURE");
  const char* direct_apk_root = std::getenv("DARWIN_ART_DIRECT_APK_ROOT");
  const bool run_direct_apk =
      direct_apk_path != nullptr && direct_apk_path[0] != '\0' &&
      direct_apk_root != nullptr && direct_apk_root[0] != '\0';
  const char* libcxx_collections_path =
      std::getenv("DARWIN_ART_ANDROID_LIBCXX_COLLECTIONS_FIXTURE");
  const char* libcxx_exception_path =
      std::getenv("DARWIN_ART_ANDROID_LIBCXX_EXCEPTION_FIXTURE");
  const bool run_libcxx_acceptance =
      libcxx_collections_path != nullptr && libcxx_collections_path[0] != '\0' &&
      libcxx_exception_path != nullptr && libcxx_exception_path[0] != '\0';
  const char* tls_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_TLS_FIXTURE");
  const bool run_tls_acceptance =
      tls_fixture_path != nullptr && tls_fixture_path[0] != '\0';
  const char* network_fixture_path =
      std::getenv("DARWIN_ART_ANDROID_NETWORK_FIXTURE");
  const bool run_network_acceptance =
      network_fixture_path != nullptr && network_fixture_path[0] != '\0';
  const char* apk_app_package = std::getenv("DARWIN_ART_APK_APP_PACKAGE");
  const char* apk_app_activity = std::getenv("DARWIN_ART_APK_APP_ACTIVITY");
  const char* apk_app_descriptor =
      std::getenv("DARWIN_ART_APK_APP_DESCRIPTOR");
  const char* apk_app_support_dex =
      std::getenv("DARWIN_ART_APK_APP_SUPPORT_DEX");
  const char* framework_res_apk =
      std::getenv("DARWIN_ART_FRAMEWORK_RES_APK");
  const char* window_scale_value =
      std::getenv("DARWIN_ART_WINDOW_SCALE");
  const bool has_apk_app_environment =
      apk_app_package != nullptr || apk_app_activity != nullptr ||
      apk_app_descriptor != nullptr || apk_app_support_dex != nullptr ||
      framework_res_apk != nullptr;
  const bool run_apk_app =
      apk_app_package != nullptr && apk_app_package[0] != '\0' &&
      apk_app_activity != nullptr && apk_app_activity[0] != '\0' &&
      apk_app_descriptor != nullptr && apk_app_descriptor[0] == 'L' &&
      apk_app_support_dex != nullptr && apk_app_support_dex[0] != '\0' &&
      framework_res_apk != nullptr && framework_res_apk[0] == '/' &&
      std::strlen(apk_app_descriptor) >= 3u &&
      std::strlen(apk_app_descriptor) <= 513u &&
      apk_app_descriptor[std::strlen(apk_app_descriptor) - 1u] == ';';
  const bool valid_window_scale =
      window_scale_value == nullptr ||
      std::strcmp(window_scale_value, "1") == 0 ||
      std::strcmp(window_scale_value, "2") == 0;
  const jint window_scale =
      run_apk_app && window_scale_value != nullptr &&
              std::strcmp(window_scale_value, "2") == 0
          ? 2
          : 1;
  constexpr jint kApkFrameWidth = 360;
  constexpr jint kApkFrameHeight = 640;
  const bool expect_apk_widgets =
      run_apk_app &&
      std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS") != nullptr &&
      std::strcmp(std::getenv("DARWIN_ART_APK_APP_EXPECT_WIDGETS"), "1") == 0;
  if ((has_apk_app_environment && !run_apk_app) || !valid_window_scale) {
    std::cerr << "ART Android APK app environment is incomplete or invalid\n";
    return 48;
  }
  if (run_network_acceptance &&
      (run_elf_jni_fixture || run_generic_elf || run_apk_elf ||
       run_libcxx_acceptance || run_tls_acceptance || run_direct_apk)) {
    std::cerr << "ART Android network fixture requires an isolated process\n";
    return 47;
  }

  // Darwin's malloc zones can claim the fixed compressed-reference window
  // while RuntimeArgumentMap is being assembled. Reserve ART's bounded arena
  // after the one-shot process gate, but before the launcher performs its first
  // heap allocation. MemMap::Init itself is process-global and not safe for
  // concurrent callers.
  art::MemMap::Init();

  g_host_context = config->host_context;
  g_frame_callback = config->frame_callback;
  g_frame_width = 0;
  g_frame_height = 0;

  if (!darwin_art::InitializeFrameworkGraphicsRuntime()) {
    std::cerr << "ART Darwin graphics: runtime initialization failed\n";
    return 36;
  }

  std::string boot_class_path =
      std::string(config->core_oj_jar) + ":" + config->core_libart_jar + ":" +
      config->framework_jar + ":" + config->core_icu4j_jar;
  std::cerr << "Mach-O slide: 0x" << std::hex << _dyld_get_image_vmaddr_slide(0)
            << std::dec << "\n";
  art::Locks::Init();
  art::RuntimeArgumentMap options;
  options.Set(art::RuntimeArgumentMap::BootClassPath,
              art::ParseStringList<':'>::Split(boot_class_path));
  options.Set(art::RuntimeArgumentMap::BootClassPathLocations,
              art::ParseStringList<':'>::Split(boot_class_path));
  options.Set(art::RuntimeArgumentMap::Interpret, true);
  options.Set(art::RuntimeArgumentMap::UseJitCompilation, false);
  // Android's normal launcher always supplies a concrete growth limit. A
  // directly constructed RuntimeArgumentMap leaves this key at zero, which
  // MallocSpace interprets as zero capacity rather than "unlimited".
  options.Set(art::RuntimeArgumentMap::HeapGrowthLimit,
              art::MemoryKiB(heap_initial));
  options.Set(art::RuntimeArgumentMap::MemoryMaximumSize,
              art::MemoryKiB(heap_maximum));
  art::LogVerbosity verbosity{};
  verbosity.heap = true;
  options.Set(art::RuntimeArgumentMap::Verbose, verbosity);

  if (!art::Runtime::Create(std::move(options))) {
    return 1;
  }

  art::Thread* self = art::Thread::Current();
  RecordCreatedRuntime(self);
  if (self == nullptr) {
    std::cerr << "ART Darwin DEX: no current thread\n";
    return 2;
  }

  process_boundary.SetArtThread(self);
  return [&]() -> int32_t {

  art::interpreter::UnstartedRuntime::Initialize();
  art::ScopedObjectAccess soa(self);
  ScopedJniLocalFrame local_frame(self->GetJniEnv());
  if (!local_frame.IsValid()) {
    std::cerr << "ART Darwin JNI: local frame allocation failed\n";
    return 34;
  }
  art::WellKnownClasses::Init(self->GetJniEnv());

  std::vector<std::unique_ptr<const art::DexFile>>& app_dex_files =
      g_process_state.app_dex_files;
  CHECK(app_dex_files.empty());
  std::string dex_error;
  if (run_apk_app) {
    art::ArtDexFileLoader support_loader(apk_app_support_dex);
    if (!support_loader.Open(/* verify= */ true,
                             /* verify_checksum= */ true, &dex_error,
                             &app_dex_files)) {
      std::cerr << "ART Darwin support DEX: open failed: " << dex_error << "\n";
      return 3;
    }
  }
  art::ArtDexFileLoader dex_loader(config->app_dex);
  if (!dex_loader.Open(/* verify= */ true,
                       /* verify_checksum= */ true, &dex_error,
                       &app_dex_files)) {
    std::cerr << "ART Darwin DEX: open failed: " << dex_error << "\n";
    return 3;
  }
  std::vector<const art::DexFile*> app_dex_file_ptrs;
  app_dex_file_ptrs.reserve(app_dex_files.size());
  for (const auto& dex_file : app_dex_files) {
    app_dex_file_ptrs.push_back(dex_file.get());
  }

  art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();
  jobject loader_ref =
      class_linker->CreatePathClassLoader(self, app_dex_file_ptrs);
  art::StackHandleScope<13> hs(self);
  art::Handle<art::mirror::ClassLoader> app_loader =
      hs.NewHandle(soa.Decode<art::mirror::ClassLoader>(loader_ref));
  jobject app_loader_ref = soa.AddLocalReference<jobject>(app_loader.Get());
  for (const auto& dex_file : app_dex_files) {
    if (class_linker->RegisterDexFile(*dex_file, app_loader.Get()) == nullptr) {
      std::cerr << "ART Darwin DEX: registration failed\n";
      return 4;
    }
  }

  art::Handle<art::mirror::Class> hello = hs.NewHandle(class_linker->FindClass(
      self, "Ldev/darwinart/probe/Hello;",
      sizeof("Ldev/darwinart/probe/Hello;") - 1u, app_loader));
  if (hello == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: Hello class lookup failed\n";
    return 5;
  }

  art::Handle<art::mirror::Class> framework_activity = hs.NewHandle(
      class_linker->FindSystemClass(self, "Landroid/app/Activity;"));
  if (framework_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity class lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  const char* activity_descriptor =
      run_apk_app ? apk_app_descriptor : "Ldev/darwinart/probe/ProbeActivity;";
  art::Handle<art::mirror::Class> probe_activity = hs.NewHandle(
      class_linker->FindClass(self, activity_descriptor,
                              std::strlen(activity_descriptor), app_loader));
  if (probe_activity == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Activity subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_context_handle =
      hs.NewHandle(class_linker->FindClass(
          self, "Ldev/darwinart/probe/ProbeContext;",
          sizeof("Ldev/darwinart/probe/ProbeContext;") - 1u, app_loader));
  if (probe_context_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Context subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_resources_handle =
      hs.NewHandle(class_linker->FindClass(
          self, "Ldev/darwinart/probe/ProbeResources;",
          sizeof("Ldev/darwinart/probe/ProbeResources;") - 1u, app_loader));
  if (probe_resources_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android framework: Resources subclass lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> probe_view_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/ProbeView;",
                              sizeof("Ldev/darwinart/probe/ProbeView;") - 1u,
                              app_loader));
  if (probe_view_handle == nullptr || self->IsExceptionPending()) {
    std::cerr << "ART Android view: ProbeView lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }
  art::Handle<art::mirror::Class> content_root_handle = hs.NewHandle(
      class_linker->FindClass(self, "Ldev/darwinart/probe/ProbeContentRoot;",
                              sizeof("Ldev/darwinart/probe/ProbeContentRoot;") - 1u,
                              app_loader));
  art::Handle<art::mirror::Class> package_manager_handle = hs.NewHandle(
      class_linker->FindClass(self,
                              "Ldev/darwinart/probe/ProbePackageManager;",
                              sizeof("Ldev/darwinart/probe/ProbePackageManager;") - 1u,
                              app_loader));
  art::MutableHandle<art::mirror::Class> native_fixture_handle(
      hs.NewHandle<art::mirror::Class>(nullptr));
  art::MutableHandle<art::mirror::Class> network_fixture_handle(
      hs.NewHandle<art::mirror::Class>(nullptr));
  if (run_direct_apk) {
    if (run_elf_jni_fixture) {
      std::cerr << "ART Android direct APK must run in its isolated host flavor\n";
      return 46;
    }
    std::string direct_error;
    bool direct_loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      direct_loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          self->GetJniEnv(), direct_apk_path, app_loader_ref, nullptr,
          &direct_error);
    }
    if (!direct_loaded || !direct_error.empty() ||
        self->GetJniEnv()->ExceptionCheck()) {
      std::cerr << "ART Android direct APK JavaVMExt load failed, load_error="
                << direct_error << "\n";
      return 46;
    }
    g_direct_apk_loaded = true;
  }

  if (run_elf_jni_fixture) {
    native_fixture_handle.Assign(class_linker->FindClass(
        self, "Ldarwin/art/nativefixture/NativeFixture;",
        sizeof("Ldarwin/art/nativefixture/NativeFixture;") - 1u, app_loader));
  }
  if (run_network_acceptance) {
    network_fixture_handle.Assign(class_linker->FindClass(
        self, "Ldev/darwinart/probe/NetworkRuntimeFixture;",
        sizeof("Ldev/darwinart/probe/NetworkRuntimeFixture;") - 1u,
        app_loader));
  }
  if (content_root_handle == nullptr || package_manager_handle == nullptr ||
      (run_elf_jni_fixture && native_fixture_handle == nullptr) ||
      (run_network_acceptance && network_fixture_handle == nullptr) ||
      self->IsExceptionPending()) {
    std::cerr << "ART Android window: Darwin Canvas/Window lookup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 21;
  }

  jclass hello_class = soa.AddLocalReference<jclass>(hello.Get());
  jclass probe_activity_class =
      soa.AddLocalReference<jclass>(probe_activity.Get());
  jclass probe_context_class =
      soa.AddLocalReference<jclass>(probe_context_handle.Get());
  jclass probe_resources_class =
      soa.AddLocalReference<jclass>(probe_resources_handle.Get());
  jclass probe_view_class =
      soa.AddLocalReference<jclass>(probe_view_handle.Get());
  jclass probe_canvas_class = nullptr;
  if (darwin_art::GetFrameworkGraphicsBackend() ==
      darwin_art::FrameworkGraphicsBackend::kProbeCanvas) {
    art::ObjPtr<art::mirror::Class> probe_canvas = class_linker->FindClass(
        self, "Ldev/darwinart/probe/ProbeCanvas;",
        sizeof("Ldev/darwinart/probe/ProbeCanvas;") - 1u, app_loader);
    if (probe_canvas == nullptr || self->IsExceptionPending()) {
      std::cerr << "ART Android window: ProbeCanvas lookup failed\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 21;
    }
    probe_canvas_class = soa.AddLocalReference<jclass>(probe_canvas);
  }
  jclass content_root_class =
      soa.AddLocalReference<jclass>(content_root_handle.Get());
  jclass package_manager_class =
      soa.AddLocalReference<jclass>(package_manager_handle.Get());
  jclass native_fixture_class =
      run_elf_jni_fixture
          ? soa.AddLocalReference<jclass>(native_fixture_handle.Get())
          : nullptr;
  jclass network_fixture_class =
      run_network_acceptance
          ? soa.AddLocalReference<jclass>(network_fixture_handle.Get())
          : nullptr;
  art::Runtime::Current()->StartMinimalForDarwinProbe(self->GetJniEnv());
  if (!InstallProbeAndroidSystemRoot()) {
    std::cerr << "ART Android filesystem: test system root install failed\n";
    return 40;
  }
  if (!darwin_art::RegisterLibcoreNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin libcore: native registration failed\n";
    return 17;
  }
  register_java_lang_Math(self->GetJniEnv());
  if (self->GetJniEnv()->ExceptionCheck()) {
    std::cerr << "ART Darwin OpenJDK: Math native registration failed\n";
    return 37;
  }
  if (!darwin_art::RegisterIcuCharsetNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin ICU: charset native registration failed\n";
    return 20;
  }
  if (!darwin_art::RegisterFrameworkNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin framework: native registration failed\n";
    return 26;
  }
  art::Runtime::Current()->FinishMinimalForDarwinProbe();
  if (!darwin_art::InstallFrameworkResourceRuntime(self->GetJniEnv())) {
    std::cerr << "ART Darwin resources: AndroidRuntime ownership install failed\n";
    return 38;
  }
  RecordResourceRuntimeInstalled();
  if (!darwin_art::RegisterFrameworkResourceNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin resources: native registration failed\n";
    return 39;
  }
  if (!darwin_art::RegisterFrameworkGraphicsNatives(self->GetJniEnv())) {
    std::cerr << "ART Darwin graphics: native registration failed\n";
    return 35;
  }

  JNIEnv* env = self->GetJniEnv();
  if (darwin_art::GetFrameworkGraphicsBackend() ==
      darwin_art::FrameworkGraphicsBackend::kProbeCanvas) {
    g_probe_canvas_class =
        static_cast<jclass>(env->NewGlobalRef(probe_canvas_class));
    if (g_probe_canvas_class == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android window: ProbeCanvas global root failed\n";
      return 32;
    }
  }
  jclass looper_class = env->FindClass("android/os/Looper");
  jmethodID prepare_main_looper =
      looper_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(looper_class, "prepareMainLooper", "()V");
  if (prepare_main_looper != nullptr) {
    env->CallStaticVoidMethod(looper_class, prepare_main_looper);
  }
  env->DeleteLocalRef(looper_class);
  if (prepare_main_looper == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android framework: Looper.prepareMainLooper() failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 25;
  }

  if (!class_linker->EnsureInitialized(self, probe_activity, true, true)) {
    std::cerr << "ART Android framework: launcher Activity initialization failed\n"
              << self->GetException()->Dump() << "\n";
    return 22;
  }
  jmethodID activity_constructor =
      env->GetMethodID(probe_activity_class, "<init>", "()V");
  jobject activity_instance =
      activity_constructor == nullptr
          ? nullptr
          : env->NewObject(probe_activity_class, activity_constructor);
  jmethodID probe_value =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_activity_class, "probeValue", "()I");
  jint activity_result =
      run_apk_app
          ? (activity_instance == nullptr ? -1 : 42)
          : (activity_instance == nullptr || probe_value == nullptr
                 ? -1
                 : env->CallIntMethod(activity_instance, probe_value));
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android framework: ProbeActivity constructor threw\n"
              << self->GetException()->Dump() << "\n";
    return 23;
  }
  if (activity_result != 42) {
    std::cerr << "ART Android framework: expected 42, got " << activity_result
              << "\n";
    return 24;
  }

  jclass activity_class = env->GetSuperclass(probe_activity_class);
  jclass activity_info_class =
      env->FindClass("android/content/pm/ActivityInfo");
  jclass application_class = env->FindClass("android/app/Application");
  jclass intent_class = env->FindClass("android/content/Intent");
  jclass component_name_class =
      env->FindClass("android/content/ComponentName");
  jclass configuration_class =
      env->FindClass("android/content/res/Configuration");
  jmethodID configuration_constructor =
      configuration_class == nullptr
          ? nullptr
          : env->GetMethodID(configuration_class, "<init>", "()V");
  jclass asset_manager_class =
      env->FindClass("android/content/res/AssetManager");
  jmethodID activity_info_constructor =
      activity_info_class == nullptr
          ? nullptr
          : env->GetMethodID(activity_info_class, "<init>", "()V");
  jmethodID application_constructor =
      application_class == nullptr
          ? nullptr
          : env->GetMethodID(application_class, "<init>", "()V");
  jobject activity_info =
      activity_info_constructor == nullptr
          ? nullptr
          : env->NewObject(activity_info_class, activity_info_constructor);
  jobject application =
      application_constructor == nullptr
          ? nullptr
          : env->NewObject(application_class, application_constructor);
  jmethodID asset_manager_constructor =
      asset_manager_class == nullptr
          ? nullptr
          : env->GetMethodID(asset_manager_class, "<init>", "(Z)V");
  jobject asset_manager =
      asset_manager_constructor == nullptr
          ? nullptr
          : env->NewObject(asset_manager_class, asset_manager_constructor,
                           JNI_TRUE);
  jclass apk_assets_class = env->FindClass("android/content/res/ApkAssets");
  jobject framework_apk_assets = nullptr;
  jstring framework_res_path = nullptr;
  jobjectArray configured_apk_assets = nullptr;
  if (run_apk_app && apk_assets_class != nullptr && asset_manager != nullptr) {
    jmethodID load_from_path = env->GetStaticMethodID(
        apk_assets_class, "loadFromPath",
        "(Ljava/lang/String;)Landroid/content/res/ApkAssets;");
    framework_res_path = env->NewStringUTF(framework_res_apk);
    framework_apk_assets =
        load_from_path == nullptr || framework_res_path == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(apk_assets_class, load_from_path,
                                          framework_res_path);
    configured_apk_assets =
        framework_apk_assets == nullptr
            ? nullptr
            : env->NewObjectArray(1, apk_assets_class, framework_apk_assets);
  } else if (apk_assets_class != nullptr) {
    configured_apk_assets = env->NewObjectArray(0, apk_assets_class, nullptr);
  }
  jfieldID apk_assets_field =
      asset_manager_class == nullptr
          ? nullptr
          : env->GetFieldID(asset_manager_class, "mApkAssets",
                            "[Landroid/content/res/ApkAssets;");
  if (!run_apk_app && asset_manager != nullptr && apk_assets_field != nullptr &&
      configured_apk_assets != nullptr) {
    env->SetObjectField(asset_manager, apk_assets_field,
                        configured_apk_assets);
  } else if (run_apk_app && asset_manager != nullptr &&
             apk_assets_field != nullptr && configured_apk_assets != nullptr) {
    jfieldID asset_manager_object =
        env->GetFieldID(asset_manager_class, "mObject", "J");
    jmethodID native_set_apk_assets = env->GetStaticMethodID(
        asset_manager_class, "nativeSetApkAssets",
        "(J[Landroid/content/res/ApkAssets;ZZ)V");
    if (asset_manager_object != nullptr && native_set_apk_assets != nullptr) {
      const jlong native_asset_manager =
          env->GetLongField(asset_manager, asset_manager_object);
      env->CallStaticVoidMethod(asset_manager_class, native_set_apk_assets,
                                native_asset_manager, configured_apk_assets,
                                JNI_FALSE, JNI_FALSE);
      if (!env->ExceptionCheck()) {
        env->SetObjectField(asset_manager, apk_assets_field,
                            configured_apk_assets);
      }
    }
  }
  jmethodID probe_resources_constructor =
      probe_resources_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_resources_class, "<init>",
                             "(Landroid/content/res/AssetManager;Z)V");
  jmethodID configure_display_scale =
      probe_resources_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(probe_resources_class,
                                   "configureDisplayScale", "(I)V");
  if (configure_display_scale != nullptr) {
    env->CallStaticVoidMethod(probe_resources_class, configure_display_scale,
                              window_scale);
  }
  jobject probe_resources =
      probe_resources_constructor == nullptr || asset_manager == nullptr ||
              env->ExceptionCheck()
          ? nullptr
          : env->NewObject(probe_resources_class, probe_resources_constructor,
                           asset_manager,
                           run_apk_app ? JNI_TRUE : JNI_FALSE);
  if (activity_info == nullptr || application == nullptr ||
      asset_manager == nullptr || configured_apk_assets == nullptr ||
      apk_assets_field == nullptr || configure_display_scale == nullptr ||
      probe_resources == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android resources: bootstrap construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jobject package_manager =
      package_manager_class == nullptr
          ? nullptr
          : soa.AddLocalReference<jobject>(
                package_manager_handle->AllocObject(self));
  if (package_manager == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: package feature stub failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jmethodID probe_context_constructor =
      probe_context_class == nullptr
          ? nullptr
          : env->GetMethodID(probe_context_class, "<init>",
                             "(Landroid/content/res/Resources;"
                             "Landroid/content/pm/PackageManager;)V");
  jobject probe_context =
      probe_context_constructor == nullptr || probe_resources == nullptr ||
              package_manager == nullptr
          ? nullptr
          : env->NewObject(probe_context_class, probe_context_constructor,
                           probe_resources, package_manager);
  if (probe_context == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: ProbeContext construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jmethodID intent_constructor = intent_class == nullptr
                                    ? nullptr
                                    : env->GetMethodID(intent_class, "<init>", "()V");
  jmethodID component_name_constructor =
      component_name_class == nullptr
          ? nullptr
          : env->GetMethodID(component_name_class, "<init>",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID set_component =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setComponent",
                             "(Landroid/content/ComponentName;)Landroid/content/Intent;");
  jstring package_name = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  jstring class_name = env->NewStringUTF(
      run_apk_app ? apk_app_activity : "dev.darwinart.probe.ProbeActivity");
  jstring title =
      env->NewStringUTF(run_apk_app ? "Darwin ART APK" : "Darwin ART Probe");
  jobject component_name =
      component_name_constructor == nullptr
          ? nullptr
          : env->NewObject(component_name_class, component_name_constructor,
                           package_name, class_name);
  jobject intent = intent_constructor == nullptr
                       ? nullptr
                       : env->NewObject(intent_class, intent_constructor);
  jobject configuration =
      configuration_constructor == nullptr
          ? nullptr
          : env->NewObject(configuration_class, configuration_constructor);
  if (intent != nullptr && set_component != nullptr && component_name != nullptr) {
    jobject configured_intent =
        env->CallObjectMethod(intent, set_component, component_name);
    env->DeleteLocalRef(configured_intent);
  }
  static constexpr const char* kActivityAttachSignature =
      "(Landroid/content/Context;Landroid/app/ActivityThread;"
      "Landroid/app/Instrumentation;Landroid/os/IBinder;I"
      "Landroid/app/Application;Landroid/content/Intent;"
      "Landroid/content/pm/ActivityInfo;Ljava/lang/CharSequence;"
      "Landroid/app/Activity;Ljava/lang/String;"
      "Landroid/app/Activity$NonConfigurationInstances;"
      "Landroid/content/res/Configuration;Ljava/lang/String;"
      "Lcom/android/internal/app/IVoiceInteractor;Landroid/view/Window;"
      "Landroid/view/ViewRootImpl$ActivityConfigCallback;"
      "Landroid/os/IBinder;Landroid/os/IBinder;)V";
  jmethodID attach_activity =
      activity_class == nullptr
          ? nullptr
          : env->GetMethodID(activity_class, "attach", kActivityAttachSignature);
  if (activity_info == nullptr || application == nullptr ||
      probe_context == nullptr || intent == nullptr || configuration == nullptr ||
      attach_activity == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jclass context_theme_wrapper_class =
      env->FindClass("android/view/ContextThemeWrapper");
  jmethodID attach_base_context =
      context_theme_wrapper_class == nullptr
          ? nullptr
          : env->GetMethodID(context_theme_wrapper_class, "attachBaseContext",
                             "(Landroid/content/Context;)V");
  if (!run_apk_app && attach_base_context != nullptr) {
    env->CallNonvirtualVoidMethod(activity_instance,
                                  context_theme_wrapper_class,
                                  attach_base_context,
                                  probe_context);
  }
  if ((!run_apk_app && attach_base_context == nullptr) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: base Context preparation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 30;
  }
  env->CallNonvirtualVoidMethod(
      activity_instance,
      activity_class,
      attach_activity,
      probe_context,
      nullptr,
      nullptr,
      nullptr,
      static_cast<jint>(1),
      application,
      intent,
      activity_info,
      title,
      nullptr,
      nullptr,
      nullptr,
      configuration,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() threw\n"
              << self->GetException()->Dump() << "\n";
    return 30;
  }
  jmethodID get_window =
      env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject window = get_window == nullptr
                       ? nullptr
                       : env->CallObjectMethod(activity_instance, get_window);
  jclass window_class = env->FindClass("android/view/Window");
  jclass phone_window_class =
      env->FindClass("com/android/internal/policy/PhoneWindow");
  if (window == nullptr || phone_window_class == nullptr ||
      !env->IsInstanceOf(window, phone_window_class) || env->ExceptionCheck()) {
    std::cerr << "ART Android window: PhoneWindow attachment failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }

  jmethodID get_probe_theme = env->GetMethodID(
      probe_context_class, "getTheme", "()Landroid/content/res/Resources$Theme;");
  jobject probe_theme =
      get_probe_theme == nullptr
          ? nullptr
          : env->CallObjectMethod(probe_context, get_probe_theme);
  if (run_apk_app && probe_theme != nullptr) {
    jclass theme_class = env->GetObjectClass(probe_theme);
    jclass framework_style_class = env->FindClass("android/R$style");
    jfieldID framework_light_no_action_bar =
        framework_style_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_style_class,
                                    "Theme_Material_Light_NoActionBar", "I");
    jmethodID apply_style =
        theme_class == nullptr
            ? nullptr
            : env->GetMethodID(theme_class, "applyStyle", "(IZ)V");
    if (framework_light_no_action_bar != nullptr && apply_style != nullptr) {
      const jint style = env->GetStaticIntField(framework_style_class,
                                                framework_light_no_action_bar);
      env->CallVoidMethod(probe_theme, apply_style, style, JNI_TRUE);
    }
    env->DeleteLocalRef(framework_style_class);
    env->DeleteLocalRef(theme_class);
  }
  jmethodID set_activity_theme = env->GetMethodID(
      context_theme_wrapper_class, "setTheme",
      "(Landroid/content/res/Resources$Theme;)V");
  if (probe_theme == nullptr || set_activity_theme == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity theme setup failed\n";
    return 31;
  }
  env->CallVoidMethod(activity_instance, set_activity_theme, probe_theme);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.setTheme() threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  // A detached hierarchy should observe accessibility as disabled until the
  // Darwin service bridge exists. Seed the framework singleton without
  // invoking its Binder-backed constructor.
  jclass accessibility_class =
      env->FindClass("android/view/accessibility/AccessibilityManager");
  jobject accessibility =
      accessibility_class == nullptr ? nullptr : env->AllocObject(accessibility_class);
  jclass object_class = env->FindClass("java/lang/Object");
  jmethodID object_constructor =
      object_class == nullptr
          ? nullptr
          : env->GetMethodID(object_class, "<init>", "()V");
  jobject accessibility_lock =
      object_constructor == nullptr
          ? nullptr
          : env->NewObject(object_class, object_constructor);
  jfieldID accessibility_lock_field =
      accessibility_class == nullptr
          ? nullptr
          : env->GetFieldID(accessibility_class, "mLock", "Ljava/lang/Object;");
  jfieldID accessibility_instance =
      accessibility_class == nullptr
          ? nullptr
          : env->GetStaticFieldID(
                accessibility_class, "sInstance",
                "Landroid/view/accessibility/AccessibilityManager;");
  if (accessibility == nullptr || accessibility_lock == nullptr ||
      accessibility_lock_field == nullptr || accessibility_instance == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: accessibility stub setup failed\n";
    return 31;
  }
  env->SetObjectField(accessibility, accessibility_lock_field,
                      accessibility_lock);
  env->SetStaticObjectField(accessibility_class, accessibility_instance,
                            accessibility);

  jmethodID get_window_attributes =
      window_class == nullptr
          ? nullptr
          : env->GetMethodID(window_class, "getAttributes",
                             "()Landroid/view/WindowManager$LayoutParams;");
  jobject window_attributes =
      get_window_attributes == nullptr
          ? nullptr
          : env->CallObjectMethod(window, get_window_attributes);
  jclass decor_view_class =
      env->FindClass("com/android/internal/policy/DecorView");
  jmethodID decor_view_constructor =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(
                decor_view_class, "<init>",
                "(Landroid/content/Context;I"
                "Lcom/android/internal/policy/PhoneWindow;"
                "Landroid/view/WindowManager$LayoutParams;)V");
  jobject decor_view =
      decor_view_constructor == nullptr || window_attributes == nullptr
          ? nullptr
          : env->NewObject(decor_view_class, decor_view_constructor,
                           activity_instance, static_cast<jint>(-1), window,
                           window_attributes);
  jmethodID content_root_constructor =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "<init>",
                             "(Landroid/content/Context;)V");
  jobject content_root =
      content_root_constructor == nullptr
          ? nullptr
          : env->NewObject(content_root_class, content_root_constructor,
                           activity_instance);
  jmethodID add_view =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(decor_view_class, "addView",
                             "(Landroid/view/View;)V");
  jfieldID phone_decor =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mDecor",
                            "Lcom/android/internal/policy/DecorView;");
  jfieldID phone_content_parent =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mContentParent",
                            "Landroid/view/ViewGroup;");
  if (decor_view == nullptr || content_root == nullptr || add_view == nullptr ||
      phone_decor == nullptr || phone_content_parent == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }
  env->CallVoidMethod(decor_view, add_view, content_root);
  env->SetObjectField(window, phone_decor, decor_view);
  env->SetObjectField(window, phone_content_parent, content_root);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView attachment threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  jmethodID probe_on_create =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "onCreate",
                             "(Landroid/os/Bundle;)V")
          : env->GetMethodID(probe_activity_class, "probeOnCreate", "()I");
  jint lifecycle_result = -1;
  if (probe_on_create != nullptr) {
    if (run_apk_app) {
      env->CallVoidMethod(activity_instance, probe_on_create, nullptr);
      lifecycle_result = env->ExceptionCheck() ? -1 : 43;
    } else {
      lifecycle_result =
          env->CallIntMethod(activity_instance, probe_on_create);
    }
  }
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android lifecycle: Activity.onCreate() threw\n"
              << self->GetException()->Dump() << "\n";
    return 28;
  }
  // We just installed this exact DecorView in PhoneWindow.mDecor above.  The
  // detached probe Window has no ViewRoot to lazily materialize a decor, so
  // relying on PhoneWindow.getDecorView() here can legitimately return null.
  // Keep the authoritative local object instead.
  jobject attached_decor = decor_view;
  jmethodID get_child_at =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "getChildAt",
                             "(I)Landroid/view/View;");
  jobject probe_view =
      get_child_at == nullptr
          ? nullptr
          : env->CallObjectMethod(content_root, get_child_at,
                                  static_cast<jint>(0));
  bool widgets_valid = true;
  if (expect_apk_widgets) {
    static constexpr const char* kWidgetTags[]{
        "title", "checkbox", "radio", "toggle", "seek", "progress", "button"};
    static constexpr const char* kWidgetClasses[]{
        "android/widget/TextView",      "android/widget/CheckBox",
        "android/widget/RadioButton",  "android/widget/ToggleButton",
        "android/widget/SeekBar",      "android/widget/ProgressBar",
        "android/widget/Button"};
    jmethodID find_view_with_tag =
        content_root_class == nullptr
            ? nullptr
            : env->GetMethodID(content_root_class, "findViewWithTag",
                               "(Ljava/lang/Object;)Landroid/view/View;");
    widgets_valid = find_view_with_tag != nullptr;
    for (std::size_t index = 0;
         widgets_valid && index < std::size(kWidgetTags); ++index) {
      jstring tag = env->NewStringUTF(kWidgetTags[index]);
      jobject widget =
          tag == nullptr
              ? nullptr
              : env->CallObjectMethod(content_root, find_view_with_tag, tag);
      jclass widget_class = env->FindClass(kWidgetClasses[index]);
      widgets_valid = tag != nullptr && widget != nullptr &&
                      widget_class != nullptr &&
                      env->IsInstanceOf(widget, widget_class) &&
                      !env->ExceptionCheck();
      env->DeleteLocalRef(widget_class);
      env->DeleteLocalRef(widget);
      env->DeleteLocalRef(tag);
    }
  }
  if (!widgets_valid) {
    std::cerr << "ART Android APK: framework widget set is incomplete\n";
    return 33;
  }
  const jboolean decor_presented =
      attached_decor == nullptr || env->ExceptionCheck()
          ? JNI_FALSE
          : PresentContent(env, nullptr, attached_decor,
                           kApkFrameWidth * window_scale,
                           kApkFrameHeight * window_scale);
  jmethodID was_presented =
      run_apk_app
          ? nullptr
          : env->GetMethodID(probe_view_class, "wasPresented", "()Z");
  const jboolean view_presented =
      run_apk_app
          ? (decor_presented == JNI_TRUE && probe_view != nullptr
                 ? JNI_TRUE
                 : JNI_FALSE)
          : (decor_presented != JNI_TRUE || probe_view == nullptr ||
                     !env->IsInstanceOf(probe_view, probe_view_class) ||
                     was_presented == nullptr || env->ExceptionCheck()
                 ? JNI_FALSE
                 : env->CallBooleanMethod(probe_view, was_presented));
  if (view_presented != JNI_TRUE ||
      g_frame_width !=
          static_cast<std::size_t>(kApkFrameWidth * window_scale) ||
      g_frame_height !=
          static_cast<std::size_t>(kApkFrameHeight * window_scale) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android view: Activity content presentation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 33;
  }
  if (run_apk_app) {
    if (g_interactive_root != nullptr) {
      env->DeleteGlobalRef(g_interactive_root);
    }
    g_interactive_root = env->NewGlobalRef(attached_decor);
    g_interactive_width = kApkFrameWidth * window_scale;
    g_interactive_height = kApkFrameHeight * window_scale;
    if (g_interactive_root == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android input: retaining DecorView failed\n";
      return 33;
    }
  }
  env->DeleteLocalRef(application);
  env->DeleteLocalRef(activity_info);
  env->DeleteLocalRef(context_theme_wrapper_class);
  env->DeleteLocalRef(probe_theme);
  env->DeleteLocalRef(attached_decor);
  env->DeleteLocalRef(content_root);
  env->DeleteLocalRef(decor_view);
  env->DeleteLocalRef(decor_view_class);
  env->DeleteLocalRef(window_attributes);
  env->DeleteLocalRef(accessibility_lock);
  env->DeleteLocalRef(object_class);
  env->DeleteLocalRef(accessibility);
  env->DeleteLocalRef(accessibility_class);
  env->DeleteLocalRef(phone_window_class);
  env->DeleteLocalRef(window_class);
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(probe_view);
  env->DeleteLocalRef(configuration);
  env->DeleteLocalRef(component_name);
  env->DeleteLocalRef(intent);
  env->DeleteLocalRef(title);
  env->DeleteLocalRef(class_name);
  env->DeleteLocalRef(package_name);
  env->DeleteLocalRef(configuration_class);
  env->DeleteLocalRef(component_name_class);
  env->DeleteLocalRef(intent_class);
  env->DeleteLocalRef(configured_apk_assets);
  env->DeleteLocalRef(framework_res_path);
  env->DeleteLocalRef(framework_apk_assets);
  env->DeleteLocalRef(apk_assets_class);
  env->DeleteLocalRef(asset_manager);
  env->DeleteLocalRef(asset_manager_class);
  env->DeleteLocalRef(probe_context);
  env->DeleteLocalRef(probe_resources);
  env->DeleteLocalRef(probe_resources_class);
  env->DeleteLocalRef(probe_view_class);
  env->DeleteLocalRef(probe_canvas_class);
  env->DeleteLocalRef(content_root_class);
  env->DeleteLocalRef(probe_context_class);
  env->DeleteLocalRef(application_class);
  env->DeleteLocalRef(activity_info_class);
  env->DeleteLocalRef(activity_class);
  env->DeleteLocalRef(activity_instance);
  if (lifecycle_result != 43) {
    std::cerr << "ART Android lifecycle: expected 43, got " << lifecycle_result
              << "\n";
    return 29;
  }

  JNINativeMethod native_methods[]{
      {const_cast<char*>("hostPageSize"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&HostPageSize)},
      {const_cast<char*>("nativePackedIntegerStack"),
       const_cast<char*>("(IIIIIIIJII)J"),
       reinterpret_cast<void*>(&NativePackedIntegerStack)},
      {const_cast<char*>("nativePackedFloatingStack"),
       const_cast<char*>("(FFFFFFFFFD)J"),
       reinterpret_cast<void*>(&NativePackedFloatingStack)},
      {const_cast<char*>("nativePackedReferenceStack"),
       const_cast<char*>("(IIIIIIILjava/lang/Object;I)J"),
       reinterpret_cast<void*>(&NativePackedReferenceStack)},
      {const_cast<char*>("nativePackedNarrowStack"),
       const_cast<char*>("(IIIIIIZBCSIJ)J"),
       reinterpret_cast<void*>(&NativePackedNarrowStack)},
  };
  if (self->GetJniEnv()->RegisterNatives(
          hello_class, native_methods, std::size(native_methods)) != JNI_OK) {
    std::cerr << "ART Darwin JNI: RegisterNatives failed\n";
    return 6;
  }
  if (!class_linker->EnsureInitialized(self, hello, true, true)) {
    std::cerr << "ART Darwin JNI: Hello initialization failed\n";
    return 7;
  }
  art::ArtMethod* answer =
      hello->FindClassMethod("answer", "()I", art::kRuntimePointerSize);
  if (answer == nullptr) {
    std::cerr << "ART Darwin DEX: answer()I lookup failed\n";
    return 8;
  }

  art::JValue result;
  answer->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u, &result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART Darwin DEX: answer()I threw\n";
    return 9;
  }
  if (result.GetI() != 42) {
    std::cerr << "ART Darwin DEX: expected 42, got " << result.GetI() << "\n";
    return 10;
  }

  art::ArtMethod* native_round_trip = hello->FindClassMethod(
      "nativeRoundTrip", "()I", art::kRuntimePointerSize);
  if (native_round_trip == nullptr) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I lookup failed\n";
    return 11;
  }
  art::JValue native_result;
  native_round_trip->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u,
                            &native_result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART Darwin JNI: nativeRoundTrip()I threw\n";
    return 12;
  }
  if (native_result.GetI() != 42) {
    std::cerr << "ART Darwin JNI: expected 42, got " << native_result.GetI()
              << "\n";
    return 13;
  }

  art::ArtMethod* native_stack_pcs = hello->FindClassMethod(
      "nativeStackPcsRoundTrip", "()I", art::kRuntimePointerSize);
  if (native_stack_pcs == nullptr) {
    std::cerr << "ART Darwin JNI PCS: nativeStackPcsRoundTrip()I lookup failed\n";
    return 34;
  }
  art::JValue native_stack_pcs_result;
  native_stack_pcs->Invoke(self, /* args= */ nullptr, /* args_size= */ 0u,
                           &native_stack_pcs_result, "I");
  if (self->IsExceptionPending() || native_stack_pcs_result.GetI() != 42) {
    std::cerr << "ART Darwin JNI PCS: packed stack argument matrix failed result="
              << native_stack_pcs_result.GetI() << "\n";
    return 35;
  }

  art::ArtMethod* runtime_native_arraycopy = hello->FindClassMethod(
      "runtimeNativeArraycopy", "()I", art::kRuntimePointerSize);
  if (runtime_native_arraycopy == nullptr) {
    std::cerr
        << "ART runtime native: runtimeNativeArraycopy()I lookup failed\n";
    return 14;
  }
  art::JValue arraycopy_result;
  runtime_native_arraycopy->Invoke(self, /* args= */ nullptr,
                                   /* args_size= */ 0u, &arraycopy_result, "I");
  if (self->IsExceptionPending()) {
    std::cerr << "ART runtime native: runtimeNativeArraycopy()I threw\n";
    return 15;
  }
  if (arraycopy_result.GetI() != 42) {
    std::cerr << "ART runtime native: expected 42, got "
              << arraycopy_result.GetI() << "\n";
    return 16;
  }

  jmethodID java_main =
      env->GetStaticMethodID(hello_class, "main", "([Ljava/lang/String;)V");
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray java_args = string_class == nullptr
                               ? nullptr
                               : env->NewObjectArray(1, string_class, nullptr);
  jstring message = env->NewStringUTF("Hello from Darwin ART main: 안녕");
  if (java_main == nullptr || string_class == nullptr || java_args == nullptr ||
      message == nullptr) {
    std::cerr << "ART Darwin launcher: main(String[]) setup failed\n";
    return 18;
  }
  env->SetObjectArrayElement(java_args, 0, message);
  env->CallStaticVoidMethod(hello_class, java_main, java_args);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(java_args);
  env->DeleteLocalRef(string_class);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Darwin launcher: main(String[]) threw\n"
              << self->GetException()->Dump() << "\n";
    env->ExceptionDescribe();
    return 19;
  }

  if (run_network_acceptance) {
    std::string load_error;
    bool loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, network_fixture_path, app_loader_ref, network_fixture_class,
          &load_error);
    }
    if (!loaded || !load_error.empty() || env->ExceptionCheck()) {
      std::cerr << "ART Android network JavaVMExt load failed: " << load_error
                << "\n";
      return 47;
    }
    g_network_elf_loaded = true;
    BoundedLoopbackHttpServer server;
    if (!server.Start()) {
      std::cerr << "ART Android network loopback listener failed\n";
      return 47;
    }
    jmethodID loopback = env->GetStaticMethodID(
        network_fixture_class, "nativeLoopbackHttp", "(I)I");
    const jint network_result =
        loopback == nullptr
            ? -1
            : env->CallStaticIntMethod(network_fixture_class, loopback,
                                       static_cast<jint>(server.port()));
    const bool server_ok = server.Stop();
    if (network_result != 42 || !server_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android network loopback failed, result="
                << network_result << " server=" << server_ok << "\n";
      return 47;
    }
    std::cout << "ART Android network: JavaVMExt+JNI_OnLoad loopback-HTTP=42 "
                 "socket+DNS=closed Internet=no\n"
              << std::flush;
  }

  if (run_elf_jni_fixture) {
    if (!run_generic_elf) {
      std::cerr << "ART Android ELF generic graph path is missing\n";
      return 40;
    }
    if (!run_apk_elf) {
      std::cerr << "ART Android APK ELF extraction/hash boundary is missing\n";
      return 45;
    }
    if (!run_libcxx_acceptance) {
      std::cerr << "ART Android libc++ fixture paths are missing\n";
      return 43;
    }
    if (!run_tls_acceptance) {
      std::cerr << "ART Android TLS fixture path is missing\n";
      return 44;
    }
    std::string libcxx_error;
    bool collections_ok = false;
    bool exception_ok = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      JavaVM* vm =
          reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
      collections_ok = RunAndroidElfSelfTest(
          env, vm, app_loader_ref, libcxx_collections_path, &libcxx_error);
      if (collections_ok) {
        exception_ok = RunAndroidElfSelfTest(
            env, vm, app_loader_ref, libcxx_exception_path, &libcxx_error);
      }
    }
    if (!collections_ok || !exception_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android libc++ acceptance failed: " << libcxx_error
                << "\n";
      return 43;
    }
    std::cout << "ART Android libc++: real-r28c collections=189 "
                 "exception-cleanup=73 unload=sequential\n"
              << std::flush;
    std::string tls_error;
    bool tls_ok = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      JavaVM* vm =
          reinterpret_cast<JavaVM*>(art::Runtime::Current()->GetJavaVM());
      tls_ok = RunAndroidElfSelfTest(env, vm, app_loader_ref, tls_fixture_path,
                                    &tls_error);
    }
    if (!tls_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF TLS acceptance failed: " << tls_error
                << "\n";
      return 44;
    }
    std::cout << "ART Android ELF TLS: local-TLSDESC threads=4 align=64 "
                 "unload=quiescent\n"
              << std::flush;
    std::string generic_load_error;
    bool generic_loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      generic_loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, generic_elf_path, app_loader_ref, nullptr, &generic_load_error);
    }
    if (!generic_loaded || !generic_load_error.empty() || env->ExceptionCheck() ||
        darwin_art_elf_jni_fixture_registration_status() != 0) {
      std::cerr << "ART Android ELF generic graph load failed, load_error="
                << generic_load_error << "\n";
      return 40;
    }
    jmethodID generic_native_add =
        env->GetStaticMethodID(native_fixture_class, "nativeAdd", "(IJI)J");
    const jlong generic_add_result =
        generic_native_add == nullptr
            ? -1
            : env->CallStaticLongMethod(native_fixture_class,
                                        generic_native_add, 10, jlong{20}, 12);
    if (generic_add_result != 42 || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF generic RegisterNatives failed, result="
                << generic_add_result << "\n";
      return 40;
    }
    // generic_elf_path and apk_elf_path are required to be the same extracted
    // root. The successful JavaVMExt load above is therefore the APK execution
    // evidence; loading the same SONAME a second time would only exercise ART's
    // path cache and acquire no additional graph ownership.
    g_apk_elf_loaded = true;
    g_apk_sha256 = apk_sha256;
    g_apk_root_sha256 = apk_root_sha256;
    char *partial_error = nullptr;
    void *partial_handle =
        android::OpenNativeLibrary(env, 35, elf_fixture_path, app_loader_ref,
                                   nullptr, nullptr, nullptr, &partial_error);
    const bool partial_cleanup_ok =
        partial_handle == nullptr && partial_error != nullptr &&
        darwin_art_elf_jni_fixture_lifecycle_status() == 124567 &&
        darwin_art_elf_jni_fixture_namespace_lifecycle_status() == 5;
    if (partial_handle != nullptr) {
      char* close_error = nullptr;
      (void)android::CloseNativeLibrary(partial_handle, true, &close_error);
      android::NativeLoaderFreeErrorMessage(close_error);
    }
    const std::string partial_error_text =
        partial_error == nullptr ? "<none>" : partial_error;
    android::NativeLoaderFreeErrorMessage(partial_error);
    if (!partial_cleanup_ok || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: partial failure cleanup failed, lifecycle="
                << darwin_art_elf_jni_fixture_lifecycle_status()
                << " namespace="
                << darwin_art_elf_jni_fixture_namespace_lifecycle_status()
                << " error=" << partial_error_text
                << "\n";
      return 40;
    }
    std::string load_error;
    bool loaded = false;
    {
      art::ScopedThreadSuspension suspended(self, art::ThreadState::kNative);
      loaded = art::Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
          env, elf_fixture_path, app_loader_ref, native_fixture_class,
          &load_error);
    }
    const int bridge_status =
        darwin_art_elf_jni_fixture_registration_status();
    const int lifecycle_status =
        darwin_art_elf_jni_fixture_lifecycle_status();
    const int namespace_status =
        darwin_art_elf_jni_fixture_namespace_lifecycle_status();
    if (!loaded || !load_error.empty() || bridge_status != 0x7f ||
        lifecycle_status != 123 || namespace_status != 3 ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: load/registration failed, status="
                << bridge_status << " lifecycle=" << lifecycle_status
                << " namespace=" << namespace_status
                << " load_error=" << load_error << "\n";
      return 41;
    }
    jmethodID run_acceptance =
        env->GetStaticMethodID(native_fixture_class, "runAcceptance", "()I");
    const jint acceptance =
        run_acceptance == nullptr
            ? -3
            : env->CallStaticIntMethod(native_fixture_class, run_acceptance);
    if (acceptance != 42 || env->ExceptionCheck()) {
      std::cerr << "ART Android ELF JNI: nativeAdd/nativeSpill failed, result="
                << acceptance << "\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 42;
    }
    std::cout
        << "ART Android ELF JNI: graph=child-first+relocated "
           "providers=bind_builtins+__errno+strlen+fs-random-ctor+scanf+"
           "swprintf+ioctl+strftime+sendfile "
           "load+JNI_OnLoad+RegisterNatives=generic+fixture scalar-ref=all "
           "nativeUsesEnv=current stack-repack=ok\n"
        << std::flush;
  }

  run_result->hello_answer = result.GetI();
  run_result->native_round_trip = native_result.GetI();
  run_result->arraycopy_result = arraycopy_result.GetI();
  run_result->activity_probe_result = activity_result;
  run_result->lifecycle_result = lifecycle_result;
  run_result->frame_width = static_cast<uint32_t>(g_frame_width);
  run_result->frame_height = static_cast<uint32_t>(g_frame_height);
  return 0;
  }();
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_shutdown_process() {
  JavaVM* java_vm = nullptr;
  art::Thread* art_thread = nullptr;
  bool resource_runtime_installed = false;
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    switch (g_process_state.phase) {
      case ProcessPhase::kShutdownComplete:
        return DARWIN_ART_STATUS_SHUTDOWN_ALREADY_COMPLETED;
      case ProcessPhase::kShutdownFailed:
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      case ProcessPhase::kNeverStarted:
      case ProcessPhase::kCreateFailed:
      case ProcessPhase::kRunning:
      case ProcessPhase::kShuttingDown:
        return DARWIN_ART_STATUS_SHUTDOWN_NOT_READY;
      case ProcessPhase::kAwaitingShutdown:
        break;
    }

    if (!g_process_state.owner_thread_valid ||
        pthread_equal(g_process_state.owner_thread, pthread_self()) == 0 ||
        (g_process_state.art_thread != nullptr &&
         art::Thread::Current() != g_process_state.art_thread)) {
      return DARWIN_ART_STATUS_SHUTDOWN_WRONG_THREAD;
    }
    g_process_state.phase = ProcessPhase::kShuttingDown;
    java_vm = g_process_state.java_vm;
    art_thread = g_process_state.art_thread;
    resource_runtime_installed = g_process_state.resource_runtime_installed;
  }

  CHECK(java_vm != nullptr);
  if (art_thread != nullptr) {
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
    {
      art::ScopedObjectAccess soa(art_thread);
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: clearing pending exception: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      if (g_probe_canvas_class != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_probe_canvas_class);
        g_probe_canvas_class = nullptr;
      }
      if (g_pressed_view != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_pressed_view);
        g_pressed_view = nullptr;
      }
      if (g_interactive_root != nullptr) {
        art_thread->GetJniEnv()->DeleteGlobalRef(g_interactive_root);
        g_interactive_root = nullptr;
      }
      g_interactive_width = 0;
      g_interactive_height = 0;
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: global reference cleanup threw: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      if (!darwin_art::ShutdownLibcoreNatives()) {
        std::cerr << "ART Darwin shutdown: libcore host state restore failed\n";
        std::lock_guard<std::mutex> lock(g_process_state.mutex);
        g_process_state.phase = ProcessPhase::kShutdownFailed;
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
      if (resource_runtime_installed &&
          !darwin_art::ShutdownFrameworkResourceRuntime(
              art_thread->GetJniEnv())) {
        std::cerr << "ART Darwin shutdown: AndroidRuntime ownership uninstall failed\n";
        std::lock_guard<std::mutex> lock(g_process_state.mutex);
        g_process_state.phase = ProcessPhase::kShutdownFailed;
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
    }
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
  }

  // DestroyJavaVM deletes Runtime::Current(). Registered DexFile pointers must
  // remain valid through ClassLinker/Heap teardown, so release their owning
  // storage only after this returns successfully. Do not touch art_thread after
  // this call: its Thread object is owned by the destroyed Runtime.
  const jint destroy_result = java_vm->DestroyJavaVM();
  if (destroy_result != JNI_OK) {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  g_host_context = nullptr;
  g_frame_callback = nullptr;
  if (darwin_art::android_jni::TrampolineLiveCount() != 0) {
    std::cerr << "ART Darwin shutdown: ELF JNI trampolines remain live\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (g_network_elf_loaded &&
      (darwin_art_bionic_socket_broker_is_active() != 0 ||
       darwin_art_bionic_socket_broker_live_objects() != 0 ||
       darwin_art_bionic_dns_live_results_for_test() != 0 ||
       darwin_art_bionic_dns_retired_results_for_test() != 0)) {
    std::cerr << "ART Darwin shutdown: network owner did not quiesce\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (g_apk_elf_loaded) {
    std::cout << "ART Android APK ELF: apk-sha256=" << g_apk_sha256
              << " root-sha256=" << g_apk_root_sha256
              << " graph=root+child+grandchild load=JavaVMExt+NativeBridge "
                 "unload=shutdown-trampolines-zero\n"
              << std::flush;
  }
  if (g_direct_apk_loaded) {
    std::cout << "ART Android direct APK ELF: source=readonly-fd-slices "
                 "copy=0 extract=0 alignment=16384 graph=root+child+grandchild "
                 "load=JavaVMExt+NativeBridge JNI_OnLoad=0x00010006 "
                 "unload=shutdown-trampolines-zero authority=isolated-process\n"
              << std::flush;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_lifecycle_status() != 1234567) {
    std::cerr << "ART Darwin shutdown: ELF JNI graph finalizer order failed, status="
              << darwin_art_elf_jni_fixture_lifecycle_status() << "\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_namespace_lifecycle_status() != 5) {
    std::cerr << "ART Darwin shutdown: Bionic namespace teardown order failed, status="
              << darwin_art_elf_jni_fixture_namespace_lifecycle_status() << "\n";
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.phase = ProcessPhase::kShutdownFailed;
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }

  darwin_art::ShutdownIcuCharsetNatives();
  darwin_art::ShutdownFrameworkGraphicsRuntime();
  g_process_state.app_dex_files.clear();
  {
    std::lock_guard<std::mutex> lock(g_process_state.mutex);
    g_process_state.java_vm = nullptr;
    g_process_state.art_thread = nullptr;
    g_process_state.resource_runtime_installed = false;
    g_process_state.phase = ProcessPhase::kShutdownComplete;
  }
  return 0;
}
