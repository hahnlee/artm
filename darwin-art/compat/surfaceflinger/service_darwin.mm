#include "service_darwin.h"

#include "transaction_bridge.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <libproc.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/un.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

extern "C" int darwin_art_bionic_socket_broker_close(int fd);
extern "C" int darwin_art_bionic_fd_export_for_scm(int guest_fd);
extern "C" int darwin_art_bionic_fd_import_from_scm(int host_fd);
extern "C" int darwin_art_android_metal_shared_event_fence_fd(
    void* shared_event, uint64_t signal_value);

namespace {

constexpr std::array<char, 8> kRequestMagic{'D', 'A', 'R', 'T', 'S', 'F', '0', '7'};
constexpr std::array<char, 8> kResponseMagic{'D', 'A', 'R', 'T', 'S', 'F', 'R', '7'};
constexpr uint32_t kProtocolVersion = 7;
constexpr uint32_t kMaximumLayers = 4096;

struct RequestHeader {
  char magic[8];
  uint32_t version;
  uint32_t process_id;
  uint32_t target_iosurface_id;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t layer_count;
  uint32_t has_producer_fence;
  uint64_t transaction_id;
};

struct WireLayer {
  uint32_t owner_process_id;
  uint32_t layer_id;
  uint32_t parent_owner_process_id;
  uint32_t parent_id;
  uint32_t relative_parent_owner_process_id;
  uint32_t relative_parent_id;
  uint32_t iosurface_id;
  uint32_t width;
  uint32_t height;
  uint64_t what;
  uint32_t flags;
  uint32_t mask;
  uint32_t transform;
  uint32_t producer_bottom_left;
  int32_t source_left;
  int32_t source_top;
  int32_t source_right;
  int32_t source_bottom;
  int32_t destination_left;
  int32_t destination_top;
  int32_t destination_right;
  int32_t destination_bottom;
  int32_t z;
  float alpha;
};

struct ResponseHeader {
  char magic[8];
  uint32_t version;
  int32_t status;
  uint32_t reserved;
  uint32_t has_completion_fence;
};

static_assert(std::is_trivially_copyable_v<RequestHeader>);
static_assert(std::is_trivially_copyable_v<WireLayer>);
static_assert(std::is_trivially_copyable_v<ResponseHeader>);

struct CompositionJob {
  RequestHeader header{};
  std::vector<WireLayer> incoming;
  int producer_descriptor = -1;
  int completion_descriptor = -1;
};

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t result = write(fd, bytes, size);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) return false;
    bytes += result;
    size -= static_cast<size_t>(result);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size != 0) {
    const ssize_t result = read(fd, bytes, size);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) return false;
    bytes += result;
    size -= static_cast<size_t>(result);
  }
  return true;
}

bool SendDescriptor(int socket_fd, int descriptor) {
  char marker = descriptor >= 0 ? 1 : 0;
  iovec vector{.iov_base = &marker, .iov_len = sizeof(marker)};
  std::array<char, CMSG_SPACE(sizeof(int))> control{};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  if (descriptor >= 0) {
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
  }
  for (;;) {
    const ssize_t sent = sendmsg(socket_fd, &message, 0);
    if (sent < 0 && errno == EINTR) continue;
    return sent == static_cast<ssize_t>(sizeof(marker));
  }
}

bool ReceiveDescriptor(int socket_fd, bool expected, int* descriptor) {
  if (descriptor == nullptr) return false;
  *descriptor = -1;
  char marker = 0;
  iovec vector{.iov_base = &marker, .iov_len = sizeof(marker)};
  std::array<char, CMSG_SPACE(sizeof(int))> control{};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  ssize_t received = -1;
  do {
    received = recvmsg(socket_fd, &message, 0);
  } while (received < 0 && errno == EINTR);
  if (received != static_cast<ssize_t>(sizeof(marker)) ||
      (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
    return false;
  }
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len >= CMSG_LEN(sizeof(int))) {
      std::memcpy(descriptor, CMSG_DATA(header), sizeof(*descriptor));
      break;
    }
  }
  if ((marker != 0) != expected || ((*descriptor >= 0) != expected)) {
    if (*descriptor >= 0) close(*descriptor);
    *descriptor = -1;
    return false;
  }
  if (*descriptor >= 0) {
    const int flags = fcntl(*descriptor, F_GETFD);
    if (flags < 0 || fcntl(*descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
      close(*descriptor);
      *descriptor = -1;
      return false;
    }
  }
  return true;
}

