#include "darwin_android_asset_manager.h"

#ifndef LOG_TAG
#define LOG_TAG "DarwinArtAssetManager"
#endif
#ifndef LOG_FATAL_IF
#define LOG_FATAL_IF(condition, ...) \
  do {                               \
    if (condition) __builtin_trap(); \
  } while (false)
#endif

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <androidfw/Asset.h>
#include <androidfw/AssetDir.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/MutexGuard.h>
#include <android_runtime/android_util_AssetManager.h>

#include "darwin_art_bionic_fs.h"

#include <cstdint>
#include <cstring>
#include <memory>

struct AAsset {
  std::unique_ptr<android::Asset> asset;
};

struct AAssetDir {
  std::unique_ptr<android::AssetDir> directory;
  size_t next = 0;
};

namespace {

android::Asset::AccessMode AccessModeForNdk(int mode) {
  switch (mode) {
    case AASSET_MODE_RANDOM:
      return android::Asset::ACCESS_RANDOM;
    case AASSET_MODE_STREAMING:
      return android::Asset::ACCESS_STREAMING;
    case AASSET_MODE_BUFFER:
      return android::Asset::ACCESS_BUFFER;
    default:
      return android::Asset::ACCESS_UNKNOWN;
  }
}

template <typename Result, typename Operation>
Result WithAssetManager(AAssetManager* manager, Result failure,
                        Operation&& operation) {
  android::Guarded<android::AssetManager2>* guarded =
      android::AssetManagerForNdkAssetManager(manager);
  if (guarded == nullptr) return failure;
  android::ScopedLock<android::AssetManager2> locked(*guarded);
  auto active_operation = locked->StartOperation();
  return operation(*locked);
}

}  // namespace

extern "C" AAssetManager* AAssetManager_fromJava(JNIEnv* env,
                                                   jobject asset_manager) {
  if (env == nullptr || asset_manager == nullptr) return nullptr;
  return android::NdkAssetManagerForJavaObject(env, asset_manager);
}

extern "C" AAsset* AAssetManager_open(AAssetManager* manager,
                                        const char* filename, int mode) {
  if (filename == nullptr) return nullptr;
  std::unique_ptr<android::Asset> opened = WithAssetManager(
      manager, std::unique_ptr<android::Asset>{},
      [&](android::AssetManager2& assets) {
        return assets.Open(filename, AccessModeForNdk(mode));
      });
  if (opened == nullptr) return nullptr;
  return new AAsset{std::move(opened)};
}

extern "C" AAssetDir* AAssetManager_openDir(AAssetManager* manager,
                                              const char* directory_name) {
  if (directory_name == nullptr) return nullptr;
  std::unique_ptr<android::AssetDir> opened = WithAssetManager(
      manager, std::unique_ptr<android::AssetDir>{},
      [&](android::AssetManager2& assets) {
        return assets.OpenDir(directory_name);
      });
  if (opened == nullptr) return nullptr;
  return new AAssetDir{std::move(opened), 0};
}

extern "C" const char* AAssetDir_getNextFileName(AAssetDir* directory) {
  if (directory == nullptr || directory->directory == nullptr ||
      directory->next >= directory->directory->getFileCount()) {
    return nullptr;
  }
  return directory->directory->getFileName(directory->next++).c_str();
}

extern "C" void AAssetDir_rewind(AAssetDir* directory) {
  if (directory != nullptr) directory->next = 0;
}

extern "C" void AAssetDir_close(AAssetDir* directory) { delete directory; }

extern "C" int AAsset_read(AAsset* asset, void* buffer, size_t count) {
  if (asset == nullptr || asset->asset == nullptr ||
      (buffer == nullptr && count != 0)) {
    return -1;
  }
  const ssize_t result = asset->asset->read(buffer, count);
  return result > INT32_MAX ? INT32_MAX : static_cast<int>(result);
}

extern "C" off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
  if (asset == nullptr || asset->asset == nullptr) return -1;
  return static_cast<off_t>(asset->asset->seek(offset, whence));
}

