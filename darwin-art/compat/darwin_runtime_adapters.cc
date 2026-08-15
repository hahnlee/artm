#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "jni/jni_env_ext.h"
#include "hprof/hprof.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_art_elf_jni_fixture_identity.h"
#include "darwin_art_elf_loader.h"
#include "darwin_art_bionic_builtin_adapters.h"
#include "darwin_art_bionic_dso_lifecycle.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_provider_namespace.h"
#include "darwin_art_jni_proxy.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"
#include "odr_statslog/odr_statslog.h"
#include "palette/palette.h"
#include "runtime_image.h"
#include "thread.h"
#include "unwindstack/AndroidUnwinder.h"

namespace android {
namespace {

constexpr uint64_t kElfLibraryMagic = UINT64_C(0x44415257454c464a);
constexpr int kElfOpened = 1 << 0;
constexpr int kElfOnLoadCalled = 1 << 1;
constexpr int kElfFoundFixtureClass = 1 << 2;
constexpr int kElfCapturedRegistration = 1 << 3;
constexpr int kElfInstalledRegistration = 1 << 4;
constexpr int kElfClassifiedTrampolines = 1 << 5;
constexpr int kElfBionicProvidersRouted = 1 << 6;
constexpr uint32_t kFixtureErrnoRouteMask = 1u << 0;
constexpr uint32_t kFixtureStrlenRouteMask = 1u << 1;
constexpr uint32_t kFixtureOpenRouteMask = 1u << 2;
constexpr uint32_t kFixtureReadRouteMask = 1u << 3;
constexpr uint32_t kFixtureCloseRouteMask = 1u << 4;
constexpr uint32_t kFixtureAllProviderRouteMask =
    kFixtureErrnoRouteMask | kFixtureStrlenRouteMask | kFixtureOpenRouteMask |
    kFixtureReadRouteMask | kFixtureCloseRouteMask;
constexpr uint32_t kFixtureNativeAddEntryMask = 1u << 0;
constexpr uint32_t kFixtureNativeSpillEntryMask = 1u << 1;
constexpr uint32_t kFixtureNativeUsesEnvEntryMask = 1u << 2;
constexpr uint32_t kFixtureAllEntryMask = 0xffu;

std::atomic<int> g_elf_fixture_status{0};
std::atomic<uint32_t> g_elf_classified_trampoline_mask{0};
std::atomic<int> g_elf_fixture_lifecycle{0};
std::atomic<uint32_t> g_elf_fixture_provider_routes{0};
std::atomic<int> g_elf_fixture_namespace_lifecycle{0};

extern "C" uintptr_t darwin_art_bionic_rust_provider_closure_anchor();

void SetNativeLoaderError(char** error_msg, const std::string& message) {
  if (error_msg != nullptr) {
    *error_msg = strdup(message.c_str());
  }
}

std::string ElfError(DarwinArtElfStatus status,
                     const DarwinArtElfErrorBuffer& error) {
  std::ostringstream stream;
  stream << darwin_art_elf_status_name(status);
  if (error.data != nullptr && error.data[0] != '\0') {
    stream << ": " << error.data;
  }
  return stream.str();
}

bool SplitTrustedLibraryPath(const char* path,
                             std::string* parent,
                             std::vector<uint8_t>* component) {
  if (path == nullptr || path[0] == '\0') return false;
  const std::string bytes(path);
  const size_t slash = bytes.find_last_of('/');
  const size_t component_offset = slash == std::string::npos ? 0 : slash + 1;
  if (component_offset == bytes.size()) return false;
  if (slash == std::string::npos) {
    *parent = ".";
  } else if (slash == 0) {
    *parent = "/";
  } else {
    *parent = bytes.substr(0, slash);
  }
  component->assign(bytes.begin() + component_offset, bytes.end());
  return true;
}

struct DiscoveredGraphDeleter {
  void operator()(DarwinArtElfDiscoveredGraph* graph) const {
    darwin_art_elf_discovered_graph_destroy(&graph);
  }
};

class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const { return fd_; }

