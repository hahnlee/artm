#include "darwin_art_registered_native_bridge.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool ValidOwner(const void* function,
                const DarwinArtAndroidFunctionOwnerV1& owner) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(function);
  return owner.image_id != 0 && owner.generation != 0 &&
         owner.executable_begin < owner.executable_end &&
         address >= owner.executable_begin && address < owner.executable_end;
}

bool ValidShorty(const char* shorty,
                 uint32_t shorty_length,
                 DarwinArtJniCallType call_type) {
  if (shorty == nullptr || shorty_length == 0 ||
      std::strlen(shorty) != shorty_length ||
      (call_type != DARWIN_ART_JNI_CALL_REGULAR &&
       call_type != DARWIN_ART_JNI_CALL_CRITICAL_NATIVE)) {
    return false;
  }
  const auto valid_type = [](char type) {
    return std::strchr("VZBCSIJFDL", type) != nullptr;
  };
  if (!valid_type(shorty[0]) ||
      (call_type == DARWIN_ART_JNI_CALL_CRITICAL_NATIVE &&
       shorty[0] == 'L')) {
    return false;
  }
  for (uint32_t index = 1; index < shorty_length; ++index) {
    if (!valid_type(shorty[index]) || shorty[index] == 'V') {
      return false;
    }
    // ART forbids references in @CriticalNative declarations. Rejecting them
    // here prevents a regular-JNI implicit-argument thunk from being cached
    // under a critical call key.
    if (call_type == DARWIN_ART_JNI_CALL_CRITICAL_NATIVE &&
        shorty[index] == 'L') {
      return false;
    }
  }
  return true;
}

struct Key {
  uint64_t image_id;
  uint64_t generation;
  uintptr_t function;
  std::string shorty;
  DarwinArtJniCallType call_type;

  bool operator==(const Key& other) const {
    return image_id == other.image_id && generation == other.generation &&
           function == other.function && shorty == other.shorty &&
           call_type == other.call_type;
  }
};

struct KeyHash {
  size_t operator()(const Key& key) const {
    size_t hash = std::hash<uint64_t>{}(key.image_id);
    const auto combine = [&hash](size_t value) {
      hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    combine(std::hash<uint64_t>{}(key.generation));
    combine(std::hash<uintptr_t>{}(key.function));
    combine(std::hash<std::string>{}(key.shorty));
    combine(std::hash<int>{}(static_cast<int>(key.call_type)));
    return hash;
  }
};

struct Entry {
  void* thunk;
};

}  // namespace

struct DarwinArtRegisteredNativeCache {
  explicit DarwinArtRegisteredNativeCache(
      const DarwinArtRegisteredNativeThunkFactoryV1& value)
      : factory(value) {}

  DarwinArtRegisteredNativeThunkFactoryV1 factory;
  std::mutex mutex;
  std::unordered_map<Key, Entry, KeyHash> entries;
};

namespace {

bool LookupOwner(DarwinArtRegisteredNativeCache* cache,
                 const void* function,
                 DarwinArtAndroidFunctionOwnerV1* owner_out) {
  if (cache == nullptr || function == nullptr || owner_out == nullptr) {
    return false;
  }
  DarwinArtAndroidFunctionOwnerV1 owner{};
  if (cache->factory.lookup_owner(cache->factory.context, function, &owner) ==
          0 ||
      !ValidOwner(function, owner)) {
    return false;
  }
  *owner_out = owner;
  return true;
}

}  // namespace

extern "C" DarwinArtRegisteredNativeCache*
darwin_art_registered_native_cache_create(
    const DarwinArtRegisteredNativeThunkFactoryV1* factory) {
  if (factory == nullptr ||
      factory->abi_version != DARWIN_ART_REGISTERED_NATIVE_BRIDGE_ABI_VERSION ||
      factory->struct_size != sizeof(*factory) ||
      factory->lookup_owner == nullptr || factory->release_owner == nullptr ||
      factory->build_thunk == nullptr || factory->destroy_thunk == nullptr) {
    return nullptr;
  }
  return new DarwinArtRegisteredNativeCache(*factory);
}

