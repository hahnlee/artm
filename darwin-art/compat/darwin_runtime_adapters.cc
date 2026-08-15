#include <dlfcn.h>

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "hprof/hprof.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_art_elf_jni_fixture_identity.h"
#include "darwin_art_elf_loader.h"
#include "darwin_art_jni_proxy.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"
#include "odr_statslog/odr_statslog.h"
#include "palette/palette.h"
#include "runtime_image.h"
#include "unwindstack/AndroidUnwinder.h"

namespace android {
namespace {

constexpr uint64_t kElfLibraryMagic = UINT64_C(0x44415257454c464a);
constexpr int kElfOpened = 1 << 0;
constexpr int kElfOnLoadCalled = 1 << 1;
constexpr int kElfFoundFixtureClass = 1 << 2;
constexpr int kElfCapturedRegistration = 1 << 3;
constexpr int kElfInstalledRegistration = 1 << 4;
constexpr int kElfClassifiedTrampolines = 1 << 5;

std::atomic<int> g_elf_fixture_status{0};
std::atomic<uint32_t> g_elf_classified_trampoline_mask{0};

void SetNativeLoaderError(char** error_msg, const std::string& message) {
  if (error_msg != nullptr) {
    *error_msg = strdup(message.c_str());
  }
}

std::string ElfError(DarwinArtElfStatus status,
                     const DarwinArtElfErrorBuffer& error) {
  std::ostringstream stream;
  stream << darwin_art_elf_status_name(status);
  if (error.data != nullptr && error.data[0] != '\0') {
    stream << ": " << error.data;
  }
  return stream.str();
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
  CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '\0');
  for (size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = kHex[digest[index] >> 4];
    result[index * 2 + 1] = kHex[digest[index] & 0xf];
  }
  return result;
}

bool ReadExactElfFixture(const char* path,
                         std::vector<uint8_t>* bytes,
                         std::string* error) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    *error = std::string("cannot open Android ELF library: ") + path;
    return false;
  }
  const std::streampos length = input.tellg();
  if (length < 0 || static_cast<uint64_t>(length) != kDarwinArtElfJniFixtureSize) {
    *error = "Android ELF library is outside the first-fixture capability boundary";
    return false;
  }
  bytes->resize(static_cast<size_t>(length));
  input.seekg(0, std::ios::beg);
  input.read(reinterpret_cast<char*>(bytes->data()),
             static_cast<std::streamsize>(bytes->size()));
  if (!input || Sha256(*bytes) != kDarwinArtElfJniFixtureSha256) {
    *error = "Android ELF fixture identity/hash mismatch";
    return false;
  }
  return true;
}

struct ElfLibrary {
  uint64_t magic = kElfLibraryMagic;
  DarwinArtElfHandle* image = nullptr;
  JNIEnv* art_env = nullptr;
  uintptr_t jni_on_load = 0;
  uintptr_t jni_on_unload = 0;
  alignas(DARWIN_ART_JNI_PROXY_STORAGE_ALIGNMENT)
      std::array<unsigned char, DARWIN_ART_JNI_PROXY_STORAGE_SIZE> proxy_storage{};
  DarwinArtJniProxy* proxy = nullptr;
  darwin_art::android_jni::FixtureTrampolineSet* trampolines = nullptr;
};

ElfLibrary* AsElfLibrary(void* handle) {
  auto* library = static_cast<ElfLibrary*>(handle);
  return library != nullptr && library->magic == kElfLibraryMagic ? library : nullptr;
}

void* ProxyFindClass(void* context, const char* name) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || library->art_env == nullptr || name == nullptr) {
    return nullptr;
  }
  void* clazz = library->art_env->FindClass(name);
  if (clazz != nullptr &&
      std::strcmp(name, "darwin/art/nativefixture/NativeFixture") == 0) {
    g_elf_fixture_status.fetch_or(kElfFoundFixtureClass, std::memory_order_relaxed);
  }
  return clazz;
}

