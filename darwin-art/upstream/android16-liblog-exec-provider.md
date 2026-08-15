# Android 16 executable liblog provider

This provider closes the API 35 `liblog.so` virtual SONAME with real addresses
from the pinned Android 16 AOSP Darwin archive. It is separate from the ELF
loader and the namespace policy crate so it can be integrated without changing
runtime ownership.

The provider table contains exactly the 18 sorted public symbols exported by
the NDK r28c API 35 `liblog.so` stub. It is compiled normally against
`liblog-darwin.a`; no `dlsym`, Darwin global lookup, generated shim, or fake
function body is used. Resolution is an explicit name/ordinal table. Unknown
symbols and non-empty GNU versions return address zero because the API 35 NDK
stub is unversioned.

The gate also links a dylib whose exported-symbol list contains only the four
provider C ABI functions. AOSP archive globals, including private liblog APIs,
cannot be discovered through the dylib's external Mach-O namespace. Their real
addresses are reachable only for the 18 allowlisted Android names.

Run:

```sh
tools/build-android16-liblog-exec-provider.sh
```

Acceptance checks all addresses are nonzero and distinct, compares the provider
manifest with the NDK stub, rejects private/version-mismatched lookups, and calls
the resolved `__android_log_write`, variadic `__android_log_print`,
`__android_log_write_log_message`, `__android_log_set_logger`, and
`__android_log_stderr_logger` APIs using a capturing logger.

