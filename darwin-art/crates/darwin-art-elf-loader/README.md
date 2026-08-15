# Darwin ART ELF loader vertical slice

This standalone crate proves the smallest honest Android `.so` execution path on
Apple Silicon. It reserves one Darwin virtual-address range for an AArch64
`ET_DYN`, copies and zero-fills checked `PT_LOAD` segments, applies only
`R_AARCH64_RELATIVE` `DT_RELA` entries, installs final page protections, executes
`DT_INIT_ARRAY` once in table order, and calls a defined no-argument exported
function.

Run `./run-gate.sh` on Apple Silicon with Android NDK r28 or newer. The fixtures
are linked without libc, imports, or startup objects. The gate also builds
negative import and TLS fixtures and mutates the positive ELF to exercise W^X,
overlap, integer-overflow, truncation, and file-bounds rejection.

This is intentionally not a general dynamic linker. `DT_NEEDED`, undefined
symbols, TLS, PLT, `DT_REL`, `DT_RELR`, non-relative relocations, `DT_INIT`,
preinit/finalizer arrays, text relocations, and RPATH/RUNPATH return explicit
capability errors. `LoadedElf` owns the complete `mmap` reservation and unmaps it
on every error path or `Drop`; initializer and exported function pointers never
escape that lifetime.
