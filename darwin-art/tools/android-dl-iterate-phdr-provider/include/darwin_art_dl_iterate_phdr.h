#ifndef DARWIN_ART_DL_ITERATE_PHDR_H_
#define DARWIN_ART_DL_ITERATE_PHDR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DARWIN_ART_LOADED_IMAGE_SOURCE_ABI_VERSION 1u

typedef struct DarwinArtAndroidElf64Phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} DarwinArtAndroidElf64Phdr;

typedef struct DarwinArtAndroidDlPhdrInfo {
  uint64_t dlpi_addr;
  const char* dlpi_name;
  const DarwinArtAndroidElf64Phdr* dlpi_phdr;
  uint16_t dlpi_phnum;
  uint16_t reserved_padding[3];
  uint64_t dlpi_adds;
  uint64_t dlpi_subs;
  size_t dlpi_tls_modid;
  void* dlpi_tls_data;
} DarwinArtAndroidDlPhdrInfo;

typedef struct DarwinArtLoadedImageRecordV1 {
  uint64_t image_id;
  uint64_t generation;
  uint64_t load_bias;
  const char* soname;
  const DarwinArtAndroidElf64Phdr* phdrs;
  uint16_t phnum;
  uint16_t reserved16[3];
  size_t tls_modid;
  void* tls_data_for_current_thread;
} DarwinArtLoadedImageRecordV1;

typedef struct DarwinArtLoadedImageSnapshotV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* lease;
  const DarwinArtLoadedImageRecordV1* records;
  size_t record_count;
  uint64_t load_events;
  uint64_t unload_events;
} DarwinArtLoadedImageSnapshotV1;

typedef int (*DarwinArtAcquireLoadedImageSnapshotV1)(
    void* context,
    DarwinArtLoadedImageSnapshotV1* snapshot_out);
typedef void (*DarwinArtReleaseLoadedImageSnapshotV1)(void* context,
                                                      void* lease);

typedef struct DarwinArtLoadedImageSourceV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* context;
  DarwinArtAcquireLoadedImageSnapshotV1 acquire;
  DarwinArtReleaseLoadedImageSnapshotV1 release;
} DarwinArtLoadedImageSourceV1;

typedef int (*DarwinArtAndroidDlIterateCallback)(
    DarwinArtAndroidDlPhdrInfo* info,
    size_t info_size,
    void* data);

// Process-lifetime bind. The source table and context must outlive all Android
// images. Rebinding is rejected.
int darwin_art_dl_phdr_bind_source(const DarwinArtLoadedImageSourceV1* source);

// Actual libdl.so@LIBC provider implementation.
int darwin_art_bionic_dl_iterate_phdr(
    DarwinArtAndroidDlIterateCallback callback,
    void* data);

void* darwin_art_dl_phdr_resolve(const char* soname,
                                 const char* symbol,
                                 const char* version);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_DL_ITERATE_PHDR_H_
