#include "darwin_android_jni_trampoline.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include "darwin_art_elf_jni_fixture_identity.h"

namespace darwin_art::android_jni {
namespace {

constexpr size_t kAddOffset = 0;
constexpr size_t kAddSize = 32;
constexpr size_t kSpillOffset = kAddOffset + kAddSize;
constexpr size_t kSpillInstructionCount = 18;
constexpr size_t kSpillSize = kSpillInstructionCount * sizeof(uint32_t) +
                              2 * sizeof(uint64_t);
constexpr size_t kGeneratedSize = kSpillOffset + kSpillSize;
constexpr size_t kRegistryCapacity = 8;
constexpr size_t kDarwinSavedFrameSize = 16;
constexpr size_t kAndroidOutgoingStackSize =
    (kDarwinArtElfJniFixtureAndroidD4StackOffset + sizeof(uint64_t) + 15u) &
    ~size_t{15u};

static_assert(kDarwinArtElfJniFixtureDarwinRefStackOffset == 0u);
static_assert(kDarwinArtElfJniFixtureDarwinF4StackOffset == 8u);
static_assert(kDarwinArtElfJniFixtureDarwinF5StackOffset == 12u);
static_assert(kDarwinArtElfJniFixtureDarwinD4StackOffset == 16u);
static_assert(kDarwinArtElfJniFixtureAndroidRefStackOffset == 0u);
static_assert(kDarwinArtElfJniFixtureAndroidF4StackOffset == 8u);
static_assert(kDarwinArtElfJniFixtureAndroidF5StackOffset == 16u);
static_assert(kDarwinArtElfJniFixtureAndroidD4StackOffset == 24u);
static_assert(kAndroidOutgoingStackSize == 32u);

constexpr uint32_t EncodeUnsignedLoadStore(uint32_t opcode,
                                           uint32_t target_register,
                                           uint32_t base_register,
                                           size_t byte_offset,
                                           size_t scale) {
  return opcode |
         (static_cast<uint32_t>(byte_offset / scale) << 10) |
         (base_register << 5) | target_register;
}

constexpr uint32_t EncodeLdrX(uint32_t target_register,
                              uint32_t base_register,
                              size_t byte_offset) {
  return EncodeUnsignedLoadStore(0xf9400000u, target_register, base_register,
                                 byte_offset, sizeof(uint64_t));
}

constexpr uint32_t EncodeStrX(uint32_t target_register,
                              uint32_t base_register,
                              size_t byte_offset) {
  return EncodeUnsignedLoadStore(0xf9000000u, target_register, base_register,
                                 byte_offset, sizeof(uint64_t));
}

constexpr uint32_t EncodeLdrW(uint32_t target_register,
                              uint32_t base_register,
                              size_t byte_offset) {
  return EncodeUnsignedLoadStore(0xb9400000u, target_register, base_register,
                                 byte_offset, sizeof(uint32_t));
}

constexpr uint32_t EncodeStrW(uint32_t target_register,
                              uint32_t base_register,
                              size_t byte_offset) {
  return EncodeUnsignedLoadStore(0xb9000000u, target_register, base_register,
                                 byte_offset, sizeof(uint32_t));
}

constexpr uint32_t EncodeSubSp(size_t byte_count) {
  return 0xd10003ffu | (static_cast<uint32_t>(byte_count) << 10);
}

static_assert(kDarwinArtElfJniFixtureDarwinRefStackOffset % sizeof(uint64_t) == 0);
static_assert(kDarwinArtElfJniFixtureDarwinF4StackOffset % sizeof(uint32_t) == 0);
static_assert(kDarwinArtElfJniFixtureDarwinF5StackOffset % sizeof(uint32_t) == 0);
static_assert(kDarwinArtElfJniFixtureDarwinD4StackOffset % sizeof(uint64_t) == 0);
static_assert(kDarwinArtElfJniFixtureAndroidRefStackOffset % sizeof(uint64_t) == 0);
static_assert(kDarwinArtElfJniFixtureAndroidF4StackOffset % sizeof(uint32_t) == 0);
static_assert(kDarwinArtElfJniFixtureAndroidF5StackOffset % sizeof(uint32_t) == 0);
static_assert(kDarwinArtElfJniFixtureAndroidD4StackOffset % sizeof(uint64_t) == 0);

struct RegistryEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t native_add = 0;
  uintptr_t native_spill = 0;
  uint64_t generation = 0;
};

std::array<RegistryEntry, kRegistryCapacity> g_registry;
std::mutex g_registry_mutex;
std::atomic<uint64_t> g_next_generation{1};
std::atomic<size_t> g_live_count{0};

void Write32(uint8_t* destination, size_t offset, uint32_t value) {
  std::memcpy(destination + offset, &value, sizeof(value));
}

void Write64(uint8_t* destination, size_t offset, uintptr_t value) {
  static_assert(sizeof(value) == sizeof(uint64_t));
  std::memcpy(destination + offset, &value, sizeof(value));
}

bool Publish(const RegistryEntry& entry) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  for (RegistryEntry& slot : g_registry) {
    if (slot.generation == 0) {
      slot = entry;
      return true;
    }
  }
  return false;
}

