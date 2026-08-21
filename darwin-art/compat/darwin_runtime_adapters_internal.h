#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "darwin_android_jni_trampoline.h"
#include "darwin_android_elf_image_registry.h"
#include "darwin_art_elf_jni_fixture_identity.h"
#include "darwin_art_elf_loader.h"
#include "darwin_art_bionic_dso_lifecycle.h"
#include "darwin_art_bionic_provider_namespace.h"
#include "darwin_art_jni_proxy.h"

namespace android {

inline constexpr uint64_t kElfLibraryMagic = UINT64_C(0x44415257454c464a);
inline constexpr int kElfOpened = 1 << 0;
inline constexpr int kElfOnLoadCalled = 1 << 1;
inline constexpr int kElfFoundFixtureClass = 1 << 2;
inline constexpr int kElfCapturedRegistration = 1 << 3;
inline constexpr int kElfInstalledRegistration = 1 << 4;
inline constexpr int kElfClassifiedTrampolines = 1 << 5;
inline constexpr int kElfBionicProvidersRouted = 1 << 6;
inline constexpr uint32_t kFixtureErrnoRouteMask = 1u << 0;
inline constexpr uint32_t kFixtureStrlenRouteMask = 1u << 1;
inline constexpr uint32_t kFixtureOpenRouteMask = 1u << 2;
inline constexpr uint32_t kFixtureReadRouteMask = 1u << 3;
inline constexpr uint32_t kFixtureCloseRouteMask = 1u << 4;
inline constexpr uint32_t kFixtureScanfRouteMask = 1u << 5;
inline constexpr uint32_t kFixtureVsscanfRouteMask = 1u << 6;
inline constexpr uint32_t kFixtureSwprintfRouteMask = 1u << 7;
inline constexpr uint32_t kFixtureIoctlRouteMask = 1u << 8;
inline constexpr uint32_t kFixtureStrftimeRouteMask = 1u << 9;
inline constexpr uint32_t kFixtureSendfileRouteMask = 1u << 10;
inline constexpr uint32_t kFixtureAllProviderRouteMask =
    kFixtureErrnoRouteMask | kFixtureStrlenRouteMask | kFixtureOpenRouteMask |
    kFixtureReadRouteMask | kFixtureCloseRouteMask | kFixtureScanfRouteMask |
    kFixtureVsscanfRouteMask | kFixtureSwprintfRouteMask |
    kFixtureIoctlRouteMask | kFixtureStrftimeRouteMask |
    kFixtureSendfileRouteMask;
inline constexpr uint32_t kFixtureNativeAddEntryMask = 1u << 0;
inline constexpr uint32_t kFixtureNativeSpillEntryMask = 1u << 1;
inline constexpr uint32_t kFixtureNativeUsesEnvEntryMask = 1u << 2;
inline constexpr uint32_t kFixtureAllEntryMask = 0xffu;

extern std::atomic<int> g_elf_fixture_status;
extern std::atomic<uint32_t> g_elf_classified_trampoline_mask;
extern std::atomic<int> g_elf_fixture_lifecycle;
extern std::atomic<uint32_t> g_elf_fixture_provider_routes;
extern std::atomic<int> g_elf_fixture_namespace_lifecycle;

struct ElfLibrary {
  uint64_t magic = kElfLibraryMagic;
  bool fixture_graph = false;
  DarwinArtElfGraphHandle* graph = nullptr;
  DarwinArtBionicNamespace* provider_namespace = nullptr;
  DarwinArtBionicDsoLifecycleOwner* dso_lifecycle = nullptr;
  darwin_art_image_registry::Owner* image_registry = nullptr;
  bool filesystem_owner = false;
  bool network_owner = false;
  bool stdio_owner = false;
  bool ioctl_owner = false;
  bool strftime_owner = false;
  bool sendfile_owner = false;
  uintptr_t jni_on_load = 0;
  uintptr_t jni_on_unload = 0;
  alignas(DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT)
      std::array<unsigned char, DARWIN_ART_JNI_PROXY_STORAGE_SIZE> proxy_storage{};
  DarwinArtJniProxy* proxy = nullptr;
  darwin_art::android_jni::TrampolineSet* trampolines = nullptr;
};

int PublishRuntimeElfImage(void* context, uintptr_t start, uintptr_t end);
int FinalizeRuntimeElfImage(void* context, uintptr_t start, uintptr_t end);
void TeardownProviderNamespace(ElfLibrary* library);
bool LookupOptionalElfSymbol(ElfLibrary* library,
                             const char* name,
                             uintptr_t* address,
                             std::string* error);

void* ProxyCurrentEnv(void* context);
void* ProxyFindClass(void* context, const char* name);
int32_t ProxyThrowNew(void* context, void* clazz, const char* message);

}  // namespace android
