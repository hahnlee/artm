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
libraries. It also runs Android 16's real `Activity.attach()`, retains the
framework-created `PhoneWindow`, constructs its real `DecorView`, and executes
the platform and app `Activity.onCreate()` bodies using a minimal host context.
The app's real `Activity.setContentView(View)` call adds a normally constructed
View beneath that DecorView. The first vertical graphics slice executes the
complete software `DecorView -> ViewGroup -> View.draw(Canvas) -> onDraw()`
traversal, then presents the resulting frame in an independent AppKit
`NSWindow`. The runtime is now a process-scoped C ABI dynamic library. A Rust
host owns its lifecycle and the persistent IOSurface/Metal/AppKit presentation
surface:

1. verify the native host is ARM64 macOS with 16 KiB pages;
2. fetch revision-locked ART subtrees without Git metadata;
3. adapt ART's ELF-oriented assembly conventions to Mach-O;
4. call an ART-style assembly entrypoint from Rust and return through Rust;
5. compile ART's page-size-agnostic path and verify 16 KiB on Darwin;
6. build small Mach-O ARM64 `libartbase` and `libdexfile` archives;
7. build revision-locked AOSP Skia as a CPU-only Mach-O ARM64 archive and
   pixel-verify real `SkSurface`, `SkCanvas`, `SkPaint`, and `SkPath` output;
8. compile Java to DEX and verify it with AOSP `DexFileVerifier`;
9. generate ART's real ARM64 ABI constants and compile context, optimized
   `__memcmp16`, plus quick/JNI/native entrypoint assembly;
10. compile the C++ switch interpreter and a 209-object Runtime/ClassLinker/GC/
   quick-entrypoint initialization spine as Mach-O archives;
11. link the real bootstrap probe with no unresolved native symbols;
12. create ART successfully with the pinned Android 16 core boot JARs;
13. load `Hello` from an app-only `PathClassLoader` and execute
    `Hello.answer()` through `ArtMethod::Invoke`;
14. register `Hello.hostPageSize()` through JNI and execute
    `Java -> JNI -> getpagesize() -> Java`, returning `42` from the wrapper;
15. register ART's complete runtime-native table plus the Darwin libcore subset
    needed to initialize `System`, then execute AOSP `System.arraycopy()`;
