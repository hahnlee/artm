#ifndef DARWIN_ART_BIONIC_LOCALE_H_
#define DARWIN_ART_BIONIC_LOCALE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* DarwinArtAndroidLocale;

typedef struct DarwinArtAndroidMbState {
  uint8_t sequence[4];
  uint8_t reserved[4];
} DarwinArtAndroidMbState;

typedef struct DarwinArtAndroidLconv {
  char* decimal_point;
  char* thousands_sep;
  char* grouping;
  char* int_curr_symbol;
  char* currency_symbol;
  char* mon_decimal_point;
  char* mon_thousands_sep;
  char* mon_grouping;
  char* positive_sign;
  char* negative_sign;
  char int_frac_digits;
  char frac_digits;
  char p_cs_precedes;
  char p_sep_by_space;
  char n_cs_precedes;
  char n_sep_by_space;
  char p_sign_posn;
  char n_sign_posn;
  char int_p_cs_precedes;
  char int_p_sep_by_space;
  char int_n_cs_precedes;
  char int_n_sep_by_space;
  char int_p_sign_posn;
  char int_n_sign_posn;
} DarwinArtAndroidLconv;

size_t darwin_art_bionic___ctype_get_mb_cur_max(void);
uint32_t darwin_art_bionic_btowc(int byte);
void darwin_art_bionic_freelocale(DarwinArtAndroidLocale locale);
DarwinArtAndroidLconv* darwin_art_bionic_localeconv(void);
size_t darwin_art_bionic_mbrlen(const char* source,
                                size_t length,
                                DarwinArtAndroidMbState* state);
size_t darwin_art_bionic_mbrtowc(uint32_t* output,
                                const char* source,
                                size_t length,
                                DarwinArtAndroidMbState* state);
size_t darwin_art_bionic_mbsnrtowcs(uint32_t* destination,
                                   const char** source,
                                   size_t source_length,
                                   size_t destination_length,
                                   DarwinArtAndroidMbState* state);
size_t darwin_art_bionic_mbsrtowcs(uint32_t* destination,
                                  const char** source,
                                  size_t destination_length,
                                  DarwinArtAndroidMbState* state);
int darwin_art_bionic_mbtowc(uint32_t* output,
                            const char* source,
                            size_t length);
DarwinArtAndroidLocale darwin_art_bionic_newlocale(
    int category_mask,
    const char* locale_name,
    DarwinArtAndroidLocale base);
char* darwin_art_bionic_setlocale(int category, const char* locale_name);
int darwin_art_bionic_strcoll_l(const char* left,
                               const char* right,
                               DarwinArtAndroidLocale locale);
size_t darwin_art_bionic_strxfrm_l(char* destination,
                                  const char* source,
                                  size_t length,
                                  DarwinArtAndroidLocale locale);
DarwinArtAndroidLocale darwin_art_bionic_uselocale(
    DarwinArtAndroidLocale locale);
size_t darwin_art_bionic_wcrtomb(char* destination,
                                uint32_t code_point,
                                DarwinArtAndroidMbState* state);
int darwin_art_bionic_wcscoll_l(const uint32_t* left,
                               const uint32_t* right,
                               DarwinArtAndroidLocale locale);
size_t darwin_art_bionic_wcsnrtombs(char* destination,
                                   const uint32_t** source,
                                   size_t source_length,
                                   size_t destination_length,
                                   DarwinArtAndroidMbState* state);
size_t darwin_art_bionic_wcsxfrm_l(uint32_t* destination,
                                  const uint32_t* source,
                                  size_t length,
                                  DarwinArtAndroidLocale locale);
int darwin_art_bionic_wctob(uint32_t code_point);

void* darwin_art_bionic_locale_resolve(const char* soname,
                                       const char* symbol,
                                       const char* version);
int darwin_art_bionic_locale_capability(const char* capability);
size_t darwin_art_bionic_locale_live_handle_count(void);

/* Hidden standalone-gate helpers; never exposed by the libc resolver. */
void darwin_art_bionic_locale_test_prepare_host_state(void);
int darwin_art_bionic_locale_test_host_state_is_preserved(void);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_LOCALE_H_