int Connect(const char* path) {
  if (path == nullptr || path[0] == '\0') return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (std::strlen(path) >= sizeof(address.sun_path)) return -1;
  std::memcpy(address.sun_path, path, std::strlen(path) + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

struct RetainedLayer {
  uint32_t submitting_process_id = 0;
  WireLayer layer{};
};

struct TargetState {
  uint32_t width = 0;
  uint32_t height = 0;
  std::map<uint32_t, RetainedLayer> layers;
};

struct ServiceState {
  std::mutex mutex;
  std::map<uint32_t, TargetState> targets;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> global_layers;
  uint32_t next_global_layer = 1;
};

ServiceState& State() {
  static ServiceState state;
  return state;
}

uint32_t GlobalLayerId(ServiceState& state, uint32_t process_id,
                       uint32_t layer_id) {
  if (layer_id == 0) return 0;
  const auto key = std::make_pair(process_id, layer_id);
  auto [found, inserted] =
      state.global_layers.emplace(key, state.next_global_layer);
  if (inserted) ++state.next_global_layer;
  return found->second;
}

bool ProcessAlive(uint32_t process_id) {
  if (process_id == 0) return false;
  if (kill(static_cast<pid_t>(process_id), 0) == 0) {
    // kill(pid, 0) also succeeds for a zombie. Android SurfaceFlinger drops a
    // client's layers when its Binder process dies; retaining a Darwin zombie
    // here leaves Chromium's final fullscreen renderer buffer above the new
    // tab-hub frame until the parent eventually reaps it.
    proc_bsdinfo info{};
    const int bytes = proc_pidinfo(static_cast<int>(process_id),
                                   PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    return bytes != sizeof(info) || info.pbi_status != SZOMB;
  }
  return errno == EPERM;
}

bool DestroyProcessLayers(ServiceState& state, uint32_t process_id) {
  std::vector<uint32_t> layer_ids;
  for (const auto& [key, global_id] : state.global_layers) {
    if (key.first == process_id) layer_ids.push_back(global_id);
  }
  if (!layer_ids.empty() &&
      !darwin_art_surfaceflinger_destroy_layer_handles(layer_ids.data(),
                                                       layer_ids.size())) {
    return false;
  }
  for (auto iterator = state.global_layers.begin();
       iterator != state.global_layers.end();) {
    if (iterator->first.first == process_id)
      iterator = state.global_layers.erase(iterator);
    else
      ++iterator;
  }
  return true;
}

void SignalCompletion(int descriptor, bool successful) {
  if (descriptor < 0) return;
  if (successful) {
    constexpr uint64_t kSignaledFence =
        UINT64_C(0x44415257494e4653);  // "DARWINFS"
    ssize_t written = -1;
    do {
      written = write(descriptor, &kSignaledFence, sizeof(kSignaledFence));
    } while (written < 0 && errno == EINTR);
  }
  close(descriptor);
}

void ProcessRequest(CompositionJob job);

// SurfaceFlinger applies transactions in receive order and composes from the
// latched state on one compositor thread.  The old Darwin shim detached one
// thread per request, which allowed Metal submissions from transaction N+1 to
// overtake N after the retained-state mutex was released.  Keep the central
// service's ingress bounded and FIFO; the worker is intentionally leaked for
// the profile lifetime because the service has no shutdown transaction.
class CompositionQueue {
 public:
  static constexpr size_t kCapacity = 128;

  bool Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return true;
    try {
      std::thread([this] { Run(); }).detach();
    } catch (...) {
      return false;
    }
    started_ = true;
    return true;
  }

  bool Enqueue(CompositionJob job) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return false;
    // Waiting here is the SurfaceFlinger-style backpressure boundary: a
    // producer cannot create an unbounded set of composition threads/fences,
    // while accepted requests retain strict transaction order.
    not_full_.wait(lock, [this] { return queue_.size() < kCapacity; });
    queue_.push_back(std::move(job));
    not_empty_.notify_one();
    return true;
  }

 private:
  void Run() {
    for (;;) {
      CompositionJob job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty(); });
        job = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
      }
      ProcessRequest(std::move(job));
    }
  }

  std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<CompositionJob> queue_;
  bool started_ = false;
};

CompositionQueue& CompositionJobs() {
  // The worker must outlive all client/service threads and is therefore
  // intentionally process-lifetime storage rather than a destructed static.
  static auto* queue = new CompositionQueue();
  return *queue;
}