int32_t ProxyRegisterNatives(void* context,
                             void* clazz,
                             const DarwinArtJniNativeMethod* methods,
                             int32_t count) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || clazz == nullptr || methods == nullptr || count != 2) {
    return DARWIN_ART_JNI_ERR;
  }
  if (library->trampolines != nullptr || library->proxy == nullptr ||
      library->art_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  constexpr std::array<std::pair<const char*, const char*>, 2> kExpected = {{
      {"nativeAdd", "(IJI)J"},
      {"nativeSpill", kDarwinArtElfJniFixtureSpillSignature},
  }};
  for (size_t index = 0; index < kExpected.size(); ++index) {
    if (methods[index].name == nullptr || methods[index].signature == nullptr ||
        methods[index].function == nullptr ||
        std::strcmp(methods[index].name, kExpected[index].first) != 0 ||
        std::strcmp(methods[index].signature, kExpected[index].second) != 0) {
      return DARWIN_ART_JNI_ERR;
    }
    if (library->art_env->GetStaticMethodID(
            static_cast<jclass>(clazz), kExpected[index].first,
            kExpected[index].second) == nullptr) {
      return DARWIN_ART_JNI_ERR;
    }
  }
  g_elf_fixture_status.fetch_or(kElfCapturedRegistration, std::memory_order_relaxed);
  JavaVM* proxy_vm =
      static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy));
  void* proxy_env = nullptr;
  if (proxy_vm == nullptr ||
      proxy_vm->GetEnv(&proxy_env, JNI_VERSION_1_6) != JNI_OK ||
      proxy_env == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  std::string trampoline_error;
  auto* trampolines = darwin_art::android_jni::CreateFixtureTrampolines(
      proxy_env, methods[0].function, methods[1].function, &trampoline_error);
  if (trampolines == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  void* native_add_entry =
      darwin_art::android_jni::FixtureNativeAddEntry(trampolines);
  void* native_spill_entry =
      darwin_art::android_jni::FixtureNativeSpillEntry(trampolines);
  const auto* native_add_bytes = static_cast<const uint8_t*>(native_add_entry);
  if (darwin_art::android_jni::FixtureTrampolineGeneration(trampolines) == 0 ||
      !darwin_art::android_jni::IsFixtureTrampolineEntry(native_add_entry) ||
      !darwin_art::android_jni::IsFixtureTrampolineEntry(native_spill_entry) ||
      darwin_art::android_jni::IsFixtureTrampolineEntry(native_add_bytes + 4) ||
      darwin_art::android_jni::FixtureTrampolineEntryMask(native_add_entry) !=
          darwin_art::android_jni::kFixtureNativeAddEntryMask ||
      darwin_art::android_jni::FixtureTrampolineEntryMask(native_spill_entry) !=
          darwin_art::android_jni::kFixtureNativeSpillEntryMask ||
      darwin_art::android_jni::FixtureTrampolineEntryMask(native_add_bytes + 4) != 0) {
    darwin_art::android_jni::DestroyFixtureTrampolines(trampolines);
    return DARWIN_ART_JNI_ERR;
  }
  JNINativeMethod bridged_methods[] = {
      {const_cast<char*>(kExpected[0].first),
       const_cast<char*>(kExpected[0].second),
       native_add_entry},
      {const_cast<char*>(kExpected[1].first),
       const_cast<char*>(kExpected[1].second),
       native_spill_entry},
  };
  const jint status = library->art_env->RegisterNatives(
      static_cast<jclass>(clazz), bridged_methods, std::size(bridged_methods));
  if (status != JNI_OK) {
    jthrowable registration_failure = library->art_env->ExceptionOccurred();
    if (registration_failure != nullptr) {
      library->art_env->ExceptionClear();
    }
    library->art_env->UnregisterNatives(static_cast<jclass>(clazz));
    jthrowable rollback_failure = library->art_env->ExceptionOccurred();
    if (rollback_failure != nullptr) {
      library->art_env->ExceptionClear();
    }
    darwin_art::android_jni::DestroyFixtureTrampolines(trampolines);
    if (registration_failure != nullptr) {
      library->art_env->Throw(registration_failure);
      library->art_env->DeleteLocalRef(registration_failure);
    } else if (rollback_failure != nullptr) {
      library->art_env->Throw(rollback_failure);
    }
    library->art_env->DeleteLocalRef(rollback_failure);
    return DARWIN_ART_JNI_ERR;
  }
  library->trampolines = trampolines;
  g_elf_fixture_status.fetch_or(kElfInstalledRegistration,
                                std::memory_order_relaxed);
  return DARWIN_ART_JNI_OK;
}

int32_t ProxyThrowNew(void* context, void* clazz, const char* message) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || library->art_env == nullptr || clazz == nullptr ||
      message == nullptr) {
    return DARWIN_ART_JNI_ERR;
  }
  return library->art_env->ThrowNew(static_cast<jclass>(clazz), message);
}

