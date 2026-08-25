#include "darwin_art_bionic_dso_lifecycle.h"

#include <errno.h>
#include <stddef.h>

extern void* darwin_art_bionic_android_dlopen_ext(const char*, int,
                                                   const void*);
extern int darwin_art_bionic_dlclose(void*);
extern char* darwin_art_bionic_dlerror(void);
extern void* darwin_art_bionic_dlopen(const char*, int);
extern void* darwin_art_bionic_dlsym(void*, const char*);

int darwin_art_bionic___cxa_atexit(DarwinArtBionicDsoDestructor function,
                                   void* argument, void* dso) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_dso_cxa_atexit_core(function, argument, dso);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic___cxa_thread_atexit_impl(
    DarwinArtBionicDsoDestructor function, void* argument, void* dso) {
  const int saved_host_errno = errno;
  const int result =
      darwin_art_bionic_dso_cxa_thread_atexit_core(function, argument, dso);
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

int darwin_art_bionic_dladdr_unsupported(const void* address, void* info) {
  (void)address;
  (void)info;
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
    {"__cxa_thread_atexit_impl",
     (DarwinArtBionicDsoFunction)darwin_art_bionic___cxa_thread_atexit_impl},
    {"__register_atfork",
     (DarwinArtBionicDsoFunction)darwin_art_bionic___register_atfork},
    {"android_dlopen_ext",
     (DarwinArtBionicDsoFunction)darwin_art_bionic_android_dlopen_ext},
    {"dladdr", (DarwinArtBionicDsoFunction)darwin_art_bionic_dladdr_unsupported},
    {"dlclose", (DarwinArtBionicDsoFunction)darwin_art_bionic_dlclose},
    {"dlerror", (DarwinArtBionicDsoFunction)darwin_art_bionic_dlerror},
    {"dlopen", (DarwinArtBionicDsoFunction)darwin_art_bionic_dlopen},
    {"dlsym", (DarwinArtBionicDsoFunction)darwin_art_bionic_dlsym},
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
