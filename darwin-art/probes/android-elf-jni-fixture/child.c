#include <stdint.h>

extern void darwin_art_fixture_record_lifecycle(int phase);

static int g_child_state;

__attribute__((constructor)) static void ChildInitialize(void) {
  g_child_state = 20;
  darwin_art_fixture_record_lifecycle(1);
}

__attribute__((destructor)) static void ChildFinalize(void) {
  darwin_art_fixture_record_lifecycle(5);
}

__attribute__((visibility("default"))) int DarwinArtFixtureChildValue(void) {
  return g_child_state;
}