extern "C" off64_t AAsset_seek64(AAsset* asset, off64_t offset, int whence) {
  if (asset == nullptr || asset->asset == nullptr) return -1;
  return asset->asset->seek(offset, whence);
}

extern "C" void AAsset_close(AAsset* asset) { delete asset; }

extern "C" const void* AAsset_getBuffer(AAsset* asset) {
  if (asset == nullptr || asset->asset == nullptr) return nullptr;
  return asset->asset->getBuffer(false);
}

extern "C" off_t AAsset_getLength(AAsset* asset) {
  if (asset == nullptr || asset->asset == nullptr) return 0;
  return static_cast<off_t>(asset->asset->getLength());
}

extern "C" off64_t AAsset_getLength64(AAsset* asset) {
  if (asset == nullptr || asset->asset == nullptr) return 0;
  return asset->asset->getLength();
}

extern "C" off_t AAsset_getRemainingLength(AAsset* asset) {
  if (asset == nullptr || asset->asset == nullptr) return 0;
  return static_cast<off_t>(asset->asset->getRemainingLength());
}

extern "C" off64_t AAsset_getRemainingLength64(AAsset* asset) {
  if (asset == nullptr || asset->asset == nullptr) return 0;
  return asset->asset->getRemainingLength();
}

extern "C" int AAsset_openFileDescriptor(AAsset* asset, off_t* start,
                                           off_t* length) {
  if (asset == nullptr || asset->asset == nullptr) return -1;
  off64_t start64 = 0;
  off64_t length64 = 0;
  const int descriptor = asset->asset->openFileDescriptor(&start64, &length64);
  if (descriptor >= 0) {
    if (start != nullptr) *start = static_cast<off_t>(start64);
    if (length != nullptr) *length = static_cast<off_t>(length64);
    return darwin_art_bionic_fs_adopt_host_fd_core(descriptor);
  }
  return -1;
}

extern "C" int AAsset_openFileDescriptor64(AAsset* asset, off64_t* start,
                                             off64_t* length) {
  if (asset == nullptr || asset->asset == nullptr) return -1;
  const int descriptor = asset->asset->openFileDescriptor(start, length);
  return descriptor < 0 ? -1
                        : darwin_art_bionic_fs_adopt_host_fd_core(descriptor);
}

void* darwin_art_android_asset_manager_symbol(const char* symbol) {
  if (symbol == nullptr) return nullptr;
#define DARWIN_ART_ASSET_SYMBOL(name)            \
  if (std::strcmp(symbol, #name) == 0) {         \
    return reinterpret_cast<void*>(&name);       \
  }
  DARWIN_ART_ASSET_SYMBOL(AAssetManager_fromJava)
  DARWIN_ART_ASSET_SYMBOL(AAssetManager_open)
  DARWIN_ART_ASSET_SYMBOL(AAssetManager_openDir)
  DARWIN_ART_ASSET_SYMBOL(AAssetDir_getNextFileName)
  DARWIN_ART_ASSET_SYMBOL(AAssetDir_rewind)
  DARWIN_ART_ASSET_SYMBOL(AAssetDir_close)
  DARWIN_ART_ASSET_SYMBOL(AAsset_read)
  DARWIN_ART_ASSET_SYMBOL(AAsset_seek)
  DARWIN_ART_ASSET_SYMBOL(AAsset_seek64)
  DARWIN_ART_ASSET_SYMBOL(AAsset_close)
  DARWIN_ART_ASSET_SYMBOL(AAsset_getBuffer)
  DARWIN_ART_ASSET_SYMBOL(AAsset_getLength)
  DARWIN_ART_ASSET_SYMBOL(AAsset_getLength64)
  DARWIN_ART_ASSET_SYMBOL(AAsset_getRemainingLength)
  DARWIN_ART_ASSET_SYMBOL(AAsset_getRemainingLength64)
  DARWIN_ART_ASSET_SYMBOL(AAsset_openFileDescriptor)
  DARWIN_ART_ASSET_SYMBOL(AAsset_openFileDescriptor64)
#undef DARWIN_ART_ASSET_SYMBOL
  return nullptr;
}