bool WaitForProducer(int descriptor) {
  if (descriptor < 0) return true;
  // Android SurfaceFlinger does not let one broken acquire fence stall the
  // compositor forever. Keep the previous latched target visible and bound
  // this compatibility worker's wait; the caller's completion fence is then
  // failed and the retained state remains unchanged because ProcessRequest
  // waits before applying its incoming snapshot.
  constexpr auto kFenceWaitBudget = std::chrono::milliseconds(250);
  constexpr int kFenceWaitSliceMs = 4;
  const auto deadline = std::chrono::steady_clock::now() + kFenceWaitBudget;
  pollfd waiter{.fd = descriptor, .events = POLLIN | POLLHUP, .revents = 0};
  int result = -1;
  for (;;) {
    do {
      result = poll(&waiter, 1, kFenceWaitSliceMs);
    } while (result < 0 && errno == EINTR);
    if (result != 0 || std::chrono::steady_clock::now() >= deadline) break;
  }
  if (result == 0) {
    std::fprintf(stderr,
                 "ART SurfaceFlinger: acquire fence timed out after %lld ms\n",
                 static_cast<long long>(kFenceWaitBudget.count()));
    close(descriptor);
    return false;
  }
  if (result <= 0 || (waiter.revents & (POLLERR | POLLNVAL)) != 0) {
    close(descriptor);
    return false;
  }
  std::array<uint8_t, sizeof(uint64_t)> marker{};
  ssize_t received = -1;
  do {
    received = read(descriptor, marker.data(), marker.size());
  } while (received < 0 && errno == EINTR);
  close(descriptor);
  return received > 0;
}

void MergeRetainedLayer(WireLayer& destination, const WireLayer& source) {
  destination.owner_process_id = source.owner_process_id;
  destination.layer_id = source.layer_id;
  destination.what = source.what;
  if ((source.what & DARWIN_ART_SF_REPARENT) != 0) {
    destination.parent_owner_process_id = source.parent_owner_process_id;
    destination.parent_id = source.parent_id;
  }
  if ((source.what & DARWIN_ART_SF_RELATIVE_LAYER_CHANGED) != 0) {
    destination.relative_parent_owner_process_id =
        source.relative_parent_owner_process_id;
    destination.relative_parent_id = source.relative_parent_id;
  }
  if ((source.what & DARWIN_ART_SF_LAYER_CHANGED) != 0) {
    destination.relative_parent_owner_process_id = 0;
    destination.relative_parent_id = 0;
  }
  if ((source.what & DARWIN_ART_SF_FLAGS_CHANGED) != 0) {
    destination.flags =
        (destination.flags & ~source.mask) | (source.flags & source.mask);
  }
  if ((source.what & DARWIN_ART_SF_BUFFER_TRANSFORM_CHANGED) != 0)
    destination.transform = source.transform;
  if ((source.what & DARWIN_ART_SF_LAYER_CHANGED) != 0)
    destination.z = source.z;
  if ((source.what & DARWIN_ART_SF_ALPHA_CHANGED) != 0)
    destination.alpha = source.alpha;
  if ((source.what & DARWIN_ART_SF_BUFFER_CHANGED) != 0) {
    destination.iosurface_id = source.iosurface_id;
    destination.width = source.width;
    destination.height = source.height;
    destination.producer_bottom_left = source.producer_bottom_left;
    destination.source_left = source.source_left;
    destination.source_top = source.source_top;
    destination.source_right = source.source_right;
    destination.source_bottom = source.source_bottom;
  }
  if ((source.what & (DARWIN_ART_SF_POSITION_CHANGED |
                      DARWIN_ART_SF_DESTINATION_FRAME_CHANGED)) != 0) {
    destination.destination_left = source.destination_left;
    destination.destination_top = source.destination_top;
    destination.destination_right = source.destination_right;
    destination.destination_bottom = source.destination_bottom;
  }
}

