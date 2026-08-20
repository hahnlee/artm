#include "darwin_provider_owners.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_ioctl.h"
#include "darwin_art_bionic_sendfile.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_art_bionic_stdio.h"
#include "darwin_art_bionic_strftime.h"

extern "C" DarwinArtBionicSendfileTransferStatus
darwin_art_bionic_fs_sendfile_transfer(
    void* context, const DarwinArtBionicSendfileRequest* request,
    DarwinArtBionicSendfileResult* result);

namespace darwin_art::providers {
namespace {

struct FilesystemOwnerState {
  std::mutex mutex;
  size_t users = 0;
  dev_t device = 0;
  ino_t inode = 0;
};

FilesystemOwnerState g_filesystem_owner;

struct NetworkOwnerState {
  std::mutex mutex;
  size_t users = 0;
};

NetworkOwnerState g_network_owner;

struct SingletonOwnerState {
  std::mutex mutex;
  size_t users = 0;
};

SingletonOwnerState g_ioctl_owner;
SingletonOwnerState g_strftime_owner;
SingletonOwnerState g_sendfile_owner;

struct HookState {
  std::mutex mutex;
  void* context = nullptr;
  AcquireHook acquire = nullptr;
  ReleaseHook release = nullptr;
};

HookState g_hooks;

struct HookSnapshot {
  void* context;
  AcquireHook acquire;
  ReleaseHook release;
};

HookSnapshot hooks() {
  std::lock_guard<std::mutex> lock(g_hooks.mutex);
  return {g_hooks.context, g_hooks.acquire, g_hooks.release};
}

}  // namespace

bool acquire_filesystem_direct(int directory_fd, std::string* error) {
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
  const DarwinArtBionicFsProcessOwnerStatus status_code =
      darwin_art_bionic_fs_process_install(
          directory_fd, kGuestRoot, sizeof(kGuestRoot), kGuestRoot,
          sizeof(kGuestRoot));
  if (status_code != DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK) {
    *error = "Bionic filesystem process-owner install failed: " +
             std::to_string(static_cast<int>(status_code));
    return false;
  }
  g_filesystem_owner.users = 1;
  g_filesystem_owner.device = status.st_dev;
  g_filesystem_owner.inode = status.st_ino;
  return true;
}

void release_filesystem_direct() {
  std::lock_guard<std::mutex> lock(g_filesystem_owner.mutex);
  if (g_filesystem_owner.users == 0) std::abort();
  --g_filesystem_owner.users;
  if (g_filesystem_owner.users != 0) return;
  if (darwin_art_bionic_fs_process_uninstall() !=
      DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK) {
    std::abort();
  }
  g_filesystem_owner.device = 0;
  g_filesystem_owner.inode = 0;
}

bool acquire_network_direct(std::string* error) {
  std::lock_guard<std::mutex> lock(g_network_owner.mutex);
  if (g_network_owner.users != 0) {
    if (g_network_owner.users == std::numeric_limits<size_t>::max()) {
      *error = "Bionic network graph-owner count overflow";
      return false;
    }
    ++g_network_owner.users;
    return true;
  }
  if (darwin_art_bionic_socket_broker_activate() != 0) {
    *error = "Bionic socket broker activation failed";
    return false;
  }
  g_network_owner.users = 1;
  return true;
}

void release_network_direct() {
  std::lock_guard<std::mutex> lock(g_network_owner.mutex);
  if (g_network_owner.users == 0) std::abort();
  --g_network_owner.users;
  if (g_network_owner.users != 0) return;
  if (darwin_art_bionic_socket_broker_live_objects() != 0 ||
      darwin_art_bionic_socket_broker_deactivate() != 0) {
    std::abort();
  }
}

bool acquire_stdio_direct(std::string* error) {
  if (darwin_art_bionic_stdio_process_install() == 0) return true;
  *error = "Bionic stdio process-owner install failed";
  return false;
}

void release_stdio_direct() {
  if (darwin_art_bionic_stdio_process_uninstall() != 0) std::abort();
}

bool acquire_ioctl_direct(std::string* error) {
  std::lock_guard<std::mutex> lock(g_ioctl_owner.mutex);
  if (g_ioctl_owner.users != 0) {
    if (g_ioctl_owner.users == std::numeric_limits<size_t>::max()) {
      *error = "Bionic ioctl graph-owner count overflow";
      return false;
    }
    ++g_ioctl_owner.users;
    return true;
  }
  const DarwinArtBionicIoctlLifecycleStatus status =
      darwin_art_bionic_ioctl_activate(
          &darwin_art_bionic_fs_ioctl_fd_lookup, nullptr);
  if (status != DARWIN_ART_BIONIC_IOCTL_LIFECYCLE_OK) {
    *error = "Bionic ioctl process-owner activation failed: " +
             std::to_string(static_cast<int>(status));
    return false;
  }
  g_ioctl_owner.users = 1;
  return true;
}

void release_ioctl_direct() {
  std::lock_guard<std::mutex> lock(g_ioctl_owner.mutex);
  if (g_ioctl_owner.users == 0) std::abort();
  --g_ioctl_owner.users;
  if (g_ioctl_owner.users != 0) return;
  if (darwin_art_bionic_ioctl_deactivate() !=
      DARWIN_ART_BIONIC_IOCTL_LIFECYCLE_OK) std::abort();
}

bool acquire_strftime_direct(std::string* error) {
  std::lock_guard<std::mutex> lock(g_strftime_owner.mutex);
  if (g_strftime_owner.users != 0) {
    if (g_strftime_owner.users == std::numeric_limits<size_t>::max()) {
      *error = "Bionic strftime graph-owner count overflow";
      return false;
    }
    ++g_strftime_owner.users;
    return true;
  }
  const DarwinArtBionicStrftimeLifecycleStatus status =
      darwin_art_bionic_strftime_activate("UTC", 0, "UTC", 0);
  if (status != DARWIN_ART_BIONIC_STRFTIME_OK) {
    *error = "Bionic strftime process-owner activation failed: " +
             std::to_string(static_cast<int>(status));
    return false;
  }
  g_strftime_owner.users = 1;
  return true;
}

void release_strftime_direct() {
  std::lock_guard<std::mutex> lock(g_strftime_owner.mutex);
  if (g_strftime_owner.users == 0) std::abort();
  --g_strftime_owner.users;
  if (g_strftime_owner.users != 0) return;
  if (darwin_art_bionic_strftime_deactivate() !=
      DARWIN_ART_BIONIC_STRFTIME_OK) std::abort();
}

bool acquire_sendfile_direct(std::string* error) {
  std::lock_guard<std::mutex> lock(g_sendfile_owner.mutex);
  if (g_sendfile_owner.users != 0) {
    if (g_sendfile_owner.users == std::numeric_limits<size_t>::max()) {
      *error = "Bionic sendfile graph-owner count overflow";
      return false;
    }
    ++g_sendfile_owner.users;
    return true;
  }
  const DarwinArtBionicSendfileLifecycleStatus status =
      darwin_art_bionic_sendfile_activate(
          &darwin_art_bionic_fs_sendfile_transfer, nullptr);
  if (status != DARWIN_ART_BIONIC_SENDFILE_LIFECYCLE_OK) {
    *error = "Bionic sendfile process-owner activation failed: " +
             std::to_string(static_cast<int>(status));
    return false;
  }
  g_sendfile_owner.users = 1;
  return true;
}

void release_sendfile_direct() {
  std::lock_guard<std::mutex> lock(g_sendfile_owner.mutex);
  if (g_sendfile_owner.users == 0) std::abort();
  --g_sendfile_owner.users;
  if (g_sendfile_owner.users != 0) return;
  if (darwin_art_bionic_sendfile_deactivate() !=
      DARWIN_ART_BIONIC_SENDFILE_LIFECYCLE_OK) std::abort();
}

bool acquire_filesystem(int directory_fd, std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status = hook.acquire(
        hook.context, static_cast<uint32_t>(Kind::Filesystem), directory_fd);
    if (status == 0) return true;
    *error = "Rust filesystem provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_filesystem_direct(directory_fd, error);
}

void release_filesystem() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Filesystem)) != 0) {
      std::abort();
    }
    return;
  }
  release_filesystem_direct();
}

