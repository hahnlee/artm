#include <locale.h>
#include <stddef.h>
#include <wchar.h>

_Static_assert(sizeof(locale_t) == 8, "Android arm64 locale_t drift");
_Static_assert(sizeof(mbstate_t) == 8, "Android arm64 mbstate_t drift");
_Static_assert(_Alignof(mbstate_t) == 1, "Android arm64 mbstate_t alignment drift");
_Static_assert(sizeof(wchar_t) == 4, "Android arm64 wchar_t drift");
_Static_assert(sizeof(struct lconv) == 96, "Android arm64 lconv drift");
_Static_assert(LC_ALL_MASK == 0x1fbf, "Android locale mask drift");

static size_t (*mbrtowc_signature)(wchar_t*, const char*, size_t, mbstate_t*) =
    mbrtowc;
static locale_t (*newlocale_signature)(int, const char*, locale_t) = newlocale;

int main(void) {
  return mbrtowc_signature == 0 || newlocale_signature == 0;
}