extern "C" void darwin_art_registered_native_cache_destroy(
    DarwinArtRegisteredNativeCache* cache) {
  if (cache == nullptr) {
    return;
  }
  std::vector<void*> doomed;
  {
    std::lock_guard<std::mutex> lock(cache->mutex);
    for (const auto& [key, entry] : cache->entries) {
      (void)key;
      doomed.push_back(entry.thunk);
    }
    cache->entries.clear();
  }
  for (void* thunk : doomed) {
    cache->factory.destroy_thunk(cache->factory.context, thunk);
  }
  delete cache;
}

extern "C" bool darwin_art_is_android_function_pointer(
    DarwinArtRegisteredNativeCache* cache,
    const void* function) {
  DarwinArtAndroidFunctionOwnerV1 owner{};
  const bool owned = LookupOwner(cache, function, &owner);
  if (owned) {
    cache->factory.release_owner(cache->factory.context, &owner);
  }
  return owned;
}

extern "C" void* darwin_art_get_registered_native_trampoline(
    DarwinArtRegisteredNativeCache* cache,
    const void* android_function,
    const char* shorty,
    uint32_t shorty_length,
    DarwinArtJniCallType call_type) {
  if (cache == nullptr ||
      !ValidShorty(shorty, shorty_length, call_type)) {
    return nullptr;
  }
  DarwinArtAndroidFunctionOwnerV1 owner{};
  if (!LookupOwner(cache, android_function, &owner)) {
    return nullptr;
  }
  Key key{owner.image_id,
          owner.generation,
          reinterpret_cast<uintptr_t>(android_function),
          std::string(shorty, shorty_length),
          call_type};

  std::unique_lock<std::mutex> lock(cache->mutex);
  const auto found = cache->entries.find(key);
  if (found != cache->entries.end()) {
    void* thunk = found->second.thunk;
    lock.unlock();
    cache->factory.release_owner(cache->factory.context, &owner);
    return thunk;
  }
  void* thunk = cache->factory.build_thunk(cache->factory.context,
                                           android_function,
                                           &owner,
                                           shorty,
                                           shorty_length,
                                           call_type);
  if (thunk == nullptr) {
    lock.unlock();
    cache->factory.release_owner(cache->factory.context, &owner);
    return nullptr;
  }
  cache->entries.emplace(std::move(key), Entry{thunk});
  lock.unlock();
  cache->factory.release_owner(cache->factory.context, &owner);
  return thunk;
}

extern "C" DarwinArtRegisteredNativeResolution
darwin_art_resolve_registered_native(
    DarwinArtRegisteredNativeCache* cache,
    const void* function,
    bool class_loader_namespace_is_bridged,
    const char* shorty,
    uint32_t shorty_length,
    DarwinArtJniCallType call_type,
    const void** callable_out) {
  if (cache == nullptr || function == nullptr || callable_out == nullptr) {
    return DARWIN_ART_REGISTERED_NATIVE_ERROR;
  }
  *callable_out = nullptr;
  const bool owned = darwin_art_is_android_function_pointer(cache, function);
  if (!class_loader_namespace_is_bridged && !owned) {
    *callable_out = function;
    return DARWIN_ART_REGISTERED_NATIVE_DIRECT;
  }
  if (!owned) {
    return DARWIN_ART_REGISTERED_NATIVE_ERROR;
  }
  void* thunk = darwin_art_get_registered_native_trampoline(
      cache, function, shorty, shorty_length, call_type);
  if (thunk == nullptr) {
    return DARWIN_ART_REGISTERED_NATIVE_ERROR;
  }
  *callable_out = thunk;
  return DARWIN_ART_REGISTERED_NATIVE_TRAMPOLINE;
}

extern "C" size_t darwin_art_registered_native_cache_retire_image(
    DarwinArtRegisteredNativeCache* cache,
    uint64_t image_id,
    uint64_t generation) {
  if (cache == nullptr || image_id == 0 || generation == 0) {
    return 0;
  }
  std::vector<void*> doomed;
  {
    std::lock_guard<std::mutex> lock(cache->mutex);
    for (auto iterator = cache->entries.begin();
         iterator != cache->entries.end();) {
      if (iterator->first.image_id == image_id &&
          iterator->first.generation == generation) {
        doomed.push_back(iterator->second.thunk);
        iterator = cache->entries.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  for (void* thunk : doomed) {
    cache->factory.destroy_thunk(cache->factory.context, thunk);
  }
  return doomed.size();
}

extern "C" size_t darwin_art_registered_native_cache_size(
    DarwinArtRegisteredNativeCache* cache) {
  if (cache == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(cache->mutex);
  return cache->entries.size();
}