void Unpublish(const RegistryEntry& entry) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  for (RegistryEntry& slot : g_registry) {
    if (slot.generation == entry.generation && slot.start == entry.start) {
      slot = {};
      return;
    }
  }
}

std::string ErrnoMessage(const char* operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

}  // namespace

struct FixtureTrampolineSet {
  void* mapping = nullptr;
  size_t mapping_size = 0;
  RegistryEntry registry;
};

FixtureTrampolineSet* CreateFixtureTrampolines(void* proxy_jni_env,
                                               void* native_add,
                                               void* native_spill,
                                               std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (proxy_jni_env == nullptr || native_add == nullptr || native_spill == nullptr) {
    if (error != nullptr) {
      *error = "fixture trampoline target is null";
    }
    return nullptr;
  }
  const long page_size_result = sysconf(_SC_PAGESIZE);
  if (page_size_result <= 0 ||
      static_cast<size_t>(page_size_result) < kGeneratedSize) {
    if (error != nullptr) {
      *error = "invalid Darwin executable page size";
    }
    return nullptr;
  }
  const size_t page_size = static_cast<size_t>(page_size_result);
  void* mapping = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    if (error != nullptr) {
      *error = ErrnoMessage("mmap trampoline page");
    }
    return nullptr;
  }
  auto* bytes = static_cast<uint8_t*>(mapping);

  // nativeAdd: all arguments are register-resident. Substitute only x0 with
  // the proxy JNIEnv and tail-call the Android AAPCS64 function.
  Write32(bytes, kAddOffset + 0, 0x58000080u);   // ldr x0, +16
  Write32(bytes, kAddOffset + 4, 0x580000b0u);   // ldr x16, +20
  Write32(bytes, kAddOffset + 8, 0xd61f0200u);   // br x16
  Write32(bytes, kAddOffset + 12, 0xd503201fu);  // nop
  Write64(bytes, kAddOffset + 16, reinterpret_cast<uintptr_t>(proxy_jni_env));
  Write64(bytes, kAddOffset + 24, reinterpret_cast<uintptr_t>(native_add));

  // nativeSpill: preserve Darwin's incoming SP, create Android's four 8-byte
  // outgoing slots, and move only the stack-resident tail. x1-x7 and v0-v7
  // remain untouched; x0 is replaced immediately before the Android call.
  constexpr uint32_t kX9 = 9;
  constexpr uint32_t kX29 = 29;
  constexpr uint32_t kSp = 31;
  const std::array<uint32_t, kSpillInstructionCount> spill = {
      0xa9bf7bfdu,  // stp x29, x30, [sp, #-16]!
      0x910003fdu,  // mov x29, sp
      EncodeSubSp(kAndroidOutgoingStackSize),
      EncodeLdrX(kX9, kX29, kDarwinSavedFrameSize +
                                 kDarwinArtElfJniFixtureDarwinRefStackOffset),
      EncodeStrX(kX9, kSp, kDarwinArtElfJniFixtureAndroidRefStackOffset),
      EncodeLdrW(kX9, kX29, kDarwinSavedFrameSize +
                                 kDarwinArtElfJniFixtureDarwinF4StackOffset),
      EncodeStrW(kX9, kSp, kDarwinArtElfJniFixtureAndroidF4StackOffset),
      EncodeLdrW(kX9, kX29, kDarwinSavedFrameSize +
                                 kDarwinArtElfJniFixtureDarwinF5StackOffset),
      EncodeStrW(kX9, kSp, kDarwinArtElfJniFixtureAndroidF5StackOffset),
      EncodeLdrX(kX9, kX29, kDarwinSavedFrameSize +
                                 kDarwinArtElfJniFixtureDarwinD4StackOffset),
      EncodeStrX(kX9, kSp, kDarwinArtElfJniFixtureAndroidD4StackOffset),
      0x580000e0u,  // ldr x0, +28            proxy JNIEnv literal
      0x58000110u,  // ldr x16, +32           Android target literal
      0xd63f0200u,  // blr x16
      0x910003bfu,  // mov sp, x29
      0xa8c17bfdu,  // ldp x29, x30, [sp], #16
      0xd65f03c0u,  // ret
      0xd503201fu,  // nop / literal alignment
  };
  for (size_t index = 0; index < spill.size(); ++index) {
    Write32(bytes, kSpillOffset + index * sizeof(uint32_t), spill[index]);
  }
  Write64(bytes, kSpillOffset + 72, reinterpret_cast<uintptr_t>(proxy_jni_env));
  Write64(bytes, kSpillOffset + 80, reinterpret_cast<uintptr_t>(native_spill));

  __builtin___clear_cache(reinterpret_cast<char*>(mapping),
                          reinterpret_cast<char*>(mapping) + kGeneratedSize);
  if (mprotect(mapping, page_size, PROT_READ | PROT_EXEC) != 0) {
    if (error != nullptr) {
      *error = ErrnoMessage("mprotect trampoline page RX");
    }
    munmap(mapping, page_size);
    return nullptr;
  }

  auto* trampolines = new (std::nothrow) FixtureTrampolineSet;
  if (trampolines == nullptr) {
    if (error != nullptr) {
      *error = "allocate fixture trampoline owner";
    }
    munmap(mapping, page_size);
    return nullptr;
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(mapping);
  trampolines->mapping = mapping;
  trampolines->mapping_size = page_size;
  trampolines->registry = {
      start,
      start + kGeneratedSize,
      start + kAddOffset,
      start + kSpillOffset,
      g_next_generation.fetch_add(1, std::memory_order_relaxed),
  };
  if (trampolines->registry.generation == 0 ||
      !Publish(trampolines->registry)) {
    if (error != nullptr) {
      *error = "publish fixture trampoline range";
    }
    munmap(mapping, page_size);
    delete trampolines;
    return nullptr;
  }
  g_live_count.fetch_add(1, std::memory_order_relaxed);
  return trampolines;
}

