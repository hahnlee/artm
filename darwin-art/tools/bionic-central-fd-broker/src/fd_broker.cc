#include "darwin_art_bionic_fd_broker.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kTokenMarker = 0x40000000U;
constexpr uint32_t kSlotBits = 10;
constexpr uint32_t kSlotMask = (1U << kSlotBits) - 1;
constexpr uint32_t kGenerationMask = (1U << 20) - 1;
constexpr size_t kSlotCount = 1U << kSlotBits;
constexpr size_t kCopyChunk = 4096;

bool ValidKind(DarwinArtFdKind kind) {
  return kind >= DARWIN_ART_FD_FS_FILE && kind <= DARWIN_ART_FD_SOCKET;
}

struct Owner {
  DarwinArtFdOwnerHandle handle = 0;
  DarwinArtFdKind kind = DARWIN_ART_FD_FS_FILE;
  DarwinArtFdOwnerV1 callbacks{};
  bool draining = false;
  size_t live = 0;
  size_t active = 0;
};

struct Slot {
  uint32_t generation = 1;
  bool retired = false;
  bool live = false;
  bool closing = false;
  DarwinArtFdOwnerHandle owner = 0;
  uint64_t object = 0;
  size_t active = 0;
  std::condition_variable changed;
};

struct Lease {
  size_t slot_index = 0;
  uint32_t generation = 0;
  DarwinArtFdOwnerHandle owner_handle = 0;
  DarwinArtFdKind kind = DARWIN_ART_FD_FS_FILE;
  DarwinArtFdOwnerV1 callbacks{};
  uint64_t object = 0;
};

struct DarwinArtFdBrokerImpl {
  std::mutex mutex;
  std::array<Slot, kSlotCount> slots;
  std::vector<size_t> free_slots;
  std::map<DarwinArtFdOwnerHandle, Owner> owners;
  DarwinArtFdOwnerHandle next_owner = 1;
  bool destroying = false;

  DarwinArtFdBrokerImpl() {
    free_slots.reserve(kSlotCount);
    for (size_t index = kSlotCount; index-- > 0;) {
      free_slots.push_back(index);
    }
  }
};

DarwinArtFdBrokerImpl *Impl(DarwinArtFdBroker *broker) {
  return reinterpret_cast<DarwinArtFdBrokerImpl *>(broker);
}

uint32_t MakeToken(size_t slot, uint32_t generation) {
  return kTokenMarker | (generation << kSlotBits) | static_cast<uint32_t>(slot);
}

bool DecodeToken(int fd, size_t *slot, uint32_t *generation) {
  const uint32_t token = static_cast<uint32_t>(fd);
  if (fd < 0 || (token & kTokenMarker) == 0 || (token & 0x80000000U) != 0) {
    return false;
  }
  const uint32_t decoded_generation = (token >> kSlotBits) & kGenerationMask;
  if (decoded_generation == 0) {
    return false;
  }
  *slot = token & kSlotMask;
  *generation = decoded_generation;
  return *slot < kSlotCount;
}

DarwinArtFdBrokerStatus AcquireLocked(DarwinArtFdBrokerImpl *broker, int fd,
                                      std::optional<DarwinArtFdKind> kind,
                                      Lease *lease) {
  size_t slot_index = 0;
  uint32_t generation = 0;
  if (!DecodeToken(fd, &slot_index, &generation)) {
    return DARWIN_ART_FD_BROKER_STALE;
  }
  Slot &slot = broker->slots[slot_index];
  if (!slot.live || slot.closing || slot.generation != generation) {
    return DARWIN_ART_FD_BROKER_STALE;
  }
  auto found = broker->owners.find(slot.owner);
  if (found == broker->owners.end()) {
    return DARWIN_ART_FD_BROKER_STALE;
  }
  Owner &owner = found->second;
  if (kind.has_value() && owner.kind != kind.value()) {
    return DARWIN_ART_FD_BROKER_WRONG_KIND;
  }
  ++slot.active;
  ++owner.active;
  *lease = Lease{slot_index, generation,      owner.handle,
                 owner.kind, owner.callbacks, slot.object};
  return DARWIN_ART_FD_BROKER_OK;
}

void ReleaseLocked(DarwinArtFdBrokerImpl *broker, const Lease &lease) {
  Slot &slot = broker->slots[lease.slot_index];
  auto owner = broker->owners.find(lease.owner_handle);
  if (slot.generation != lease.generation || slot.active == 0 ||
      owner == broker->owners.end() || owner->second.active == 0) {
    std::terminate();
  }
  --slot.active;
  --owner->second.active;
  if (slot.active == 0) {
    slot.changed.notify_all();
  }
}

