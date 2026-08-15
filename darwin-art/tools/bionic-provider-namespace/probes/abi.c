#include "darwin_art_bionic_builtin_adapters.h"
#include "darwin_art_bionic_provider_namespace.h"

_Static_assert(DARWIN_ART_BIONIC_PROVIDER_COUNT == 28, "provider count");
_Static_assert(sizeof(uintptr_t) == sizeof(void *), "address width");

int main(void) {
  DarwinArtBionicNamespaceResult result = {
      DARWIN_ART_BIONIC_NAMESPACE_OK,
      DARWIN_ART_BIONIC_PROVIDER_LEAF,
      1,
  };
  return result.address == 1 ? 0 : 1;
}
