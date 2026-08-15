#ifndef DARWIN_ART_BIONIC_STDIO_H_
#define DARWIN_ART_BIONIC_STDIO_H_
#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((aligned(8))) DarwinArtAndroidFile {
  unsigned char opaque[152];
} DarwinArtAndroidFile;
extern DarwinArtAndroidFile darwin_art_bionic___sF[3];
typedef void (*DarwinArtBionicStdioFunction)(void);

DarwinArtAndroidFile* darwin_art_bionic_fopen(const char*, const char*);
int darwin_art_bionic_fclose(DarwinArtAndroidFile*);
int darwin_art_bionic_fflush(DarwinArtAndroidFile*);
int darwin_art_bionic_fileno(DarwinArtAndroidFile*);
size_t darwin_art_bionic_fread(void*, size_t, size_t, DarwinArtAndroidFile*);
size_t darwin_art_bionic_fwrite(const void*, size_t, size_t, DarwinArtAndroidFile*);
int darwin_art_bionic_fseek(DarwinArtAndroidFile*, long, int);
int darwin_art_bionic_fseeko(DarwinArtAndroidFile*, int64_t, int);
int64_t darwin_art_bionic_ftello(DarwinArtAndroidFile*);
int darwin_art_bionic_fputc(int, DarwinArtAndroidFile*);
int darwin_art_bionic_getc(DarwinArtAndroidFile*);
int darwin_art_bionic_ungetc(int, DarwinArtAndroidFile*);
DarwinArtBionicStdioFunction darwin_art_bionic_stdio_resolve(const char*);

DarwinArtAndroidFile* darwin_art_bionic_stdio_fopen_core(const char*, const char*);
int darwin_art_bionic_stdio_fclose_core(DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_fflush_core(DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_fileno_core(DarwinArtAndroidFile*);
size_t darwin_art_bionic_stdio_fread_core(void*, size_t, size_t, DarwinArtAndroidFile*);
size_t darwin_art_bionic_stdio_fwrite_core(const void*, size_t, size_t, DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_fseek_core(DarwinArtAndroidFile*, int64_t, int);
int64_t darwin_art_bionic_stdio_ftello_core(DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_fputc_core(int, DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_getc_core(DarwinArtAndroidFile*);
int darwin_art_bionic_stdio_ungetc_core(int, DarwinArtAndroidFile*);
#endif
