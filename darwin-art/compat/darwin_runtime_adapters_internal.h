#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "darwin_android_jni_trampoline.h"
#include "darwin_android_elf_image_registry.h"
#include "darwin_art_elf_jni_fixture_identity.h"
#include "darwin_art_elf_loader.h"
#include "darwin_art_bionic_dso_lifecycle.h"
#include "darwin_art_bionic_provider_namespace.h"
#include "darwin_art_jni_proxy.h"
#include "darwin_art_runtime_native_owner.h"

namespace android {

inline constexpr uint64_t kElfLibraryMagic = UINT64_C(0x44415257454c464a);
inline constexpr uint32_t kNativeOwnerFilesystem = 20;
inline constexpr uint32_t kNativeOwnerStdio = 30;
inline constexpr uint32_t kNativeOwnerIoctl = 40;
inline constexpr uint32_t kNativeOwnerSendfile = 50;
inline constexpr uint32_t kNativeOwnerStrftime = 60;
inline constexpr uint32_t kNativeOwnerNetwork = 70;
inline constexpr uint32_t kNativeOwnerVm = 75;
inline constexpr uint32_t kNativeOwnerDso = 80;
inline constexpr uint32_t kNativeOwnerImageRegistry = 90;
inline constexpr uint32_t kNativeOwnerNamespace = 100;
inline constexpr uint32_t kNativeOwnerAndroidUnwind = 105;
inline constexpr uint32_t kNativeOwnerGraph = 110;
inline constexpr uint32_t kNativeOwnerGraphHandle = 120;
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
  RuntimeNativeOwner* native_owner = nullptr;
  bool fixture_graph = false;
  DarwinArtElfGraphHandle* graph = nullptr;
  DarwinArtElfHandle* android_unwind_provider = nullptr;
  DarwinArtBionicNamespace* provider_namespace = nullptr;
  DarwinArtBionicDsoLifecycleOwner* dso_lifecycle = nullptr;
  darwin_art_image_registry::Owner* image_registry = nullptr;
  void* egl_provider = nullptr;
  void* gles_provider = nullptr;
  void* z_provider = nullptr;
  uintptr_t jni_on_load = 0;
  uintptr_t jni_on_unload = 0;
  alignas(DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT)
      std::array<unsigned char, DARWIN_ART_JNI_PROXY_STORAGE_SIZE> proxy_storage{};
  DarwinArtJniProxy* proxy = nullptr;
  JavaVM* art_vm = nullptr;
  // Initiating app loader for JNI_OnLoad/ProxyFindClass.  NativeBridge's
  // callback has no class-loader argument, so retain the lease per image.
  void* app_loader = nullptr;
  darwin_art::android_jni::TrampolineSet* trampolines = nullptr;
  // JNI_OnLoad may register methods on more than one app class.  Each
  // RegisterNatives call gets an independent executable trampoline mapping;
  // all mappings remain owned by the image until its graph is torn down.
  std::mutex trampoline_mutex;
  std::vector<darwin_art::android_jni::TrampolineSet*> trampoline_sets;
  std::unordered_map<std::string, void*> exported_jni_trampolines;
  std::mutex method_descriptor_mutex;
  std::unordered_map<void*, std::string> method_descriptors;
};

int PublishRuntimeElfImage(void* context, uintptr_t start, uintptr_t end);
int FinalizeRuntimeElfImage(void* context, uintptr_t start, uintptr_t end);
void TeardownProviderNamespace(ElfLibrary* library);
bool LookupOptionalElfSymbol(ElfLibrary* library,
                             const char* name,
                             uintptr_t* address,
                             std::string* error);
bool IsExactFixtureGraph(const char* root_soname,
                         const DarwinArtElfGraphSource* sources,
                         size_t source_count);
DarwinArtElfResolveStatus ResolveRuntimeProvider(
    void* context,
    const DarwinArtElfSymbolRequest* request,
    uintptr_t* out_address,
    DarwinArtElfErrorBuffer* error);
int DropRuntimeElfGraph(void* value, void* context);
int DropRuntimeAndroidUnwindProvider(void* value, void* context);
void DestroyRuntimeElfTrampolines(ElfLibrary* library);
int DropRuntimeElfLibrary(void* value, void* context);
int DropRuntimeElfImageRegistry(void* value, void* context);
int DropRuntimeDsoLifecycle(void* value, void* context);
int DropRuntimeProviderNamespace(void* value, void* context);
int DropRuntimeProviderKind(void* value, void* context);
ElfLibrary* AsElfLibrary(void* handle);
int32_t ProxyRegisterNatives(void* context,
                             void* clazz,
                             const DarwinArtJniNativeMethod* methods,
                             int32_t count);

void* ProxyCurrentEnv(void* context);
int32_t ProxyAttachCurrentThread(void* context, void* arguments,
                                 int32_t as_daemon);
int32_t ProxyDetachCurrentThread(void* context);
void* ProxyFindClass(void* context, const char* name);
int32_t ProxyThrowNew(void* context, void* clazz, const char* message);
void* ProxyGetMethodId(void* context, void* clazz, const char* name,
                       const char* signature, int32_t is_static);
uint64_t ProxyCallMethodV(void* context, void* object, void* method,
                          void* android_va_list, int32_t return_shorty,
                          int32_t is_static);

}  // namespace android