void DebugSurfacePixels(const char* phase, uint64_t transaction_id,
                        uint32_t layer_id, IOSurfaceRef surface) {
  if (surface == nullptr ||
      std::getenv("DARWIN_ART_DEBUG_SURFACECONTROL_PIXELS") == nullptr) {
    return;
  }
  if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess) {
    std::fprintf(stderr,
                 "ART SurfaceFlinger: %s pixels transaction=%llu layer=%u "
                 "lock=failed\n",
                 phase, static_cast<unsigned long long>(transaction_id),
                 layer_id);
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t width = IOSurfaceGetWidth(surface);
  const size_t height = IOSurfaceGetHeight(surface);
  const size_t row_bytes = IOSurfaceGetBytesPerRow(surface);
  const size_t element_bytes = IOSurfaceGetBytesPerElement(surface);
  uint64_t hash = UINT64_C(14695981039346656037);
  if (bytes != nullptr && element_bytes >= 4) {
    const size_t visible_row_bytes = width * element_bytes;
    for (size_t row = 0; row < height; ++row) {
      const uint8_t* pixel = bytes + row * row_bytes;
      for (size_t index = 0; index < visible_row_bytes; ++index) {
        hash ^= pixel[index];
        hash *= UINT64_C(1099511628211);
      }
    }
  }
  std::fprintf(stderr,
               "ART SurfaceFlinger: %s pixels transaction=%llu layer=%u "
               "surface=%u size=%zux%zu element=%zu hash=%016llx",
               phase, static_cast<unsigned long long>(transaction_id), layer_id,
               IOSurfaceGetID(surface), width, height, element_bytes,
               static_cast<unsigned long long>(hash));
  for (double y : std::array<double, 3>{0.25, 0.5, 0.75}) {
    for (double x : std::array<double, 3>{0.25, 0.5, 0.75}) {
      const size_t px = std::min(static_cast<size_t>(width * x), width - 1);
      const size_t py = std::min(static_cast<size_t>(height * y), height - 1);
      const uint8_t* pixel = bytes == nullptr || element_bytes < 4
                                 ? nullptr
                                 : bytes + py * row_bytes + px * element_bytes;
      if (pixel == nullptr)
        std::fprintf(stderr, " [%zu,%zu]=unmapped", px, py);
      else
        std::fprintf(stderr, " [%zu,%zu]=%u,%u,%u,%u", px, py, pixel[0],
                     pixel[1], pixel[2], pixel[3]);
    }
  }
  std::fprintf(stderr, "\n");
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
}

