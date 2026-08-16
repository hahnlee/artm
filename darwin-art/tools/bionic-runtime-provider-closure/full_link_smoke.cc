#include "darwin_art_bionic_builtin_adapters.h"
#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_fs.h"
#include "darwin_art_bionic_socket_broker.h"

#include <android/log.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uintptr_t darwin_art_bionic_rust_provider_closure_anchor(void);
extern "C" int darwin_art_bionic_syslog_activate(const char *guest_program_tag);
extern "C" void darwin_art_bionic_syslog(int priority, const char *format, ...);
extern "C" int darwin_art_bionic_stdio_process_install(void);
extern "C" int darwin_art_bionic_stdio_process_uninstall(void);
extern "C" uint32_t darwin_art_bionic_getwc(void *file);
extern "C" uint32_t darwin_art_bionic_ungetwc(uint32_t wc, void *file);
extern "C" unsigned char darwin_art_bionic___sF[456];

namespace {

struct Expected {
  const char *soname;
  const char *symbol;
  const char *version;
  DarwinArtBionicProviderId owner;
};

constexpr Expected kExpected[] = {
#include "ownership.inc"
};

int captured_priority = 0;
char captured_tag[32]{};
char captured_message[32]{};

void Capture(const __android_log_message *message) {
  if (message == nullptr || message->tag == nullptr ||
      message->message == nullptr) {
    return;
  }
  captured_priority = message->priority;
  std::snprintf(captured_tag, sizeof(captured_tag), "%s", message->tag);
  std::snprintf(captured_message, sizeof(captured_message), "%s",
                message->message);
}

} // namespace

int main() {
  if (darwin_art_bionic_rust_provider_closure_anchor() == 0)
    return 10;
  char root_path[] = "/tmp/darwin-art-provider-closure.XXXXXX";
  if (mkdtemp(root_path) == nullptr)
    return 18;
  const int root_fd = open(root_path, O_RDONLY | O_DIRECTORY);
  const int fixture_fd =
      root_fd < 0 ? -1 : openat(root_fd, "fixture", O_CREAT | O_WRONLY, 0600);
  if (fixture_fd < 0 || write(fixture_fd, "x", 1) != 1 ||
      close(fixture_fd) != 0) {
    return 19;
  }
  static constexpr uint8_t kRoot[] = {'/'};
  if (darwin_art_bionic_fs_process_install(root_fd, kRoot, sizeof(kRoot), kRoot,
                                           sizeof(kRoot)) !=
          DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK ||
      darwin_art_bionic_socket_broker_activate() != 0) {
    return 20;
  }
  char guest_program_tag[] = "GuestProgram";
  if (darwin_art_bionic_syslog_activate(guest_program_tag) != 0 ||
      darwin_art_bionic_syslog_activate("SecondProgram") != -1) {
    return 11;
  }
  guest_program_tag[0] = 'X';
  __android_log_set_logger(Capture);
  darwin_art_bionic_syslog(6, "owned-copy");
  __android_log_set_logger(__android_log_stderr_logger);
  if (captured_priority != ANDROID_LOG_INFO ||
      std::strcmp(captured_tag, "GuestProgram") != 0 ||
      std::strcmp(captured_message, "owned-copy") != 0) {
    return 12;
  }
  if (darwin_art_bionic_stdio_process_install() != 0 ||
      darwin_art_bionic_ungetwc('Q', darwin_art_bionic___sF) != 'Q' ||
      darwin_art_bionic_getwc(darwin_art_bionic___sF) != 'Q') {
    return 16;
  }
  DarwinArtBionicNamespace *instance = darwin_art_bionic_namespace_create();
  if (instance == nullptr ||
      darwin_art_bionic_namespace_bind_builtins(instance, nullptr) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK ||
      darwin_art_bionic_namespace_seal(instance) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 13;
  }
  for (const Expected &expected : kExpected) {
    const auto result = darwin_art_bionic_namespace_resolve(
        instance, expected.soname, expected.symbol,
        expected.version[0] == '\0' ? nullptr : expected.version);
    if (result.status != DARWIN_ART_BIONIC_NAMESPACE_OK ||
        result.owner != expected.owner || result.address == 0) {
      std::fprintf(stderr,
                   "bionic-runtime-provider-closure: route failed "
                   "%s:%s@%s status=%s owner=%s\n",
                   expected.soname, expected.symbol, expected.version,
                   darwin_art_bionic_namespace_status_name(result.status),
                   darwin_art_bionic_provider_name(result.owner));
      return 14;
    }
  }
  const auto open_route =
      darwin_art_bionic_namespace_resolve(instance, "libc.so", "open", "LIBC");
  const auto close_route =
      darwin_art_bionic_namespace_resolve(instance, "libc.so", "close", "LIBC");
  using GuestOpen = int (*)(const char *, int, uint32_t);
  using GuestClose = int (*)(int);
  const int guest_fd =
      reinterpret_cast<GuestOpen>(open_route.address)("/fixture", 0, 0);
  if (open_route.owner != DARWIN_ART_BIONIC_PROVIDER_FILESYSTEM ||
      close_route.owner != DARWIN_ART_BIONIC_PROVIDER_CENTRAL_FD_BROKER ||
      guest_fd < 0 ||
      reinterpret_cast<GuestClose>(close_route.address)(guest_fd) != 0) {
    return 21;
  }
  if (darwin_art_bionic_namespace_teardown(instance) !=
      DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 15;
  }
  darwin_art_bionic_namespace_destroy(instance);
  if (darwin_art_bionic_socket_broker_live_objects() != 0 ||
      darwin_art_bionic_socket_broker_deactivate() != 0 ||
      darwin_art_bionic_stdio_process_uninstall() != 0 ||
      darwin_art_bionic_fs_process_uninstall() !=
          DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK) {
    return 17;
  }
  unlinkat(root_fd, "fixture", 0);
  close(root_fd);
  rmdir(root_path);
  std::fprintf(stderr, "bionic-runtime-provider-closure: PASS bind_builtins=32 "
                       "routes=185 actual-resolvers=yes close=broker-or-fs "
                       "wide-stdio=central-lease syslog-tag=owned-copy\n");
  return 0;
}
