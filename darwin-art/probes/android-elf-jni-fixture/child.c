#include <stdint.h>

extern void darwin_art_fixture_record_lifecycle(int phase);
extern int DarwinArtFixtureGrandchildValue(void);
extern int __cxa_atexit(void (*function)(void*), void* argument, void* dso);

__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

static int g_child_state;

static void ChildCxaFinalize(void* argument) {
  if (argument == &g_child_state) {
    darwin_art_fixture_record_lifecycle(6);
  }
}

__attribute__((constructor)) static void ChildInitialize(void) {
  g_child_state = DarwinArtFixtureGrandchildValue() + 10;
  if (__cxa_atexit(&ChildCxaFinalize, &g_child_state, __dso_handle) != 0) {
    g_child_state = -1;
  }
  darwin_art_fixture_record_lifecycle(1);
}

__attribute__((destructor)) static void ChildFinalize(void) {
  darwin_art_fixture_record_lifecycle(7);
}

__attribute__((visibility("default"))) int DarwinArtFixtureChildValue(void) {
  return g_child_state;
}
