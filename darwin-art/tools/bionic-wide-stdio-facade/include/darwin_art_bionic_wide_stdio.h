#ifndef DARWIN_ART_BIONIC_WIDE_STDIO_H_
#define DARWIN_ART_BIONIC_WIDE_STDIO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define DARWIN_ART_WIDE_STDIO_NOEXCEPT noexcept
#else
#define DARWIN_ART_WIDE_STDIO_NOEXCEPT
#endif

#define DARWIN_ART_BIONIC_WIDE_STDIO_BACKEND_ABI 1u
#define DARWIN_ART_BIONIC_WEOF UINT32_C(0xffffffff)

typedef struct DarwinArtAndroidFile DarwinArtAndroidFile;
typedef struct DarwinArtBionicWideStdioActivation
    DarwinArtBionicWideStdioActivation;
typedef void (*DarwinArtBionicWideStdioFunction)(void);

/*
 * The central stdio owner returns a lease that keeps one Android FILE token
 * live and locked through release(). All callbacks other than acquire() take
 * that lease, never a Darwin FILE*. Callback failures must store Android errno.
 * Byte-oriented stdio and this callback must share the orientation field.
 */
typedef struct DarwinArtBionicWideStdioBackendV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* context;
  int (*acquire)(void* context, DarwinArtAndroidFile* file, void** lease);
  void (*release)(void* context, void* lease);
  int (*orient_wide)(void* context, void* lease);
  int (*read_byte)(void* context, void* lease, uint8_t* output);
  int (*write_bytes)(void* context,
                     void* lease,
                     const uint8_t* bytes,
                     size_t length);
  void (*set_error)(void* context, void* lease);
  void (*clear_error_and_eof)(void* context, void* lease);
} DarwinArtBionicWideStdioBackendV1;

DarwinArtBionicWideStdioActivation* darwin_art_bionic_wide_stdio_install(
    const DarwinArtBionicWideStdioBackendV1* backend)
    DARWIN_ART_WIDE_STDIO_NOEXCEPT;
int darwin_art_bionic_wide_stdio_uninstall(
    DarwinArtBionicWideStdioActivation** activation)
    DARWIN_ART_WIDE_STDIO_NOEXCEPT;

/* Called by the central owner while its exclusive stream lease is held. */
int darwin_art_bionic_wide_stdio_reset(DarwinArtAndroidFile* file)
    DARWIN_ART_WIDE_STDIO_NOEXCEPT;
int darwin_art_bionic_wide_stdio_forget(DarwinArtAndroidFile* file)
    DARWIN_ART_WIDE_STDIO_NOEXCEPT;

uint32_t darwin_art_bionic_fputwc(uint32_t wc, DarwinArtAndroidFile* file);
uint32_t darwin_art_bionic_getwc(DarwinArtAndroidFile* file);
uint32_t darwin_art_bionic_ungetwc(uint32_t wc, DarwinArtAndroidFile* file);

uint32_t darwin_art_bionic_wide_stdio_fputwc_core(
    uint32_t wc,
    DarwinArtAndroidFile* file) DARWIN_ART_WIDE_STDIO_NOEXCEPT;
uint32_t darwin_art_bionic_wide_stdio_getwc_core(DarwinArtAndroidFile* file)
    DARWIN_ART_WIDE_STDIO_NOEXCEPT;
uint32_t darwin_art_bionic_wide_stdio_ungetwc_core(
    uint32_t wc,
    DarwinArtAndroidFile* file) DARWIN_ART_WIDE_STDIO_NOEXCEPT;

DarwinArtBionicWideStdioFunction darwin_art_bionic_wide_stdio_resolve(
    const char* soname,
    const char* symbol,
    const char* version) DARWIN_ART_WIDE_STDIO_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef DARWIN_ART_WIDE_STDIO_NOEXCEPT

#endif