16. create ART's normal main-thread peer and invoke `public static void
    main(String[])` through JNI;
17. use Android's real `PrintStream -> CharsetICU -> StreamEncoder -> IoBridge`
    path, backed by host ICU4C and Darwin `write(2)`;
18. load Android 16's real framework DEX, prepare its main `Looper`, and
    instantiate an app DEX class that directly extends `android.app.Activity`;
19. call the real `Activity.attach()` with a Darwin-backed context, retain its
    default `PhoneWindow`, and normally construct the framework `DecorView`;
20. execute the real platform `Activity.onCreate()` body plus the app override,
    including `Activity.setContentView(new ProbeView(this))`;
21. run `DecorView.measure/layout/draw()` and the nested base `View.draw(Canvas)`
    traversal against a Darwin software Canvas, copy its 640x360 frame out of
    the callback, then present through one persistent IOSurface, Metal texture,
    `CAMetalLayer`, and `NSWindow`;
22. draw 120 frames with upstream Skia directly into the mapped IOSurface and
    present them with zero staging copies;
23. stress Darwin `LockSupport.park/unpark` permits, wakeups, and timeouts;
24. shut ART down through the C ABI after every successful runtime probe.

Run it with:

```bash
cargo run -p art-bootstrap -- all
```

Expected final lines include:

```text
probe-asm: ART Darwin ARM64 assembly result: 42
probe-pagesize: ART Darwin page size: 16384
build-foundation: libartbase Darwin: 1.500ms
build-skia: Skia Darwin raster: 64x64 rowBytes=256 hash=a4bb4cdb0b4779ea Skia Android framework utils: base-canvas=same surface=same reset-clip=64x64 ...
build-dex: AOSP DEX: verified=yes version=35 classes=12 methods=288 ... corrupt=rejected
build-runtime-platform: Mach-O arm64 objects=3 archive=...
build-runtime-core: pthread monitor bootstrap objects=2 archive=...
build-runtime-arm64: generated ABI constants, Mach-O objects=10 archive=...
build-interpreter-core: AOSP C++ interpreter Mach-O objects=7 archive=...
build-runtime-bootstrap: ART runtime initialization spine Mach-O objects=209 compiled=0 cached=209 archive=...
audit-runtime-link: C ABI dylib closure complete undefined=0 exports=9
probe-runtime-dex: Activity.attach() + PhoneWindow + DecorView.draw(Canvas) -> Darwin
probe-park: ART Darwin park: pre-permit=yes wakeups=200 timeout=yes
```

To keep the native window visible for three seconds, run:

```bash
cargo run -p art-bootstrap -- probe-window
```

The current Activity frame producer is deliberately small:
`ProbeView.onDraw()` submits a Java `0xAARRGGBB` bitmap through Android's
`Canvas.drawBitmap()` API. The C callback borrows that frame only while ART is
runnable; Rust makes one tightly packed owned copy, returns across the ART
boundary, then uploads it into a persistent IOSurface-backed Metal texture.
The same `NSWindow`, `CAMetalLayer`, IOSurface, texture, and command queue remain
alive for the session. No frame creates a `CFData`, `CGImage`, window, or
texture.

This proves the real `Activity.onCreate() -> PhoneWindow.setContentView() ->
DecorView -> ViewGroup.dispatchDraw() -> View.draw(Canvas) -> onDraw()` control
path. Separately, `build-skia` maps that same IOSurface and wraps its base address
with upstream `SkCanvas::MakeRasterDirect`; 120 frames are rasterized and Metal-
presented with zero staging copies. The two paths are not connected yet:
Android's `Canvas` is still `ProbeCanvas`, not the upstream HWUI `SkiaCanvas`,
and the DecorView still uses a programmatic `android.R.id.content` root plus a
minimal resource backend. The next graphics gate is the full upstream Android
Canvas/Paint JNI and software HWUI closure, followed by real framework-resource
parsing. Input and GPU HWUI remain deferred.

The first Android 16 HWUI host compile gate is reproducible with:

```bash
bash tools/compile-android16-hwui-canvas-gate.sh
```

It compiles the unmodified upstream `SkiaCanvas.cpp`, `hwui/Canvas.cpp`, Canvas
JNI, and Paint JNI sources as ARM64 Mach-O objects and records their direct
undefined-symbol closure. The exact source and dependency revisions are pinned
under `upstream/`; the ordered module-level closure and current
registrar closure are documented in
`upstream/android16-hwui-host-closure.md`.

Skia itself now builds with `SK_BUILD_FOR_ANDROID_FRAMEWORK`; an executable gate
calls the real `SkAndroidFrameworkUtils` surface, wrapped-canvas, and clip-reset
operations while preserving the existing raster and direct-IOSurface golden
hashes. A narrow locked patch now keeps Darwin's host platform branches while
selecting Android's `@CriticalNative` function signatures and Skia's no-RTTI
contract; representative Canvas/Paint symbols are checked to contain no
`JNIEnv*` or `jclass` parameters. The four JNI/HWUI objects are still compile
proofs rather than safe ART registrars because their complete module dependency
and registration closure is not linked yet.

The first platform foundation modules are also real Android.bp-derived ARM64
archives rather than symbol stubs:

```bash
cargo run -p art-bootstrap -- build-hwui-canvas
cargo run -p art-bootstrap -- build-graphics-foundations
cargo run -p art-bootstrap -- build-nativehelper
cargo run -p art-bootstrap -- build-ui-types
cargo run -p art-bootstrap -- build-graphics-codecs
cargo run -p art-bootstrap -- build-harfbuzz
cargo run -p art-bootstrap -- build-minikin
cargo run -p art-bootstrap -- build-skia-text
```

`liblog-darwin.a` contains all eight Darwin-host sources and resolves the two
log functions in the current HWUI closure. `libcutils-darwin.a` contains its
base, host, non-Windows, and whole-static socket source groups (19 objects).
The nativehelper gate builds the four-object `libnativehelper_any_vm` archive
and the seven-object `libnativehelper_jvm` whole-static equivalent, including
the real JNI registration, exception, NIO, and file-descriptor helpers. The
nativehelper archive must use normal archive extraction in the final link;
force-loading it would also pull `JniInvocation`'s `JNI_CreateJavaVM` alongside
ART's VM implementation.
`libutils-darwin.a` follows its Darwin core source selection and whole-static
Binder support archive (19 objects total). `libui-types.a` adds the complete
four-source host module for Android `ColorSpace`, `Rect`, `Region`, and
`Transform` types.
The graphics codec gate builds Android's complete Darwin ARM64 selections for
zlib (19 sources), libpng (18 sources), and FreeType (26 sources). These are
the pinned raster-font dependencies needed before enabling Skia's FreeType
font manager; they do not use Homebrew libraries or replacement symbols.
The next two gates compile all 53 Android HarfBuzz translation units and all
31 Minikin translation units. Their remaining unresolved closure is kept
visible; the next required module is the pinned Android ICU host build and its
data image, not a local symbol shim.
`build-skia-text` keeps the no-font baseline untouched and produces a separate
Skia archive wired to the pinned AOSP FreeType/libpng/zlib archives. Its actual
Roboto `Click` raster is locked at five glyphs, 1,097 ink pixels, and pixel hash
`1f94df6816828ca2`; the final binary has no Homebrew or CoreText dependency.

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
fallback on Darwin and has an executable stress probe. It shares each ART
thread's existing wait mutex and condition variable rather than adding another
pair to `Thread`; this also keeps the upstream `Thread` object layout consistent
across all runtime archives.

The three production ARM64 entrypoint assembly files now compile as Mach-O and
remove the `_art_quick_*`/`_art_jni_*` family from the Runtime link closure.
For this PoC their DWARF CFI directives are emitted into clearly named
`darwin-no-cfi` generated sources because Apple's assembler rejects parts of
ART's ELF-oriented CFI state machine. Correct Darwin unwind metadata is required
before exceptions and stack walking through these stubs can be considered safe.

`audit-runtime-link` builds `libdarwin_art_runtime.dylib`, restricts it to the
nine versioned runtime/surface C ABI exports, and builds the Rust host. The
current native closure is complete; the command fails if any undefined symbol,
unexpected export, or quick/JNI/context assembly regression appears. The host
copies the borrowed frame only after validating dimensions and stride, owns all
AppKit presentation, destroys its surface, and then requests ART shutdown on
the same native thread. `DestroyJavaVM` performs the ART thread-list teardown;
registered application DexFiles remain owned until that teardown is complete.

The linked runtime can be executed through its Rust host directly:

```bash
target/debug/darwin-art-host \
  _build/runtime-link-probe/libdarwin_art_runtime.dylib \
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
`ART Android window: Activity.attach()=PhoneWindow+DecorView`,
`ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=640x360`,
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
loaded. The current real `Activity.attach()` uses a minimal AssetManager/resource
backend, a null `Instrumentation`, and no remote services. It constructs and
retains the framework `PhoneWindow`; the launcher normally constructs its
`DecorView` and installs a programmatic content root because the complete
compiled `framework-res.apk` parser is not ported yet. The content `View` is
normally constructed and traversed. Android Skia/HWUI, full decor resources,
input dispatch, and incremental frame scheduling remain deferred.

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