bool acquire_network(std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status =
        hook.acquire(hook.context, static_cast<uint32_t>(Kind::Network), -1);
    if (status == 0) return true;
    *error = "Rust network provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_network_direct(error);
}

void release_network() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Network)) != 0) {
      std::abort();
    }
    return;
  }
  release_network_direct();
}

bool acquire_stdio(std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status =
        hook.acquire(hook.context, static_cast<uint32_t>(Kind::Stdio), -1);
    if (status == 0) return true;
    *error = "Rust stdio provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_stdio_direct(error);
}

void release_stdio() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Stdio)) != 0) {
      std::abort();
    }
    return;
  }
  release_stdio_direct();
}

bool acquire_ioctl(std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status =
        hook.acquire(hook.context, static_cast<uint32_t>(Kind::Ioctl), -1);
    if (status == 0) return true;
    *error = "Rust ioctl provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_ioctl_direct(error);
}

void release_ioctl() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Ioctl)) != 0) {
      std::abort();
    }
    return;
  }
  release_ioctl_direct();
}

bool acquire_strftime(std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status =
        hook.acquire(hook.context, static_cast<uint32_t>(Kind::Strftime), -1);
    if (status == 0) return true;
    *error = "Rust strftime provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_strftime_direct(error);
}

void release_strftime() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Strftime)) != 0) {
      std::abort();
    }
    return;
  }
  release_strftime_direct();
}