 private:
  int fd_ = -1;
};

struct FilesystemOwnerState {
  std::mutex mutex;
  size_t users = 0;
  dev_t device = 0;
  ino_t inode = 0;
};

FilesystemOwnerState g_filesystem_owner;

bool AcquireFilesystemOwner(int directory_fd, std::string* error) {
  struct stat status {};
  if (directory_fd < 0 || fstat(directory_fd, &status) != 0 ||
      !S_ISDIR(status.st_mode)) {
    *error = "trusted filesystem authority is not a live directory";
    return false;
  }
  std::lock_guard<std::mutex> lock(g_filesystem_owner.mutex);
  if (g_filesystem_owner.users != 0) {
    if (g_filesystem_owner.device != status.st_dev ||
        g_filesystem_owner.inode != status.st_ino) {
      *error = "another Android ELF graph owns a different filesystem authority";
      return false;
    }
    if (g_filesystem_owner.users == std::numeric_limits<size_t>::max()) {
      *error = "Bionic filesystem graph-owner count overflow";
      return false;
    }
    ++g_filesystem_owner.users;
    return true;
  }
  constexpr uint8_t kGuestRoot[] = {'/'};
  const DarwinArtBionicFsProcessOwnerStatus install_status =
      darwin_art_bionic_fs_process_install(
          directory_fd, kGuestRoot, std::size(kGuestRoot), kGuestRoot,
          std::size(kGuestRoot));
  if (install_status != DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK) {
    *error = "Bionic filesystem process-owner install failed: " +
             std::to_string(static_cast<int>(install_status));
    return false;
  }
  g_filesystem_owner.users = 1;
  g_filesystem_owner.device = status.st_dev;
  g_filesystem_owner.inode = status.st_ino;
  return true;
}

void ReleaseFilesystemOwner() {
  std::lock_guard<std::mutex> lock(g_filesystem_owner.mutex);
  if (g_filesystem_owner.users == 0) std::abort();
  --g_filesystem_owner.users;
  if (g_filesystem_owner.users != 0) return;
  if (darwin_art_bionic_fs_process_uninstall() !=
      DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK) {
    // Continuing could destroy the provider namespace while a guest call
    // still owns filesystem state. This is an embedding lifecycle invariant.
    std::abort();
  }
  g_filesystem_owner.device = 0;
  g_filesystem_owner.inode = 0;
}

std::string Sha256(const uint8_t* bytes, size_t size) {
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
  CC_SHA256(bytes, static_cast<CC_LONG>(size), digest.data());
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '\0');
  for (size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = kHex[digest[index] >> 4];
    result[index * 2 + 1] = kHex[digest[index] & 0xf];
  }
  return result;
}

bool IsExactFixtureGraph(const char* root_soname,
                         const DarwinArtElfGraphSource* sources,
                         size_t source_count) {
  if (root_soname == nullptr ||
      std::strcmp(root_soname, kDarwinArtElfJniFixtureSoname) != 0 ||
      sources == nullptr || source_count != 3) {
    return false;
  }
  struct Expected {
    const char* soname;
    size_t size;
    const char* sha256;
  };
  const Expected expected[] = {
      {kDarwinArtElfJniFixtureSoname, kDarwinArtElfJniFixtureSize,
       kDarwinArtElfJniFixtureSha256},
      {kDarwinArtElfJniChildSoname, kDarwinArtElfJniChildSize,
       kDarwinArtElfJniChildSha256},
      {kDarwinArtElfJniGrandchildSoname, kDarwinArtElfJniGrandchildSize,
       kDarwinArtElfJniGrandchildSha256},
  };
  for (const Expected& member : expected) {
    bool found = false;
    for (size_t index = 0; index < source_count; ++index) {
      const DarwinArtElfGraphSource& source = sources[index];
      if (source.soname != nullptr &&
          std::strcmp(source.soname, member.soname) == 0) {
        if (found || source.bytes == nullptr || source.length != member.size ||
            Sha256(source.bytes, source.length) != member.sha256) {
          return false;
        }
        found = true;
      }
    }
    if (!found) return false;
  }
  return true;
}

struct ElfLibrary {
  uint64_t magic = kElfLibraryMagic;
  bool fixture_graph = false;
  DarwinArtElfGraphHandle* graph = nullptr;
  DarwinArtBionicNamespace* provider_namespace = nullptr;
  DarwinArtBionicDsoLifecycleOwner* dso_lifecycle = nullptr;
  bool filesystem_owner = false;
  uintptr_t jni_on_load = 0;
  uintptr_t jni_on_unload = 0;
  alignas(DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT)
      std::array<unsigned char, DARWIN_ART_JNI_PROXY_STORAGE_SIZE> proxy_storage{};
  DarwinArtJniProxy* proxy = nullptr;
  darwin_art::android_jni::TrampolineSet* trampolines = nullptr;
};

void TeardownProviderNamespace(ElfLibrary* library) {
  if (library == nullptr) return;
  if (library->dso_lifecycle != nullptr) {
    darwin_art_bionic_dso_lifecycle_owner_destroy(library->dso_lifecycle);
    library->dso_lifecycle = nullptr;
  }
  if (library->filesystem_owner) {
    ReleaseFilesystemOwner();
    library->filesystem_owner = false;
  }
  if (library->provider_namespace == nullptr) return;
  const DarwinArtBionicNamespaceStatus status =
      darwin_art_bionic_namespace_teardown(library->provider_namespace);
  if (status == DARWIN_ART_BIONIC_NAMESPACE_OK && library->fixture_graph) {
    g_elf_fixture_namespace_lifecycle.store(5, std::memory_order_relaxed);
  }
  darwin_art_bionic_namespace_destroy(library->provider_namespace);
  library->provider_namespace = nullptr;
}

void FixtureRecordLifecycle(int phase) {
  if (phase < 1 || phase > 7) {
    g_elf_fixture_lifecycle.store(-phase, std::memory_order_relaxed);
    return;
  }
  int observed = g_elf_fixture_lifecycle.load(std::memory_order_relaxed);
  while (!g_elf_fixture_lifecycle.compare_exchange_weak(
      observed, observed * 10 + phase, std::memory_order_relaxed)) {}
}

