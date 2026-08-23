#include "darwin_art_bionic_dso_lifecycle.h"

#include <errno.h>
#include <stddef.h>

int darwin_art_bionic___cxa_atexit(DarwinArtBionicDsoDestructor function,
                                   void* argument, void* dso) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_dso_cxa_atexit_core(function, argument, dso);
  errno = saved_host_errno;
  return result;
}

void darwin_art_bionic___cxa_finalize(void* dso) {
  const int saved_host_errno = errno;
  darwin_art_bionic_dso_cxa_finalize_core(dso);
  errno = saved_host_errno;
}

int darwin_art_bionic___register_atfork(void* prepare, void* parent,
                                        void* child, void* arg) {
  (void)prepare;
  (void)parent;
  (void)child;
  (void)arg;
  return 0;
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
  DarwinArtBionicDsoFunction address;
} Binding;

static const Binding kBindings[] = {
    {"__cxa_atexit",
     (DarwinArtBionicDsoFunction)darwin_art_bionic___cxa_atexit},
    {"__cxa_finalize",
     (DarwinArtBionicDsoFunction)darwin_art_bionic___cxa_finalize},
    {"__register_atfork",
     (DarwinArtBionicDsoFunction)darwin_art_bionic___register_atfork},
};

DarwinArtBionicDsoFunction darwin_art_bionic_dso_lifecycle_resolve(
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