thread_local ElfLibrary* g_pending_on_load = nullptr;
thread_local ElfLibrary* g_pending_on_unload = nullptr;

jint ElfJniOnLoadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_load, nullptr);
  if (library == nullptr || library->jni_on_load == 0 || library->proxy == nullptr) {
    return JNI_ERR;
  }
  g_elf_fixture_status.fetch_or(kElfOnLoadCalled, std::memory_order_relaxed);
  using AndroidJniOnLoad = jint (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnLoad>(library->jni_on_load);
  // JNI_OnLoad itself has only two register arguments, so its Android and
  // Darwin AArch64 calling sequences coincide. The ELF library sees only the
  // closed proxy VM and never ART's real JavaVM function table.
  const jint result =
      function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
  // The backend JNIEnv belongs to the synchronous ART load thread. This fixed
  // first-fixture proxy is lifecycle-only; never leave that pointer reusable.
  library->art_env = nullptr;
  return result;
}

void ElfJniOnUnloadTrampoline(JavaVM*, void*) {
  ElfLibrary* library = std::exchange(g_pending_on_unload, nullptr);
  if (library == nullptr || library->jni_on_unload == 0 || library->proxy == nullptr) {
    return;
  }
  using AndroidJniOnUnload = void (*)(JavaVM*, void*);
  auto function = reinterpret_cast<AndroidJniOnUnload>(library->jni_on_unload);
  function(static_cast<JavaVM*>(darwin_art_jni_proxy_java_vm(library->proxy)), nullptr);
}

bool LookupElfSymbol(ElfLibrary* library,
                     const char* name,
                     uintptr_t* address,
                     std::string* error) {
  std::array<char, 1024> storage{};
  DarwinArtElfErrorBuffer error_buffer{storage.data(), storage.size(), 0};
  const DarwinArtElfStatus status =
      darwin_art_elf_lookup(library->image, name, address, &error_buffer);
  if (status == DARWIN_ART_ELF_OK) {
    return true;
  }
  *error = ElfError(status, error_buffer);
  return false;
}

}  // namespace

extern "C" {

bool LoadNativeBridge(const char* library, const NativeBridgeRuntimeCallbacks*) {
  // An empty name means that ART explicitly requested no translation bridge.
  return library == nullptr || library[0] == '\0';
}

bool PreInitializeNativeBridge(const char*, const char*) { return true; }
void PreZygoteForkNativeBridge() {}
bool InitializeNativeBridge(JNIEnv*, const char*) { return true; }
bool NativeBridgeInitialized() { return false; }
uint32_t NativeBridgeGetVersion() { return 0; }

void UnloadNativeBridge() {}

void* NativeBridgeGetTrampoline2(void* handle,
                                 const char* name,
                                 const char*,
                                 uint32_t,
                                 JNICallType) {
  ElfLibrary* library = AsElfLibrary(handle);
  if (library == nullptr || name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "JNI_OnLoad") == 0 && library->jni_on_load != 0) {
    if (g_pending_on_load != nullptr) {
      return nullptr;
    }
    g_pending_on_load = library;
    return reinterpret_cast<void*>(&ElfJniOnLoadTrampoline);
  }
  if (std::strcmp(name, "JNI_OnUnload") == 0 && library->jni_on_unload != 0) {
    if (g_pending_on_unload != nullptr) {
      return nullptr;
    }
    g_pending_on_unload = library;
    return reinterpret_cast<void*>(&ElfJniOnUnloadTrampoline);
  }
  // Ordinary JNI methods need a NativeBridgeGetTrampoline2 per-shorty PCS
  // repacker. Returning null is an explicit capability failure, never a raw
  // Android function pointer or Darwin global-symbol fallback.
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* pointer) {
  const uint32_t entry_mask =
      darwin_art::android_jni::FixtureTrampolineEntryMask(pointer);
  const uint32_t observed =
      g_elf_classified_trampoline_mask.fetch_or(entry_mask,
                                                std::memory_order_relaxed) |
      entry_mask;
  if (observed == (darwin_art::android_jni::kFixtureNativeAddEntryMask |
                   darwin_art::android_jni::kFixtureNativeSpillEntryMask)) {
    g_elf_fixture_status.fetch_or(kElfClassifiedTrampolines,
                                  std::memory_order_relaxed);
  }
  return entry_mask != 0;
}

