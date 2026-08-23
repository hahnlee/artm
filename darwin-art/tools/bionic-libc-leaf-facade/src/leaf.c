#include "darwin_art_bionic_libc_leaf.h"

#include <limits.h>

_Static_assert(CHAR_BIT == 8, "the facade requires 8-bit bytes");
_Static_assert(sizeof(wchar_t) == 4, "Bionic arm64 wchar_t is 32-bit");

void* darwin_art_bionic_memchr(const void* source, int value, size_t length) {
  const unsigned char* bytes = (const unsigned char*)source;
  const unsigned char wanted = (unsigned char)value;
  for (size_t index = 0; index < length; ++index) {
    if (bytes[index] == wanted) return (void*)(bytes + index);
  }
  return NULL;
}

int darwin_art_bionic_memcmp(const void* left, const void* right, size_t length) {
  const unsigned char* a = (const unsigned char*)left;
  const unsigned char* b = (const unsigned char*)right;
  for (size_t index = 0; index < length; ++index) {
    if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
  }
  return 0;
}

void* darwin_art_bionic_memcpy(void* destination, const void* source, size_t length) {
  unsigned char* output = (unsigned char*)destination;
  const unsigned char* input = (const unsigned char*)source;
  for (size_t index = 0; index < length; ++index) output[index] = input[index];
  return destination;
}

void* darwin_art_bionic_memmove(void* destination, const void* source, size_t length) {
  unsigned char* output = (unsigned char*)destination;
  const unsigned char* input = (const unsigned char*)source;
  if ((uintptr_t)output <= (uintptr_t)input ||
      (uintptr_t)output - (uintptr_t)input >= length) {
    for (size_t index = 0; index < length; ++index) output[index] = input[index];
  } else {
    while (length != 0) {
      --length;
      output[length] = input[length];
    }
  }
  return destination;
}

void* darwin_art_bionic_memset(void* destination, int value, size_t length) {
  unsigned char* output = (unsigned char*)destination;
  for (size_t index = 0; index < length; ++index) output[index] = (unsigned char)value;
  return destination;
}

int darwin_art_bionic_strcmp(const char* left, const char* right) {
  const unsigned char* a = (const unsigned char*)left;
  const unsigned char* b = (const unsigned char*)right;
  while (*a == *b && *a != 0) {
    ++a;
    ++b;
  }
  return *a == *b ? 0 : (*a < *b ? -1 : 1);
}

const char* darwin_art_bionic_strchr(const char* string, int value) {
  const unsigned char wanted = (unsigned char)value;
  for (const unsigned char* cursor = (const unsigned char*)string;; ++cursor) {
    if (*cursor == wanted) return (const char*)cursor;
    if (*cursor == 0) return NULL;
  }
}

size_t darwin_art_bionic_strlen(const char* string) {
  const char* end = string;
  while (*end != '\0') ++end;
  return (size_t)(end - string);
}

int darwin_art_bionic_strncmp(const char* left, const char* right, size_t length) {
  const unsigned char* a = (const unsigned char*)left;
  const unsigned char* b = (const unsigned char*)right;
  while (length != 0) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (*a == 0) return 0;
    ++a;
    ++b;
    --length;
  }
  return 0;
}

const char* darwin_art_bionic_strstr(const char* haystack, const char* needle) {
  if (*needle == '\0') return haystack;
  for (const char* candidate = haystack; *candidate != '\0'; ++candidate) {
    const char* a = candidate;
    const char* b = needle;
    while (*a != '\0' && *b != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*b == '\0') return candidate;
  }
  return NULL;
}

static int Contains(const char* set, unsigned char value) {
  for (; *set != '\0'; ++set) if ((unsigned char)*set == value) return 1;
  return 0;
}

size_t darwin_art_bionic_strspn(const char* string, const char* accept) {
  size_t length = 0;
  while (string[length] != '\0' && Contains(accept, (unsigned char)string[length])) ++length;
  return length;
}

size_t darwin_art_bionic_strcspn(const char* string, const char* reject) {
  size_t length = 0;
  while (string[length] != '\0' && !Contains(reject, (unsigned char)string[length])) ++length;
  return length;
}

const char* darwin_art_bionic_strpbrk(const char* string, const char* accept) {
  for (; *string != '\0'; ++string) if (Contains(accept, (unsigned char)*string)) return string;
  return NULL;
}

