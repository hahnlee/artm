# Darwin ART

Experimental Apple Silicon port of Android Runtime. ART remains C++/ARM64
assembly and is compiled as Mach-O. Rust owns the bootstrap tooling, Darwin
integration, launcher, and eventually the Android ELF compatibility loader.

## Current gate

The current gate is intentionally smaller than `dalvikvm`. It links and runs a
complete native `Runtime::Create()` probe through Android 16 boot-class-path
initialization, invokes a generated DEX `main(String[])` in ART's C++
interpreter, round-trips through JNI into Darwin, and sends Android
`System.out` through ICU and Darwin `write(2)` without loading Android `.so`
libraries. It also runs Android 16's real `Activity.attach()`, creates a
`PhoneWindow`, and dispatches the platform `Activity.onCreate()` using a
minimal host context:

1. verify the native host is ARM64 macOS with 16 KiB pages;
2. fetch revision-locked ART subtrees without Git metadata;
3. adapt ART's ELF-oriented assembly conventions to Mach-O;
4. call an ART-style assembly entrypoint from Rust and return through Rust;
5. compile ART's page-size-agnostic path and verify 16 KiB on Darwin;
6. build small Mach-O ARM64 `libartbase` and `libdexfile` archives;
7. compile Java to DEX and verify it with AOSP `DexFileVerifier`;
8. generate ART's real ARM64 ABI constants and compile context, optimized
   `__memcmp16`, plus quick/JNI/native entrypoint assembly;
9. compile the C++ switch interpreter and a 209-object Runtime/ClassLinker/GC/
   quick-entrypoint initialization spine as Mach-O archives;
10. link the real bootstrap probe with no unresolved native symbols;
11. create ART successfully with the pinned Android 16 core boot JARs;
12. load `Hello` from an app-only `PathClassLoader` and execute
    `Hello.answer()` through `ArtMethod::Invoke`;
13. register `Hello.hostPageSize()` through JNI and execute
    `Java -> JNI -> getpagesize() -> Java`, returning `42` from the wrapper;
14. register ART's complete runtime-native table plus the Darwin libcore subset
    needed to initialize `System`, then execute AOSP `System.arraycopy()`;
15. create ART's normal main-thread peer and invoke `public static void
    main(String[])` through JNI;
16. use Android's real `PrintStream -> CharsetICU -> StreamEncoder -> IoBridge`
    path, backed by host ICU4C and Darwin `write(2)`;
17. load Android 16's real framework DEX, prepare its main `Looper`, and
    instantiate an app DEX class that directly extends `android.app.Activity`;
18. call the real `Activity.attach()` with a Darwin-backed context and create
    Android's concrete `PhoneWindow`;
19. execute the real platform `Activity.onCreate()` body plus the app override;
20. stress Darwin `LockSupport.park/unpark` permits, wakeups, and timeouts.

Run it with:

```bash
cargo run -p art-bootstrap -- all
```

Expected final lines include:

```text
probe-asm: ART Darwin ARM64 assembly result: 42
probe-pagesize: ART Darwin page size: 16384
build-foundation: libartbase Darwin: 1.500ms
build-dex: AOSP DEX: verified=yes version=35 classes=6 methods=68 ... corrupt=rejected
build-runtime-platform: Mach-O arm64 objects=3 archive=...
build-runtime-core: pthread monitor bootstrap objects=2 archive=...
build-runtime-arm64: generated ABI constants, Mach-O objects=10 archive=...
build-interpreter-core: AOSP C++ interpreter Mach-O objects=7 archive=...
build-runtime-bootstrap: ART runtime initialization spine Mach-O objects=209 compiled=0 cached=209 archive=...
audit-runtime-link: closure complete undefined=0
probe-runtime-dex: Activity.attach() + PhoneWindow + onCreate() -> Darwin
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
  _prebuilt/android-16/bootclasspath/framework.jar \
  _build/bootclasspath/core-icu4j.jar \
  _build/dex-probe/dex/classes.dex
```

Its success lines are `ART Darwin Runtime::Create: ok`,
`ART Darwin app ClassLoader: PathClassLoader`, and
`ART Darwin DEX interpreter: Hello.answer()=42`, followed by
`ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42`, and
`ART runtime native: System.arraycopy()=42`, the Java-emitted
`Hello from Darwin ART main: 안녕`,
`ART Android framework: ProbeActivity().probeValue()=42`,
`ART Android window: Activity.attach()=PhoneWindow`,
`ART Android lifecycle: Activity.onCreate()=43`, and
`ART Darwin launcher: main(String[])=ok`.
The generated app DEX remains separate from the boot class path. The native
probe initializes ART's unstarted-runtime
handlers, constructs a `PathClassLoader`, registers the DEX, then enables a
probe-only minimal-start gate. This gate registers ART's complete runtime-native
table, initializes intrinsics, creates the normal main `Thread` peer, and runs
root class initializers. Darwin adapters provide the POSIX, file-descriptor,
primitive-bit, ICU-metadata, system-property, and standard-output subset needed
by this gate. The locked Android `core-icu4j` Java code is converted to DEX
locally; its `NativeConverter` calls host Homebrew ICU4C 78. The framework gate
uses Darwin-native `MessageQueue` wake/poll primitives and Android's default
INFO logging threshold. Darwin clocks back `System`/`SystemClock`, and an
in-process property table backs Android `SystemProperties`; its Darwin defaults
now include the ARM64 ABI properties required by `android.os.Build`. A minimal
Binder holder/finalizer supports local framework Binder stubs, and the host ICU
bridge now covers both encoder and decoder paths. The app-only compile stub for
hidden `IContentProvider` supplies a javac signature and is not emitted into the
DEX; runtime resolution uses framework.jar's real interface. Android's
libicu/libjavacore/libopenjdk shared libraries and daemon threads are still not
loaded. The current real `Activity.attach()` uses synthetic resources/settings,
a null `Instrumentation`, and no remote services. Resource-backed DecorView,
View hierarchy, and the Darwin window backend remain deferred.

Apple ARM64 executables retain the kernel-required 4 GiB `__PAGEZERO`, so ART's
usual absolute-low-32-bit heap references cannot be used. The Darwin probe
uses a separate 4 GiB representable window beginning at 1 TiB and stores
managed references as 32-bit offsets from that base. The 1 TiB base avoids
Darwin's randomized malloc zones in the first few dozen GiB. Only a bounded
256 MiB contiguous arena inside that window is reserved for the current
64 MiB-Xmx CMS configuration; the complete 4 GiB window is never reserved.
Mach virtual reservation does not make all 256 MiB resident: physical pages are
committed as ART touches its heap spaces. Every object that inlines the
reference ABI must be compiled through the same Darwin overlay. Launchers call
`MemMap::Init()` before their first heap allocation so the arena cannot be
claimed during runtime-option construction.

The Darwin MVP defaults to concurrent mark sweep. Concurrent mark compact stays
compiled in stop-the-world fallback form because Darwin has neither Linux
`userfaultfd` nor `mremap`; it is not selected as the default collector.

Matching Android 16 `core-oj.jar`, `core-libart.jar`, and `framework.jar` remain
ignored local inputs under `_prebuilt/android-16/bootclasspath`. `sync` downloads only the
revision-locked 2.99 MB `core-icu4j` class JAR and `d8` produces its ignored DEX
JAR locally. Verify all four inputs, including all five framework DEX files,
with:

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