void SetResolverError(DarwinArtElfErrorBuffer* error, const char* message) {
  if (error == nullptr || message == nullptr) return;
  const size_t required = std::strlen(message) + 1;
  error->required = required;
  if (error->data == nullptr || error->capacity == 0) return;
  const size_t copied = std::min(required - 1, error->capacity - 1);
  std::memcpy(error->data, message, copied);
  error->data[copied] = '\0';
}

DarwinArtElfResolveStatus ResolveRuntimeProvider(
    void* context,
    const DarwinArtElfSymbolRequest* request,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error) {
  if (request == nullptr || out_address == nullptr ||
      request->abi_version != DARWIN_ART_ELF_ABI_VERSION ||
      request->symbol == nullptr) {
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  auto* library = static_cast<ElfLibrary*>(context);
  if (std::strcmp(request->symbol, "darwin_art_fixture_record_lifecycle") == 0) {
    if (library == nullptr || !library->fixture_graph) {
      SetResolverError(error, "fixture lifecycle provider is reserved to the fixture graph");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
    if (request->version_soname != nullptr || request->version_name != nullptr) {
      SetResolverError(error, "fixture lifecycle provider must be unversioned");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
    bool provider_is_explicit = false;
    for (size_t index = 0; index < request->needed_library_count; ++index) {
      const char* soname = request->needed_libraries[index];
      provider_is_explicit = provider_is_explicit ||
                             (soname != nullptr &&
                              std::strcmp(soname,
                                          kDarwinArtElfJniHostProviderSoname) == 0);
    }
    if (!provider_is_explicit) {
      SetResolverError(error, "fixture lifecycle provider is not explicit");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
    *out_address = reinterpret_cast<uintptr_t>(&FixtureRecordLifecycle);
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }

  if (library == nullptr || library->provider_namespace == nullptr) {
    SetResolverError(error, "Bionic provider namespace is unavailable");
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  const char* provider_soname = request->version_soname;
  const char* provider_version = request->version_name;
  if ((provider_soname == nullptr) != (provider_version == nullptr)) {
    SetResolverError(error, "Bionic symbol version request is incomplete");
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  if (provider_soname == nullptr) {
    for (size_t index = 0; index < request->needed_library_count; ++index) {
      const char* soname = request->needed_libraries[index];
      if (soname != nullptr && std::strcmp(soname, "liblog.so") == 0) {
        provider_soname = soname;
        break;
      }
    }
    if (provider_soname == nullptr) {
      SetResolverError(error, "unversioned Bionic import has no exact provider");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
  }
  const DarwinArtBionicNamespaceResult result =
      darwin_art_bionic_namespace_resolve(
          library->provider_namespace, provider_soname, request->symbol,
          provider_version);
  if (result.status != DARWIN_ART_BIONIC_NAMESPACE_OK || result.address == 0) {
    SetResolverError(error,
                     darwin_art_bionic_namespace_status_name(result.status));
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  uint32_t route = 0;
  if (std::strcmp(request->symbol, "__errno") == 0) {
    route = kFixtureErrnoRouteMask;
  } else if (std::strcmp(request->symbol, "strlen") == 0) {
    route = kFixtureStrlenRouteMask;
  } else if (std::strcmp(request->symbol, "open") == 0) {
    route = kFixtureOpenRouteMask;
  } else if (std::strcmp(request->symbol, "read") == 0) {
    route = kFixtureReadRouteMask;
  } else if (std::strcmp(request->symbol, "close") == 0) {
    route = kFixtureCloseRouteMask;
  }
  if (route != 0 && library->fixture_graph) {
    g_elf_fixture_provider_routes.fetch_or(route, std::memory_order_relaxed);
  }
  *out_address = result.address;
  return DARWIN_ART_ELF_RESOLVE_FOUND;
}

ElfLibrary* AsElfLibrary(void* handle) {
  auto* library = static_cast<ElfLibrary*>(handle);
  return library != nullptr && library->magic == kElfLibraryMagic ? library : nullptr;
}

JNIEnv* CurrentArtEnv() {
  art::Thread* self = art::Thread::Current();
  return self == nullptr ? nullptr : static_cast<JNIEnv*>(self->GetJniEnv());
}

bool ParseDescriptorType(const char** cursor,
                         bool allow_void,
                         char* shorty_type) {
  const char* current = *cursor;
  switch (*current) {
    case 'V':
      if (!allow_void) {
        return false;
      }
      *shorty_type = 'V';
      *cursor = current + 1;
      return true;
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
      *shorty_type = *current;
      *cursor = current + 1;
      return true;
    case 'L': {
      const char* end = std::strchr(current + 1, ';');
      if (end == nullptr || end == current + 1) {
        return false;
      }
      *shorty_type = 'L';
      *cursor = end + 1;
      return true;
    }
    case '[': {
      do {
        ++current;
      } while (*current == '[');
      char component = 0;
      if (!ParseDescriptorType(&current, false, &component)) {
        return false;
      }
      *shorty_type = 'L';
      *cursor = current;
      return true;
    }
    default:
      return false;
  }
}

bool DescriptorToShorty(const char* descriptor, std::string* shorty) {
  if (descriptor == nullptr || shorty == nullptr || descriptor[0] != '(') {
    return false;
  }
  const char* cursor = descriptor + 1;
  std::string arguments;
  while (*cursor != ')') {
    char type = 0;
    if (*cursor == '\0' || !ParseDescriptorType(&cursor, false, &type)) {
      return false;
    }
    arguments.push_back(type);
  }
  ++cursor;
  char return_type = 0;
  if (!ParseDescriptorType(&cursor, true, &return_type) || *cursor != '\0') {
    return false;
  }
  shorty->assign(1, return_type);
  shorty->append(arguments);
  return true;
}

void* ProxyFindClass(void* context, const char* name) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || name == nullptr) {
    return nullptr;
  }
  void* clazz = art_env->FindClass(name);
  if (library->fixture_graph && clazz != nullptr &&
      std::strcmp(name, "darwin/art/nativefixture/NativeFixture") == 0) {
    g_elf_fixture_status.fetch_or(kElfFoundFixtureClass, std::memory_order_relaxed);
  }
  return clazz;
}

int32_t ProxyRegisterNatives(void* context,
                             void* clazz,
                             const DarwinArtJniNativeMethod* methods,
                             int32_t count) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || !library->fixture_graph || clazz == nullptr ||
      methods == nullptr || count != 8) {
    return DARWIN_ART_JNI_ERR;
  }
  JNIEnv* art_env = CurrentArtEnv();
  if (library->trampolines != nullptr || library->proxy == nullptr ||
      art_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  constexpr std::array<std::pair<const char*, const char*>, 8> kExpected = {{
      {"nativeAdd", "(IJI)J"},
      {"nativeSpill", kDarwinArtElfJniFixtureSpillSignature},
      {"nativeUsesEnv", "()I"},
      {"nativeNarrowStack", "(IIIIIIZBCSIJLjava/lang/Object;)I"},
      {"nativeEcho", "(Ljava/lang/Object;)Ljava/lang/Object;"},
      {"nativeFloat", "(F)F"},
      {"nativeDouble", "(D)D"},
      {"nativeVoid", "()V"},
  }};
  for (size_t index = 0; index < kExpected.size(); ++index) {
    if (methods[index].name == nullptr || methods[index].signature == nullptr ||
        methods[index].function == nullptr ||
        std::strcmp(methods[index].name, kExpected[index].first) != 0 ||
        std::strcmp(methods[index].signature, kExpected[index].second) != 0) {
      return DARWIN_ART_JNI_ERR;
    }
    if (art_env->GetStaticMethodID(
            static_cast<jclass>(clazz), kExpected[index].first,
            kExpected[index].second) == nullptr) {
      return DARWIN_ART_JNI_ERR;
    }
  }
  g_elf_fixture_status.fetch_or(kElfCapturedRegistration, std::memory_order_relaxed);
  JavaVM* proxy_vm =
      static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy));
  void* proxy_env = nullptr;
  if (proxy_vm == nullptr ||
      proxy_vm->GetEnv(&proxy_env, JNI_VERSION_1_6) != JNI_OK ||
      proxy_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  std::array<std::string, 8> shorties;
  std::array<darwin_art::android_jni::TrampolineRequest, 8> requests;
  for (size_t index = 0; index < requests.size(); ++index) {
    if (!DescriptorToShorty(methods[index].signature, &shorties[index])) {
      return DARWIN_ART_JNI_ERR;
    }
    requests[index] = {methods[index].function, shorties[index].c_str(),
                       uint32_t{1} << index};
  }
  std::string trampoline_error;
  auto* trampolines = darwin_art::android_jni::CreateRegularTrampolines(
      proxy_env, requests.data(), requests.size(), &trampoline_error);
  if (trampolines == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  void* native_add_entry = darwin_art::android_jni::TrampolineEntry(trampolines, 0);
  void* native_spill_entry = darwin_art::android_jni::TrampolineEntry(trampolines, 1);
  void* native_uses_env_entry =
      darwin_art::android_jni::TrampolineEntry(trampolines, 2);
  const auto* native_add_bytes = static_cast<const uint8_t*>(native_add_entry);
  bool all_entries_valid = true;
  for (size_t index = 0; index < requests.size(); ++index) {
    void* entry = darwin_art::android_jni::TrampolineEntry(trampolines, index);
    all_entries_valid = all_entries_valid && entry != nullptr &&
                        darwin_art::android_jni::TrampolineEntryMask(entry) ==
                            (uint32_t{1} << index);
  }
  if (darwin_art::android_jni::TrampolineGeneration(trampolines) == 0 ||
      darwin_art::android_jni::TrampolineCount(trampolines) != requests.size() ||
      !all_entries_valid ||
      !darwin_art::android_jni::IsTrampolineEntry(native_add_entry) ||
      !darwin_art::android_jni::IsTrampolineEntry(native_spill_entry) ||
      !darwin_art::android_jni::IsTrampolineEntry(native_uses_env_entry) ||
      darwin_art::android_jni::IsTrampolineEntry(native_add_bytes + 4) ||
      darwin_art::android_jni::TrampolineEntryMask(native_add_entry) !=
          kFixtureNativeAddEntryMask ||
      darwin_art::android_jni::TrampolineEntryMask(native_spill_entry) !=
          kFixtureNativeSpillEntryMask ||
      darwin_art::android_jni::TrampolineEntryMask(native_uses_env_entry) !=
          kFixtureNativeUsesEnvEntryMask ||
      darwin_art::android_jni::TrampolineEntryMask(native_add_bytes + 4) != 0) {
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
    return DARWIN_ART_JNI_ERR;
  }
  JNINativeMethod bridged_methods[] = {
      {const_cast<char*>(kExpected[0].first),
       const_cast<char*>(kExpected[0].second),
       native_add_entry},
      {const_cast<char*>(kExpected[1].first),
       const_cast<char*>(kExpected[1].second),
       native_spill_entry},
      {const_cast<char*>(kExpected[2].first),
       const_cast<char*>(kExpected[2].second),
       native_uses_env_entry},
      {const_cast<char*>(kExpected[3].first),
       const_cast<char*>(kExpected[3].second),
       darwin_art::android_jni::TrampolineEntry(trampolines, 3)},
      {const_cast<char*>(kExpected[4].first),
       const_cast<char*>(kExpected[4].second),
       darwin_art::android_jni::TrampolineEntry(trampolines, 4)},
      {const_cast<char*>(kExpected[5].first),
       const_cast<char*>(kExpected[5].second),
       darwin_art::android_jni::TrampolineEntry(trampolines, 5)},
      {const_cast<char*>(kExpected[6].first),
       const_cast<char*>(kExpected[6].second),
       darwin_art::android_jni::TrampolineEntry(trampolines, 6)},
      {const_cast<char*>(kExpected[7].first),
       const_cast<char*>(kExpected[7].second),
       darwin_art::android_jni::TrampolineEntry(trampolines, 7)},
  };
  const jint status = art_env->RegisterNatives(
      static_cast<jclass>(clazz), bridged_methods, std::size(bridged_methods));
  if (status != JNI_OK) {
    jthrowable registration_failure = art_env->ExceptionOccurred();
    if (registration_failure != nullptr) {
      art_env->ExceptionClear();
    }
    art_env->UnregisterNatives(static_cast<jclass>(clazz));
    jthrowable rollback_failure = art_env->ExceptionOccurred();
    if (rollback_failure != nullptr) {
      art_env->ExceptionClear();
    }
    darwin_art::android_jni::DestroyRegularTrampolines(trampolines);
    if (registration_failure != nullptr) {
      art_env->Throw(registration_failure);
      art_env->DeleteLocalRef(registration_failure);
    } else if (rollback_failure != nullptr) {
      art_env->Throw(rollback_failure);
    }
    art_env->DeleteLocalRef(rollback_failure);
    return DARWIN_ART_JNI_ERR;
  }
  library->trampolines = trampolines;
  g_elf_fixture_status.fetch_or(kElfInstalledRegistration,
                                std::memory_order_relaxed);
  return DARWIN_ART_JNI_OK;
}

int32_t ProxyThrowNew(void* context, void* clazz, const char* message) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || clazz == nullptr ||
      message == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  return art_env->ThrowNew(static_cast<jclass>(clazz), message);
}

thread_local ElfLibrary* g_pending_on_load = nullptr;
thread_local ElfLibrary* g_pending_on_unload = nullptr;

jint ElfJniOnLoadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_load, nullptr);
  if (library == nullptr || library->jni_on_load == 0 || library->proxy == nullptr) {
    return JNI_ERR;
  }
  if (library->fixture_graph) {
    g_elf_fixture_status.fetch_or(kElfOnLoadCalled, std::memory_order_relaxed);
  }
  using AndroidJniOnLoad = jint (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnLoad>(library->jni_on_load);
  // JNI_OnLoad itself has only two register arguments, so its Android and
  // Darwin AArch64 calling sequences coincide. The ELF library sees only the
  // closed proxy VM and never ART's real JavaVM function table.
  const jint result =
      function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
  return result;
}

void ElfJniOnUnloadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_unload, nullptr);
  if (library == nullptr || library->jni_on_unload == 0 || library->proxy == nullptr) {
    return;
  }
  using AndroidJniOnUnload = void (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnUnload>(library->jni_on_unload);
  function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
}

bool LookupOptionalElfSymbol(ElfLibrary* library,
                             const char* name,
                             uintptr_t* address,
                             std::string* error) {
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer error_buffer{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status =
      darwin_art_elf_graph_lookup_root(library->graph, name, address, &error_buffer);
  if (status == DARWIN_ART_ELF_OK) return true;
  if (status == DARWIN_ART_ELF_SYMBOL_NOT_FOUND) {
    *address = 0;
    return true;
  }
  *error = ElfError(status, error_buffer);
  return false;
}

}  // namespace

extern "C" {

bool LoadNativeBridge(const char* library, const NativeBridgeRuntimeCallbacks*) {
  // An empty name means that ART explicitly requested no translation bridge.
  return library == nullptr || library[0] == '\0';
}

bool PreInitializeNativeBridge(const char*, const char*) { return true; }
void PreZygoteForkNativeBridge() {}
bool InitializeNativeBridge(JNIEnv*, const char*) { return true; }
bool NativeBridgeInitialized() { return false; }
uint32_t NativeBridgeGetVersion() { return 0; }

void UnloadNativeBridge() {}

void* NativeBridgeGetTrampoline2(void* handle,
                                 const char* name,
                                 const char*,
                                 uint32_t,
                                 JNICallType) {
  ElfLibrary* library = AsElfLibrary(handle);
  if (library == nullptr || name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "JNI_OnLoad") == 0 && library->jni_on_load != 0) {
    if (g_pending_on_load != nullptr) {
      return nullptr;
    }
    g_pending_on_load = library;
    return reinterpret_cast<void*>(&ElfJniOnLoadTrampoline);
  }
  if (std::strcmp(name, "JNI_OnUnload") == 0 && library->jni_on_unload != 0) {
    if (g_pending_on_unload != nullptr) {
      return nullptr;
    }
    g_pending_on_unload = library;
    return reinterpret_cast<void*>(&ElfJniOnUnloadTrampoline);
  }
  // Ordinary JNI methods need a NativeBridgeGetTrampoline2 per-shorty PCS
  // repacker. Returning null is an explicit capability failure, never a raw
  // Android function pointer or Darwin global-symbol fallback.
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* pointer) {
  // The only current regular-JNI trampoline registry is created by the exact
  // hash-identified fixture graph; ProxyRegisterNatives rejects every generic
  // graph. This global mask is non-concurrent acceptance instrumentation, not
  // product ownership state. Supporting simultaneous fixture loads requires a
  // per-generation owner lookup instead of extending this mask.
  const uint32_t entry_mask =
      darwin_art::android_jni::TrampolineEntryMask(pointer);
  const uint32_t observed =
      g_elf_classified_trampoline_mask.fetch_or(entry_mask,
                                                std::memory_order_relaxed) |
      entry_mask;
  if (observed == kFixtureAllEntryMask) {
    g_elf_fixture_status.fetch_or(kElfClassifiedTrampolines,
                                  std::memory_order_relaxed);
  }
  return entry_mask != 0;
}

void* OpenNativeLibrary(JNIEnv* env,
                        int32_t,
                        const char* path,
                        jobject,
                        const char*,
                        jstring,
                        bool* needs_native_bridge,
                        char** error_msg) {
  if (needs_native_bridge != nullptr) {
    *needs_native_bridge = false;
  }
  const char* providers[] = {kDarwinArtElfJniHostProviderSoname, "libc.so",
                             "libdl.so", "liblog.so", "libm.so"};
  std::array<char, 1024> discovery_error_storage{};
  DarwinArtElfErrorBuffer discovery_error{discovery_error_storage.data(),
                                           discovery_error_storage.size(), 0};
  DarwinArtElfDiscoveredGraph* discovered_raw = nullptr;
  DarwinArtElfStatus discovery_status = DARWIN_ART_ELF_INVALID_ARGUMENT;
  int root_is_elf = 0;
  std::string parent_path;
  std::vector<uint8_t> root_component;
  ScopedFd trusted_directory;
  if (SplitTrustedLibraryPath(path, &parent_path, &root_component)) {
    // The native-loader caller selects this parent directory as trusted host
    // authority. O_NOFOLLOW protects its final component only; this does not
    // claim that its ancestors form a symlink-proof sandbox. The authority
    // must also prevent concurrent mutation of authorized regular files.
    ScopedFd opened_directory(open(parent_path.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                       O_NOFOLLOW));
    if (opened_directory.get() < 0) {
      // Preserve dyld handling for non-ELF paths whose parent cannot be
      // delegated as a trusted directory. dyld cannot execute Android ELF.
      discovery_status = DARWIN_ART_ELF_IO;
    } else {
      discovery_status = darwin_art_elf_discover_sibling_graph(
          opened_directory.get(), root_component.data(), root_component.size(),
          providers, std::size(providers), &root_is_elf, &discovered_raw,
          &discovery_error);
      if (discovery_status == DARWIN_ART_ELF_OK) {
        trusted_directory = std::move(opened_directory);
      }
    }
  }
  std::unique_ptr<DarwinArtElfDiscoveredGraph, DiscoveredGraphDeleter> discovered(
      discovered_raw);
  if (discovery_status == DARWIN_ART_ELF_OK) {
    if (env == nullptr) {
      SetNativeLoaderError(error_msg, "Android ELF load requires a live JNIEnv");
      return nullptr;
    }
    const char* root_soname = nullptr;
    const DarwinArtElfGraphSource* sources = nullptr;
    size_t source_count = 0;
    DarwinArtElfStatus status = darwin_art_elf_discovered_graph_root_soname(
        discovered.get(), &root_soname, &discovery_error);
    if (status == DARWIN_ART_ELF_OK) {
      status = darwin_art_elf_discovered_graph_sources(
          discovered.get(), &sources, &source_count, &discovery_error);
    }
    if (status != DARWIN_ART_ELF_OK || root_soname == nullptr || sources == nullptr ||
        source_count == 0) {
      SetNativeLoaderError(error_msg,
                           "Android ELF discovery produced an invalid graph view");
      return nullptr;
    }
    std::string error;
    auto library = std::make_unique<ElfLibrary>();
    library->fixture_graph =
        IsExactFixtureGraph(root_soname, sources, source_count);
    if (library->fixture_graph) {
      g_elf_fixture_lifecycle.store(0, std::memory_order_relaxed);
      g_elf_fixture_provider_routes.store(0, std::memory_order_relaxed);
      g_elf_fixture_namespace_lifecycle.store(0, std::memory_order_relaxed);
    }
    if (darwin_art_bionic_rust_provider_closure_anchor() == 0) {
      SetNativeLoaderError(error_msg, "Bionic Rust provider closure is unavailable");
      return nullptr;
    }
    library->dso_lifecycle =
        darwin_art_bionic_dso_lifecycle_owner_create();
    if (library->dso_lifecycle == nullptr) {
      SetNativeLoaderError(error_msg,
                           "Bionic DSO lifecycle activation failed");
      return nullptr;
    }
    library->provider_namespace = darwin_art_bionic_namespace_create();
    if (library->provider_namespace == nullptr) {
      SetNativeLoaderError(error_msg, "Bionic provider namespace allocation failed");
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(1, std::memory_order_relaxed);
    }
    DarwinArtBionicNamespaceStatus namespace_status =
        darwin_art_bionic_namespace_bind_builtins(
            library->provider_namespace, nullptr);
    if (namespace_status == DARWIN_ART_BIONIC_NAMESPACE_OK) {
      namespace_status =
          darwin_art_bionic_namespace_seal(library->provider_namespace);
    }
    if (namespace_status != DARWIN_ART_BIONIC_NAMESPACE_OK) {
      SetNativeLoaderError(
          error_msg,
          std::string("Bionic provider namespace setup failed: ") +
              darwin_art_bionic_namespace_status_name(namespace_status));
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(2, std::memory_order_relaxed);
    }
    if (!AcquireFilesystemOwner(trusted_directory.get(), &error)) {
      SetNativeLoaderError(error_msg,
                           "Bionic filesystem setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    library->filesystem_owner = true;
    DarwinArtElfLoadOptions options{
        DARWIN_ART_ELF_ABI_VERSION, &ResolveRuntimeProvider, library.get()};
    DarwinArtElfLifecycleCallbacks lifecycle{
        DARWIN_ART_ELF_ABI_VERSION,
        &darwin_art_bionic_dso_lifecycle_publish_image,
        &darwin_art_bionic_dso_lifecycle_finalize_image,
        library->dso_lifecycle};
    std::array<char, 1024> error_storage{};
    DarwinArtElfErrorBuffer error_buffer{error_storage.data(), error_storage.size(), 0};
    status = darwin_art_elf_graph_load_with_lifecycle(
        root_soname, sources, source_count, providers,
        std::size(providers), &options, &lifecycle, &library->graph,
        &error_buffer);
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg,
                           "Android ELF graph load failed: " + ElfError(status, error_buffer));
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph &&
        g_elf_fixture_provider_routes.load(std::memory_order_relaxed) !=
            kFixtureAllProviderRouteMask) {
      SetNativeLoaderError(error_msg,
                           "Android ELF did not route all Bionic provider imports");
      if (darwin_art_elf_graph_unload(&library->graph, nullptr) ==
          DARWIN_ART_ELF_OK) {
        if (library->fixture_graph) {
          g_elf_fixture_namespace_lifecycle.store(4,
                                                  std::memory_order_relaxed);
        }
      }
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(3, std::memory_order_relaxed);
    }
    DarwinArtJniBackend backend{library.get(), &ProxyFindClass, &ProxyRegisterNatives,
                                &ProxyThrowNew};
    library->proxy = darwin_art_jni_proxy_init(
        library->proxy_storage.data(), library->proxy_storage.size(), &backend);
    if (library->proxy == nullptr ||
        !LookupOptionalElfSymbol(library.get(), "JNI_OnLoad", &library->jni_on_load,
                                &error) ||
        !LookupOptionalElfSymbol(library.get(), "JNI_OnUnload", &library->jni_on_unload,
                                &error) ||
        (library->fixture_graph &&
         (library->jni_on_load == 0 || library->jni_on_unload == 0))) {
      SetNativeLoaderError(error_msg,
                           library->proxy == nullptr ? "Android JNI proxy initialization failed"
                                                     : "Android ELF lifecycle preflight failed: " + error);
      if (darwin_art_elf_graph_unload(&library->graph, nullptr) ==
          DARWIN_ART_ELF_OK) {
        if (library->fixture_graph) {
          g_elf_fixture_namespace_lifecycle.store(4,
                                                  std::memory_order_relaxed);
        }
      }
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (needs_native_bridge == nullptr) {
      SetNativeLoaderError(error_msg, "Android ELF load requires needs_native_bridge ownership");
      if (darwin_art_elf_graph_unload(&library->graph, nullptr) ==
          DARWIN_ART_ELF_OK) {
        if (library->fixture_graph) {
          g_elf_fixture_namespace_lifecycle.store(4,
                                                  std::memory_order_relaxed);
        }
      }
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    *needs_native_bridge = true;
    if (library->fixture_graph) {
      g_elf_classified_trampoline_mask.store(0, std::memory_order_relaxed);
      g_elf_fixture_status.store(kElfOpened | kElfBionicProvidersRouted,
                                 std::memory_order_relaxed);
    }
    return library.release();
  }

  if (path != nullptr && root_is_elf != 0) {
    SetNativeLoaderError(error_msg,
                         "Android ELF discovery failed: " +
                             ElfError(discovery_status, discovery_error));
    return nullptr;
  }

  void* handle = path == nullptr ? nullptr : dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr && error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library load failed" : message);
  }
  return handle;
}

bool CloseNativeLibrary(void* handle, bool needs_native_bridge, char** error_msg) {
  if (needs_native_bridge) {
    ElfLibrary* library = AsElfLibrary(handle);
    if (library == nullptr) {
      SetNativeLoaderError(error_msg, "invalid Android ELF NativeBridge handle");
      return false;
    }
    darwin_art::android_jni::DestroyRegularTrampolines(library->trampolines);
    library->trampolines = nullptr;
    std::array<char, 1024> storage{};
    DarwinArtElfErrorBuffer error_buffer{storage.data(), storage.size(), 0};
    const DarwinArtElfStatus status =
        darwin_art_elf_graph_unload(&library->graph, &error_buffer);
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg, "Android ELF unload failed: " + ElfError(status, error_buffer));
      return false;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(4, std::memory_order_relaxed);
    }
    darwin_art_bionic_dso_lifecycle_owner_destroy(library->dso_lifecycle);
    library->dso_lifecycle = nullptr;
    if (library->filesystem_owner) {
      ReleaseFilesystemOwner();
      library->filesystem_owner = false;
    }
    const DarwinArtBionicNamespaceStatus namespace_status =
        darwin_art_bionic_namespace_teardown(library->provider_namespace);
    if (namespace_status != DARWIN_ART_BIONIC_NAMESPACE_OK) {
      SetNativeLoaderError(
          error_msg,
          std::string("Bionic provider namespace teardown failed: ") +
              darwin_art_bionic_namespace_status_name(namespace_status));
      return false;
    }
    darwin_art_bionic_namespace_destroy(library->provider_namespace);
    library->provider_namespace = nullptr;
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(5, std::memory_order_relaxed);
    }
    library->magic = 0;
    delete library;
    return true;
  }
  if (handle == nullptr || dlclose(handle) == 0) {
    return true;
  }
  if (error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library close failed" : message);
  }
  return false;
}