void CaptureTargetPpm(IOSurfaceRef surface, uint64_t transaction_id) {
  const char* path =
      std::getenv("DARWIN_ART_DEBUG_SURFACECONTROL_CAPTURE_PATH");
  if (surface == nullptr || path == nullptr || path[0] == '\0') return;
  std::string resolved_path(path);
  constexpr char kTransactionToken[] = "{transaction}";
  const size_t token_offset = resolved_path.find(kTransactionToken);
  if (token_offset != std::string::npos) {
    resolved_path.replace(token_offset, std::strlen(kTransactionToken),
                          std::to_string(transaction_id));
  }
  if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) !=
      kIOReturnSuccess) {
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t width = IOSurfaceGetWidth(surface);
  const size_t height = IOSurfaceGetHeight(surface);
  const size_t row_bytes = IOSurfaceGetBytesPerRow(surface);
  const size_t element_bytes = IOSurfaceGetBytesPerElement(surface);
  FILE* output = bytes == nullptr || element_bytes < 4
      ? nullptr
      : std::fopen(resolved_path.c_str(), "wb");
  if (output != nullptr) {
    std::fprintf(output, "P6\n%zu %zu\n255\n", width, height);
    std::vector<uint8_t> rgb(width * 3);
    for (size_t row = 0; row < height; ++row) {
      const uint8_t* source = bytes + row * row_bytes;
      for (size_t column = 0; column < width; ++column) {
        rgb[column * 3] = source[column * element_bytes + 2];
        rgb[column * 3 + 1] = source[column * element_bytes + 1];
        rgb[column * 3 + 2] = source[column * element_bytes];
      }
      std::fwrite(rgb.data(), 1, rgb.size(), output);
    }
    std::fclose(output);
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
}

void ProcessRequest(CompositionJob job) {
  RequestHeader header = job.header;
  std::vector<WireLayer> incoming = std::move(job.incoming);
  int producer_descriptor = job.producer_descriptor;
  int completion_descriptor = job.completion_descriptor;
  @autoreleasepool {
    if (!WaitForProducer(producer_descriptor)) {
      std::fprintf(stderr,
                   "ART SurfaceFlinger: producer fence failed pid=%u "
                   "transaction=%llu\n",
                   header.process_id,
                   static_cast<unsigned long long>(header.transaction_id));
      SignalCompletion(completion_descriptor, false);
      return;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    IOSurfaceRef target_surface = IOSurfaceLookup(header.target_iosurface_id);
    bool valid = device != nil && target_surface != nullptr;
    std::vector<IOSurfaceRef> retained_surfaces;
    std::vector<DarwinArtMetalComposerLayer> composition;
    {
      std::lock_guard<std::mutex> lock(State().mutex);
      ServiceState& state = State();
      TargetState& target = state.targets[header.target_iosurface_id];
      target.width = header.target_width;
      target.height = header.target_height;
      std::vector<uint32_t> dead_owners;
      for (const auto& [global_id, retained] : target.layers) {
        (void)global_id;
        const uint32_t owner = retained.layer.owner_process_id == 0
            ? retained.submitting_process_id
            : retained.layer.owner_process_id;
        if (!ProcessAlive(owner) &&
            std::find(dead_owners.begin(), dead_owners.end(), owner) ==
                dead_owners.end()) {
          dead_owners.push_back(owner);
        }
      }
      for (uint32_t owner : dead_owners) {
        if (!DestroyProcessLayers(state, owner)) valid = false;
        for (auto iterator = target.layers.begin();
             iterator != target.layers.end();) {
          const uint32_t retained_owner =
              iterator->second.layer.owner_process_id == 0
                  ? iterator->second.submitting_process_id
                  : iterator->second.layer.owner_process_id;
          if (retained_owner == owner)
            iterator = target.layers.erase(iterator);
          else
            ++iterator;
        }
      }

      std::vector<DarwinArtSurfaceFlingerLayerUpdate> updates;
      updates.reserve(incoming.size());
      for (const WireLayer& layer : incoming) {
        const uint32_t owner_process_id = layer.owner_process_id == 0
            ? header.process_id
            : layer.owner_process_id;
        const uint32_t parent_owner_process_id =
            layer.parent_owner_process_id == 0
                ? owner_process_id
                : layer.parent_owner_process_id;
        const uint32_t relative_parent_owner_process_id =
            layer.relative_parent_owner_process_id == 0
                ? owner_process_id
                : layer.relative_parent_owner_process_id;
        updates.push_back({
            .layer_id = GlobalLayerId(state, owner_process_id, layer.layer_id),
            .parent_id =
                GlobalLayerId(state, parent_owner_process_id, layer.parent_id),
            .relative_parent_id = GlobalLayerId(
                state, relative_parent_owner_process_id,
                layer.relative_parent_id),
            .what = layer.what,
            .flags = layer.flags,
            .mask = layer.mask,
            .transform = layer.transform,
            .x = static_cast<float>(layer.destination_left),
            .y = static_cast<float>(layer.destination_top),
            .z = layer.z,
            .alpha = layer.alpha,
            .destination_left = layer.destination_left,
            .destination_top = layer.destination_top,
            .destination_right = layer.destination_right,
            .destination_bottom = layer.destination_bottom,
        });
        const uint32_t global_id =
            GlobalLayerId(state, owner_process_id, layer.layer_id);
        auto [retained, inserted] = target.layers.try_emplace(
            global_id, RetainedLayer{.submitting_process_id = header.process_id,
                                     .layer = layer});
        if (!inserted) {
          retained->second.submitting_process_id = header.process_id;
          MergeRetainedLayer(retained->second.layer, layer);
        }
      }
      DarwinArtSurfaceFlingerCommitResult commit{};
      const uint64_t central_transaction_id =
          (static_cast<uint64_t>(header.process_id) << 32) ^
          (header.transaction_id & UINT64_C(0xffffffff));
      if (!darwin_art_surfaceflinger_commit_transaction(
              central_transaction_id, updates.data(), updates.size(),
              &commit)) {
        std::fprintf(stderr,
                     "ART SurfaceFlinger: AOSP transaction flush failed "
                     "pid=%u transaction=%llu layers=%zu\n",
                     header.process_id,
                     static_cast<unsigned long long>(central_transaction_id),
                     updates.size());
        valid = false;
      }

      struct VisibleLayer {
        uint32_t process_id;
        WireLayer layer;
      };
      std::map<uint32_t, VisibleLayer> visible_layers;
      for (const auto& [global_id, retained] : target.layers) {
        const WireLayer& layer = retained.layer;
        constexpr uint32_t kLayerHidden = 1u;
        if (layer.iosurface_id == 0 ||
            (layer.flags & kLayerHidden) != 0) {
          continue;
        }
        visible_layers.emplace(
            global_id,
            VisibleLayer{.process_id = retained.submitting_process_id,
                         .layer = layer});
      }
      size_t layer_order_count = 0;
      std::vector<uint32_t> layer_order;
      if (!darwin_art_surfaceflinger_copy_layer_order(
              nullptr, 0, &layer_order_count)) {
        valid = false;
      } else {
        layer_order.resize(layer_order_count);
        if (!darwin_art_surfaceflinger_copy_layer_order(
                layer_order.data(), layer_order.size(), &layer_order_count)) {
          valid = false;
        }
      }
      size_t visible_ordered_count = 0;
      for (uint32_t global_id : layer_order) {
        const auto found = visible_layers.find(global_id);
        if (found == visible_layers.end()) continue;
        ++visible_ordered_count;
        const VisibleLayer& visible = found->second;
        const WireLayer& layer = visible.layer;
        const uint32_t owner_process_id = layer.owner_process_id == 0
            ? visible.process_id
            : layer.owner_process_id;
        const uint32_t parent_owner_process_id =
            layer.parent_owner_process_id == 0
                ? owner_process_id
                : layer.parent_owner_process_id;
        const uint32_t relative_parent_owner_process_id =
            layer.relative_parent_owner_process_id == 0
                ? owner_process_id
                : layer.relative_parent_owner_process_id;
        IOSurfaceRef surface = IOSurfaceLookup(layer.iosurface_id);
        if (surface == nullptr) continue;
        DebugSurfacePixels("source", header.transaction_id, global_id, surface);
        retained_surfaces.push_back(surface);
        composition.push_back({
            .owner_process_id = owner_process_id,
            .layer_id = global_id,
            .parent_owner_process_id = parent_owner_process_id,
            .parent_id = GlobalLayerId(state, parent_owner_process_id,
                                       layer.parent_id),
            .relative_parent_owner_process_id =
                relative_parent_owner_process_id,
            .relative_parent_id = GlobalLayerId(
                state, relative_parent_owner_process_id,
                layer.relative_parent_id),
            .what = layer.what,
            .flags = layer.flags,
            .mask = layer.mask,
            .transform = layer.transform,
            .producer_bottom_left = layer.producer_bottom_left != 0,
            .iosurface = surface,
            .width = layer.width,
            .height = layer.height,
            .source_left = layer.source_left,
            .source_top = layer.source_top,
            .source_right = layer.source_right,
            .source_bottom = layer.source_bottom,
            .destination_left = layer.destination_left,
            .destination_top = layer.destination_top,
            .destination_right = layer.destination_right,
            .destination_bottom = layer.destination_bottom,
            .z = layer.z,
            .alpha = layer.alpha,
        });
      }
      if (visible_ordered_count != visible_layers.size()) {
        std::fprintf(stderr,
                     "ART SurfaceFlinger: AOSP hierarchy omitted visible "
                     "layers expected=%zu ordered=%zu\n",
                     visible_layers.size(), visible_ordered_count);
        for (const auto& [global_id, visible] : visible_layers) {
          std::fprintf(stderr,
                       "ART SurfaceFlinger: omitted candidate pid=%u "
                       "local=%u global=%u parent-local=%u parent-global=%u "
                       "what=0x%llx surface=%u\n",
                       visible.process_id, visible.layer.layer_id, global_id,
                       visible.layer.parent_id,
                       GlobalLayerId(
                                     state,
                                     visible.layer.parent_owner_process_id == 0
                                         ? (visible.layer.owner_process_id == 0
                                                ? visible.process_id
                                                : visible.layer.owner_process_id)
                                         : visible.layer.parent_owner_process_id,
                                     visible.layer.parent_id),
                       static_cast<unsigned long long>(visible.layer.what),
                       visible.layer.iosurface_id);
        }
        valid = false;
      }
    }

    void* completion = nullptr;
    uint64_t completion_value = 0;
    if (valid &&
        !darwin_art_metal_composer_compose(
            device, target_surface, header.target_width, header.target_height,
            composition.data(), composition.size(), nullptr, 0, &completion,
            &completion_value)) {
      valid = false;
    }
    if (valid && completion != nullptr && completion_value != 0) {
      id<MTLSharedEvent> event = reinterpret_cast<id<MTLSharedEvent>>(completion);
      MTLSharedEventListener* listener =
          [[MTLSharedEventListener alloc] init];
      if (listener == nil) {
        valid = false;
      } else {
        IOSurfaceRef debug_target =
            std::getenv("DARWIN_ART_DEBUG_SURFACECONTROL_PIXELS") == nullptr
                ? nullptr
                : target_surface;
        if (debug_target != nullptr) CFRetain(debug_target);
        [event notifyListener:listener
                      atValue:completion_value
                        block:^(id<MTLSharedEvent>, uint64_t) {
                          if (debug_target != nullptr) {
                            DebugSurfacePixels("target", header.transaction_id,
                                               0, debug_target);
                            CaptureTargetPpm(debug_target,
                                             header.transaction_id);
                            CFRelease(debug_target);
                          }
                          SignalCompletion(completion_descriptor, true);
                        }];
        [listener release];
      }
    } else {
      valid = false;
    }
    if (!valid) {
      std::fprintf(stderr,
                   "ART SurfaceFlinger: central compose failed pid=%u "
                   "transaction=%llu layers=%zu target=%u\n",
                   header.process_id,
                   static_cast<unsigned long long>(header.transaction_id),
                   composition.size(), header.target_iosurface_id);
      SignalCompletion(completion_descriptor, false);
    } else if (std::getenv("DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS") !=
               nullptr) {
      std::fprintf(stderr,
                   "ART SurfaceFlinger: central compose queued owner=%d "
                   "client=%u transaction=%llu layers=%zu target=%u\n",
                   getpid(), header.process_id,
                   static_cast<unsigned long long>(header.transaction_id),
                   composition.size(), header.target_iosurface_id);
    }
    if (completion != nullptr) CFRelease(completion);
    if (target_surface != nullptr) CFRelease(target_surface);
    for (IOSurfaceRef surface : retained_surfaces) CFRelease(surface);
    if (device != nil) [device release];
  }
}

bool HandleRequest(int client) {
  RequestHeader header{};
  if (!ReadAll(client, &header, sizeof(header)) ||
      std::memcmp(header.magic, kRequestMagic.data(), kRequestMagic.size()) !=
          0 ||
      header.version != kProtocolVersion || header.process_id == 0 ||
      header.target_iosurface_id == 0 || header.target_width == 0 ||
      header.target_height == 0 || header.layer_count > kMaximumLayers ||
      header.has_producer_fence > 1) {
    return false;
  }
  std::vector<WireLayer> incoming(header.layer_count);
  if (!incoming.empty() &&
      !ReadAll(client, incoming.data(), incoming.size() * sizeof(WireLayer))) {
    return false;
  }
  int producer_descriptor = -1;
  if (!ReceiveDescriptor(client, header.has_producer_fence != 0,
                         &producer_descriptor)) {
    return false;
  }
  int completion_pipe[2]{-1, -1};
  int32_t status = 0;
  if (pipe(completion_pipe) != 0) status = errno;
  if (status == 0) {
    for (int descriptor : completion_pipe) {
      const int flags = fcntl(descriptor, F_GETFD);
      if (flags < 0 ||
          fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        status = errno;
        break;
      }
    }
#ifdef F_SETNOSIGPIPE
    if (status == 0 &&
        fcntl(completion_pipe[1], F_SETNOSIGPIPE, 1) != 0) {
      status = errno;
    }
#endif
  }
  ResponseHeader response{};
  std::memcpy(response.magic, kResponseMagic.data(), kResponseMagic.size());
  response.version = kProtocolVersion;
  response.status = status;
  response.has_completion_fence = status == 0 ? 1 : 0;
  const bool sent = WriteAll(client, &response, sizeof(response)) &&
                    SendDescriptor(client,
                                   status == 0 ? completion_pipe[0] : -1);
  if (completion_pipe[0] >= 0) close(completion_pipe[0]);
  if (!sent || status != 0) {
    if (completion_pipe[1] >= 0) close(completion_pipe[1]);
    if (producer_descriptor >= 0) close(producer_descriptor);
    return false;
  }
  if (!CompositionJobs().Enqueue(CompositionJob{
          .header = header,
          .incoming = std::move(incoming),
          .producer_descriptor = producer_descriptor,
          .completion_descriptor = completion_pipe[1]})) {
    if (producer_descriptor >= 0) close(producer_descriptor);
    SignalCompletion(completion_pipe[1], false);
    return false;
  }
  return true;
}

void Serve(int listener) {
  for (;;) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
#ifdef F_SETNOSIGPIPE
    (void)fcntl(client, F_SETNOSIGPIPE, 1);
#endif
    (void)HandleRequest(client);
    close(client);
  }
  close(listener);
}

}  // namespace

