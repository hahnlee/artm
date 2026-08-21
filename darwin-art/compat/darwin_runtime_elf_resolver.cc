#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

#include "darwin_runtime_adapters_internal.h"

namespace android {
namespace {

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

}  // namespace

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
  if (provider_soname == nullptr &&
      std::strcmp(request->symbol, "__cxa_thread_atexit_impl") == 0) {
    return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
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
  } else if (std::strcmp(request->symbol, "sscanf") == 0) {
    route = kFixtureScanfRouteMask;
  } else if (std::strcmp(request->symbol, "vsscanf") == 0) {
    route = kFixtureVsscanfRouteMask;
  } else if (std::strcmp(request->symbol, "swprintf") == 0) {
    route = kFixtureSwprintfRouteMask;
  } else if (std::strcmp(request->symbol, "ioctl") == 0) {
    route = kFixtureIoctlRouteMask;
  } else if (std::strcmp(request->symbol, "strftime_l") == 0) {
    route = kFixtureStrftimeRouteMask;
  } else if (std::strcmp(request->symbol, "sendfile") == 0) {
    route = kFixtureSendfileRouteMask;
  }
  if (route != 0 && library->fixture_graph) {
    g_elf_fixture_provider_routes.fetch_or(route, std::memory_order_relaxed);
  }
  *out_address = result.address;
  return DARWIN_ART_ELF_RESOLVE_FOUND;
}

}  // namespace android