void NativeLoaderFreeErrorMessage(char* message) {
  std::free(message);
}

void ResetNativeLoader() {}

}  // extern "C"
}  // namespace android

extern "C" int darwin_art_elf_jni_fixture_registration_status() {
  return android::g_elf_fixture_status.load(std::memory_order_relaxed);
}

extern "C" int darwin_art_elf_jni_fixture_lifecycle_status() {
  return android::g_elf_fixture_lifecycle.load(std::memory_order_relaxed);
}

extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status() {
  return android::g_elf_fixture_namespace_lifecycle.load(
      std::memory_order_relaxed);
}

extern "C" palette_status_t PaletteSchedGetPriority(int32_t, int32_t* java_priority) {
  if (java_priority == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *java_priority = 5;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteSchedSetPriority(int32_t, int32_t) {
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteWriteCrashThreadStacks(const char*, size_t) {
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteDebugStoreGetString(char* result, size_t max_size) {
  if (result != nullptr && max_size != 0) {
    result[0] = '\0';
  }
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteTraceEnabled(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteTraceBegin(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceEnd() { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceIntegerValue(const char*, int32_t) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyDexFileLoaded(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteNotifyOatFileLoaded(const char*) { return PALETTE_STATUS_OK; }

extern "C" palette_status_t PaletteShouldReportJniInvocations(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteNotifyBeginJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyEndJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}

extern "C" void* __hwasan_tag_pointer(const volatile void* pointer, unsigned char) {
  return const_cast<void*>(pointer);
}
extern "C" void __hwasan_handle_longjmp(const void*) {}

namespace art {
namespace hprof {
void DumpHeap(const char*, int, bool) {}
}  // namespace hprof

std::string RuntimeImage::GetRuntimeImagePath(const std::string&) { return {}; }

bool RuntimeImage::WriteImageToDisk(std::string* error_msg) {
  if (error_msg != nullptr) {
    *error_msg = "runtime images are not supported on Darwin";
  }
  return false;
}

namespace odrefresh {
bool UploadStatsIfAvailable(std::string*) { return true; }
}  // namespace odrefresh

std::ostream& operator<<(std::ostream& stream, const Intrinsics& intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kNone:
      return stream << "None";
#define PRINT_INTRINSIC(Name, ...) case Intrinsics::k##Name: return stream << #Name;
    ART_INTRINSICS_LIST(PRINT_INTRINSIC)
#undef PRINT_INTRINSIC
  }
  return stream << "Intrinsics[" << static_cast<int>(intrinsic) << "]";
}
}  // namespace art

extern "C" void SkipAddSignalHandler(bool) {}

namespace unwindstack {
bool AndroidLocalUnwinder::InternalInitialize(ErrorData&) {
  return false;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData&) {
  return false;
}
}  // namespace unwindstack