extern "C" bool darwin_art_surfaceflinger_service_start() {
  static std::once_flag once;
  static bool started = false;
  std::call_once(once, [] {
    const char* path = std::getenv("DARWIN_ART_SURFACEFLINGER_SOCKET");
    if (path == nullptr || path[0] == '\0') return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(address.sun_path)) return;
    std::memcpy(address.sun_path, path, std::strlen(path) + 1);
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return;
    unlink(path);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0 ||
        chmod(path, 0600) != 0 || listen(listener, 32) != 0) {
      close(listener);
      return;
    }
    if (!CompositionJobs().Start()) {
      close(listener);
      unlink(path);
      return;
    }
    std::thread(Serve, listener).detach();
    started = true;
    std::fprintf(stderr, "ART SurfaceFlinger: central service ready socket=%s\n",
                 path);
  });
  return started;
}

extern "C" int darwin_art_surfaceflinger_service_present(
    uint32_t target_iosurface_id, uint32_t target_width,
    uint32_t target_height, uint64_t transaction_id,
    const DarwinArtMetalComposerLayer* layers, size_t layer_count,
    void* producer_event, uint64_t producer_value) {
  const char* path = std::getenv("DARWIN_ART_SURFACEFLINGER_SOCKET");
  if (path == nullptr || path[0] == '\0' ||
      ((producer_event == nullptr) != (producer_value == 0)) ||
      layer_count > kMaximumLayers ||
      (layer_count != 0 && layers == nullptr)) {
    return -1;
  }
  int producer_host_descriptor = -1;
  if (producer_event != nullptr) {
    const int producer_guest_descriptor =
        darwin_art_android_metal_shared_event_fence_fd(producer_event,
                                                       producer_value);
    if (producer_guest_descriptor < 0) return -1;
    producer_host_descriptor =
        darwin_art_bionic_fd_export_for_scm(producer_guest_descriptor);
    (void)darwin_art_bionic_socket_broker_close(producer_guest_descriptor);
    if (producer_host_descriptor < 0) return -1;
  }
  std::vector<WireLayer> wire_layers;
  wire_layers.reserve(layer_count);
  for (size_t index = 0; index < layer_count; ++index) {
    const DarwinArtMetalComposerLayer& layer = layers[index];
    auto surface = reinterpret_cast<IOSurfaceRef>(layer.iosurface);
    const uint32_t surface_id =
        surface == nullptr ? 0 : IOSurfaceGetID(surface);
    if (layer.layer_id == 0) continue;
    wire_layers.push_back({
        .owner_process_id = layer.owner_process_id,
        .layer_id = layer.layer_id,
        .parent_owner_process_id = layer.parent_owner_process_id,
        .parent_id = layer.parent_id,
        .relative_parent_owner_process_id =
            layer.relative_parent_owner_process_id,
        .relative_parent_id = layer.relative_parent_id,
        .iosurface_id = surface_id,
        .width = layer.width,
        .height = layer.height,
        .what = layer.what,
        .flags = layer.flags,
        .mask = layer.mask,
        .transform = layer.transform,
        .producer_bottom_left = layer.producer_bottom_left ? 1u : 0u,
        .source_left = layer.source_left,
        .source_top = layer.source_top,
        .source_right = layer.source_right,
        .source_bottom = layer.source_bottom,
        .destination_left = layer.destination_left,
        .destination_top = layer.destination_top,
        .destination_right = layer.destination_right,
        .destination_bottom = layer.destination_bottom,
        .z = layer.z,
        .alpha = layer.alpha,
    });
  }
  const int fd = Connect(path);
  if (fd < 0) {
    std::fprintf(stderr, "ART SurfaceFlinger client: connect failed path=%s errno=%d\n",
                 path, errno);
    return -1;
  }
  RequestHeader request{};
  std::memcpy(request.magic, kRequestMagic.data(), kRequestMagic.size());
  request.version = kProtocolVersion;
  request.process_id = static_cast<uint32_t>(getpid());
  request.target_iosurface_id = target_iosurface_id;
  request.target_width = target_width;
  request.target_height = target_height;
  request.layer_count = static_cast<uint32_t>(wire_layers.size());
  request.has_producer_fence = producer_host_descriptor >= 0 ? 1 : 0;
  request.transaction_id = transaction_id;
  const bool written =
      WriteAll(fd, &request, sizeof(request)) &&
      (wire_layers.empty() ||
       WriteAll(fd, wire_layers.data(),
                wire_layers.size() * sizeof(WireLayer))) &&
      SendDescriptor(fd, producer_host_descriptor);
  if (producer_host_descriptor >= 0) close(producer_host_descriptor);
  ResponseHeader response{};
  const bool response_read = written && ReadAll(fd, &response, sizeof(response));
  if (!response_read ||
      std::memcmp(response.magic, kResponseMagic.data(), kResponseMagic.size()) !=
          0 ||
      response.version != kProtocolVersion || response.status != 0 ||
      response.has_completion_fence != 1) {
    std::fprintf(stderr,
                 "ART SurfaceFlinger client: response failed written=%d "
                 "read=%d status=%d fence=%u errno=%d\n",
                 written, response_read, response.status,
                 response.has_completion_fence, errno);
    close(fd);
    return -1;
  }
  int completion_host_descriptor = -1;
  const bool received =
      ReceiveDescriptor(fd, true, &completion_host_descriptor);
  close(fd);
  if (!received || completion_host_descriptor < 0) return -1;
  const int completion_guest_descriptor =
      darwin_art_bionic_fd_import_from_scm(completion_host_descriptor);
  if (completion_guest_descriptor < 0) {
    close(completion_host_descriptor);
    return -1;
  }
  return completion_guest_descriptor;
}