const char* darwin_art_bionic_strrchr(const char* string, int value) {
  const char* found = NULL;
  const unsigned char wanted = (unsigned char)value;
  for (const unsigned char* cursor = (const unsigned char*)string;; ++cursor) {
    if (*cursor == wanted) found = (const char*)cursor;
    if (*cursor == 0) return found;
  }
}

char* darwin_art_bionic_strcpy(char* destination, const char* source) {
  char* result = destination;
  while ((*destination++ = *source++) != '\0') {}
  return result;
}

int darwin_art_bionic_atoi(const char* string) {
  while (*string == ' ' || *string == '\t' || *string == '\n' || *string == '\r') ++string;
  int sign = 1;
  if (*string == '-' || *string == '+') { if (*string++ == '-') sign = -1; }
  int value = 0;
  while (*string >= '0' && *string <= '9') value = value * 10 + (*string++ - '0');
  return sign * value;
}

void* darwin_art_bionic_bsearch(const void* key, const void* base, size_t count,
                                size_t size, int (*compare)(const void*, const void*)) {
  const unsigned char* bytes = (const unsigned char*)base;
  size_t low = 0;
  while (low < count) {
    const size_t middle = low + (count - low) / 2;
    const void* candidate = bytes + middle * size;
    const int order = compare(key, candidate);
    if (order == 0) return (void*)candidate;
    if (order < 0) count = middle;
    else low = middle + 1;
  }
  return NULL;
}

wchar_t* darwin_art_bionic_wmemchr(const wchar_t* source, wchar_t value, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (source[index] == value) return (wchar_t*)(source + index);
  }
  return NULL;
}

int darwin_art_bionic_wmemcmp(const wchar_t* left, const wchar_t* right, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    const uint32_t a = (uint32_t)left[index];
    const uint32_t b = (uint32_t)right[index];
    if (a != b) return a < b ? -1 : 1;
  }
  return 0;
}

size_t darwin_art_bionic_wcslen(const wchar_t* string) {
  const wchar_t* end = string;
  while (*end != 0) ++end;
  return (size_t)(end - string);
}

static int NameCompare(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return (unsigned char)*left < (unsigned char)*right
             ? -1
             : ((unsigned char)*left != (unsigned char)*right);
}

static const DarwinArtBionicLeafBinding kBindings[] = {
    {"atoi", (DarwinArtBionicFunction)darwin_art_bionic_atoi},
    {"bsearch", (DarwinArtBionicFunction)darwin_art_bionic_bsearch},
    {"memchr", (DarwinArtBionicFunction)darwin_art_bionic_memchr},
    {"memcmp", (DarwinArtBionicFunction)darwin_art_bionic_memcmp},
    {"memcpy", (DarwinArtBionicFunction)darwin_art_bionic_memcpy},
    {"memmove", (DarwinArtBionicFunction)darwin_art_bionic_memmove},
    {"memset", (DarwinArtBionicFunction)darwin_art_bionic_memset},
    {"strchr", (DarwinArtBionicFunction)darwin_art_bionic_strchr},
    {"strcmp", (DarwinArtBionicFunction)darwin_art_bionic_strcmp},
    {"strcpy", (DarwinArtBionicFunction)darwin_art_bionic_strcpy},
    {"strcspn", (DarwinArtBionicFunction)darwin_art_bionic_strcspn},
    {"strlen", (DarwinArtBionicFunction)darwin_art_bionic_strlen},
    {"strncmp", (DarwinArtBionicFunction)darwin_art_bionic_strncmp},
    {"strpbrk", (DarwinArtBionicFunction)darwin_art_bionic_strpbrk},
    {"strrchr", (DarwinArtBionicFunction)darwin_art_bionic_strrchr},
    {"strspn", (DarwinArtBionicFunction)darwin_art_bionic_strspn},
    {"strstr", (DarwinArtBionicFunction)darwin_art_bionic_strstr},
    {"wcslen", (DarwinArtBionicFunction)darwin_art_bionic_wcslen},
    {"wmemchr", (DarwinArtBionicFunction)darwin_art_bionic_wmemchr},
    {"wmemcmp", (DarwinArtBionicFunction)darwin_art_bionic_wmemcmp},
};

const DarwinArtBionicLeafBinding* darwin_art_bionic_libc_leaf_table(size_t* count) {
  if (count != NULL) *count = sizeof(kBindings) / sizeof(kBindings[0]);
  return kBindings;
}

DarwinArtBionicFunction darwin_art_bionic_libc_leaf_resolve(const char* import_name) {
  if (import_name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = NameCompare(import_name, kBindings[middle].import_name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}
