# Android 35 libc++ provider coverage

This gate measures coherent standalone provider ownership against the exact
160 `libc.so` imports of the SHA-locked NDK r28c/API 35 arm64
`libc++_shared.so`. It is deliberately stricter than counting functions that
happen to exist in Darwin libc: a symbol is covered only when a project facade
claims its Android ABI and semantic state.

The current composed namespace owns 147 imports through twenty-one providers
(including the distinct liblog owner):

| Provider | Imports | Boundary |
|---|---:|---|
| leaf | 11 | state-free byte and 32-bit wide-character operations |
| allocator | 4 | Darwin allocation owner with explicit Android failure seam |
| errno | 1 | pthread-local Bionic errno cell |
| filesystem | 29 | virtual FDs, byte paths, translated stat/dirent |
| time | 3 | Android clocks, nanosleep, and sysconf selectors |
| pthread | 24 | TLS, once, mutex/attributes, condition variables, rwlocks, join/detach |
| process-state | 3 | immutable environment, properties, and auxv snapshot |
| phdr | 1 | loader-owned Android ELF program-header snapshots |
| stdio | 13 | Android FILE tokens, `__sF`, and fixed-register binary I/O |
| locale | 31 | ICU 76 locale, multibyte, and wide-character semantics |
| numeric | 6 | narrow integer conversion |
| float-conversion | 2 | renamed AOSP gdtoa `strtod`/`strtof` |
| format | 3 | Android variadic formatting ABI |
| formatted-stdio | 2 | bounded formatter output committed to provider-local Android `FILE` tokens |
| strerror | 1 | Bionic errno message ownership |
| wide-integer | 4 | unsigned Android wchar32 integer conversion |
| wide-float | 2 | ICU whitespace plus AOSP gdtoa `wcstod`/`wcstof` |
| abort | 2 | Bionic abort/message state |
| syslog | 3 | Android variadic capture, Bionic state, and AOSP liblog routing |
| lifecycle | 2 | Bionic DSO finalizer ownership |

Coverage by the original ABI classification is A 11/11, B 65/76, C 64/65,
and D 7/8. The remaining 13 imports stay explicit capability failures. In
particular, `wcstold` remains rejected because Android arm64 uses binary128
while Darwin arm64 uses binary64 for `long double`.

These manifests feed the generated closed provider namespace used by ART's ELF
graph resolver. The runtime closure still requires exactly one allocator,
errno/gdtoa, and ICU owner; the wide-float archive contains none of those
dependencies itself. The formatted-stdio archive likewise owns only
`fprintf`/`vfprintf` and reuses the one format, stdio, allocator, and errno
owners.

Run `tools/audit-android35-libcxx-provider-coverage.sh`. It hash-locks every
input manifest, rejects providers outside the actual libc++ import set,
rejects duplicate owners, and pins provider and capability-class counts.
