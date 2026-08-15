# Android 35 libc++ provider coverage

This gate measures coherent standalone provider ownership against the exact
160 `libc.so` imports of the SHA-locked NDK r28c/API 35 arm64
`libc++_shared.so`. It is deliberately stricter than counting functions that
happen to exist in Darwin libc: a symbol is covered only when a project facade
claims its Android ABI and semantic state.

The current unique ownership set is 73 imports:

| Provider | Imports | Boundary |
|---|---:|---|
| leaf | 11 | state-free byte and 32-bit wide-character operations |
| allocator | 4 | Darwin allocation owner with explicit Android failure seam |
| errno | 1 | pthread-local Bionic errno cell |
| filesystem | 13 | virtual FDs, byte paths, translated stat/dirent |
| time | 3 | Android clocks, nanosleep, and sysconf selectors |
| pthread | 24 | TLS, once, mutex/attributes, condition variables, rwlocks, join/detach |
| process-state | 3 | immutable environment, properties, and auxv snapshot |
| phdr | 1 | loader-owned Android ELF program-header snapshots |
| stdio | 13 | Android FILE tokens, `__sF`, and fixed-register binary I/O |

Coverage by the original ABI classification is A 11/11, B 29/76, C 30/65,
and D 3/8. The remaining 87 imports stay explicit capability failures. The
largest gaps are Android `FILE`/stdio and locale state, translated numeric and
formatting routines, additional writable filesystem operations, process
lifecycle, and raw syscall handling.

This number does **not** mean the 58 providers are already composed into one
ART ELF namespace. Most are independently executable gates. The runtime must
still establish one resolver order, reject duplicate owners, share the same
errno/TLS/process snapshots, and prove teardown as a unit. The audit prints
`scope=standalone-gates-not-yet-one-runtime-namespace` to prevent that
overclaim.

Run `tools/audit-android35-libcxx-provider-coverage.sh`. It hash-locks every
input manifest, rejects providers outside the actual libc++ import set,
rejects duplicate owners, and pins provider and capability-class counts.
