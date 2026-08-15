#include "darwin_art_bionic_dso_lifecycle.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static DarwinArtBionicDsoDestructor gFunction;
static void* gArgument;
static void* gDso;
static unsigned gCalls;

int darwin_art_bionic_dso_cxa_atexit_core(
    DarwinArtBionicDsoDestructor function, void* argument, void* dso) {
  if (function == NULL) return -1;
  gFunction = function;
  gArgument = argument;
  gDso = dso;
  return 0;
}

void darwin_art_bionic_dso_cxa_finalize_core(void* dso) {
  if (gFunction == NULL || dso != gDso) return;
  DarwinArtBionicDsoDestructor function = gFunction;
  void* argument = gArgument;
  gFunction = NULL;
  function(argument);
}

static void Destructor(void* argument) {
  if (argument != gArgument) abort();
  ++gCalls;
}

int main(void) {
  uintptr_t argument_storage = UINT64_C(0x1020304050607080);
  uintptr_t dso_storage = UINT64_C(0x8877665544332211);
  errno = 31001;
  if (darwin_art_bionic___cxa_atexit(Destructor, &argument_storage,
                                     &dso_storage) != 0 ||
      errno != 31001 || gFunction != Destructor || gArgument != &argument_storage ||
      gDso != &dso_storage)
    return 1;
  if (darwin_art_bionic_dso_lifecycle_resolve("__cxa_atexit") !=
          (DarwinArtBionicDsoFunction)darwin_art_bionic___cxa_atexit ||
      darwin_art_bionic_dso_lifecycle_resolve("__cxa_finalize") !=
          (DarwinArtBionicDsoFunction)darwin_art_bionic___cxa_finalize ||
      darwin_art_bionic_dso_lifecycle_resolve("cxa_finalize") != NULL)
    return 2;
  errno = 31002;
  darwin_art_bionic___cxa_finalize(&dso_storage);
  if (errno != 31002 || gCalls != 1 || gFunction != NULL) return 3;
  darwin_art_bionic___cxa_finalize(&dso_storage);
  if (gCalls != 1) return 4;
  return 0;
}
