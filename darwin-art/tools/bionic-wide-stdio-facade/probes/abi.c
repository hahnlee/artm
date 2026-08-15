#include "darwin_art_bionic_wide_stdio.h"
#include "darwin_art_bionic_stdio.h"

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

_Static_assert(sizeof(wchar_t) == 4, "Android wchar_t is 32-bit");
_Static_assert((wchar_t)-1 > 0, "Android wchar_t is unsigned");
_Static_assert(sizeof(DarwinArtAndroidFile) == 152,
               "provider-local Android FILE token drift");
_Static_assert(_Alignof(DarwinArtAndroidFile) == 8,
               "provider-local Android FILE token alignment drift");
_Static_assert(WEOF == UINT32_C(0xffffffff), "Android WEOF drift");
_Static_assert(sizeof(DarwinArtBionicWideStdioBackendV1) == 72,
               "backend ABI drift");

static uint32_t (*const check_fputwc)(uint32_t, DarwinArtAndroidFile*) =
    darwin_art_bionic_fputwc;
static uint32_t (*const check_getwc)(DarwinArtAndroidFile*) =
    darwin_art_bionic_getwc;
static uint32_t (*const check_ungetwc)(uint32_t, DarwinArtAndroidFile*) =
    darwin_art_bionic_ungetwc;

int main(void) {
  return check_fputwc == 0 || check_getwc == 0 || check_ungetwc == 0;
}
