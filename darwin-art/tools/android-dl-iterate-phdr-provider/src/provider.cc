#include "darwin_art_dl_iterate_phdr.h"

#include <cstring>
#include <mutex>

namespace {

static_assert(sizeof(DarwinArtAndroidElf64Phdr) == 56);
static_assert(sizeof(DarwinArtAndroidDlPhdrInfo) == 64);
static_assert(offsetof(DarwinArtAndroidDlPhdrInfo, dlpi_adds) == 32);

std::mutex& SourceMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

DarwinArtLoadedImageSourceV1& Source() {
  static DarwinArtLoadedImageSourceV1* source =
      new DarwinArtLoadedImageSourceV1{};
  return *source;
}

class SnapshotLease {
 public:
  SnapshotLease(const DarwinArtLoadedImageSourceV1& source,
                DarwinArtLoadedImageSnapshotV1 snapshot)
      : source_(source), snapshot_(snapshot) {}
  ~SnapshotLease() {
    if (snapshot_.lease != nullptr) {
      source_.release(source_.context, snapshot_.lease);
    }
  }
  const DarwinArtLoadedImageSnapshotV1& snapshot() const { return snapshot_; }

 private:
  DarwinArtLoadedImageSourceV1 source_;
  DarwinArtLoadedImageSnapshotV1 snapshot_;
};

bool ValidRecord(const DarwinArtLoadedImageRecordV1& record) {
  return record.image_id != 0 && record.generation != 0 &&
         record.soname != nullptr && record.phdrs != nullptr &&
         record.phnum != 0;
}

}  // namespace

extern "C" int darwin_art_dl_phdr_bind_source(
    const DarwinArtLoadedImageSourceV1* source) {
  if (source == nullptr ||
      source->abi_version != DARWIN_ART_LOADED_IMAGE_SOURCE_ABI_VERSION ||
      source->struct_size != sizeof(*source) || source->context == nullptr ||
      source->acquire == nullptr || source->release == nullptr) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(SourceMutex());
  if (Source().acquire != nullptr) return -1;
  Source() = *source;
  return 0;
}

extern "C" int darwin_art_bionic_dl_iterate_phdr(
    DarwinArtAndroidDlIterateCallback callback,
    void* data) {
  if (callback == nullptr) return -1;
  DarwinArtLoadedImageSourceV1 source{};
  {
    std::lock_guard<std::mutex> lock(SourceMutex());
    source = Source();
  }
  if (source.acquire == nullptr) return -1;
  DarwinArtLoadedImageSnapshotV1 snapshot{};
  if (source.acquire(source.context, &snapshot) != 0 ||
      snapshot.abi_version != DARWIN_ART_LOADED_IMAGE_SOURCE_ABI_VERSION ||
      snapshot.struct_size != sizeof(snapshot) || snapshot.lease == nullptr ||
      (snapshot.record_count != 0 && snapshot.records == nullptr)) {
    if (snapshot.lease != nullptr) {
      source.release(source.context, snapshot.lease);
    }
    return -1;
  }
  SnapshotLease lease(source, snapshot);
  for (size_t index = 0; index < lease.snapshot().record_count; ++index) {
    const DarwinArtLoadedImageRecordV1& record = lease.snapshot().records[index];
    if (!ValidRecord(record)) return -1;
    DarwinArtAndroidDlPhdrInfo info{};
    info.dlpi_addr = record.load_bias;
    info.dlpi_name = record.soname;
    info.dlpi_phdr = record.phdrs;
    info.dlpi_phnum = record.phnum;
    info.dlpi_adds = lease.snapshot().load_events;
    info.dlpi_subs = lease.snapshot().unload_events;
    info.dlpi_tls_modid = record.tls_modid;
    info.dlpi_tls_data = record.tls_data_for_current_thread;
    const int result = callback(&info, sizeof(info), data);
    if (result != 0) return result;
  }
  return 0;
}

extern "C" void* darwin_art_dl_phdr_resolve(const char* soname,
                                             const char* symbol,
                                             const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, "libdl.so") != 0 ||
      std::strcmp(symbol, "dl_iterate_phdr") != 0 ||
      std::strcmp(version, "LIBC") != 0) {
    return nullptr;
  }
  return reinterpret_cast<void*>(&darwin_art_bionic_dl_iterate_phdr);
}
