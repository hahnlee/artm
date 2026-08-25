#include <CommonCrypto/CommonDigest.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include "darwin_runtime_adapters_internal.h"
#include "darwin_angle_egl.h"

extern "C" void AndroidBitmap_getInfo(void) __attribute__((weak_import));
extern "C" void AndroidBitmap_lockPixels(void) __attribute__((weak_import));
extern "C" void AndroidBitmap_unlockPixels(void) __attribute__((weak_import));

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

bool NeedsLibrary(const DarwinArtElfSymbolRequest* request,
                  const char* soname) {
  for (size_t index = 0; index < request->needed_library_count; ++index) {
    if (request->needed_libraries[index] != nullptr &&
        std::strcmp(request->needed_libraries[index], soname) == 0) {
      return true;
    }
  }
  return false;
}

void* OpenAngleProvider(const char* filename,
                        void** slot,
                        DarwinArtElfErrorBuffer* error) {
  if (*slot != nullptr) return *slot;
  const char* directory = std::getenv("DARWIN_ART_ANGLE_DIRECTORY");
  if (directory == nullptr || directory[0] != '/') {
    SetResolverError(error,
                     "ANGLE provider requires absolute DARWIN_ART_ANGLE_DIRECTORY");
    return nullptr;
  }
  const std::string path = std::string(directory) + "/" + filename;
  *slot = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (*slot == nullptr) {
    const char* message = dlerror();
    SetResolverError(error, message == nullptr ? "ANGLE provider dlopen failed" : message);
  }
  return *slot;
}

