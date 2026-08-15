#include "darwin_art_bionic_process_state.h"

#include <errno.h>
#include <stddef.h>

char* darwin_art_bionic_getenv(const char* name) {
  const int saved_host_errno = errno;
  char* result = darwin_art_bionic_process_getenv_core(name);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic___system_property_get(const char* name, char* value) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_process_property_get_core(name, value);
  errno = saved_host_errno;
  return result;
}

unsigned long darwin_art_bionic_getauxval(unsigned long type) {
  const int saved_host_errno = errno;
  const unsigned long result = darwin_art_bionic_process_getauxval_core(type);
  errno = saved_host_errno;
  return result;
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

typedef struct Binding {
  const char* name;
  DarwinArtBionicProcessFunction address;
} Binding;

static const Binding kBindings[] = {
    {"__system_property_get",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___system_property_get},
    {"getauxval", (DarwinArtBionicProcessFunction)darwin_art_bionic_getauxval},
    {"getenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_getenv},
};

DarwinArtBionicProcessFunction darwin_art_bionic_process_state_resolve(
    const char* name) {
  if (name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = NameCompare(name, kBindings[middle].name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}
