#ifndef DARWIN_ART_BIONIC_BUILTIN_ADAPTERS_H_
#define DARWIN_ART_BIONIC_BUILTIN_ADAPTERS_H_

#include "darwin_art_bionic_provider_namespace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtBionicProviderReleaseHooks {
  void *context[DARWIN_ART_BIONIC_PROVIDER_COUNT];
  DarwinArtBionicProviderRelease release[DARWIN_ART_BIONIC_PROVIDER_COUNT];
} DarwinArtBionicProviderReleaseHooks;

/* Binds adapters for all generated provider resolver entrypoints.
 * Linking this object intentionally requires every standalone provider. */
DarwinArtBionicNamespaceStatus darwin_art_bionic_namespace_bind_builtins(
    DarwinArtBionicNamespace *namespace_instance,
    const DarwinArtBionicProviderReleaseHooks *release_hooks);

#ifdef __cplusplus
}
#endif

#endif // DARWIN_ART_BIONIC_BUILTIN_ADAPTERS_H_