DarwinArtElfResolveStatus ResolvePlatformProvider(
    ElfLibrary* library,
    const DarwinArtElfSymbolRequest* request,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error) {
  if (request->version_soname != nullptr || request->version_name != nullptr) {
    return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
  }
  uintptr_t result = 0;
  size_t matches = 0;
  auto consider = [&](void* handle) {
    if (handle == nullptr) return;
    dlerror();
    void* symbol = dlsym(handle, request->symbol);
    if (symbol != nullptr && dlerror() == nullptr) {
      result = reinterpret_cast<uintptr_t>(symbol);
      ++matches;
    }
  };
  auto consider_address = [&](void* symbol) {
    if (symbol != nullptr) {
      result = reinterpret_cast<uintptr_t>(symbol);
      ++matches;
    }
  };
  if (NeedsLibrary(request, "libGLESv2.so") &&
      std::strncmp(request->symbol, "gl", 2) == 0) {
    const bool debug_angle = std::getenv("DARWIN_ART_DEBUG_ANGLE") != nullptr;
    if (debug_angle && std::strcmp(request->symbol, "glTexImage2D") == 0) {
      consider_address(reinterpret_cast<void*>(&darwin_art_android_glTexImage2D));
    } else if (debug_angle &&
               std::strcmp(request->symbol, "glTexSubImage2D") == 0) {
      consider_address(
          reinterpret_cast<void*>(&darwin_art_android_glTexSubImage2D));
    } else if (debug_angle &&
               std::strcmp(request->symbol, "glDrawArrays") == 0) {
      consider_address(reinterpret_cast<void*>(&darwin_art_android_glDrawArrays));
    } else if (debug_angle &&
               std::strcmp(request->symbol, "glDrawElements") == 0) {
      consider_address(
          reinterpret_cast<void*>(&darwin_art_android_glDrawElements));
    } else if (debug_angle &&
               std::strcmp(request->symbol, "glUseProgram") == 0) {
      consider_address(reinterpret_cast<void*>(&darwin_art_android_glUseProgram));
    } else {
      consider(OpenAngleProvider("libGLESv2.dylib",
                                 &library->gles_provider, error));
    }
  }
  if (NeedsLibrary(request, "libEGL.so") &&
      std::strncmp(request->symbol, "egl", 3) == 0) {
    if (std::strcmp(request->symbol, "eglCreateWindowSurface") == 0) {
      consider_address(reinterpret_cast<void*>(
          &darwin_art_android_eglCreateWindowSurface));
    } else if (std::strcmp(request->symbol, "eglSwapBuffers") == 0) {
      consider_address(
          reinterpret_cast<void*>(&darwin_art_android_eglSwapBuffers));
    } else if (std::strcmp(request->symbol, "eglDestroySurface") == 0) {
      consider_address(
          reinterpret_cast<void*>(&darwin_art_android_eglDestroySurface));
    } else {
      consider(OpenAngleProvider("libEGL.dylib", &library->egl_provider, error));
    }
  }
  const bool zlib_symbol = std::strncmp(request->symbol, "inflate", 7) == 0 ||
                           std::strncmp(request->symbol, "deflate", 7) == 0 ||
                           std::strcmp(request->symbol, "adler32") == 0 ||
                           std::strcmp(request->symbol, "crc32") == 0 ||
                           std::strcmp(request->symbol, "compress") == 0 ||
                           std::strcmp(request->symbol, "compress2") == 0 ||
                           std::strcmp(request->symbol, "uncompress") == 0 ||
                           std::strcmp(request->symbol, "zlibVersion") == 0;
  if (NeedsLibrary(request, "libz.so") && zlib_symbol) {
    if (library->z_provider == nullptr) {
      library->z_provider = dlopen("/usr/lib/libz.1.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    consider(library->z_provider);
  }
  if (NeedsLibrary(request, "libjnigraphics.so") &&
      (std::strcmp(request->symbol, "AndroidBitmap_getInfo") == 0 ||
       std::strcmp(request->symbol, "AndroidBitmap_lockPixels") == 0 ||
       std::strcmp(request->symbol, "AndroidBitmap_unlockPixels") == 0)) {
    if (std::strcmp(request->symbol, "AndroidBitmap_getInfo") == 0) {
      consider_address(reinterpret_cast<void*>(&AndroidBitmap_getInfo));
    } else if (std::strcmp(request->symbol, "AndroidBitmap_lockPixels") == 0) {
      consider_address(reinterpret_cast<void*>(&AndroidBitmap_lockPixels));
    } else {
      consider_address(reinterpret_cast<void*>(&AndroidBitmap_unlockPixels));
    }
  }
  if (matches == 1) {
    *out_address = result;
    return DARWIN_ART_ELF_RESOLVE_FOUND;
  }
  if (matches > 1) {
    SetResolverError(error, "platform import is exported by multiple providers");
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
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
  const DarwinArtElfResolveStatus cached =
      ResolveCachedElfProvider(request, out_address, error);
  if (cached != DARWIN_ART_ELF_RESOLVE_NOT_FOUND) return cached;
  if (request->version_soname != nullptr && request->version_name != nullptr &&
      std::strcmp(request->version_soname, "libc.so") == 0 &&
      std::strcmp(request->version_name, "LIBC_R") == 0) {
    if (library->android_unwind_provider == nullptr) {
      SetResolverError(error, "Android LIBC_R provider is unavailable");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
    std::array<char, 512> lookup_storage{};
    DarwinArtElfErrorBuffer lookup_error{lookup_storage.data(),
                                          lookup_storage.size(), 0};
    const DarwinArtElfStatus status = darwin_art_elf_lookup(
        library->android_unwind_provider, request->symbol, out_address,
        &lookup_error);
    if (status == DARWIN_ART_ELF_OK) return DARWIN_ART_ELF_RESOLVE_FOUND;
    SetResolverError(error, status == DARWIN_ART_ELF_SYMBOL_NOT_FOUND
                                ? "Android LIBC_R symbol is unsupported"
                                : lookup_storage.data());
    return DARWIN_ART_ELF_RESOLVE_ERROR;
  }
  const DarwinArtElfResolveStatus platform =
      ResolvePlatformProvider(library, request, out_address, error);
  if (platform != DARWIN_ART_ELF_RESOLVE_NOT_FOUND) return platform;
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
      if (request->symbol_weak != 0) {
        return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
      }
      SetResolverError(error, "unversioned Bionic import has no exact provider");
      return DARWIN_ART_ELF_RESOLVE_ERROR;
    }
  }
  const DarwinArtBionicNamespaceResult result =
      darwin_art_bionic_namespace_resolve(
          library->provider_namespace, provider_soname, request->symbol,
          provider_version);
  if (result.status != DARWIN_ART_BIONIC_NAMESPACE_OK || result.address == 0) {
    if (request->symbol_weak != 0) {
      return DARWIN_ART_ELF_RESOLVE_NOT_FOUND;
    }
    const std::string detail =
        std::string(darwin_art_bionic_namespace_status_name(result.status)) +
        " soname=" + (provider_soname == nullptr ? "<null>" : provider_soname) +
        " symbol=" + request->symbol +
        " version=" + (provider_version == nullptr ? "<null>" : provider_version);
    SetResolverError(error, detail.c_str());
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
