#ifndef DARWIN_ART_BIONIC_NUMERIC_H_
#define DARWIN_ART_BIONIC_NUMERIC_H_

typedef void* DarwinArtAndroidLocale;
typedef void (*DarwinArtBionicNumericFunction)(void);

#ifdef __cplusplus
extern "C" {
#endif

long darwin_art_bionic_strtol(const char*, char**, int);
long long darwin_art_bionic_strtoll(const char*, char**, int);
unsigned long darwin_art_bionic_strtoul(const char*, char**, int);
unsigned long long darwin_art_bionic_strtoull(const char*, char**, int);
long long darwin_art_bionic_strtoll_l(const char*, char**, int,
                                      DarwinArtAndroidLocale);
unsigned long long darwin_art_bionic_strtoull_l(const char*, char**, int,
                                                DarwinArtAndroidLocale);

DarwinArtBionicNumericFunction darwin_art_bionic_numeric_resolve(const char*);

#ifdef __cplusplus
}
#endif
#endif
