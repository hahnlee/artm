# Darwin ART

Experimental Apple Silicon port of Android Runtime. ART remains C++/ARM64
assembly and is compiled as Mach-O. Rust owns the bootstrap tooling, Darwin
integration, launcher, and eventually the Android ELF compatibility loader.

## Current gate

The current gate is intentionally smaller than `dalvikvm`. It links and runs a
complete native `Runtime::Create()` probe through Android 16 boot-class-path
initialization, executes a generated DEX method in ART's C++ interpreter, and
round-trips from interpreted Java through JNI into a Darwin function:

1. verify the native host is ARM64 macOS with 16 KiB pages;
2. fetch revision-locked ART subtrees without Git metadata;
3. adapt ART's ELF-oriented assembly conventions to Mach-O;
4. call an ART-style assembly entrypoint from Rust and return through Rust;
5. compile ART's page-size-agnostic path and verify 16 KiB on Darwin;
6. build small Mach-O ARM64 `libartbase` and `libdexfile` archives;
7. compile Java to DEX and verify it with AOSP `DexFileVerifier`;
8. generate ART's real ARM64 ABI constants and compile context, optimized
   `__memcmp16`, plus quick/JNI/native entrypoint assembly;
9. compile the C++ switch interpreter and a 166-object Runtime/ClassLinker/GC/
   quick-entrypoint initialization spine as Mach-O archives;
10. link the real bootstrap probe with no unresolved native symbols;
11. create ART successfully with the pinned Android 16 core boot JARs;
12. load `Hello` from an app-only `PathClassLoader` and execute
    `Hello.answer()` through `ArtMethod::Invoke`;
13. register `Hello.hostPageSize()` through JNI and execute
    `Java -> JNI -> getpagesize() -> Java`, returning `42` from the wrapper;
14. stress Darwin `LockSupport.park/unpark` permits, wakeups, and timeouts.

Run it with:

```bash
cargo run -p art-bootstrap -- all
```

Expected final lines include:

```text
probe-asm: ART Darwin ARM64 assembly result: 42
probe-pagesize: ART Darwin page size: 16384
build-foundation: libartbase Darwin: 1.500ms
build-dex: AOSP DEX: verified=yes version=35 classes=1 methods=5 class[0]=Ldev/darwinart/probe/Hello; corrupt=rejected
build-runtime-platform: Mach-O arm64 objects=3 archive=...
build-runtime-core: pthread monitor bootstrap objects=2 archive=...
build-runtime-arm64: generated ABI constants, Mach-O objects=10 archive=...
build-interpreter-core: AOSP C++ interpreter Mach-O objects=7 archive=...
build-runtime-bootstrap: ART runtime initialization spine Mach-O objects=166 compiled=0 cached=166 archive=...
audit-runtime-link: closure complete undefined=0
probe-runtime-dex: PathClassLoader -> interpreter -> JNI -> Darwin -> nativeRoundTrip()=42
probe-park: ART Darwin park: pre-permit=yes wakeups=200 timeout=yes
```

`build-runtime-bootstrap` keeps a dependency-aware object cache. Clang emits a
depfile for every translation unit; the bootstrapper fingerprints the complete
compile command, compiler/macOS identity, and SHA-256 of every referenced source
and project header. File hashes are memoized using Darwin inode and timestamp
metadata, so an unchanged warm build avoids rereading common AOSP headers. A
source-only edit rebuilds only the objects whose depfiles contain that source.
All cache files remain under ignored `_build/` paths.

`build-dex` requires Homebrew OpenJDK 17 headers and an Android SDK build-tools
installation containing `d8`. It generates the test DEX locally; no APK or
prebuilt DEX is stored in the repository.

