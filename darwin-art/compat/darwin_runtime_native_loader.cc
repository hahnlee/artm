#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "darwin_provider_owners.h"
#include "darwin_art_bionic_builtin_adapters.h"
#include "darwin_runtime_adapters_internal.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"

namespace android {
namespace {

extern "C" uintptr_t darwin_art_bionic_rust_provider_closure_anchor();

void SetNativeLoaderError(char** error_msg, const std::string& message) {
  if (error_msg != nullptr) *error_msg = strdup(message.c_str());
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
  *parent = slash == std::string::npos ? "." :
            slash == 0 ? "/" : bytes.substr(0, slash);
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
  ~ScopedFd() { if (fd_ >= 0) close(fd_); }
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

bool AttachNativeOwner(ElfLibrary* library,
                       uint32_t order,
                       void* value,
                       RuntimeNativeOwnerDropFn drop_fn,
                       std::string* error) {
  if (library == nullptr || library->native_owner == nullptr || value == nullptr ||
      darwin_art_runtime_native_owner_attach(library->native_owner, order, value,
                                              library, drop_fn) != 0) {
    if (value != nullptr && drop_fn != nullptr) {
      (void)drop_fn(value, library);
    }
    if (error != nullptr) *error = "Rust native owner rejected resource slot";
    return false;
  }
  return true;
}

}  // namespace

ElfLibrary* AsElfLibrary(void* handle) {
  auto* library = static_cast<ElfLibrary*>(
      darwin_art_runtime_native_owner_lookup(
          static_cast<RuntimeNativeOwner*>(handle), kNativeOwnerGraphHandle));
  return library != nullptr && library->magic == kElfLibraryMagic ? library : nullptr;
}

extern "C" void* OpenNativeLibrary(JNIEnv* env,
                                    int32_t,
                                    const char* path,
                                    jobject,
                                    const char*,
                                    jstring,
                                    bool* needs_native_bridge,
                                    char** error_msg) {
  if (needs_native_bridge != nullptr) *needs_native_bridge = false;
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
    ScopedFd opened_directory(open(parent_path.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                       O_NOFOLLOW));
    if (opened_directory.get() < 0) {
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
    library->native_owner = darwin_art_runtime_native_owner_create();
    if (library->native_owner == nullptr) {
      SetNativeLoaderError(error_msg, "Rust native owner allocation failed");
      return nullptr;
    }
    library->fixture_graph = IsExactFixtureGraph(root_soname, sources, source_count);
    if (library->fixture_graph) {
      g_elf_fixture_lifecycle.store(0, std::memory_order_relaxed);
      g_elf_fixture_provider_routes.store(0, std::memory_order_relaxed);
      g_elf_fixture_namespace_lifecycle.store(0, std::memory_order_relaxed);
    }
    if (darwin_art_bionic_rust_provider_closure_anchor() == 0) {
      SetNativeLoaderError(error_msg, "Bionic Rust provider closure is unavailable");
      return nullptr;
    }
    library->dso_lifecycle = darwin_art_bionic_dso_lifecycle_owner_create();
    if (library->dso_lifecycle == nullptr) {
      SetNativeLoaderError(error_msg, "Bionic DSO lifecycle activation failed");
      return nullptr;
    }
    if (!AttachNativeOwner(library.get(), kNativeOwnerDso, library->dso_lifecycle,
                           &DropRuntimeDsoLifecycle, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner DSO slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    library->image_registry = darwin_art_image_registry::Create(
        root_soname, sources, source_count, providers, std::size(providers), &error);
    if (library->image_registry == nullptr) {
      SetNativeLoaderError(error_msg,
                           "Android ELF image registry setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(library.get(), kNativeOwnerImageRegistry,
                           library->image_registry, &DropRuntimeElfImageRegistry,
                           &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner image slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    library->provider_namespace = darwin_art_bionic_namespace_create();
    if (library->provider_namespace == nullptr) {
      SetNativeLoaderError(error_msg, "Bionic provider namespace allocation failed");
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(library.get(), kNativeOwnerNamespace,
                           library->provider_namespace, &DropRuntimeProviderNamespace,
                           &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner namespace slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(1, std::memory_order_relaxed);
    }
    DarwinArtBionicNamespaceStatus namespace_status =
        darwin_art_bionic_namespace_bind_builtins(library->provider_namespace, nullptr);
    if (namespace_status == DARWIN_ART_BIONIC_NAMESPACE_OK) {
      namespace_status = darwin_art_bionic_namespace_seal(library->provider_namespace);
    }
    if (namespace_status != DARWIN_ART_BIONIC_NAMESPACE_OK) {
      SetNativeLoaderError(error_msg,
                           std::string("Bionic provider namespace setup failed: ") +
                               darwin_art_bionic_namespace_status_name(namespace_status));
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) {
      g_elf_fixture_namespace_lifecycle.store(2, std::memory_order_relaxed);
    }
    if (!darwin_art::providers::acquire_filesystem(trusted_directory.get(), &error)) {
      SetNativeLoaderError(error_msg, "Bionic filesystem setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerFilesystem,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Filesystem)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner filesystem slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!darwin_art::providers::acquire_sendfile(&error)) {
      SetNativeLoaderError(error_msg, "Bionic sendfile setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerSendfile,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Sendfile)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner sendfile slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!darwin_art::providers::acquire_ioctl(&error)) {
      SetNativeLoaderError(error_msg, "Bionic ioctl setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerIoctl,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Ioctl)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner ioctl slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!darwin_art::providers::acquire_strftime(&error)) {
      SetNativeLoaderError(error_msg, "Bionic strftime setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerStrftime,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Strftime)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner strftime slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!darwin_art::providers::acquire_stdio(&error)) {
      SetNativeLoaderError(error_msg, "Bionic stdio setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerStdio,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Stdio)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner stdio slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!darwin_art::providers::acquire_network(&error)) {
      SetNativeLoaderError(error_msg, "Bionic network setup failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(
            library.get(), kNativeOwnerNetwork,
            reinterpret_cast<void*>(static_cast<uintptr_t>(
                darwin_art::providers::Kind::Network)),
            &DropRuntimeProviderKind, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner network slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION,
                                    &ResolveRuntimeProvider, library.get()};
    DarwinArtElfLifecycleCallbacks lifecycle{DARWIN_ART_ELF_ABI_VERSION,
                                             &PublishRuntimeElfImage,
                                             &FinalizeRuntimeElfImage,
                                             library.get()};
    std::array<char, 1024> error_storage{};
    DarwinArtElfErrorBuffer error_buffer{error_storage.data(), error_storage.size(), 0};
    status = darwin_art_elf_graph_load_with_lifecycle(
        root_soname, sources, source_count, providers, std::size(providers),
        &options, &lifecycle, &library->graph, &error_buffer);
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg,
                           "Android ELF graph load failed: " + ElfError(status, error_buffer));
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (!AttachNativeOwner(library.get(), kNativeOwnerGraph, library->graph,
                           &DropRuntimeElfGraph, &error)) {
      SetNativeLoaderError(error_msg, "Rust native owner graph slot failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph &&
        g_elf_fixture_provider_routes.load(std::memory_order_relaxed) !=
            kFixtureAllProviderRouteMask) {
      SetNativeLoaderError(error_msg, "Android ELF did not route all Bionic provider imports");
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (library->fixture_graph) g_elf_fixture_namespace_lifecycle.store(3, std::memory_order_relaxed);
    DarwinArtJniBackend backend{library.get(), &ProxyCurrentEnv, &ProxyFindClass,
                                &ProxyRegisterNatives, &ProxyThrowNew};
    library->proxy = darwin_art_jni_proxy_init(
        library->proxy_storage.data(), library->proxy_storage.size(), &backend);
    if (library->proxy == nullptr ||
        !LookupOptionalElfSymbol(library.get(), "JNI_OnLoad", &library->jni_on_load, &error) ||
        !LookupOptionalElfSymbol(library.get(), "JNI_OnUnload", &library->jni_on_unload, &error) ||
        (library->fixture_graph && (library->jni_on_load == 0 || library->jni_on_unload == 0))) {
      SetNativeLoaderError(error_msg,
                           library->proxy == nullptr ? "Android JNI proxy initialization failed"
                                                     : "Android ELF lifecycle preflight failed: " + error);
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    if (needs_native_bridge == nullptr) {
      SetNativeLoaderError(error_msg, "Android ELF load requires needs_native_bridge ownership");
      TeardownProviderNamespace(library.get());
      return nullptr;
    }
    *needs_native_bridge = true;
    if (library->fixture_graph) {
      g_elf_classified_trampoline_mask.store(0, std::memory_order_relaxed);
      g_elf_fixture_status.store(kElfOpened | kElfBionicProvidersRouted,
                                 std::memory_order_relaxed);
    }
    ElfLibrary* library_value = library.release();
    RuntimeNativeOwner* graph_handle =
        darwin_art_runtime_native_owner_create();
    if (graph_handle == nullptr ||
        darwin_art_runtime_native_owner_attach(
            graph_handle, kNativeOwnerGraphHandle, library_value, nullptr,
            &DropRuntimeElfLibrary) != 0) {
      if (graph_handle != nullptr) {
        (void)darwin_art_runtime_native_owner_destroy(graph_handle);
      }
      (void)DropRuntimeElfLibrary(library_value, nullptr);
      SetNativeLoaderError(error_msg, "Rust graph handle publication failed");
      return nullptr;
    }
    return graph_handle;
  }
  if (path != nullptr && root_is_elf != 0) {
    SetNativeLoaderError(error_msg, "Android ELF discovery failed: " +
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

extern "C" bool CloseNativeLibrary(void* handle,
                                    bool needs_native_bridge,
                                    char** error_msg) {
  if (needs_native_bridge) {
    ElfLibrary* library = AsElfLibrary(handle);
    if (library == nullptr) {
      SetNativeLoaderError(error_msg, "invalid Android ELF NativeBridge handle");
      return false;
    }
    darwin_art::android_jni::DestroyRegularTrampolines(library->trampolines);
    library->trampolines = nullptr;
    const int status = darwin_art_runtime_native_owner_destroy(
        static_cast<RuntimeNativeOwner*>(handle));
    if (status != 0) {
      SetNativeLoaderError(error_msg, "Rust graph owner teardown failed");
      return false;
    }
    return true;
  }
  if (handle == nullptr || dlclose(handle) == 0) return true;
  if (error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library close failed" : message);
  }
  return false;
}

}  // namespace android