void* OpenNativeLibrary(JNIEnv* env,
                        int32_t,
                        const char* path,
                        jobject,
                        const char*,
                        jstring,
                        bool* needs_native_bridge,
                        char** error_msg) {
  if (needs_native_bridge != nullptr) {
    *needs_native_bridge = false;
  }
  bool is_elf = false;
  if (path != nullptr) {
    std::ifstream header(path, std::ios::binary);
    std::array<unsigned char, 4> magic{};
    if (header.read(reinterpret_cast<char*>(magic.data()), magic.size())) {
      is_elf = magic == std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'};
    }
  }
  if (is_elf) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (env == nullptr || !ReadExactElfFixture(path, &bytes, &error)) {
      SetNativeLoaderError(error_msg,
                           env == nullptr ? "Android ELF load requires a live JNIEnv" : error);
      return nullptr;
    }
    auto library = std::make_unique<ElfLibrary>();
    library->art_env = env;
    DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION, nullptr, nullptr};
    std::array<char, 1024> error_storage{};
    DarwinArtElfErrorBuffer error_buffer{error_storage.data(), error_storage.size(), 0};
    DarwinArtElfStatus status = darwin_art_elf_load_bytes(
        bytes.data(), bytes.size(), &options, &library->image, &error_buffer);
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg, "Android ELF load failed: " + ElfError(status, error_buffer));
      return nullptr;
    }
    status = darwin_art_elf_run_initializers(library->image, &error_buffer);
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg,
                           "Android ELF initialization failed: " + ElfError(status, error_buffer));
      darwin_art_elf_unload(&library->image, nullptr);
      return nullptr;
    }
    DarwinArtJniBackend backend{library.get(), &ProxyFindClass, &ProxyRegisterNatives,
                                &ProxyThrowNew};
    library->proxy = darwin_art_jni_proxy_init(
        library->proxy_storage.data(), library->proxy_storage.size(), &backend);
    if (library->proxy == nullptr ||
        !LookupElfSymbol(library.get(), "JNI_OnLoad", &library->jni_on_load, &error) ||
        !LookupElfSymbol(library.get(), "JNI_OnUnload", &library->jni_on_unload, &error)) {
      SetNativeLoaderError(error_msg,
                           library->proxy == nullptr ? "Android JNI proxy initialization failed"
                                                     : "Android ELF lifecycle preflight failed: " + error);
      darwin_art_elf_unload(&library->image, nullptr);
      return nullptr;
    }
    if (needs_native_bridge == nullptr) {
      SetNativeLoaderError(error_msg, "Android ELF load requires needs_native_bridge ownership");
      darwin_art_elf_unload(&library->image, nullptr);
      return nullptr;
    }
    *needs_native_bridge = true;
    g_elf_classified_trampoline_mask.store(0, std::memory_order_relaxed);
    g_elf_fixture_status.store(kElfOpened, std::memory_order_relaxed);
    return library.release();
  }

  void* handle = path == nullptr ? nullptr : dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr && error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library load failed" : message);
  }
  return handle;
}

