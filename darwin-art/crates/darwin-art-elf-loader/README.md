# Darwin ART ELF loader vertical slice

This standalone crate proves a bounded Android `.so` execution path on Apple
Silicon. It reserves one Darwin virtual-address range for an AArch64 `ET_DYN`,
copies and zero-fills checked `PT_LOAD` segments, applies
`R_AARCH64_RELATIVE`, `R_AARCH64_ABS64`, `R_AARCH64_GLOB_DAT`, and
`R_AARCH64_JUMP_SLOT` RELA entries, installs final page protections, executes
`DT_INIT_ARRAY` once in table order, calls a defined no-argument export, and
executes `DT_FINI_ARRAY` in reverse table order followed by `DT_FINI` at unload.

Run `./run-gate.sh` on Apple Silicon with Android NDK r28 or newer. The fixtures
are linked without libc or startup objects. A versioned virtual provider proves
real function, data, and function-pointer imports. Its addresses come only from
the caller's `SymbolResolver`; the loader never calls `dlopen`, `dlsym`, or dyld.
`DT_NEEDED` order and strings are preserved, and GNU VERSYM/VERNEED symbol,
version, flags, hidden bit, and provider SONAME are passed to the resolver.
Missing weak undefined symbols resolve to zero; missing strong symbols fail.

`ClosedElfNamespace` adds a recursive graph layer without introducing path
search. Each ELF byte source is registered under its exact embedded
`DT_SONAME`; an optional caller-owned `SymbolResolver` is the only non-ELF
provider source. All objects are staged before relocation, so dependency cycles
can resolve against already mapped peers. Lookup is limited to the requesting
object and its breadth-first `DT_NEEDED` closure. Versioned requests are pinned
to their VERNEED provider SONAME and matched against GNU VERDEF/VERSYM exports.
Constructors run dependency-first (deterministic DFS postorder for a cycle).
Clones of one loaded graph share an owner, and the final clone close unmaps
objects in reverse constructor order, running each object's finalizers before
its mapping is released. Separate `load` calls do not reuse a namespace-global
`dlopen` cache. Non-final clones cannot trigger teardown.

The NDK r28c graph fixture loads a parent and two real AArch64 dependencies. It
also covers missing weak imports, a GNU version mismatch, an accepted two-node
cycle, unknown dependency rollback, and two independent namespaces containing
different bytes under the same SONAME. Source insertion order is deliberately
different from dependency traversal order.

This is intentionally not a general dynamic linker. PLT relocation is eager and
requires `DT_BIND_NOW`, `DF_BIND_NOW`, or `DF_1_NOW`; lazy binding is rejected.
GNU RELRO is also rejected rather than silently weakened. TLS, `DT_REL`,
`DT_RELR`, other relocation kinds, `DT_INIT`, preinit arrays, text relocations,
symbolic lookup, and RPATH/RUNPATH return explicit capability errors.
`LoadedElf` owns the complete `mmap` reservation and unmaps it on every error
path or `Drop`. Resolver-provided addresses must remain valid until the image is
dropped.

Finalizer metadata and relocated targets are validated before constructors can
run. An image that was never initialized never runs finalizers; relocation or
preflight failure therefore tears mappings down silently. `LoadedElf::close`
and `Drop` share the same exactly-once path. Graph construction completes all
relocation/finalizer preflight before its dependency-first constructor pass, so
there is no structurally partially initialized graph. A constructor terminating
the process or unwinding across its C ABI is outside this loader's recovery
contract.

This slice calls only ELF `DT_FINI_ARRAY` and `DT_FINI`. Bionic's C++
`__cxa_atexit`/`__cxa_finalize(dso_handle)` registry is a separate provider seam:
a composed Bionic lifecycle provider must finalize its DSO registrations before
the ELF object's arrays run. That orchestration is intentionally not synthesized
by this loader. ELF TLS remains rejected, including finalizers that would
require a guest TLS runtime.

Graph exports with default or protected visibility participate in the closed
dependency scope. Nonzero `SHN_ABS` definitions are rejected explicitly rather
than being mistaken for mapping-relative addresses.

The fixtures use only the no-argument AArch64 procedure-call subset. Android JNI
functions with spilled arguments still need a per-shorty Darwin-to-Android PCS
repacking trampoline; this gate does not claim general JNI `.so` compatibility.

## C ABI seam

`include/darwin_art_elf_loader.h` exposes the crate as a static library with
bytes/path loading, a closed synchronous resolver callback, initializer control,
export lookup, and explicit unload. The additive graph ABI accepts an exact root
SONAME, an in-memory SONAME-to-bytes table, and an explicit virtual-provider
SONAME set. `graph_load` performs the complete recursive relocation and
dependency-first constructor transaction before publishing its opaque handle;
root lookup and dependent-first graph unload use that same owner. There is no C
ABI path search. Every entrypoint returns a stable structured status and
optionally fills `DarwinArtElfErrorBuffer`; `required` includes the terminating
NUL even when the message was truncated. Rust panics are caught at every
fallible C entrypoint.

Handles are unique and opaque. Init/lookup are internally synchronized and may
move across ART-attached threads. Unload may also run on another thread, but the
adapter must first establish quiescence: no concurrent ABI operation and no code
executing through a borrowed lookup/import address. Resolver request storage is
valid only during the callback, while every FOUND address must remain valid until
unload. Because lazy binding is rejected, the callback/context is not retained.

A NativeBridge open adapter must keep the handle private until initializer and
preflight success, and unload it privately on failure. Export addresses are
invalid after unload; callers own their exact ABI. The Objective-C++ smoke
validates header layout, both single-image load forms, an atomic three-object
graph load, versioned imports, cross-thread lookup/unload, lifecycle, structured
errors, and idempotent unload.
