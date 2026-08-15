#include "darwin_art_bionic_builtin_adapters.h"

#include <cstdint>
#include <cstdio>

extern "C" uintptr_t darwin_art_bionic_rust_provider_closure_anchor(void);

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

}  // namespace

int main() {
  if (darwin_art_bionic_rust_provider_closure_anchor() == 0) return 10;
  DarwinArtBionicNamespace* instance = darwin_art_bionic_namespace_create();
  if (instance == nullptr ||
      darwin_art_bionic_namespace_bind_builtins(instance, nullptr) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK ||
      darwin_art_bionic_namespace_seal(instance) !=
          DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 11;
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
      return 12;
    }
  }
  if (darwin_art_bionic_namespace_teardown(instance) !=
      DARWIN_ART_BIONIC_NAMESPACE_OK) {
    return 13;
  }
  darwin_art_bionic_namespace_destroy(instance);
  std::fprintf(stderr,
               "bionic-runtime-provider-closure: PASS bind_builtins=17 "
               "routes=155 actual-resolvers=yes\n");
  return 0;
}