void DestroyFixtureTrampolines(FixtureTrampolineSet* trampolines) {
  if (trampolines == nullptr) {
    return;
  }
  Unpublish(trampolines->registry);
  g_live_count.fetch_sub(1, std::memory_order_relaxed);
  if (trampolines->mapping != nullptr && trampolines->mapping_size != 0) {
    munmap(trampolines->mapping, trampolines->mapping_size);
  }
  delete trampolines;
}

void* FixtureNativeAddEntry(const FixtureTrampolineSet* trampolines) {
  return trampolines == nullptr
             ? nullptr
             : reinterpret_cast<void*>(trampolines->registry.native_add);
}

void* FixtureNativeSpillEntry(const FixtureTrampolineSet* trampolines) {
  return trampolines == nullptr
             ? nullptr
             : reinterpret_cast<void*>(trampolines->registry.native_spill);
}

uint64_t FixtureTrampolineGeneration(const FixtureTrampolineSet* trampolines) {
  return trampolines == nullptr ? 0 : trampolines->registry.generation;
}

size_t FixtureTrampolineLiveCount() {
  return g_live_count.load(std::memory_order_relaxed);
}

uint32_t FixtureTrampolineEntryMask(const void* pointer) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  for (const RegistryEntry& entry : g_registry) {
    if (entry.generation == 0 || address < entry.start || address >= entry.end) {
      continue;
    }
    if (address == entry.native_add) {
      return kFixtureNativeAddEntryMask;
    }
    if (address == entry.native_spill) {
      return kFixtureNativeSpillEntryMask;
    }
  }
  return 0;
}

bool IsFixtureTrampolineEntry(const void* pointer) {
  return FixtureTrampolineEntryMask(pointer) != 0;
}

}  // namespace darwin_art::android_jni
