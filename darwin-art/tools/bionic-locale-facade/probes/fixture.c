#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int StringEquals(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return *left == *right;
}

__attribute__((visibility("default"))) int bionic_locale_fixture_basic(void) {
  errno = 777;
  if (!StringEquals(setlocale(LC_ALL, 0), "C.UTF-8") ||
      __ctype_get_mb_cur_max() != 4 || errno != 777)
    return 1;
  if (!StringEquals(setlocale(LC_CTYPE, "C"), "C") ||
      __ctype_get_mb_cur_max() != 1)
    return 2;
  if (!StringEquals(setlocale(LC_ALL, "POSIX"), "C") ||
      !StringEquals(setlocale(LC_ALL, ""), "C.UTF-8") ||
      !StringEquals(setlocale(LC_ALL, "en_US.UTF-8"), "C.UTF-8"))
    return 3;
  errno = 0;
  if (setlocale(LC_ALL, "ko_KR.UTF-8") != 0 || errno != ENOENT) return 4;
  errno = 0;
  if (setlocale(99, "C") != 0 || errno != EINVAL) return 5;

  locale_t c = newlocale(LC_ALL_MASK, "C", 0);
  locale_t utf8 = newlocale(LC_ALL_MASK, "C.UTF-8", 0);
  if (c == 0 || utf8 == 0 || c == utf8) return 6;
  if (uselocale(c) != LC_GLOBAL_LOCALE || __ctype_get_mb_cur_max() != 1)
    return 7;
  if (uselocale(utf8) != c || __ctype_get_mb_cur_max() != 4) return 8;
  if (uselocale(0) != utf8 || uselocale(LC_GLOBAL_LOCALE) != utf8 ||
      __ctype_get_mb_cur_max() != 4)
    return 9;

  struct lconv* values = localeconv();
  if (values == 0 || values->decimal_point[0] != '.' ||
      values->decimal_point[1] != '\0' || values->thousands_sep[0] != '\0' ||
      values->int_frac_digits != CHAR_MAX ||
      values->int_n_sign_posn != CHAR_MAX)
    return 10;
  freelocale(c);
  freelocale(utf8);

  errno = 0;
  if (newlocale(LC_ALL_MASK, "de_DE.UTF-8", 0) != 0 || errno != ENOENT)
    return 11;
  errno = 0;
  if (newlocale((int)0x40000000, "C", 0) != 0 || errno != EINVAL)
    return 12;
  return 42;
}