bool CloseNativeLibrary(void* handle, bool needs_native_bridge, char** error_msg) {
  if (needs_native_bridge) {
    ElfLibrary* library = AsElfLibrary(handle);
    if (library == nullptr) {
      SetNativeLoaderError(error_msg, "invalid Android ELF NativeBridge handle");
      return false;
    }
    library->magic = 0;
    darwin_art::android_jni::DestroyFixtureTrampolines(library->trampolines);
    library->trampolines = nullptr;
    std::array<char, 1024> storage{};
    DarwinArtElfErrorBuffer error_buffer{storage.data(), storage.size(), 0};
    const DarwinArtElfStatus status =
        darwin_art_elf_unload(&library->image, &error_buffer);
    delete library;
    if (status != DARWIN_ART_ELF_OK) {
      SetNativeLoaderError(error_msg, "Android ELF unload failed: " + ElfError(status, error_buffer));
      return false;
    }
    return true;
  }
  if (handle == nullptr || dlclose(handle) == 0) {
    return true;
  }
  if (error_msg != nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message == nullptr ? "Darwin native library close failed" : message);
  }
  return false;
}

void NativeLoaderFreeErrorMessage(char* message) {
  std::free(message);
}

void ResetNativeLoader() {}

}  // extern "C"
}  // namespace android

extern "C" int darwin_art_elf_jni_fixture_registration_status() {
  return android::g_elf_fixture_status.load(std::memory_order_relaxed);
}

extern "C" palette_status_t PaletteSchedGetPriority(int32_t, int32_t* java_priority) {
  if (java_priority == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *java_priority = 5;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteSchedSetPriority(int32_t, int32_t) {
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteWriteCrashThreadStacks(const char*, size_t) {
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteDebugStoreGetString(char* result, size_t max_size) {
  if (result != nullptr && max_size != 0) {
    result[0] = '\0';
  }
  return PALETTE_STATUS_NOT_SUPPORTED;
}

extern "C" palette_status_t PaletteTraceEnabled(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteTraceBegin(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceEnd() { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteTraceIntegerValue(const char*, int32_t) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyDexFileLoaded(const char*) { return PALETTE_STATUS_OK; }
extern "C" palette_status_t PaletteNotifyOatFileLoaded(const char*) { return PALETTE_STATUS_OK; }

extern "C" palette_status_t PaletteShouldReportJniInvocations(bool* enabled) {
  if (enabled == nullptr) {
    return PALETTE_STATUS_INVALID_ARGUMENT;
  }
  *enabled = false;
  return PALETTE_STATUS_OK;
}

extern "C" palette_status_t PaletteNotifyBeginJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}
extern "C" palette_status_t PaletteNotifyEndJniInvocation(JNIEnv*) {
  return PALETTE_STATUS_OK;
}

extern "C" void* __hwasan_tag_pointer(const volatile void* pointer, unsigned char) {
  return const_cast<void*>(pointer);
}
extern "C" void __hwasan_handle_longjmp(const void*) {}

namespace art {
namespace hprof {
void DumpHeap(const char*, int, bool) {}
}  // namespace hprof

std::string RuntimeImage::GetRuntimeImagePath(const std::string&) { return {}; }

bool RuntimeImage::WriteImageToDisk(std::string* error_msg) {
  if (error_msg != nullptr) {
    *error_msg = "runtime images are not supported on Darwin";
  }
  return false;
}

namespace odrefresh {
bool UploadStatsIfAvailable(std::string*) { return true; }
}  // namespace odrefresh

std::ostream& operator<<(std::ostream& stream, const Intrinsics& intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kNone:
      return stream << "None";
#define PRINT_INTRINSIC(Name, ...) case Intrinsics::k##Name: return stream << #Name;
    ART_INTRINSICS_LIST(PRINT_INTRINSIC)
#undef PRINT_INTRINSIC
  }
  return stream << "Intrinsics[" << static_cast<int>(intrinsic) << "]";
}
}  // namespace art

extern "C" void SkipAddSignalHandler(bool) {}

namespace unwindstack {
bool AndroidLocalUnwinder::InternalInitialize(ErrorData&) {
  return false;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData&) {
  return false;
}
}  // namespace unwindstack