The runtime bootstrap uses Android's pinned JNI header rather than the host JDK
ABI. ART's existing Darwin pthread fallback is retained. The monitor patch only
supports uncontended inflation by the current thread; multithreaded execution is
blocked until empty-checkpoint wakeups and cross-thread inflation are implemented.
`LockSupport.park/unpark` now uses ART's pthread-backed mutex/condition variable
fallback on Darwin and has an executable stress probe. It intentionally spends
one mutex and condition variable per ART thread; lazy allocation is a later
memory optimization.

The three production ARM64 entrypoint assembly files now compile as Mach-O and
remove the `_art_quick_*`/`_art_jni_*` family from the Runtime link closure.
For this PoC their DWARF CFI directives are emitted into clearly named
`darwin-no-cfi` generated sources because Apple's assembler rejects parts of
ART's ELF-oriented CFI state machine. Correct Darwin unwind metadata is required
before exceptions and stack walking through these stubs can be considered safe.

`audit-runtime-link` performs a real `Runtime::Create()` executable link. The
current native closure is complete; the command fails if any undefined symbol
or quick/JNI/context assembly regression appears.

The linked runtime probe can be executed directly:

```bash
_build/runtime-link-probe/runtime-link-probe \
  _prebuilt/android-16/bootclasspath/core-oj.jar \
  _prebuilt/android-16/bootclasspath/core-libart.jar \
  _build/dex-probe/dex/classes.dex
```

Its success lines are `ART Darwin Runtime::Create: ok`,
`ART Darwin app ClassLoader: PathClassLoader`, and
`ART Darwin DEX interpreter: Hello.answer()=42`, followed by
`ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42`. The generated DEX
remains separate from the boot class path. The native probe initializes ART's
unstarted-runtime handlers, constructs a `PathClassLoader`, registers the DEX,
then enables a probe-only minimal-start gate for normal JNI dispatch. It does
not load Android's libicu/libjavacore/libopenjdk JNI libraries or start daemon
threads. Normal Java launcher startup and the full runtime native-method
surface remain deferred.

Apple ARM64 executables retain the kernel-required 4 GiB `__PAGEZERO`, so ART's
usual absolute-low-32-bit heap references cannot be used. The Darwin probe
reserves a separate 4 GiB virtual window beginning at 16 GiB and stores managed
references as 32-bit offsets from that base. This reserves address space, not
4 GiB of physical memory. Every object that inlines the reference ABI must be
compiled through the same Darwin overlay.

The Darwin MVP defaults to concurrent mark sweep. Concurrent mark compact stays
compiled in stop-the-world fallback form because Darwin has neither Linux
`userfaultfd` nor `mremap`; it is not selected as the default collector.

Matching Android 16 `core-oj.jar` and `core-libart.jar` can be kept as ignored
local inputs under `_prebuilt/android-16/bootclasspath`. Verify their embedded
DEX files with:

```bash
cargo run -p art-bootstrap -- verify-bootclasspath
```

The runtime compiler enables AOSP's `USE_D8_DESUGAR` configuration so native
mirror class sizing matches these D8-built boot JARs. This is a Soong build
configuration dependency, not a Darwin-specific `java.lang.String` layout.

Generated content is kept out of version control:

```text
_downloads/  locked compressed source archives
_aosp/       extracted source, no .git directory
_build/      generated assembly and probe binaries
_prebuilt/   local Android boot class path inputs
target/      Rust build output
```

The source lock currently pins the peeled `android-16.0.0_r1` ART commit.
Gitiles archives are extracted into staging and the actual upstream source is
SHA-256 checked before it is moved into `_aosp`. Existing source trees are
never automatically deleted or replaced.

## Language boundary

- C++/assembly: ART runtime, GC, interpreter, JIT, quick/JNI entrypoints.
- Rust: source materializer, patch validation, process launcher, Darwin shims,
  APK orchestration, IPC, and the future ELF/Bionic compatibility layer.
- Swift/Objective-C: only where AppKit or platform APIs are substantially easier
  to expose than through Rust FFI.

See [PORTING.md](PORTING.md) for the execution gates.