__attribute__((visibility("default"))) int bionic_locale_fixture_multibyte(void) {
  mbstate_t state = {};
  wchar_t wide = 0;
  if (mbrtowc(&wide, "\xf0\x9f", 2, &state) != (size_t)-2) return 20;
  if (mbrtowc(&wide, "\x98\x80", 2, &state) != 2 ||
      (uint32_t)wide != 0x1f600)
    return 21;
  errno = 0;
  if (mbrtowc(&wide, "\xe0\x80\x80", 3, &state) != (size_t)-1 ||
      errno != EILSEQ)
    return 22;
  if (mbrtowc(&wide, "A", 1, &state) != 1 || wide != L'A') return 23;
  if (mbrtowc(&wide, "", 0, &state) != (size_t)-2) return 24;
  if (mbrlen("\xc2\xa2", 2, &state) != 2) return 25;
  errno = 0;
  if (mbtowc(&wide, "\xe2", 1) != -1 || errno != EILSEQ) return 26;
  if (mbtowc(0, 0, 0) != 0 || mbtowc(&wide, "Z", 1) != 1 || wide != L'Z')
    return 27;
  if (btowc('Q') != L'Q' || btowc(EOF) != WEOF || btowc(0xe2) != WEOF)
    return 28;

  char encoded[8] = {};
  state = (mbstate_t){};
  if (wcrtomb(encoded, (wchar_t)0x1f600, &state) != 4 ||
      (unsigned char)encoded[0] != 0xf0 ||
      (unsigned char)encoded[3] != 0x80)
    return 29;
  if (wcrtomb(encoded, (wchar_t)0xd800, &state) != 3) return 30;
  errno = 0;
  if (wcrtomb(encoded, (wchar_t)0x200000, &state) != (size_t)-1 ||
      errno != EILSEQ)
    return 31;
  if (wctob(L'R') != 'R' || wctob((wint_t)0x1f600) != EOF) return 32;

  const char input[] = "A\xc2\xa2\xf0\x9f\x98\x80";
  const char* input_cursor = input;
  wchar_t wide_output[8] = {};
  state = (mbstate_t){};
  if (mbsrtowcs(wide_output, &input_cursor, 8, &state) != 3 ||
      input_cursor != 0 || wide_output[0] != L'A' ||
      (uint32_t)wide_output[1] != 0xa2 ||
      (uint32_t)wide_output[2] != 0x1f600)
    return 33;

  const char incomplete[] = "\xe2\x82";
  input_cursor = incomplete;
  state = (mbstate_t){};
  errno = 0;
  if (mbsnrtowcs(wide_output, &input_cursor, 2, 8, &state) != (size_t)-1 ||
      errno != EILSEQ || input_cursor != incomplete + 2)
    return 34;

  const wchar_t wide_input[] = {L'A', (wchar_t)0xa2, (wchar_t)0x1f600, 0};
  const wchar_t* wide_cursor = wide_input;
  char byte_output[16] = {};
  state = (mbstate_t){};
  if (wcsnrtombs(byte_output, &wide_cursor, 4, sizeof(byte_output), &state) !=
          7 ||
      wide_cursor != 0 || byte_output[0] != 'A' ||
      (unsigned char)byte_output[1] != 0xc2 ||
      (unsigned char)byte_output[3] != 0xf0)
    return 35;
  return 42;
}

__attribute__((visibility("default"))) int bionic_locale_fixture_collation(void) {
  locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", 0);
  if (locale == 0) return 40;
  if (strcoll_l("abc", "abd", locale) >= 0) return 41;
  char transformed[3] = {'x', 'x', 'x'};
  if (strxfrm_l(transformed, "abcd", sizeof(transformed), locale) != 4 ||
      transformed[0] != 'a' || transformed[1] != 'b' || transformed[2] != 0)
    return 42;
  const wchar_t high[] = {(wchar_t)0xffffffffU, 0};
  const wchar_t low[] = {(wchar_t)0x7fffffffU, 0};
  if (wcscoll_l(high, low, locale) <= 0) return 43;
  wchar_t wide_transform[2] = {(wchar_t)'x', (wchar_t)'x'};
  const wchar_t wide_source[] = {L'a', L'b', L'c', 0};
  if (wcsxfrm_l(wide_transform, wide_source, 2, locale) != 3 ||
      wide_transform[0] != L'a' || wide_transform[1] != 0)
    return 44;
  freelocale(locale);
  return 42;
}

__attribute__((visibility("default"))) int bionic_locale_fixture_thread(int utf8) {
  locale_t locale = newlocale(LC_ALL_MASK, utf8 ? "C.UTF-8" : "C", 0);
  if (locale == 0 || uselocale(locale) != LC_GLOBAL_LOCALE) return 50;
  for (int iteration = 0; iteration < 10000; ++iteration) {
    if (__ctype_get_mb_cur_max() != (utf8 ? 4U : 1U)) return 51;
  }
  wchar_t output = 0;
  if (mbrtowc(&output, "\xf0", 1, 0) != (size_t)-2 ||
      mbrtowc(&output, "\x9f\x98\x80", 3, 0) != 3 ||
      (uint32_t)output != 0x1f600)
    return 52;
  if (uselocale(LC_GLOBAL_LOCALE) != locale) return 53;
  freelocale(locale);
  return 42;
}
