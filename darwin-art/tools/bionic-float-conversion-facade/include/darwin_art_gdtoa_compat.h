#ifndef DARWIN_ART_GDTOA_COMPAT_H_
#define DARWIN_ART_GDTOA_COMPAT_H_

#define __BEGIN_HIDDEN_DECLS _Pragma("GCC visibility push(hidden)")
#define __END_HIDDEN_DECLS _Pragma("GCC visibility pop")
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))
#define DEF_STRONG(symbol)
#define DEF_WEAK(symbol)
#define PROTO_NORMAL(symbol)

#endif  // DARWIN_ART_GDTOA_COMPAT_H_
