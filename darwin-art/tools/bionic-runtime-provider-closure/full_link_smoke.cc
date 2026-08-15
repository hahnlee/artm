#include "darwin_art_bionic_builtin_adapters.h"

#include <android/log.h>

#include <cstdint>
#include <cstdio>
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
  const char* soname;
  const char* symbol;
  const char* version;
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

}  // namespace

int main() {
  if (darwin_art_bionic_rust_provider_closure_anchor() == 0) return 10;
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
  DarwinArtBionicNamespace* instance = darwin_art_bionic_namespace_create();
  if (instance == nullptr ||
      darwin_art_bionic_namespace_bind_builtins(instance, nullptr) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK ||
      darwin_art_bionic_namespace_seal(instance) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 13;
  }
  for (const Expected& expected : kExpected) {
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
  if (darwin_art_bionic_stdio_process_uninstall() != 0) return 17;
  if (darwin_art_bionic_namespace_teardown(instance) !=
      DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 15;
  }
  darwin_art_bionic_namespace_destroy(instance);
  std::fprintf(stderr,
               "bionic-runtime-provider-closure: PASS bind_builtins=28 "
               "routes=177 actual-resolvers=yes wide-stdio=central-lease "
               "syslog-tag=owned-copy\n");
  return 0;
}
