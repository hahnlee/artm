# Android 35 libc++ acceptance fixture

This gate pins the NDK r28c/API 35 arm64 `libc++_shared.so` and proves two
separate boundaries without substituting a Darwin C++ runtime.

`audit.sh` first gives the real 9 MiB ELF to `darwin-art-elf-loader`. A closed
placeholder resolver is used only for a **non-executing structural proof**: all
4,267 supported RELA entries are applied, all 160 strong `@LIBC` imports plus
the one optional unversioned `__cxa_thread_atexit_impl` are observed, GNU
versions and `DT_NEEDED` ownership are checked, RELRO is sealed, and a real
libc++ export is looked up. No initializer or finalizer is called with the
placeholder addresses.

The gate then builds `libdarwin_art_libcxx_consumer.so`. Its only dependency is
the pinned `libc++_shared.so`, and its four imports are libc++ exports. The
no-argument `darwin_art_libcxx_collections` function crosses libc++ heap and ABI
boundaries with a non-SSO `std::string`, `std::vector`, `std::sort`, bounds
checking, allocation, and deletion. A real closed-namespace execution must
return `189` after libc++, libc, libm, and libdl providers have been initialized.

Exceptions are deliberately outside this first executable consumer. Compiling
the equivalent throw/catch probe introduces `_Unwind_Resume@LIBC_R`; the
current Bionic provider namespace is intentionally pinned to `@LIBC`. Claiming
exception execution before adding an exact `LIBC_R` owner and validating ELF
unwind registration would overstate compatibility. Iostream is also deferred;
it adds global stream initialization and obscures whether this focused
collection/allocator path works.

Run:

```sh
./tools/android35-libcxx-acceptance/audit.sh
```