bool acquire_sendfile(std::string* error) {
  const HookSnapshot hook = hooks();
  if (hook.acquire != nullptr) {
    const int32_t status =
        hook.acquire(hook.context, static_cast<uint32_t>(Kind::Sendfile), -1);
    if (status == 0) return true;
    *error = "Rust sendfile provider owner rejected activation: " +
             std::to_string(status);
    return false;
  }
  return acquire_sendfile_direct(error);
}

void release_sendfile() {
  const HookSnapshot hook = hooks();
  if (hook.release != nullptr) {
    if (hook.release(hook.context, static_cast<uint32_t>(Kind::Sendfile)) != 0) {
      std::abort();
    }
    return;
  }
  release_sendfile_direct();
}

extern "C" void darwin_art_provider_install_hooks(
    void* context, AcquireHook acquire, ReleaseHook release) {
  std::lock_guard<std::mutex> lock(g_hooks.mutex);
  g_hooks.context = context;
  g_hooks.acquire = acquire;
  g_hooks.release = release;
}

extern "C" void darwin_art_provider_clear_hooks() {
  std::lock_guard<std::mutex> lock(g_hooks.mutex);
  g_hooks.context = nullptr;
  g_hooks.acquire = nullptr;
  g_hooks.release = nullptr;
}

extern "C" int32_t darwin_art_provider_native_acquire(uint32_t kind,
                                                       int32_t authority_fd) {
  std::string error;
  bool ok = false;
  switch (static_cast<Kind>(kind)) {
    case Kind::Filesystem:
      ok = acquire_filesystem_direct(authority_fd, &error);
      break;
    case Kind::Network:
      ok = acquire_network_direct(&error);
      break;
    case Kind::Stdio:
      ok = acquire_stdio_direct(&error);
      break;
    case Kind::Ioctl:
      ok = acquire_ioctl_direct(&error);
      break;
    case Kind::Strftime:
      ok = acquire_strftime_direct(&error);
      break;
    case Kind::Sendfile:
      ok = acquire_sendfile_direct(&error);
      break;
  }
  return ok ? 0 : -1;
}

extern "C" int32_t darwin_art_provider_native_release(uint32_t kind) {
  switch (static_cast<Kind>(kind)) {
    case Kind::Filesystem:
      release_filesystem_direct();
      break;
    case Kind::Network:
      release_network_direct();
      break;
    case Kind::Stdio:
      release_stdio_direct();
      break;
    case Kind::Ioctl:
      release_ioctl_direct();
      break;
    case Kind::Strftime:
      release_strftime_direct();
      break;
    case Kind::Sendfile:
      release_sendfile_direct();
      break;
    default:
      return -1;
  }
  return 0;
}

}  // namespace darwin_art::providers
