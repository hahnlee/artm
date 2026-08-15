# Darwin ART ELF loader vertical slice

This standalone crate proves a bounded Android `.so` execution path on Apple
Silicon. It reserves one Darwin virtual-address range for an AArch64 `ET_DYN`,
copies and zero-fills checked `PT_LOAD` segments, applies
`R_AARCH64_RELATIVE`, `R_AARCH64_ABS64`, `R_AARCH64_GLOB_DAT`, and
`R_AARCH64_JUMP_SLOT` RELA entries, installs final page protections, executes
`DT_INIT_ARRAY` once in table order, and calls a defined no-argument export.

Run `./run-gate.sh` on Apple Silicon with Android NDK r28 or newer. The fixtures
are linked without libc or startup objects. A versioned virtual provider proves
real function, data, and function-pointer imports. Its addresses come only from
the caller's `SymbolResolver`; the loader never calls `dlopen`, `dlsym`, or dyld.
`DT_NEEDED` order and strings are preserved, and GNU VERSYM/VERNEED symbol,
version, flags, hidden bit, and provider SONAME are passed to the resolver.
Missing weak undefined symbols resolve to zero; missing strong symbols fail.

This is intentionally not a general dynamic linker. PLT relocation is eager and
requires `DT_BIND_NOW`, `DF_BIND_NOW`, or `DF_1_NOW`; lazy binding is rejected.
GNU RELRO is also rejected rather than silently weakened. TLS, `DT_REL`,
`DT_RELR`, version definitions, other relocation kinds, `DT_INIT`,
preinit/finalizer arrays, text relocations, symbolic lookup, and RPATH/RUNPATH
return explicit capability errors. `LoadedElf` owns the complete `mmap`
reservation and unmaps it on every error path or `Drop`. Resolver-provided
addresses must remain valid until the image is dropped.

The fixtures use only the no-argument AArch64 procedure-call subset. Android JNI
functions with spilled arguments still need a per-shorty Darwin-to-Android PCS
repacking trampoline; this gate does not claim general JNI `.so` compatibility.

## C ABI seam

`include/darwin_art_elf_loader.h` exposes the crate as a static library with
bytes/path loading, a closed synchronous resolver callback, initializer control,
export lookup, and explicit unload. Every entrypoint returns a stable structured
status and optionally fills `DarwinArtElfErrorBuffer`; `required` includes the
terminating NUL even when the message was truncated. Rust panics are caught at
every fallible C entrypoint.

Handles are unique and opaque. Init/lookup are internally synchronized and may
move across ART-attached threads. Unload may also run on another thread, but the
adapter must first establish quiescence: no concurrent ABI operation and no code
executing through a borrowed lookup/import address. Resolver request storage is
valid only during the callback, while every FOUND address must remain valid until
unload. Because lazy binding is rejected, the callback/context is not retained.

A NativeBridge open adapter must keep the handle private until initializer and
preflight success, and unload it privately on failure. Export addresses are
invalid after unload; callers own their exact ABI. The Objective-C++ smoke
validates header layout, both load forms, versioned imports, cross-thread
lookup/unload, lifecycle, structured errors, and idempotent unload.
