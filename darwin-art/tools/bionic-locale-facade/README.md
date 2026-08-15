# Android 35 Bionic locale/multibyte facade

This standalone Darwin arm64 provider owns 19 of the exact 32 Class-C
locale-related imports in the pinned NDK r28c/API 35 arm64
`libc++_shared.so`. It implements one coherent state boundary rather than
forwarding Android objects to similarly named Darwin functions.

The supported owner contains:

- `newlocale`, `freelocale`, `uselocale`, `setlocale`, `localeconv`, and
  `__ctype_get_mb_cur_max`;
- `btowc`, `wctob`, `mbrlen`, `mbrtowc`, `mbtowc`, `mbsrtowcs`,
  `mbsnrtowcs`, `wcrtomb`, and `wcsnrtombs`;
- Bionic's C-locale `strcoll_l`, `strxfrm_l`, `wcscoll_l`, and `wcsxfrm_l`.

Only the names Bionic itself accepts are supported: `C`, `POSIX`, `C.UTF-8`,
`en_US.UTF-8`, and the empty environment-selection name, which Bionic maps to
UTF-8 without consulting the host environment. Unknown names fail with Android
`ENOENT`; invalid categories and masks fail with Android `EINVAL`.

## State and ABI boundary

Android `locale_t` is represented by provider-owned immutable entries in a
locked side table. `uselocale` stores only the provider handle and mode in
Darwin thread-local state; the all-ones Android `LC_GLOBAL_LOCALE` sentinel is
handled explicitly. No Android handle is cast to Darwin `locale_t`, and the
provider never calls host `setlocale`, `newlocale`, or multibyte routines. The
process locale is one atomic UTF-8/C mode, while explicit thread locales remain
stable across concurrent process-mode updates.

Android arm64 `mbstate_t` is exactly eight bytes with the first four bytes
holding Bionic's partial UTF-8 sequence. The provider implements the pinned
`mbrtoc32`/`c32rtomb` algorithms directly, including incomplete `-2`, illegal
`-1` plus Android `EILSEQ`, reset-after-error, overlong/surrogate decode
rejection, and Bionic's historical encoder range through `0x1fffff`. Null-state
conversion storage is thread-local rather than a Darwin `mbstate_t`.

Android arm64 uses unsigned 32-bit `wchar_t`; collation and transformation use
explicit `uint32_t` comparison/copy loops. This preserves ordering above
`INT32_MAX` and avoids passing Android wide strings to Darwin wide APIs.
`localeconv` exposes the Android 96-byte layout and unsigned-char `CHAR_MAX`
bytes, not Darwin's `struct lconv`.

All provider calls save and restore Darwin pthread errno and floating-point
environment. Capability failures are written only to the existing standalone
Bionic errno TLS owner. The locale archive deliberately leaves that provider
as an unresolved integration dependency, preventing a duplicate `__errno`
owner.

## Explicitly unsupported

The ten `isw*_l` imports and `towlower_l`/`towupper_l` are not approximated with
Darwin tables: pinned Bionic obtains their Unicode properties and mappings from
Android ICU. They require the Android ICU data/version owner before they can be
added coherently. `strftime_l` is also excluded because its implementation is
the full Bionic tzcode `strftime` owner, including timezone state and Android
format extensions; forwarding to Darwin would overclaim compatibility.

Thus the closed resolver exposes exactly 19 `libc.so@LIBC` symbols. It has no
`dlsym`, dyld, unprefixed interposition, host-global locale mutation, or fallback
for the remaining 13 imports.

## Provenance and acceptance

Run:

```sh
tools/bionic-locale-facade/audit.sh
```

The gate sparsely materializes 21 SHA-locked Bionic implementation/header files
without Git metadata. It derives the 32-symbol cluster from the canonical
160-import manifest, checks the 19/13 partition, validates Android
`locale_t`/`mbstate_t`/`wchar_t`/`lconv` layouts, and builds a real NDK AArch64
ELF importing all 19 functions plus `__errno`. The loaded fixture exercises
locale aliases and failures, UTF-8 partial/invalid conversion and source-pointer
rules, unsigned wide collation, and eight concurrent thread locales. A separate
ASan/UBSan stress performs 800 locale lifecycles and requires zero live handles.
Host errno and fenv preservation are checked across the full Android ELF run.

Artifacts are written to `_build/bionic-locale-facade`.