void SetResult(DarwinArtFdIoResult *result, intptr_t value, int android_errno) {
  result->value = value;
  result->android_errno = android_errno;
}

DarwinArtFdBrokerStatus
CloseImpl(DarwinArtFdBrokerImpl *broker,
          std::optional<DarwinArtFdOwnerHandle> expected, int fd,
          DarwinArtFdIoResult *result) {
  if (broker == nullptr || result == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  size_t slot_index = 0;
  uint32_t generation = 0;
  if (!DecodeToken(fd, &slot_index, &generation)) {
    return DARWIN_ART_FD_BROKER_STALE;
  }

  DarwinArtFdOwnerV1 callbacks{};
  uint64_t object = 0;
  {
    std::unique_lock lock(broker->mutex);
    Slot &slot = broker->slots[slot_index];
    if (!slot.live || slot.closing || slot.generation != generation) {
      return DARWIN_ART_FD_BROKER_STALE;
    }
    if (expected.has_value() && slot.owner != expected.value()) {
      return DARWIN_ART_FD_BROKER_WRONG_OWNER;
    }
    auto owner = broker->owners.find(slot.owner);
    if (owner == broker->owners.end()) {
      return DARWIN_ART_FD_BROKER_STALE;
    }
    slot.closing = true;
    slot.changed.wait(lock, [&] { return slot.active == 0; });
    callbacks = owner->second.callbacks;
    object = slot.object;
  }

  int android_errno = 0;
  const int close_result =
      callbacks.close == nullptr
          ? 0
          : callbacks.close(callbacks.context, object, &android_errno);
  {
    std::lock_guard lock(broker->mutex);
    Slot &slot = broker->slots[slot_index];
    auto owner = broker->owners.find(slot.owner);
    if (!slot.live || !slot.closing || slot.generation != generation ||
        slot.active != 0 || owner == broker->owners.end() ||
        owner->second.live == 0) {
      std::terminate();
    }
    slot.live = false;
    slot.closing = false;
    slot.owner = 0;
    slot.object = 0;
    --owner->second.live;
    if (slot.generation == kGenerationMask) {
      slot.retired = true;
    } else {
      ++slot.generation;
      broker->free_slots.push_back(slot_index);
    }
  }
  SetResult(result, close_result, close_result == 0 ? 0 : android_errno);
  return DARWIN_ART_FD_BROKER_OK;
}

template <typename Invoke>
DarwinArtFdBrokerStatus DispatchOne(DarwinArtFdBrokerImpl *broker, int fd,
                                    std::optional<DarwinArtFdKind> kind,
                                    DarwinArtFdIoResult *result,
                                    Invoke invoke) {
  if (broker == nullptr || result == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  Lease lease;
  {
    std::lock_guard lock(broker->mutex);
    const DarwinArtFdBrokerStatus status =
        AcquireLocked(broker, fd, kind, &lease);
    if (status != DARWIN_ART_FD_BROKER_OK) {
      return status;
    }
  }
  int android_errno = 0;
  const intptr_t value = invoke(lease, &android_errno);
  {
    std::lock_guard lock(broker->mutex);
    ReleaseLocked(broker, lease);
  }
  SetResult(result, value, value < 0 ? android_errno : 0);
  return DARWIN_ART_FD_BROKER_OK;
}

} // namespace

extern "C" DarwinArtFdBroker *darwin_art_fd_broker_create() {
  return reinterpret_cast<DarwinArtFdBroker *>(new DarwinArtFdBrokerImpl());
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_destroy(DarwinArtFdBroker *broker) {
  if (broker == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  {
    std::lock_guard lock(impl->mutex);
    for (const Slot &slot : impl->slots) {
      if (slot.live || slot.active != 0 || slot.closing) {
        return DARWIN_ART_FD_BROKER_BUSY;
      }
    }
    if (!impl->owners.empty()) {
      return DARWIN_ART_FD_BROKER_BUSY;
    }
    impl->destroying = true;
  }
  delete impl;
  return DARWIN_ART_FD_BROKER_OK;
}

extern "C" DarwinArtFdBrokerStatus darwin_art_fd_broker_install_owner(
    DarwinArtFdBroker *broker, DarwinArtFdKind kind,
    const DarwinArtFdOwnerV1 *callbacks, DarwinArtFdOwnerHandle *owner) {
  if (broker == nullptr || callbacks == nullptr || owner == nullptr ||
      !ValidKind(kind) ||
      callbacks->abi_version != DARWIN_ART_FD_OWNER_ABI_V1 ||
      callbacks->struct_size < sizeof(DarwinArtFdOwnerV1)) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  std::lock_guard lock(impl->mutex);
  if (impl->destroying || impl->next_owner == 0) {
    return DARWIN_ART_FD_BROKER_EXHAUSTED;
  }
  const DarwinArtFdOwnerHandle handle = impl->next_owner++;
  impl->owners.emplace(handle, Owner{handle, kind, *callbacks, false, 0, 0});
  *owner = handle;
  return DARWIN_ART_FD_BROKER_OK;
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_uninstall_owner(DarwinArtFdBroker *broker,
                                     DarwinArtFdOwnerHandle owner) {
  if (broker == nullptr || owner == 0) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  std::lock_guard lock(impl->mutex);
  auto found = impl->owners.find(owner);
  if (found == impl->owners.end()) {
    return DARWIN_ART_FD_BROKER_STALE;
  }
  found->second.draining = true;
  if (found->second.live != 0 || found->second.active != 0) {
    return DARWIN_ART_FD_BROKER_BUSY;
  }
  impl->owners.erase(found);
  return DARWIN_ART_FD_BROKER_OK;
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_publish(DarwinArtFdBroker *broker,
                             DarwinArtFdOwnerHandle owner, uint64_t object,
                             int *guest_fd) {
  if (broker == nullptr || owner == 0 || guest_fd == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  std::lock_guard lock(impl->mutex);
  auto found = impl->owners.find(owner);
  if (found == impl->owners.end()) {
    return DARWIN_ART_FD_BROKER_STALE;
  }
  if (found->second.draining) {
    return DARWIN_ART_FD_BROKER_DRAINING;
  }
  while (!impl->free_slots.empty() &&
         impl->slots[impl->free_slots.back()].retired) {
    impl->free_slots.pop_back();
  }
  if (impl->free_slots.empty()) {
    return DARWIN_ART_FD_BROKER_EXHAUSTED;
  }
  const size_t slot_index = impl->free_slots.back();
  impl->free_slots.pop_back();
  Slot &slot = impl->slots[slot_index];
  if (slot.live || slot.closing || slot.active != 0 || slot.retired) {
    std::terminate();
  }
  slot.live = true;
  slot.owner = owner;
  slot.object = object;
  ++found->second.live;
  *guest_fd = static_cast<int>(MakeToken(slot_index, slot.generation));
  return DARWIN_ART_FD_BROKER_OK;
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_close(DarwinArtFdBroker *broker, int guest_fd,
                           DarwinArtFdIoResult *result) {
  return CloseImpl(Impl(broker), std::nullopt, guest_fd, result);
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_close_owned(DarwinArtFdBroker *broker,
                                 DarwinArtFdOwnerHandle owner, int guest_fd,
                                 DarwinArtFdIoResult *result) {
  if (owner == 0) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  return CloseImpl(Impl(broker), owner, guest_fd, result);
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_read(DarwinArtFdBroker *broker, int guest_fd, void *bytes,
                          size_t count, DarwinArtFdIoResult *result) {
  if (count != 0 && bytes == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  return DispatchOne(Impl(broker), guest_fd, std::nullopt, result,
                     [&](const Lease &lease, int *error) {
                       if (lease.callbacks.read == nullptr) {
                         *error = 9;
                         return static_cast<intptr_t>(-1);
                       }
                       return lease.callbacks.read(lease.callbacks.context,
                                                   lease.object, bytes, count,
                                                   error);
                     });
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_write(DarwinArtFdBroker *broker, int guest_fd,
                           const void *bytes, size_t count,
                           DarwinArtFdIoResult *result) {
  if (count != 0 && bytes == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  return DispatchOne(Impl(broker), guest_fd, std::nullopt, result,
                     [&](const Lease &lease, int *error) {
                       if (lease.callbacks.write == nullptr) {
                         *error = 9;
                         return static_cast<intptr_t>(-1);
                       }
                       return lease.callbacks.write(lease.callbacks.context,
                                                    lease.object, bytes, count,
                                                    error);
                     });
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_ioctl(DarwinArtFdBroker *broker, int guest_fd,
                           DarwinArtFdKind required_kind, uint64_t request,
                           void *argument, DarwinArtFdIoResult *result) {
  if (!ValidKind(required_kind)) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  return DispatchOne(
      Impl(broker), guest_fd, required_kind, result,
      [&](const Lease &lease, int *error) {
        if (lease.callbacks.ioctl == nullptr) {
          *error = 25;
          return static_cast<intptr_t>(-1);
        }
        return static_cast<intptr_t>(lease.callbacks.ioctl(
            lease.callbacks.context, lease.object, request, argument, error));
      });
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_poll(DarwinArtFdBroker *broker,
                          DarwinArtFdPollEntry *entries, size_t count,
                          DarwinArtFdIoResult *result) {
  if (broker == nullptr || result == nullptr ||
      (count != 0 && entries == nullptr)) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  intptr_t ready = 0;
  for (size_t index = 0; index < count; ++index) {
    entries[index].revents = 0;
    if (entries[index].fd < 0) {
      continue;
    }
    DarwinArtFdIoResult one{};
    const DarwinArtFdBrokerStatus status = DispatchOne(
        impl, entries[index].fd, std::nullopt, &one,
        [&](const Lease &lease, int *error) {
          if (lease.callbacks.poll == nullptr) {
            entries[index].revents = 0x20;
            *error = 0;
            return static_cast<intptr_t>(1);
          }
          return static_cast<intptr_t>(lease.callbacks.poll(
              lease.callbacks.context, lease.object, entries[index].events,
              &entries[index].revents, error));
        });
    if (status == DARWIN_ART_FD_BROKER_STALE) {
      entries[index].revents = 0x20;
      ++ready;
    } else if (status != DARWIN_ART_FD_BROKER_OK) {
      return status;
    } else if (one.value > 0 || entries[index].revents != 0) {
      ++ready;
    }
  }
  SetResult(result, ready, 0);
  return DARWIN_ART_FD_BROKER_OK;
}

extern "C" DarwinArtFdBrokerStatus
darwin_art_fd_broker_sendfile(DarwinArtFdBroker *broker, int output_fd,
                              int input_fd, size_t count,
                              DarwinArtFdIoResult *result) {
  if (broker == nullptr || result == nullptr) {
    return DARWIN_ART_FD_BROKER_INVALID_ARGUMENT;
  }
  DarwinArtFdBrokerImpl *impl = Impl(broker);
  Lease input;
  Lease output;
  {
    std::lock_guard lock(impl->mutex);
    DarwinArtFdBrokerStatus status =
        AcquireLocked(impl, input_fd, DARWIN_ART_FD_FS_FILE, &input);
    if (status != DARWIN_ART_FD_BROKER_OK) {
      return status;
    }
    status = AcquireLocked(impl, output_fd, std::nullopt, &output);
    if (status != DARWIN_ART_FD_BROKER_OK) {
      ReleaseLocked(impl, input);
      return status;
    }
    if (output.kind != DARWIN_ART_FD_FS_FILE &&
        output.kind != DARWIN_ART_FD_SOCKET) {
      ReleaseLocked(impl, output);
      ReleaseLocked(impl, input);
      return DARWIN_ART_FD_BROKER_WRONG_KIND;
    }
  }

  std::array<uint8_t, kCopyChunk> buffer{};
  size_t transferred = 0;
  int android_errno = 0;
  while (transferred < count) {
    const size_t requested = std::min(buffer.size(), count - transferred);
    if (input.callbacks.read == nullptr || output.callbacks.write == nullptr) {
      android_errno = 9;
      transferred = static_cast<size_t>(-1);
      break;
    }
    const intptr_t read =
        input.callbacks.read(input.callbacks.context, input.object,
                             buffer.data(), requested, &android_errno);
    if (read > static_cast<intptr_t>(requested)) {
      android_errno = 5;
      transferred = static_cast<size_t>(-1);
      break;
    }
    if (read <= 0) {
      if (read < 0) {
        transferred = static_cast<size_t>(-1);
      }
      break;
    }
    const intptr_t written = output.callbacks.write(
        output.callbacks.context, output.object, buffer.data(),
        static_cast<size_t>(read), &android_errno);
    if (written < 0 || written > read) {
      if (written > read) {
        android_errno = 5;
      }
      transferred = static_cast<size_t>(-1);
      break;
    }
    transferred += static_cast<size_t>(written);
    if (written < read) {
      break;
    }
  }
  {
    std::lock_guard lock(impl->mutex);
    ReleaseLocked(impl, output);
    ReleaseLocked(impl, input);
  }
  if (transferred == static_cast<size_t>(-1)) {
    SetResult(result, -1, android_errno);
  } else {
    SetResult(result, static_cast<intptr_t>(transferred), 0);
  }
  return DARWIN_ART_FD_BROKER_OK;
}
