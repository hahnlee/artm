#ifndef DARWIN_ART_BIONIC_MATH_H_
#define DARWIN_ART_BIONIC_MATH_H_
#ifdef __cplusplus
extern "C" {
#endif
void* darwin_art_bionic_math_resolve(const char* soname, const char* symbol,
                                     const char* version);
#ifdef __cplusplus
}
#endif
#endif
