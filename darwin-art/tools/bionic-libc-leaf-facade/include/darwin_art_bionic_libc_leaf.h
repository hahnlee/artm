#ifndef DARWIN_ART_BIONIC_LIBC_LEAF_H_
#define DARWIN_ART_BIONIC_LIBC_LEAF_H_

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicFunction)(void);

typedef struct DarwinArtBionicLeafBinding {
  const char* import_name;
  DarwinArtBionicFunction address;
} DarwinArtBionicLeafBinding;

void* darwin_art_bionic_memchr(const void* source, int value, size_t length);
int darwin_art_bionic_memcmp(const void* left, const void* right, size_t length);
void* darwin_art_bionic_memcpy(void* destination, const void* source, size_t length);
void* darwin_art_bionic_memmove(void* destination, const void* source, size_t length);
void* darwin_art_bionic_memset(void* destination, int value, size_t length);
int darwin_art_bionic_strcmp(const char* left, const char* right);
size_t darwin_art_bionic_strlen(const char* string);
int darwin_art_bionic_strncmp(const char* left, const char* right, size_t length);
wchar_t* darwin_art_bionic_wmemchr(const wchar_t* source, wchar_t value, size_t length);
int darwin_art_bionic_wmemcmp(const wchar_t* left, const wchar_t* right, size_t length);
size_t darwin_art_bionic_wcslen(const wchar_t* string);

const DarwinArtBionicLeafBinding* darwin_art_bionic_libc_leaf_table(size_t* count);
DarwinArtBionicFunction darwin_art_bionic_libc_leaf_resolve(const char* import_name);

#ifdef __cplusplus
}
#endif

#endif

