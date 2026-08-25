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

void* darwin_art_bionic___memcpy_chk(void* destination, const void* source,
                                     size_t length, size_t destination_size) {
  if (length > destination_size) __builtin_trap();
  return darwin_art_bionic_memcpy(destination, source, length);
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

void* darwin_art_bionic___memmove_chk(void* destination, const void* source,
                                      size_t length, size_t destination_size) {
  if (length > destination_size) __builtin_trap();
  return darwin_art_bionic_memmove(destination, source, length);
}

void* darwin_art_bionic_memset(void* destination, int value, size_t length) {
  unsigned char* output = (unsigned char*)destination;
  for (size_t index = 0; index < length; ++index) output[index] = (unsigned char)value;
  return destination;
}

void* darwin_art_bionic___memset_chk(void* destination, int value,
                                     size_t length, size_t destination_size) {
  if (length > destination_size) __builtin_trap();
  return darwin_art_bionic_memset(destination, value, length);
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

static unsigned char FoldAscii(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

int darwin_art_bionic_strcasecmp(const char* left, const char* right) {
  const unsigned char* a = (const unsigned char*)left;
  const unsigned char* b = (const unsigned char*)right;
  for (;;) {
    const unsigned char folded_a = FoldAscii(*a);
    const unsigned char folded_b = FoldAscii(*b);
    if (folded_a != folded_b) return (int)folded_a - (int)folded_b;
    if (*a == 0) return 0;
    ++a;
    ++b;
  }
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

size_t darwin_art_bionic___strlen_chk(const char* string, size_t buffer_size) {
  const size_t length = darwin_art_bionic_strlen(string);
  if (length >= buffer_size) __builtin_trap();
  return length;
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

long darwin_art_bionic_atol(const char* string) {
  while (*string == ' ' || *string == '\t' || *string == '\n' ||
         *string == '\r' || *string == '\f' || *string == '\v') {
    ++string;
  }
  int negative = 0;
  if (*string == '-' || *string == '+') negative = *string++ == '-';
  unsigned long value = 0;
  while (*string >= '0' && *string <= '9') {
    value = value * 10 + (unsigned long)(*string++ - '0');
  }
  return negative ? -(long)value : (long)value;
}

char* darwin_art_bionic_strcat(char* destination, const char* source) {
  char* result = destination;
  while (*destination != '\0') ++destination;
  while ((*destination++ = *source++) != '\0') {}
  return result;
}

char* darwin_art_bionic_strncat(char* destination, const char* source,
                                size_t length) {
  char* result = destination;
  while (*destination != '\0') ++destination;
  while (length != 0 && *source != '\0') {
    *destination++ = *source++;
    --length;
  }
  *destination = '\0';
  return result;
}

char* darwin_art_bionic_strncpy(char* destination, const char* source,
                                size_t length) {
  char* result = destination;
  while (length != 0 && *source != '\0') {
    *destination++ = *source++;
    --length;
  }
  while (length-- != 0) *destination++ = '\0';
  return result;
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

static void SwapElements(unsigned char* left, unsigned char* right,
                         size_t size) {
  if (left == right) return;
  for (size_t index = 0; index < size; ++index) {
    const unsigned char byte = left[index];
    left[index] = right[index];
    right[index] = byte;
  }
}

static void SiftDown(unsigned char* bytes, size_t root, size_t count,
                     size_t size, int (*compare)(const void*, const void*)) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        compare(bytes + child * size, bytes + (child + 1) * size) < 0) {
      ++child;
    }
    if (compare(bytes + root * size, bytes + child * size) >= 0) return;
    SwapElements(bytes + root * size, bytes + child * size, size);
    root = child;
  }
}

void darwin_art_bionic_qsort(void* base, size_t count, size_t size,
                             int (*compare)(const void*, const void*)) {
  if (count < 2 || size == 0) return;
  if (base == NULL || compare == NULL || count > SIZE_MAX / size) {
    __builtin_trap();
  }
  unsigned char* bytes = (unsigned char*)base;
  for (size_t start = count / 2; start != 0; --start) {
    SiftDown(bytes, start - 1, count, size, compare);
  }
  for (size_t end = count; end > 1; --end) {
    SwapElements(bytes, bytes + (end - 1) * size, size);
    SiftDown(bytes, 0, end - 1, size, compare);
  }
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

wchar_t* darwin_art_bionic_wmemcpy(wchar_t* destination,
                                    const wchar_t* source, size_t length) {
  for (size_t index = 0; index < length; ++index) destination[index] = source[index];
  return destination;
}

wchar_t* darwin_art_bionic_wmemmove(wchar_t* destination,
                                     const wchar_t* source, size_t length) {
  if ((uintptr_t)destination <= (uintptr_t)source ||
      (uintptr_t)destination - (uintptr_t)source >= length * sizeof(wchar_t)) {
    return darwin_art_bionic_wmemcpy(destination, source, length);
  }
  while (length != 0) {
    --length;
    destination[length] = source[length];
  }
  return destination;
}

wchar_t* darwin_art_bionic_wmemset(wchar_t* destination, wchar_t value,
                                    size_t length) {
  for (size_t index = 0; index < length; ++index) destination[index] = value;
  return destination;
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
    {"__memcpy_chk", (DarwinArtBionicFunction)darwin_art_bionic___memcpy_chk},
    {"__memmove_chk", (DarwinArtBionicFunction)darwin_art_bionic___memmove_chk},
    {"__memset_chk", (DarwinArtBionicFunction)darwin_art_bionic___memset_chk},
    {"__strlen_chk", (DarwinArtBionicFunction)darwin_art_bionic___strlen_chk},
    {"atoi", (DarwinArtBionicFunction)darwin_art_bionic_atoi},
    {"atol", (DarwinArtBionicFunction)darwin_art_bionic_atol},
    {"bsearch", (DarwinArtBionicFunction)darwin_art_bionic_bsearch},
    {"memchr", (DarwinArtBionicFunction)darwin_art_bionic_memchr},
    {"memcmp", (DarwinArtBionicFunction)darwin_art_bionic_memcmp},
    {"memcpy", (DarwinArtBionicFunction)darwin_art_bionic_memcpy},
    {"memmove", (DarwinArtBionicFunction)darwin_art_bionic_memmove},
    {"memset", (DarwinArtBionicFunction)darwin_art_bionic_memset},
    {"qsort", (DarwinArtBionicFunction)darwin_art_bionic_qsort},
    {"strcasecmp", (DarwinArtBionicFunction)darwin_art_bionic_strcasecmp},
    {"strcat", (DarwinArtBionicFunction)darwin_art_bionic_strcat},
    {"strchr", (DarwinArtBionicFunction)darwin_art_bionic_strchr},
    {"strcmp", (DarwinArtBionicFunction)darwin_art_bionic_strcmp},
    {"strcpy", (DarwinArtBionicFunction)darwin_art_bionic_strcpy},
    {"strcspn", (DarwinArtBionicFunction)darwin_art_bionic_strcspn},
    {"strlen", (DarwinArtBionicFunction)darwin_art_bionic_strlen},
    {"strncat", (DarwinArtBionicFunction)darwin_art_bionic_strncat},
    {"strncmp", (DarwinArtBionicFunction)darwin_art_bionic_strncmp},
    {"strncpy", (DarwinArtBionicFunction)darwin_art_bionic_strncpy},
    {"strpbrk", (DarwinArtBionicFunction)darwin_art_bionic_strpbrk},
    {"strrchr", (DarwinArtBionicFunction)darwin_art_bionic_strrchr},
    {"strspn", (DarwinArtBionicFunction)darwin_art_bionic_strspn},
    {"strstr", (DarwinArtBionicFunction)darwin_art_bionic_strstr},
    {"wcslen", (DarwinArtBionicFunction)darwin_art_bionic_wcslen},
    {"wmemchr", (DarwinArtBionicFunction)darwin_art_bionic_wmemchr},
    {"wmemcmp", (DarwinArtBionicFunction)darwin_art_bionic_wmemcmp},
    {"wmemcpy", (DarwinArtBionicFunction)darwin_art_bionic_wmemcpy},
    {"wmemmove", (DarwinArtBionicFunction)darwin_art_bionic_wmemmove},
    {"wmemset", (DarwinArtBionicFunction)darwin_art_bionic_wmemset},
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
