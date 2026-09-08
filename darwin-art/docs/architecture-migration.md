# Darwin ART compatibility runtime — active goal

이 문서는 현재 작업의 목표와 최신 검증 결과만 유지하는 짧은 진행 문서다.
작업을 시작할 때는 이 파일의 마지막 `Progress` 항목을 먼저 읽고, 작업이
끝날 때는 새 `Progress` 항목을 append한다. 오래된 상세 로그와 실험 기록은
Git 문서 이력 또는 휴지통 보관본에서 찾는다.

## Final goal

macOS 위에서 수정하지 않은 실제 Android APK가 Android와 같은 계약으로
동작하는 호환성 계층을 완성한다. Java/Looper/ViewRoot/Choreographer 및
Chromium owner-affine callback은 ART UI owner thread에서 실행하고, AppKit
입력·display-vsync·Metal scanout·독립 서비스는 별도 경로로 연결한다.
GPU 경로는 CPU fallback이 아니라 IOSurface/Metal zero-copy를 우선하며,
Binder/SurfaceControl/입력/파일/미디어/보안 CA 같은 공통 시스템 서비스는
앱별 probe가 아닌 런타임 계층이 제공한다.

## Current goal: Chrome performance

Android 스레드 순서를 바꾸지 않고 Chromium의 콜드 시작 tail과 steady-state
입력·합성 비용을 줄인다.

- 콜드 시작의 owner-affine `MessagePumpAndroid` 지연을 줄인다.
- AppKit→ART wake를 event-driven으로 만들고, 입력 backpressure를 bounded
  latest-wins로 유지한다.
- scanout producer에서 dirty-generation을 확인해 중복 제출을 줄이고,
  IOSurface/MTLSharedEvent fence readiness를 보존한다.
- 무진단 릴리스 벤치마크에서 물리 입력 100회 이상과 기존 Chrome
  acceptance를 모두 통과시킨다.

## Baseline (2026-09-02)

APK: installed unmodified `chrome_public_apk`, version 154.0.8024.0,
SHA-256 `2aaea8419d955677313f8b6dae3f0666916243ec55c3607a8711f46c9123b731`.
Acceptance artifact:
`_build/chromium-android-acceptance/run.8Qc6Qn` (HTTPS/macOS trust, physical
keyboard, 3 Android tabs, renderer/GPU services, Binder FD, WebGL
ANGLE-Metal, WebM/Opus/CoreAudio all PASS).

Diagnostics-enabled interaction sample: n=4,
input→framework pulse p50=9.641 ms, p95/max=2.276795 s. WebGL re-entry
p50=10.014 ms, p95/max=2.197819 s. The multi-second tail is the cold
owner-affine `MessagePumpAndroid` startup poll; warm samples are about 8–10 ms.

Interaction scanout ended at `requests=4440`, `coalesced=3995` (90.0%),
`fence_gated=121` (2.7%), `present_calls=2801`. These counters show upstream
deduplication headroom even though the latest-wins bridge already bounds the
AppKit queue.

## Progress — 2026-09-02

Goal created. Baseline collected and recorded above. No performance code has
been changed yet after the baseline; the next implementation checkpoint is an
event-driven owner wake plus producer-side scanout dirty-generation gate,
followed by a no-diagnostics 100-event benchmark. Preserve Android UI-thread
affinity throughout.

## Progress — 2026-09-02 optimization iteration

Implemented the first bounded performance changes while preserving Android
thread affinity:

- `darwin_art_surface_present_async` now claims a new SurfaceFlinger
  completion generation or ANGLE embedded IOSurface frame before enqueueing a
  Metal blit. Stable dirty sources are skipped; fence-less surfaces retain the
  legacy behavior.
- The AppKit actor's idle wait changed from 2 ms polling to a 16 ms
  `nextEventMatchingMask:` wait. AppKit still returns immediately for a real
  NSEvent, while idle wakeups drop substantially.

Validation: `cargo fmt --all -- --check`, all `darwin-art-host` tests (8/8),
and `audit-runtime-graphics-link` pass. Chrome acceptance passed with the
installed unmodified APK; diagnostic artifact
`_build/chromium-android-acceptance/run.s8xwTs` reports interaction
`requests=5340`, `dirty_skipped=5149`, `fence_gated=144`, `present_calls=46`.
The no-diagnostics follow-up
`_build/chromium-android-acceptance/run.ziztem` also passed HTTPS, 3 tabs,
physical keyboard, Binder FD, media, and WebGL ANGLE-Metal. A transient
Crashpad helper SIGTRAP run was discarded and was followed by this clean PASS.

The generation gate is therefore correctness-green and removes most redundant
GPU/AppKit submissions, but it is not yet the final benchmark: collect at
least 100 warm physical events without diagnostic logging, then optimize the
remaining cold `MessagePumpAndroid` startup tail.

## Progress — 2026-09-02 benchmark and cold-tail diagnosis

Added `tools/chromium-performance-benchmark.sh`, which generates repeated
MotionEvent DOWN/UP pairs and enables only aggregate host-side timing through
`DARWIN_ART_BENCHMARK=1`; native per-event diagnostics remain disabled. The
100-tap run against the unmodified APK passed with 204 owner-thread samples:
p50 `232 us`, p95 `12157 us`, p99 `37542 us`, max `1802737 us`.
Artifact: `/tmp/darwin-art-chromium-benchmark.kVByz6`.

The latest full Chromium acceptance also passed after the benchmark hook:
`_build/chromium-android-acceptance/run.FDPaLM` (HTTPS/macOS trust, physical
keyboard, three tabs, renderer/GPU services, Binder FD, WebGL ANGLE-Metal,
download/content URI, and WebM/Opus/CoreAudio).

A single diagnostic cold run shows the remaining multi-second tail is not a
blocking Java `MessageQueue.nativePollOnce`: the owner-thread nonblocking poll
enters a Chromium native-fd callback taking about `1.83 s` (plus a `0.63 s`
startup callback). The next optimization target is therefore callback
handoff/startup work while retaining the Android owner-thread sequence.

## Progress — 2026-09-02 final validation

The stable benchmark uses an inert window coordinate `(8,8)` so repeated input
does not open Chrome selection/action-mode UI. It passed 100 taps with 204
owner-thread samples and no diagnostic switches: p50 `167 us`, p95 `420 us`,
p99 `41759 us`, max `1813408 us`. Artifact:
`/tmp/darwin-art-chromium-benchmark.pVrQuT`.

Two experiments were rejected: reducing the native callback drain budget from
8 to 1 did not improve latency (p50 `256 us`, p95 `12366 us`), and skipping
HWUI session creation in service children caused reproducible ART
`hwuiTask` detach aborts under the 100-tap run. Both were reverted; the
Android owner-thread, EGL/IOSurface, and service-process paths remain intact.
The latest graphics-link audit and `darwin-art-host` tests pass, and the full
unmodified Chrome acceptance remains green in `run.FDPaLM`.

## Progress — 2026-09-02 Chrome surface color audit

Investigated the apparent address-bar/bottom-toolbar hue split in the Chrome
capture. The two regions are authored by Chrome's Material surface roles (the
sampled colors are approximately `236,227,228` and `228,227,236`), rather
than being a channel swap introduced by the host compositor. The runtime now
keeps the presentation contract explicit: `CAMetalLayer.colorspace` and the
Skia Metal render target are both sRGB. An experiment changing the drawable
pixel format to `BGRA8Unorm_sRGB` caused the Chromium child Surface to vanish
and was immediately reverted; the stable drawable remains `BGRA8Unorm`.

Validation: incremental graphics link/audit, `cargo fmt --all -- --check`,
and `git diff --check` pass. The post-change acceptance reached the HTTPS,
input, download, media, and E2E reports before the known host termination
tail; no color-space or graphics-link error was reported. Do not force the two
Chrome surfaces to one RGB value in the runtime: that would override the APK's
Material You theme rather than repair Android compatibility.

## Progress — 2026-09-02 Android orientation and landscape window

Added the Android-compatible orientation request path. Calls to
`Activity.setRequestedOrientation()` now terminate at the in-process
`IActivityTaskManager` bridge, where landscape/portrait requests are queued on
the Android main `Handler`. The bridge updates shared `Configuration` and
`DisplayMetrics` (including orientation, dp bounds, and smallest width), then
resizes the active IOSurface/CAMetalLayer-backed `NSWindow`; the next
ViewRoot traversal observes the same dimensions. Sensor/user requests retain
the current posture because this host has no accelerometer.

The live AppKit resize path remains authoritative for manual window changes,
so input scaling follows the rotated drawable without a second coordinate
transform. GPU acceptance now accepts either portrait or its landscape
counterpart. `build-button-dex`, the APK runtime audit, graphics-link audit,
host tests, formatting, and diff checks pass.

## Progress — 2026-09-02 Minecraft compatibility gate

The Minecraft target is intentionally external to the repository: the runner
already accepts a user-provided APK path and installs it into the private
profile store without rewriting or committing the APK. A local search found
no Minecraft APK, split APK set, OBB, or connected Android device, so an
end-to-end Minecraft launch cannot yet be verified. The next gate requires a
legitimately acquired `com.mojang.minecraftpe` package (preferably a universal
ARM64 APK, or the complete base/config split set and its external data), then
will exercise native library loading, SurfaceView/GLES/Vulkan capability
negotiation, filesystem assets, physical input, and automatic landscape
orientation.

## Progress — 2026-09-02 Minecraft acquisition CLI check

The host has `adb` and `bundletool`, but no connected Android device and no
authenticated Google Play download CLI or cached Minecraft package. `adb` can
pull an installed, user-owned package; `bundletool` only processes an existing
`.aab`/`.apks` and cannot fetch Play Store content. No third-party APK mirror
was used.

The previously installed `apkeep 1.0.0` was also checked. Its APKPure source
returned no Minecraft versions or artifact, while its Google Play source
stopped at the required email/AAS-token prompt. No credentials were present,
so acquisition remains the only blocker for the end-to-end Minecraft gate.

## Progress — 2026-09-02 Crossy Road native compatibility

Acquired Crossy Road 7.12.1 (`com.yodo1.crossyroad`) as an external ARM64
APK for a runtime-only compatibility test; the APK and extracted assets remain
outside the repository. The Android NativeLoader/ELF graph now reaches
`libmain.so`, `libunity.so`, and `libil2cpp.so`; Unity JNI registration is
image-owned even when process-wide guest libdl callbacks enter a child DSO.
GNU/LLVM zero-sized `end` load markers are accepted as dlsym-visible sentinels.

Added runtime-owned Bionic seams exercised by this native graph: `logb`,
`futimens`, `drand48`/`lrand48`/`mrand48`/`srand48`, `clock_getres`,
`sigsuspend`, and `sem_getvalue`. Provider closure, graphics-link audit,
formatting, and diff checks pass. The current gate is after Unity's
`MemoryManager: Using 'Dynamic Heap' Allocator` and SurfaceView GPU setup;
Crossy still faults in a Unity native thread at a null indirect call before
the first game frame. This is now a Unity/JNI or graphics lifecycle contract
to diagnose, not an unresolved ELF import. `getPackageCodePath()` is mapped to
the mounted APK path to avoid a framework `NameNotFoundException` during that
bootstrap.

## Progress — 2026-09-02 Crossy Road JNI/storage boundary

Forwarded the standard JNI exception slot `Throw` (slot 13) to the current ART
environment and added `Context.getObbDir(s)` backed by the profile's authorized
`Android/obb/<package>` subtree. The APK runtime DEX expectations are now
baseline `960` methods and button `1353` methods. The Bionic filesystem facade
also accepts the exact launcher-provided APK path as a read-only capability, so
Unity's `ApkAddCentralDirectory` can open its unmodified `base.apk` without a
host-path escape. JNI proxy audit, facade tests, graphics-link audit, formatting,
and diff checks pass.

Sol's address-level diagnosis found the next null jump in Unity's reflection
bridge: `JNIEnv` slot 7, `FromReflectedMethod`, was absent. It is now forwarded
to ART; the related reflection slots 8/9/12 are covered as well. A clean
Crossy run no longer reports `ApkAddCentralDirectory` failure or the slot-7
SIGSEGV and reaches repeated GPU SurfaceView composition (`720x1280`, then
`684x276`) with EGL/Metal submissions. The visible window currently reaches
the Unity/Crossy splash surface, but a first gameplay frame is not yet proven;
the next gate is Unity scene/bootstrap progress and frame-content evidence.

## Progress — 2026-09-02 Crossy Road device capability snapshot

The filesystem facade now exposes a bounded read-only Android device view for
native hardware probing: synthetic `/proc/cpuinfo`, `/proc/meminfo`,
`/proc/self/{status,statm}`, CPU possible/present/online masks, per-CPU
capacity/frequency files, and stable `stat` metadata. The values are shared by
one capability contract (8 virtual CPUs, 8 GiB memory) rather than leaking
Darwin procfs/sysfs. Native `sysconf` now serves page size, processor counts,
and physical/available page counts from that same contract.

Facade tests (14), time facade audit, provider closure, and graphics-link audit
pass. A rebuilt Crossy run confirms the synthetic files are opened and read,
but Unity still logs `SystemInfo ... Cores = 0, Memory = 0mb`; no crash or
first gameplay frame is proven yet. The next investigation is the Unity
hardware-query call path (resolver/selector or parser), followed by lifecycle
callbacks once the device values are visible.

## Progress — 2026-09-02 Crossy Road FILE scanning and topology

The native `fscanf@LIBC` imports used by Unity are now bridged generically with
an Android AAPCS64 variadic entry point and a provider-owned `FILE` cursor. The
scanner commits only the bytes consumed by the Bionic parser, so `fopen`, scan,
and `fclose` share the same guest VFS without falling back to Darwin stdio.
The standalone ARM64 fixture passes integer, string, and split-cursor scans
under ASan/UBSan/TSan; a runtime trace showed Crossy reading every synthetic
CPU capacity/frequency file successfully.

The synthetic topology now models four lower-capacity and four higher-capacity
ARM cores (512/1.8 GHz and 1024/2.4 GHz) while retaining the bounded 8 GiB
memory snapshot. Crossy reaches repeated 720x1280 and 684x276 GPU SurfaceView
Metal submissions without an ELF/JNI crash. Unity's diagnostic line still
reports `Cores = 0, Memory = 0mb`, so this remains a device-query compatibility
issue rather than a success claim; a first gameplay frame and content hash
still require a follow-up lifecycle/scene gate.

## Progress — 2026-09-02 Crossy Road native hardware parser audit

The Android AAPCS64 `fscanf` bridge now demonstrably parses Unity's per-CPU
capacity and frequency files, so formatted input is no longer the device-query
blocker. The failing trace still used homogeneous `1024`/`2400000` values for
all CPUs; the current provider and rebuilt graphics closure instead expose a
4+4 big.LITTLE snapshot (`512`/`1800000`, `1024`/`2400000`). Re-run Crossy
against that closure before changing the topology contract again.

Static inspection of Unity 6000.3 confirms that its memory reader opens
`/proc/meminfo`, matches `MemTotal:`, parses the numeric kB value, and converts
it to bytes. The current synthetic format satisfies that contract, but the
failing trace contains no `/proc/meminfo` open. The next narrow diagnostic is
therefore entry/result tracing at the Bionic `fopen` boundary, not another
meminfo-format change. `Cores = 0, Memory = 0mb` did not stop the render loop:
the same run continued publishing EGL buffers and Metal compositions without
a SIGSEGV/abort marker. Those swaps prove a live Unity surface, but not yet a
gameplay-content frame; capture and pixel/content evidence remain required.

## Progress — 2026-09-03 Crossy Road dynamic DEX/OAT fallback

Native-library reuse now keys detached worker lookups by a stable loader
namespace identity, allowing an already resident APK DSO to be leased when no
live `JNIEnv` is available. Firebase's extracted `app_resources_lib.jar` also
crosses the compatibility boundary generically: the guest private-data path is
resolved to its authorized profile backing for `DexClassLoader`, and
`UnixFileSystem` permits read-only `stat` only within that exact private root.

The helper JAR then exposed an ART Darwin bug rather than an APK issue:
`DlOpenOatFile::PreLoad()` terminated with `LOG(FATAL)` before the existing
`ElfOatFile` fallback could run. Patch 0032 makes Darwin `PreLoad()` side-effect
free; `Dlopen()` remains unsupported and the normal portable ELF reader remains
the fallback. The graphics ART archive was rebuilt and the full strict graphics
link audit passed (`registrar=51`, no fake symbols or host ICU/fmt). A fresh
Crossy run is still required to prove Firebase class loading and the first
gameplay frame; neither is claimed by this build-only checkpoint.

## Progress — 2026-09-03 Crossy Road first-gameplay gate

The unmodified Crossy Road 7.12.1 ARM64 APK now loads its Unity/IL2CPP
runtime, Firebase helper DEX, native ELF graph, and GPU SurfaceView without a
crash. The runtime reports the shared device contract (8 ARM64 cores, 8 GiB)
and continuously submits 720x1280 Metal frames. APK central-directory and
direct-fd traces show the AppLauncher and `gamesceneboot_*` bundles are read,
but the `gamescene_*` and `gamesceneglobal_*` payloads are not requested.

The current visible output is therefore the GameSceneBoot/default framebuffer
(cyan after the splash), not a gameplay scene. Unity's graphics worker remains
active, so this is a managed boot-condition/network-service gate rather than a
SurfaceFlinger target-selection failure. External hostname resolution and
the service contract needed by the app remain the next compatibility task;
first gameplay content and input are not yet claimed.

## Progress — 2026-09-03 Crossy Road DNS/CA and native-loader gate

The Android DNS facade now resolves the app's explicitly requested Unity
service hostnames with Android-style absolute-name semantics. The framework
trust bridge now exposes the macOS Security.framework roots through
`X509TrustManager.getAcceptedIssuers()`, so UnityTls no longer reports curl
certificate error 60. Native `dlopen` of an already resident APK ELF image
also follows Android's same-handle/reference-count behavior when a live JNI
environment is present, eliminating the duplicate Firebase graph SIGSEGV.

The latest 40-second run is crash-free and initializes Firebase Analytics, but
Firebase Messaging still reports missing Google Play Services and the visible
output remains the boot/default framebuffer. A first gameplay scene and
content-pixel proof remain the active gate; no APK modification is permitted.

## Progress — 2026-09-03 Crossy Road first gameplay frame

Detached ART startup now follows Android's boot-before-app ordering. The
minimal runtime start, boot/native registration, `Thread::FinishStartup()`, and
root class initializers run before any APK or support DEX class is loaded;
application ClassLoader installation and Looper preparation remain in the
post-load phase. This removes the cold-start
`AtomicInteger`/`MethodHandles.Lookup`/`System` recursive initialization crash.
The app process bootstrap also publishes `Process.sArgV0`, matching zygote's
non-null `Process.myProcessName()` contract used by Crashlytics.

The unmodified Crossy Road 7.12.1 APK now logs `TUTORIAL GAMEPLAY STARTED` and
renders an actual road/chicken gameplay scene. The authoritative run is
`/private/tmp/crossy-cold-IKErlr`: 162 captured PPM frames, with the gameplay
map visible in `frame-160.png`/`frame-160.ppm`. The full native graph and
graphics-link audit pass (`registrar=51`, no fake symbols or host ICU/fmt).

The first run then exposed a host-task ABI mismatch: its SIGSEGV mapped to
`libunity.so` offset `0x12e9fe0`, instruction `ldr s0, [x18,#0x18]`, with fault
address `0x18` and `x18=0`. The DSO contains 1,916 uses of x18 as a general
register, while a current-SDK Darwin task does not preserve x18 across
scheduling. Darwin ART now declares the host's custom-x18 task ABI before code
signing using XNU's pre-macOS-13 SDK compatibility contract. A standalone
arm64 fixture proves the default task loses x18 and the declared task preserves
it across 100,000 forced scheduling points.

The follow-up `/private/tmp/crossy-x18b-LGT6Dh` run remained free of
SIGSEGV/abort for the complete bounded capture: 1,747 gameplay PPM frames with
1,564 distinct hashes. It reached `TUTORIAL GAMEPLAY STARTED`, reloaded the game
scene, and remained live through the end; `final-gameplay.png` is the final
captured Crossy Road scene. The x18 contract is task-wide, so it covers guest
internal execution and JNI/provider transitions without APK-specific code
rewrites or signal recovery.

## Progress — 2026-09-03 Crossy Road 30-second stability capture

After restarting the profile-scoped system server with the declared x18 ABI,
the same unmodified APK ran for the full 30-second bounded window. The
authoritative capture is `/private/tmp/crossy-final-uOZN00`: 1,746 PPM frames
with 1,566 distinct full-frame hashes. `TUTORIAL GAMEPLAY STARTED` appears in
the run log, and no `DARWIN signal`, `SIGSEGV`, `SIGABRT`, or Unity crash marker
was emitted. The montage shows the live forest/road scene, chicken, score, and
Play button throughout the capture. The remaining Firebase AppMeasurement
warnings are optional Google Play Services behavior and do not terminate the
app or prevent gameplay rendering.

## Progress — 2026-09-03 Crossy Road physical input smoke test

With the compatibility window in the foreground, macOS CGEvent mouse-down/up
events were sent to the real Crossy Road window (not the synthetic probe
pointer path). A click on the Play button followed by eight forward taps
advanced the chicken through the forest and road lanes; the score changed from
30 to 80 and moving vehicles were visible. Window captures are
`/private/tmp/crossy-window-live3.png`, `/private/tmp/crossy-played.png`, and
`/private/tmp/crossy-played2.png`. This confirms the end-to-end input route from
physical host events through Android MotionEvent/Unity into gameplay state.

## Progress — 2026-09-03 Blue Archive first-frame diagnosis

The unmodified Blue Archive APK reaches Unity/IL2CPP, creates its 720x1280
SurfaceView EGL targets, and selects the supported GLES/ANGLE path when Android
Vulkan WSI is reported unavailable. The current logs contain no
`eglSwapBuffers`, ANativeWindow queue, or SurfaceFlinger frame-publication
evidence, so the visible black SurfaceView is still the consumer's empty
default buffer rather than a demonstrated ANGLE-to-IOSurface or Metal-composer
failure.

Two required APK DSOs (`lib_burst_generated.so` and `libsqlcipher.so`) were
rejected before the first frame because the ELF loader treated the byte-sized
`p_memsz` extent as the complete PT_LOAD mapping. Android lld legitimately
extends PT_GNU_RELRO through the zero-filled tail of a 16 KiB PT_LOAD page.
RELRO coverage now uses the same page-rounded PT_LOAD intervals as the actual
mapping/protection plan, while still rejecting a real unmapped page gap. The
loader's 27 unit tests pass, including fixtures for 16 KiB page padding and a
negative gap. A rebuilt runtime run must next confirm both DSOs load and then
capture the first `eglSwapBuffers` boundary; the existing MediaExtractor JNI
initialization failure is a separate likely startup-video gate if no producer
frame follows.

## Progress — 2026-09-03 Blue Archive media and bootstrap review

The latest run (`/private/tmp/blue-latest5.log`) proves the graphics transport
is live: Unity repeatedly swaps a 720x1280 EGL surface, queues ANativeWindow
buffers, and submits them through the Metal composer for more than 1,000
frames. The black middle region is app-produced black, not an uninitialized
SurfaceFlinger consumer. Unity disables its preferred NDK media path because
`AMediaExtractor_new` is unavailable, then the Java fallback reports zero
tracks for `assets/Video/logo.mp4`; the current Java MediaExtractor methods are
contract-only stubs whose `getTrackCount()` always returns zero.

The next runtime slice should therefore add one bounded extractor core and
expose it through the complete Java MediaExtractor contract first, while
keeping NDK media capability atomic until Unity's full dynamically-probed
AMediaExtractor/AMediaCodec surface API is present. After the media failure,
`NXPatcher.init()` also proves that APK ContentProviders are not installed in
Android order: the manifest contains `androidx.startup.InitializationProvider`
with `WorkManagerInitializer`, but the runtime launches the Application and
Activity without creating providers, so WorkManager remains uninitialized.
Deflater JNI is a later network-stability gate: its absence breaks Firebase
heartbeat gzip and FIS token work, but it occurs after both the startup-video
and provider-lifecycle failures and does not explain the black video region.

## Progress — 2026-09-03 Blue Archive provider and compression ABI

The app bootstrap now parses the manifest's provider metadata and installs
declared providers after `Application.attachBaseContext()` and before
`Application.onCreate()`, matching Android's startup ordering. AndroidX
Startup, WorkManager, EmojiCompat, Lifecycle, and ProfileInstaller providers
are created in manifest order. `FirebaseInitProvider` is intentionally skipped
in the detached compatibility process because the APK Application already owns
that singleton; invoking it a second time causes the Firebase fatal-exit path.
This is a runtime policy, not an APK change.

The libcore bridge now registers the Android 16 Deflater byte-array and direct
buffer entry points, CRC32/Adler32 updates, and uses the broker's Android stat
ABI when constructing `FileKey`. A fresh full-provider Blue Archive run exits
cleanly (`RC=0`) with no Deflater, checksum, WorkManager, or file-key errors.
Unity reaches GLES/ANGLE and the Metal IOSurface composer; the remaining
startup gate is still Java `MediaExtractor`, which reports zero tracks for the
APK's WebM/VP9 files named `logo.mp4` and `title.mp4`. First title-frame and
physical-input success are therefore not claimed until the extractor and VP9
decode path are implemented in the runtime.

## Progress — 2026-09-04 Blue Archive WebM/VP9 runtime path

The Java MediaExtractor contract now reads the real APK asset descriptor through
the brokered descriptor owner, parses EBML Segment/Info/Tracks/Cluster elements,
and exposes VP9 samples, timestamps, dimensions, flags, seek, and direct
ByteBuffer copies. No APK bytes are modified. The native MediaCodec bridge
loads Homebrew libvpx through the runtime's dynamic library boundary and
initializes a VP9 decoder; format metadata now includes Android planar YUV
color/stride/crop keys. A rebuilt graphics link audit passes and a 30-second
unmodified run reaches a real `Blue Archive` NSWindow without SIGSEGV when the
output queue is disabled.

The initial attempt disabled VP9 output to avoid a first-frame crash. That was
not a successful startup and has been removed. See the next progress entry for
the actual source-pointer diagnosis and shared NDK implementation.

## Progress — 2026-09-04 Blue Archive native media and presentation diagnosis

Read this entry first for continuation. Goal remains **active**: actual
unmodified Blue Archive must display its initial UI and respond to physical
input. A black window is not completion.

- Sol's ELF/stack review proved Unity's Java fallback creates an empty local
  output-buffer vector and selects NULL for dequeued index 0. The failing read
  at `0x119180` is a chroma-plane offset from a NULL **source**, not destination.
  The JNI direct pointer was valid. LLDB attach was denied by macOS; bounded
  in-process stack diagnostics supplied the evidence (`/tmp/blue-stack-run.log`).
- The fallback was selected because the runtime lacked `AMediaExtractor_new`.
  Shared WebM parsing (`compat/darwin_media_extractor.h`) and VP9 decoding
  (`compat/darwin_vp9_decoder.h`) now back Java and real libmediandk entry points.
  NDK extractor callbacks/FD reads and required format-key data symbols exist.
  Native output has exclusive slot ownership, format changes, real timestamps,
  and a retained full-capacity buffer for zero-size EOS. No app-specific output
  suppression remains; APK bytes are untouched.
- Standalone `tools/media-decoder-smoke.cc` and `media-extractor-smoke.cc` passed
  ASAN/UBSan. The latter caught a truncated-VINT OOB which was fixed. The actual
  production NDK object passed `tools/media-ndk-smoke.cc` (10 frames, format,
  EOS, slot ownership, custom-source deletion lifetime).
- Actual 60-second run `/tmp/blue-ndk-live.log` selected NDK and decoded logo
  and title frames without the old SIGSEGV. Window capture
  `/tmp/blue-ndk-first.png` is still **black**, so visual/input acceptance is open.
- Graphics diagnosis: Unity selects MoltenVK but Android WSI is unsupported.
  `vkCreateAndroidSurfaceKHR` fails late with no Unity backend fallback.
  Runtime advertised `VK_KHR_swapchain` despite this. Namespace now filters and
  rejects swapchain + dependent presentation extensions while retaining genuine
  offscreen Vulkan/AHB. Namespace unit tests passed; final rebuilt runtime and
  actual GLES fallback/window capture are the immediate next gate.

Build only with the native graph and its absolute graphics dylib target. Latest
build log `/tmp/blue-vulkan-capability-build2.log`. Do not overlap runtime runs;
an unrelated old host (PID 48115, button DEX) is outside the Blue Archive run.

### Progress — 2026-09-04 Android Vulkan WSI integration (in progress)

- Filtering unsupported swapchain extensions does not make this Unity build
  select GLES: its availability probe does not require Android WSI. Implementing
  the real Android presentation contract is therefore the current task.
- New Rust modules `vulkan_wsi.rs`, `vulkan_wsi_backend.rs`, and
  `vulkan_acquire_fences.rs` connect Vulkan surfaces/swapchains to three native
  window AHardwareBuffer/IOSurface slots and actual imported MoltenVK images.
  Base KHR swapchain/Android surface are translated, unsupported presentation
  extensions remain hidden. No host window replacement or CPU pixel path.
- Acquire uses real native-buffer readiness and semaphore/fence completion;
  present forwards a GPU completion sync FD to SurfaceControl. Internal binary
  completion semaphore is consumed after signaling to advance MoltenVK's event
  counter correctly. NativeWindow now holds queued slots until consumer release.
- Namespace tests pass (9); native linked-runtime audit passed first build.
  First actual run `/tmp/blue-wsi-live.log` reveals another crash before swapchain
  creation: MoltenVK image-view constructor receives invalid image handle
  `0x73752f61ffffffff`. Sol disassembly verified CI.image at offset 24, not a
  semaphore/pNext failure. Image create/view tracing is being added to find its
  producer. No visual success yet. Latest rebuild `/tmp/blue-wsi-build3.log`.
- Review also identified swapchain recreation/resize with a consumer-held last
  frame; retirement-aware native slot generation is being designed, not bypassed
  with premature release. Actual window + physical input remain acceptance gates.

### Progress — 2026-09-04 WSI real-frame synchronization fixes

- Rejected swapchain root cause confirmed: Unity requests `format=0` with only
  UNORM37 available. Advertising/implementing SRGB43 with matching Metal sRGB
  textures fixes selection. Vulkan producer storage is RGBA; IOSurface metadata
  now informs both local and central Metal composer source textures, keeping
  legacy ANGLE/HWUI BGRA outputs unchanged.
- Actual swapchain creation/import and repeated QueuePresent now run. First
  attempt hit SIGPIPE from an internal async fence notification (then Unity's
  crash-handler SIGSEGV loop). Read-end guard now survives public fence close
  until the callback writes/closes. Ordinary guest-pipe signals remain unchanged.
  Large diagnostic `/tmp/blue-wsi-srgb.log` is compressed losslessly to `.log.gz`.
- Next run `/tmp/blue-wsi-fence-live.log` has no prior SIGPIPE/SIGSEGV but GPU
  times out after roughly 70 frames. Sol found Unity recycles render-complete
  semaphores into acquire: FD-only counters missed their prior native waits.
  WSI acquire now signals actual completed Metal-event value + 1 at the Vulkan
  quiescent acquire boundary. This is NOT applied to generic FD imports, which
  require separate payload replacement semantics. Rebuild audit passes; current
  90-second actual run is `/tmp/blue-wsi-recycle-live.log`.
- Completion semaphores are per-image, avoiding out-of-order cross-queue event
  signals. Native slots now use bounded retired generations for same-size
  recreation and resize; no borrowed/current buffer is recycled prematurely.
- `tools/vulkan-wsi-smoke.cc` compiles, but its standalone dlopen attempt cannot
  access hidden runtime symbols. It is **not** an executed passing test; actual
  APK runs and namespace tests are the present evidence. Black captures remain
  failures; visual/input acceptance is still open.

### Progress — 2026-09-04 Blue Archive landscape / BLAST completion

Read this entry first. Goal remains active; no final visual/input acceptance.

- The 90-second WSI run processed thousands of frames without the prior GPU
  semaphore timeout. A fresh `android.system` compositor (PID 96887) corrected
  RGBA/top-left presentation. Earlier PID 48115 was subsequently identified as
  the profile's system server, not an unrelated button app; it was stopped only
  after the game exited and no other application was active.
- Manifest USER_LANDSCAPE (11) now reaches ActivityInfo and the display before
  onCreate. The real window is 640x360 points / 1280x720 backing pixels.
- `/tmp/blue-landscape-first.png` and `/tmp/blue-layout-bisect.png` show real
  animated game content plus the game's timeout Notice, but only in the top
  240 pixels. WMS relayout used a historical 120dp fallback, producing
  `1280x240 at=0,240`. Changing that to full display height makes a valid
  1280x720 Vulkan swapchain but the opaque HWUI root covers the child entirely
  (`/tmp/blue-landscape-fixed2.png`). The fallback is temporarily reverted for
  this bisect, not accepted as a fix.
- Sol confirmed two contract gaps: inherited parent geometry is not yet used
  by the Metal composer, and BLAST syncNextTransaction returns success without
  delivering its Consumer. The latter prevents the real SurfaceView draw-
  finished / transparent-hole transition. Implementing real next-buffer
  transaction ownership is the immediate visual gate, not a child-last overlay.
- Root added NativeWindow pre-apply transaction interception, per-frame slot
  serials, transaction callback collections preserved through merge, and
  explicit unapplied-transaction discard cleanup. The two native objects
  compile. Luna is implementing the BLAST JNI state machine against these APIs;
  no integrated rebuild/run yet.
- Network trace `/tmp/blue-landscape-fixed.log` resolves/connects CloudFront and
  writes TLS but never enters a receive syscall. `select` is an ENOSYS stub
  despite Unity importing/using it. Luna is implementing the general brokered
  select contract with tests. Socket readiness diagnostics are now bounded to
  avoid another runaway log. APK bytes remain unchanged.

### Progress — 2026-09-04 BLAST ownership integration (in progress)

- Read this entry first. Goal is still active; full-window presentation and
  physical input have not passed. Application-window fallback restored to full
  display height; button DEX rebuild passes (92 classes / 1393 methods).
- Per-buffer transaction completion/discard callbacks now return overwritten
  buffers at merge time with their producer acquire fence. This prevents both
  a three-slot continuous-sync stall and premature GPU buffer reuse. Platform
  and native-window objects compile; integrated runtime rebuild is pending.
- BLAST commit gating, frame-indexed future transactions and Consumer exception
  ownership are under review/implementation; do not treat initial patch as done.
- Correction to network note above: missing adapter receive logs do not prove
  no TLS reads. NativeCrypto can borrow a host descriptor, and TLS write phases
  suggest a server response. `select` compatibility is being fixed/tested but
  has not been established as the game's timeout root cause. Broker guest FDs
  use a high virtual namespace, requiring particular care with fd_set callers.

### Progress — 2026-09-04 SurfaceView transparent-region gate

- BLAST ownership now has atomic one-shot reservation, generation-checked
  commit/discard gates and an ordered draining phase. Future-frame metadata is
  separate from continuous accumulation. Integrated graphics link audit passes.
- Actual full-height run still rendered black. Targeted tracing proves the root
  layer queues once, the Unity BLAST child continues for thousands of frames,
  and SurfaceView is already `mDrawFinished=true`, `mHaveFrame=true`,
  `mSurfaceCreated=true`, `mSubLayer=-2`. Therefore BLAST completion is no
  longer the immediate visual blocker.
- The concrete missing Android contract is now identified:
  `SurfaceControl.Transaction.nativeSetTransparentRegionHint` is registered as
  a no-op. Android uses this hint with an opaque ViewRoot buffer so the
  behind-parent SurfaceView region does not cover the child. The implementation
  must preserve a bounded Region through transaction retention/wire state and
  exclude it while Metal composites the root; this is in progress. Do not solve
  it by forcing child-last ordering, which breaks Android above/below-parent
  semantics.
- Brokered `select` audit passes under ASAN/UBSAN/TSAN, including zero-timeout,
  readiness bit count and error semantics. High virtual guest descriptors remain
  intentionally unrepresentable by Android's 1024-bit fd_set. New `__read_chk`
  tracing is ready for a later network run; no timeout root cause claimed.

### Progress — 2026-09-04 zero-sized ViewRoot root cause (latest)

- Correction: transparent-region hint was a missing contract, not the proven
  cause of this game's black frame. Its end-to-end implementation and real
  Metal hole/clear smoke pass, but the APK never calls the hint JNI yet.
- `/tmp/blue-viewroot-trace.log` shows the entire Decor/UnityPlayer/SurfaceView
  tree measured/layout at 0x0; actual ViewRoot mWidth/mHeight are zero too.
  Sol traced this to ProbeResources.configureDisplaySize() calling
  CONFIGURATION.setToDefaults() during manifest landscape rotation, erasing
  the initial windowConfiguration bounds/maxBounds without restoring them.
- AOSP async relayout computes its frame locally from those resource bounds.
  This explains the 240px/fullscreen bisect: y=240→0 forces sync relayout and
  WMS repairs the frame; y=0→0 allows async relayout to keep the empty frame.
  Do not add synthetic resized callbacks or force synchronous relayout.
- Patched display configuration to restore bounds/appBounds/maxBounds each
  resize and WMS synchronous relayout to return a populated mergedConfiguration.
  DEX verification now expects baseline 36/978 and button 92/1395. Final DEX
  rebuild and actual full-window/input check are next. Goal still active.
- New actual network logs contain successful recvfrom responses. No broker
  read-loss diagnosis is supported; Firebase errors mention missing Android
  package certificate fingerprint. Game timeout remains separately unverified.

### Progress — 2026-09-04 full landscape and physical input PASS

- Configuration fix is verified on the actual APK. `/tmp/blue-bounds-fixed2.log`
  shows DecorView/UnityPlayer/SurfaceView all measured and laid out at 1280x720.
  `/tmp/blue-bounds-fixed2.png` is a fresh full-window capture of the animated
  title screen and the game's network timeout Notice (80002).
- A real macOS pointer click at screen (1100,383) pressed Confirm. The Notice
  disappeared and the title video continued, captured in
  `/tmp/blue-bounds-click.png`. No test-pointer/probe injection was used.
- Normal merged configuration delivery also exposed missing hidden
  Context.getWindowContextToken; Activity-backed ProbeContext now returns null,
  matching its non-WindowContext identity. DEX gates pass at 36/979 and 92/1396.
- Remaining end-to-end gate is the game's NXPatcher network initialization
  timeout, not graphics/input. Luna is tracing its actual Java error path.
  Firebase certificate errors are a separate clue, not an established cause.
  Goal remains active while runtime work can continue; no gameplay claim yet.

### Progress — 2026-09-04 original split identity and patcher I/O

- Correction to prior "unmodified APK" wording: the working `blue-combined.apk`
  was assembled from base and ABI split and fails apksigner verification
  (missing META-INF/MANIFEST.MF). The original `com.nexon.bluearchive.apk`
  verifies with Nexon's SHA-256 certificate
  `0c7d88dc94a84398660642a6d919496e4e192fdb540a643fb5c3cf9a23e80689`.
  Preserve original base and splits in installation; split support is in flight.
- NXPatcher 80002 specifically maps Volley NoConnectionError from POST
  `https://api-patch.nexon.com/patch/v1.1/version-check` through HurlStack.
  It is not its RequestFuture timeout (80001). Trace underlying URLConnection
  exception next; successful Firebase traffic does not validate this request.
- UnixFileSystem.getSpace0 called host statfs on guest paths, explaining the
  zero free-space report. Added a statvfs-provider-backed Darwin statfs bridge
  preserving fragment units and errors. `tools/libcore-space-smoke.c` passes,
  standalone managed UnixFileSystem acceptance and full graphics link pass.
- Own-package PackageInfo signing queries now invoke actual AOSP signature
  verification and preserve SigningDetails history, rather than manufacturing
  certificates. DEX verified at baseline 36/985 and button 92/1400 before the
  splitSourceDirs addition; actual signed-APK verification remains to run.

### Progress — 2026-09-04 original install PASS; DNS exception root cause

- Original base + arm64 split now install/run without repacking. Installed
  identity is `a36724c4362a531fb461646422c1487627de438436d6a1515838662321f10823`.
  `base.apk` SHA-256 `25479ffb2e0710285a6f11e5666273b97edb68f3d6ae16ba78fd388fd7cd45e8`
  and `split-0.apk` `2049eeaba16d4f6b29c51d0dc5551c1e3e8598274dcfce140c3e816d47602ee4`
  match their original files exactly. `/tmp/blue-original-split.png` is a fresh
  full-window capture, still showing error 80002. No gameplay claim.
- Installer preserves single-APK identities, rejects unsupported code/resource
  splits and multiple native-bearing archives, and publishes the extractor's
  sealed directory atomically. Split paths receive exact read-only capabilities
  in the filesystem facade; its unit tests pass.
- `/tmp/blue-original-cause.log` reveals the immediate HTTPS failure:
  Linux.android_getaddrinfo threw UnknownHostException during the preliminary
  AI_NUMERICHOST check. InetAddressUtils catches GaiException, not that type,
  so ordinary hostnames never reached DNS. Patched to the platform exception
  contract, with explicit bionic-to-Darwin hint/EAI conversion and a passing
  `tools/dns-hints-smoke.cc` test. Integrated run is pending.
- Java socket/connect/poll/options were also unsupported; Luna is connecting
  those JNI calls to the existing broker, with Sol reviewing FD lifetime,
  mapped IPv4-on-IPv6, errno and timeval boundaries. Do not assume that native
  Unity TLS success validates Java/Conscrypt.
- Signature verification exposed missing IncrementalManager IncFS JNI. Added
  the normal no-IncFS-signature result so AOSP can inspect .idsig then v3/v2;
  no signature bypass. Native rebuild/actual verification pending. Current
  support DEX gates pass at 36/986 and 92/1401.

### Progress — 2026-09-04 Java DNS/TCP verified, stream JNI next

- `/tmp/blue-network-integrated.log` proves the corrected numeric-IP fallback
  reaches real DNS (four patch-server IPv4 results), then broker TCP connection.
  The opt-in normal HttpsURLConnection request now reaches ConscryptEngineSocket
  TLS write and fails at the missing `SocketOutputStream.socketWrite0` JNI.
  Implement both Java socket stream natives next, not a host HTTP bypass.
- Explicit libcore build passes 135 methods, 46 regular + 7 critical supported,
  82 typed unsupported. Corrected generator preserves original method names
  and dispatches connect/bind overload implementations by descriptor.
- Important build correction: the old Ninja graph did not rebuild the external
  libcore archive or relink when that archive changed. This run used explicit
  `build-android16-libcore-darwin-linux.sh` then `art-bootstrap
  audit-runtime-graphics-link`; both pass. Graph dependency fix is under review.
- IncFS signature detection progressed to content-digest verification and
  exposed `nativeIsIncrementalFd`. Added false capability/fd/path queries for
  ordinary macOS filesystems. Framework object compiles; final link pending.

### Progress — 2026-09-04 HTTPS transport PASS; resource completion unverified

- `/tmp/blue-stream-live.log`: normal Java HttpsURLConnection/Conscrypt returns
  HTTP 400 for the deliberately empty diagnostic POST in 151ms. This is the
  expected malformed-request response, proving actual DNS/TCP/TLS/HTTP works.
- Actual game advanced through NXPatcher resource check into startDownload;
  the new blocker is NXApplicationUtil.isRunningForeground null-array NPE at
  line 31. Process metadata contract is being fixed; do not claim downloaded
  resources or gameplay yet. `/tmp/blue-after-https.png` shows the title video
  without the old 80002 Notice, but still resetting game data.
- Actual app usable-space query now returns 10,788,069,376 bytes. The final
  missing connection was Darwin's asm symbol alias surviving command-line
  macro renaming. Redirects now install after system declarations, and build
  checks require the actual bridge symbols in UnixFileSystem_md.o.
- Socket stream JNI matches Android16 descriptors (no obsolete init), uses
  broker guest FDs, finite read poll+MSG_DONTWAIT, partial writes, AOSP I/O
  exceptions, array-release discipline and async-close handling. New timeout
  option roundtrip tests pass ASan/UBSan/TSan.
- Original APK signing still fails its full v3 verifier. MAP_POPULATE used by
  AOSP digest mappings was rejected; now translated to best-effort MADV_WILLNEED
  with a native mapping-content regression test. Rebuild/actual verification
  pending; failed ParseResult exceptions are now logged for further diagnosis.

### Progress — 2026-09-04 original APK reaches patch-state serialization

- Latest original base + ABI split run: `/tmp/blue-pkglist-live.log`, window
  `/tmp/blue-pkglist-progress.png`. RunningAppProcessInfo.pkgList now reports
  the actual package; the foreground-check NPE is gone. Game reaches
  NXPatcher.StartDownloadAsyncTask -> NXFileManager.savePatch, but
  ObjectStreamClass.hasStaticInitializer(Class, boolean) JNI is missing.
  This is the current blocking implementation task; no download/gameplay claim.
- Support DEX 36/986 + 92/1401 and final graphics link/audit pass.
  MAP_POPULATE translation is now loaded; no signing-verifier warning occurs
  in this run. Added explicit successful-verification logging for the next run.
- The static-resource HTTP 400 is from optional EVE resource lookup: inspected
  SDK ignores that callback error and proceeds. Empty diagnostic POST 400 is
  separately expected and proves transport only, not game API correctness.
- DataStore rename warnings are consistent with racing writers of one .tmp
  path, not proven mount/permission failure. Read-only diagnosis found correct
  private route, existing destination and absent temp. Do not invent a rename
  fallback without capturing the failing errno.

### Progress — 2026-09-04 serialization JNI connected, actual rerun pending

- Added the platform ObjectStreamClass.hasStaticInitializer(Class, boolean)
  registration with real class/superclass initializer lookup and selective
  NoSuchMethodError handling, not a constant result. Native graph link/audit
  passed (`/tmp/blue-serialization-native.log`).
- Original APK rerun is `/tmp/blue-serialization-live.log`; inspect its latest
  exception/download progress before further edits. Explicit signer-success
  log is included in this run's support DEX. Actual mapped-content smoke on
  the 100,820,154-byte original base APK passed.

### Progress — 2026-09-04 signature and serialization verified; label metadata next

- `/tmp/blue-serialization-live.log:620` explicitly confirms AOSP content and
  signer verification success on the original base APK. Patch-state write now
  proceeds into foreground Service.onCreate, proving the missing serialization
  JNI gate is passed. No payload-download completion yet.
- Current stop: Service -> NXSystemInfo.getAppName -> ApplicationInfo.labelRes
  is zero -> Resources.NotFoundException -> game error 80108. Preserve separate
  application/launcher labels from the manifest and expose the application's
  actual resource/literal label; do not substitute a fabricated resource.
- Signing review: target SDK floor and legacy rotation exposure match AOSP.
  Cache now resets on configure and guards unconfigured manager access. General
  installer still lacks base/split certificate matching; these specific original
  files were externally verified to have the same signer, not a general guarantee.

### Progress — 2026-09-04 application label fix built

- Inspector now preserves manifest application and launcher-Activity labels
  separately. Original base reports application labelRes `0x7f1200f5`.
  ProbeContext/PackageManager expose that resource ID without masking it with
  nonLocalizedLabel; unrelated package records do not receive current-app labels.
- Native activity label path and graphics audit pass in `/tmp/blue-label-native.log`.
  Generated support DEX counts are now 36/989 and 92/1404; gates updated from
  observed output. Final gate rerun `/tmp/blue-label-dex3.log` is in progress.
  Next: original base/split run, confirm NXPatcherDownloadService.onCreate succeeds.

### Progress — 2026-09-04 download service and file downloads reached

- `/tmp/blue-label-live.log`: Service.onCreate/start/bind and
  nativeOnDownloadProgress/nativeOnFileDownloaded reached. App files total
  154 MiB (not a claim that all of it was downloaded in this run).
- Current blocker after download: OnCompleteAsyncTask -> moveFile ->
  `File.listFiles()` null for a directory. Next fix is real guest directory
  enumeration in UnixFileSystem, not substituting an empty list.
- Original base signature verification remains successful. Inspector unit
  tests pass (2), support DEX 36/989+92/1404 passes, shell syntax/diff pass.
- Label follow-up in progress: restore host title resource resolution without
  replacing ActivityInfo's resource reference; current screenshot
  `/tmp/blue-label-progress.png` displays package name in the window title.

### Progress — 2026-09-04 directory bridge built; ICU regression under repair

- Native graph rebuild and UnixFileSystem list0/managed audit passed
  (`/tmp/blue-directory-native.log`); DEX final counts 36/992 + 92/1407 pass.
  Application icon resource `0x7f0f0000` and host title fixes are included.
- Actual rerun `/tmp/blue-directory-live.log` fails before the window: ICU
  MissingResourceException for icudata/langInfo.res. Directory enumeration
  now routes all managed directories through guest FS, including the authorized
  host ICU runtime directory. Preserve that existing runtime capability with
  per-handle host/guest routing; do not fall back on arbitrary host paths.
- Standalone space and directory tests pass ASan/UBSan (host fallback, guest
  entries/dots, EOF and ENOENT). This does not cover ICU capability yet.

### Progress — 2026-09-04 ICU fixed; IL2CPP GC page-size contradiction identified

- Canonical authorized runtime-root enumeration and tagged host/guest handles
  restore ICU startup. `/tmp/blue-directory-icu-live.log` reaches patch resources
  then libil2cpp aborts; original base signature still verifies.
- Sol resolved actual ELF callsite: libil2cpp+0x19be568 calls madvise(...,4),
  failure branches to string `unmap: madvise failed` and abort. GC init calls
  sysconf(39) at +0x19c6fa4; time facade returns hardcoded 4096 while VM and
  getpagesize use host 16384. Trace the exact rejected range, then consistently
  expose 16KiB; never round outward and zero neighboring live pages.
- `sun.misc.Unsafe.allocateInstance(Class)` missing during patch deserialization
  is now connected via AOSP-equivalent JNI AllocObject; native audit passes
  `/tmp/blue-unsafe-native.log`. Not yet rerun. Host title now has a separate
  resolved display value consumed by GPU surface identity (package metadata
  resource fields remain intact).

### Progress — 2026-09-04 GC failure confirmed by exact runtime trace

- `/tmp/blue-madvise-trace-live.log:2145`: address `0x4a39c1000`, length
  `0x801000`, advice 4 -> EARLY_INVALID_RANGE errno 22 -> IL2CPP abort.
  Confirms 4KiB-vs-16KiB alignment diagnosis, not an ownership guess.
- Current trace run also confirms restored `Blue Archive` host window title
  (window 23094, PID 44866), and no prior Unsafe missing-native message.
- Fix in progress: sysconf page selectors use actual host page size; virtual
  physical/available byte counts stay 8/4GiB expressed in those pages; mincore
  vector length uses the same page size without addition overflow.

### Progress — 2026-09-04 page-size fix integrated, live acceptance running

- `/tmp/blue-pagesize-native.log` full provider closure and graphics audit PASS.
  Native sysconf 39/40 now reports host page size (16KiB here), selectors98/99
  preserve 8/4GiB totals in those units. mincore count uses host page size and
  overflow-safe ceiling division. No outward range expansion was added.
- Actual original base/split run `/tmp/blue-pagesize-live.log` started with
  bounded madvise trace; inspect it for GC discard success and next game state.
  Previous crashed PID44866 was terminated after diagnostics were complete.

### Progress — 2026-09-04 original APK download reached; moving GC gate

- Page-size correction verified in `/tmp/blue-pagesize-live.log`: IL2CPP discard
  succeeds and the real game offers 649.27MB of essential downloads. Physical
  Confirm reaches patch serialization, which fails allocating a 720904-byte
  Object array despite 100MB free: largest contiguous region is 557056 bytes.
- Patches0033/0034 add high-base forwarding and AOSP homogeneous-space compaction
  after failed fragmented allocation, not a heap-size increase. Patch0028 now
  starts standard ART daemons and sets the matching finished-starting lifecycle.
- Canonical graphics build and `/tmp/blue-moving-audit.log` native link audit PASS.
  Optional `DARWIN_ART_VERIFY_GC_COMPACTION=1` forces a graph relocation check
  covering JNI/static roots, cycles, sharing, strings, hash and monitor identity.
- Gate currently FAILS before PASS: `/tmp/blue-moving-live.log` PID11822 SIGBUS
  at original heap end `0x10010214000`. Sol is collecting the real LLDB stack;
  RosAlloc reconstruction explicitly zeroing protected capacity is suspected.
  Do not bypass the gate or report game completion. Prior failed app/server
  PIDs53505/53956 were terminated; APKs and downloaded app data are preserved.

### Progress — 2026-09-04 moving GC PASS; download exposes socket leak

- Patch0035 restores owned RosAlloc map write protection around full zeroing
  during Clear and restores the unused tail guard. Root fixed its patch format
  and added the patched source to runtime_jobs (staging alone was insufficient).
  All0033/0035 actual parent-root git-apply checks PASS. `/tmp/blue-clear-link.log`
  full native link/audit PASS; no unresolved/fake graphics owners.
- `/tmp/blue-clear-live.log` PID33805: moving-GC graph regression PASS, then
  original signed base/split game runs. Sample `/tmp/blue-download-sample.txt`
  confirms actual HeapTaskDaemon, FinalizerDaemon and watchdog threads.
- Physical Confirm advances to 242.07/649.27MB, 944/1605 files, 37.28%; latest
  visible capture `/tmp/blue-window-no-shadow.png` (window23104). Shadow-enabled
  captures intermittently returned black; `screencapture -o -l` works.
- Next blocker is confirmed socket exhaustion, not GC: saved patch reports
  contain `socket failed: EMFILE`, and `/tmp/blue-fd-list.txt` has1002 TCP sockets.
  Retry reports repeatedly serialize patch.report.data while other download
  workers contend on NXReport monitor. Sol/Luna are diagnosing socket lifecycle;
  do not delete app data, suppress reporting, or increase FD limits as a fix.

### Progress — 2026-09-04 socket pre-close implementation and regression

- Direct cause: AOSP PlainSocketImpl pre-close creates a marker socketpair and
  uses libcore dup2. Missing dup2 caused marker leaks, exhausting1024 broker
  slots. PID33805 was terminated after collecting evidence; server34304 remains.
- Added central-broker atomic Description replacement, stable target token,
  deferred exactly-once owner close after active leases drain, adapter routing
  and actual jobject-returning Linux dup2 JNI. Source APK remains unchanged.
- Focused `dup2_replacement_test.cc` passes2048 cycles, active lease/nonblocking
  replacement, invalid-source preservation, same-FD flags, target aliases and
  EPOLL owner0. ASan/UBSan PASS. Root has started full native integration in
  `/tmp/blue-dup2-native.log`; final artifact not yet accepted.
- New opt-in real Java gate `DARWIN_ART_VERIFY_SOCKET_CLOSE=1` binds localhost
  ephemeral Socket then closes twice,1200 iterations. Must pass with moving-GC
  gate before retrying download. Sol reviews remaining pre-syscall lost-wake
  race; header lock/audit update also required. Goal remains active.

### Progress — 2026-09-04 real Java gate PASS and download resumed

- Additional primary leak: Linux providers.close pointed to FS-only close,
  rejecting socket broker tokens while Java cleared its FD. Root changed it
  to socket_broker_close, which already routes ordinary files appropriately.
  Added missing socketpair JNI as well as dup2; retained AOSP close semantics.
- IMPORTANT build ordering: `audit-runtime-graphics-link` alone does not rebuild
  bootstrap adapters. Use bootstrap-internal first (or complete native graph).
  `/tmp/blue-close-adapter-build.log` compiled7 changed adapters; subsequent
  `/tmp/blue-close-adapter-link.log` audit PASS. Earlier gate failures reused
  an old close-provider object. Fixture also now acquires its own network lease
  because it executes before application native-library loading.
- `/tmp/blue-close-adapter-live.log` PID42502: movingGC PASS and realJava socket
  bind/pre-close/final-close/idempotence1200cycles PASS, including provider
  release. Log currently contains a debug boolean NUL: use `rg -a`.
- Physical Confirm resumes remaining405.82MB without deleting prior data.
  PUB directory now774MB; game shows “Download file verified” and animated
  scene frames. Network FD count7 versus prior1002. No new OOM/EMFILE observed.
  Window23121; `/tmp/blue-post-download.png` latest inspected screenshot.
  Continue through final patch processing/login/input; goal not yet complete.
- Deferred-close flush now counts queued and in-flight callbacks through owner
  bookkeeping before deactivation. Central C/ASan/UBSan/TSan audit PASS. Known
  signal-before-host-syscall cancellation race remains a separate review item;
  current active-lease test does not prove that race absent.

### Progress — 2026-09-04 unpacking boundary; filesystem investigation

- PID42502/window23121 remains alive. Fresh window capture
  `/tmp/blue-unpack-current.png` shows “Unpacking game resources...”. Download
  workers and Java versioncheck now park; 05:55:23 Unity reports null Instantiate.
  This is not proof of a particular missing prefab or a completed C# callback.
- Sol inspected original libpatcher nativeOnComplete: callback forwarding exists;
  live LLDB attach is denied. Next diagnosis needs debugger launch or bounded
  runtime tracing. No APK edits or app-data deletion.
- Root found production stdio writable paths retain data only in a Vec;
  fflush is a no-op and fclose drops it. Luna is connecting writable streams
  to guest VFS with persistence/append/update tests. Direct causality to the
  current Unity exception remains unproven. Another Luna audits Java rename0.
- Root removed a diagnostic JNI-boolean NUL from next-build logs and regenerated
  native graph (277 inputs). No new runtime build accepted yet. Goal active.

### Progress — 2026-09-04 streaming stdio implementation underway

- Confirmed downloaded ExcelDB.db is308MiB, beyond old256MiB fopen snapshot cap.
  This establishes a real unsupported file size, not yet the exact Unity caller.
  Luna now implements guest-FD streaming rather than raising the cap; root
  requested real-VFS persistence, append/update, sparse-large-file and short-I/O
  regression coverage. Do not rebuild/run until these edits stabilize.
- Root added optional DARWIN_ART_TRACE_FS_FAILURES diagnostics (max512 failures,
  no file contents, errno preserved) for open/openat/read/pread/write/pwrite/
  rename. FS audit PASS `/tmp/blue-fs-stream-audit.log`; subsequent write trace
  addition compiles with strict warnings. dup2 current regression also PASS.
- Downloaded catalog_Remote.bytes and PUB Resource Catalog bytes have identical
  SHA256388f5f77b2c6980dba3fa00e1a9d4b2cbb84229ef6b6e9ca27871de5102284bb;
  sampled bundle has valid UnityFS header. Preserve downloaded data.

### Progress — 2026-09-04 stdio integration build running

- Streaming implementation passes cargo check and byte/wide facade functional
  normal/ASan/UBSan/TSan gates per implementation agent. Separate production-VFS
  test is being finalized in bionic-stdio-wide-integration/tests/vfs_private.rs;
  do not count it as passed yet. Provider process mode no longer masks VFS
  failures with in-memory fallback. Fixture memory mode remains explicit.
- Sol found scan_core on FD streams could index an empty Vec and abort. Current
  guard returns EOPNOTSUPP for that unsupported scanner path; this prevents
  memory corruption but does NOT complete FD-backed fscanf compatibility.
- Root bootstrap-internal PASS (252 cached), full link/audit currently running
  `/tmp/blue-stream-native-link.log`. PID42502 still old runtime, unpacking
  stalled. Next run must use new artifact and TRACE_FS_FAILURES; no success
  claim until observed actual application progress.
- Java File.rename delegates to Libcore.os.rename and guest FS correctly;
  missing rename0 was ruled out. No rename compatibility patch is needed.

### Progress — 2026-09-04 production VFS regression PASS

- Luna's real VFS test exposed failed SEEK_END changing the FD offset3→7.
  Root fixed signed-offset overflow validation and restores the queried end
  position before any later validation/reset failure. CUR includes pushback.
- `cargo test -q -p bionic-stdio-wide-integration --test vfs_private` PASS in
  `/tmp/blue-stdio-vfs-test.log`: persistence/r+/append, >256MiB sparse file,
  fileno offset sharing, immutable/missing errors and failed-seek rollback.
- First full audit PASS `/tmp/blue-stream-native-link.log`, but archive may
  predate final seek correction. Root reruns `/tmp/blue-stream-final-link.log`
  and stdio final audit `/tmp/blue-stdio-final-audit.log` before app restart.

### Progress — 2026-09-04 restart exposes GC/env-lock deadlock

- Final stdio audit PASS `/tmp/blue-stdio-final2-audit.log`, production VFS test
  PASS `/tmp/blue-stdio-vfs-final.log`, full native link PASS
  `/tmp/blue-stream-final-link.log`. Original process42502 terminated, new
  PID25825/window23136 `/tmp/blue-stream-live.log` stalls black before title.
- Sol confirmed `/tmp/blue-stream-start-sample.txt`: Thread-37 holds host getenv
  lock, receives guest GC suspend and parks in sigsuspend; UnityMain GC syscall
  coordinator waits on the same lock in unconditional debug-flag getenv.
  This is not evidence that stdio persistence failed.
- Luna fixes all syscall/futex diagnostics to eager startup flags. Root changed
  FS trace to eager flag plus stack-only formatting/raw write (no getenv/stdio
  lock in runtime path), FS audit pending `/tmp/blue-fs-eager-audit.log`.
- Separate future risk: guest GC suspend while holding futex/thread-registry
  metadata locks. Requires short critical-section suspend deferral, never
  masking blocking waits. Not the observed deadlock; do not conflate fixes.

### Progress — 2026-09-04 eager diagnostics integration

- FS eager flag/manual-format/raw-write audit PASS (ASan/UBSan). Syscall eager
  flags compile and source/disassembly/ELF/ASan/UBSan audit PASS per Luna.
  Syscall enabled debug formatter still uses vsnprintf: no FILE lock but not
  claimed fully async-signal-safe. Normal disabled debug path performs no env
  lookup or lazy flag initialization. Signal counters are eager globals too.
- Root full native integration running `/tmp/blue-gc-env-link.log`. PID25825
  is still the deadlocked old artifact; must replace it after link verification.

### Progress — 2026-09-04 GC/env deadlock no longer reproduced

- `/tmp/blue-gc-env-link.log` full link PASS. Restart PID48113/window23141,
  log `/tmp/blue-gc-env-live.log`, FS diagnostics enabled. Old25825 ignored
  TERM while deadlocked and was explicitly killed after confirming identity.
- New execution renders animated game again and advances through downloaded
  file verification to “Download file verified”. `/tmp/blue-verification-current.png`
  is inspected fresh capture. At1m31s CPU125%; post-verification work active.
  Original early IndexOutOfRange burst still occurs, later null Instantiate not
  yet observed. Goal remains active; wait for actual unpack/login boundary.
- FS diagnostic budget was consumed by repeated write(fd2) EBADF; next trace
  should budget standard-descriptor failures separately. Do not assume lack of
  later path logs proves no failing resource access. Unity imports fscanf;
  its two call sites are0x44dc7c/0x44de04, not yet shown relevant to asset loads.

### Progress — 2026-09-04 null Instantiate persists; debugger now works

- PID48113 completed Java work after~5min, then06:26:55 emitted the same Unity
  null Instantiate. `/tmp/blue-unpack-null-current.png` confirms unpacking stall.
  StdIO and GC/env fixes are real but do not resolve that asset/caller boundary.
- Debug-only host copy `/tmp/blue-debug-host.Pc1iG4/darwin-art-host` signed with
  existing JIT entitlements plus get-task-allow successfully launches under LLDB:
  `/tmp/blue-debug-host.Pc1iG4/lldb-check.log`. No OS security setting, APK or
  distribution entitlements changed. Prior generic LLDB launch lacked this.
- Root added optional DARWIN_ART_LLDB_COMMAND_FILE + DARWIN_ART_DEBUG_HOST to
  launcher (bash syntax PASS). Sol prepares exception breakpoint commands;
  must pass through guest GC signals rather than swallow SIGINFO.
- FS trace source now reserves separate8-record standard-FD failure budget
  so stderr EBADF cannot exhaust512 path records; compiles, not in live binary.

### Progress — 2026-09-04 debugger exposes semaphore ABI bug

- Debugger launch works with copied signed host. Original ELF exact-header and
  Raise instruction signature checks install breakpoint correctly. Command file
  `/tmp/blue-il2cpp-debug.lldb`, callback `/tmp/blue_il2cpp_debug.py`, result
  `/tmp/blue-il2cpp-exceptions.txt`. Root passed all mapped GC host signals.
- Debug run80770 (LLDB session51541) aborts before managed exception. Root and
  Sol independently disassembled exact caller: libil2cpp+0x19c8bac handles
  sem_timedwait failure using guest errno; +0x19c8c20 aborts for unexpected errno.
- Provider sem_timedwait incorrectly returns positive110 with no errno, instead
  of POSIX -1/ETIMEDOUT. The game's10ms timeout exposes stale errno under debug.
  This is a runtime ABI bug, not an anti-debug block or merely a pause timeout.
  Luna fixes sem_* error contracts with focused tests; pthread APIs must retain
  their separate direct-error-code convention. Full integration pending.

### Progress — 2026-09-04 semaphore ABI integration

- Semaphore failures now return -1 with guest errno, including timed wait's
  ETIMEDOUT. Root review also caught Android SEM_VALUE_MAX=0x3fffffff (not
  UINT_MAX); init/post bounds and immediate timedwait permit consumption fixed.
- `tools/android-bionic-pthread-provider/sem_abi_stress.cc` covers stale errno,
  timeout, bounds, trywait, post/wait and destroy. Normal and ASan/UBSan binaries
  `/tmp/darwin-art-sem-abi-stress{,-san}` PASS (root reran both).
- Full integration `/tmp/blue-sem-link.log` in progress. Next: relaunch original
  APK under prepared LLDB script to capture actual null Instantiate caller.
  No running game process currently; system-server PID42955 retained.

### Progress — 2026-09-04 compositor failure-path self-deadlock

- Sem integration PASS; debug PID10453 passed the prior sem abort but stalled
  before managed exceptions. `/tmp/blue-sem-debug-sample.txt` proves RenderThread
  waiting inside SynchronizeAhbImagesToIosurface; Java main waits for RenderThread.
- Sol source review confirms begin_hardware_buffer_composition holds
  AhbEglImageMutex, then EnsureMetalComposerTarget failure calls end composition,
  whose producer-fence path re-locks that same nonrecursive mutex. Luna fixes
  failure cleanup to restore context and discard staging only, no submit/fence.
- PID10453 explicitly killed through LLDB after capture; session43524 closed.
  Next rebuild needs bootstrap-internal before link audit (compat source change).

### Progress — 2026-09-04 first live IL2CPP exception captures

- Cleanup patch Sol review PASS. Bootstrap compiled1/cached251 and link audit
  PASS (`/tmp/blue-composer-{bootstrap,link}.log`). Native graph regenerated.
- LLDB session18864 PID31319 now passes both startup failures. Bounded trace
  reports initial IOSurface lookup failure (id273), now returns instead of
  deadlocking. Actual managed exception objects and raw ELF-relative caller
  pointers captured in `/tmp/blue-il2cpp-exceptions.txt`.
- Initial exceptions include temporary-dir case probe, audio option cast,
  socket protocol-option support, and NXPatcher.Config constructor lookup.
  These may be caught/expected; do not equate them to the later null failure.
  Managed stack formatter empty; callback command-based bt lacks selected
  process, but raw SBProcess memory capture and formatter work. Sol reviews.
- Goal remains active: await/capture unpack failure, then fix real cause.

### Progress — 2026-09-04 unpack passed; next ELF loader blocker

- PID31319 reached real Select Voice dialog after~7min, without null Instantiate.
  Fresh `/tmp/blue-debug-afterverification.png` inspected. Physical macOS click
  (1100,374) confirmed default Japanese, and title loading screen appeared:
  `/tmp/blue-debug-voice-confirm.png`. Do not attribute causal resolution to
  semaphore alone: debugger timing also changed; normal rerun still required.
- Next actual Java uncaught exception: NgsX initialization loads libgrap.so,
  rejected by ELF loader `non-UTF-8 dynamic symbol name`. Root inspected original
  valid ELF: 8934 dynsyms, 6216 non-UTF8 defined names, zero non-UTF8 undefined
  names. ELF names are byte strings; lossy conversion/skipping is not correct.
- Luna implements exact-byte export/lookup handling; Sol architecture reviews.
  No security bypass or APK changes. Live session18864/PID31319 retained at title.

### Progress — 2026-09-04 raw ELF export integration

- Defined symbol names now stored as Vec<u8>; exported/graph/FFI lookups use
  exact bytes. Root cargo check PASS; Luna existing27 library tests PASS.
  New collision/non-UTF8+ASCII regression requested separately, in progress.
- Undefined external symbols still require UTF8 at existing Rust resolver
  boundary; this is explicit remaining scope, not silent lossy conversion.
  Original libgrap has no such undefined symbols. SONAME/version metadata
  continues through existing text contract.
- Full link `/tmp/blue-raw-symbol-link.log` running. Prior debugger PID31319
  killed via LLDB after preserving captures; session18864 closed. Next launch
  will be normal (no debugger) to verify actual behavior and next loader stage.

### Progress — 2026-09-04 normal run clears raw export rejection

- Full raw-symbol link PASS. New helper regressions (distinct invalid byte
  names vs U+FFFD bytes, ASCII lookup, required NUL) PASS; library tests29/29.
- Normal PID71866/session73376 `/tmp/blue-raw-symbol-normal.log` now reaches
  libgrap symbol resolution (no UTF8 rejection), fails at toupper_l. Startup is
  earlier because voice configuration was saved by the prior real UI click.
- Root audited all243 undefined names against central ownership. Missing libc
  set is toupper_l/tolower_l/basename/getentropy; zlib functions have existing
  explicit libz resolver. Luna locale and libc-function subtasks in parallel.
  APK and libgrap bytes untouched; no SDK/security behavior is bypassed.

### Progress — 2026-09-04 libc SDK imports and secure entropy

- Locale tolower_l/toupper_l implemented without interpreting guest locale as
  Darwin locale; locale audit PASS including Android ELF/ASan/UBSan/ICU checks.
- Process-state basename uses unchanged input + thread-local4096-byte result,
  Android ENAMETOOLONG36. getentropy uses actual host kernel entropy, limit256,
  null/nonzero EFAULT, translated host failures and preserved guest errno.
- Root review caught initial unsafe reuse of existing pseudo-random arc4random
  facade; corrected arc4random/arc4random_buf to host CSPRNG too (rand unchanged).
  This also makes existing getrandom byte backend secure. No fake crypto.
- Root integrated central ownership, generated manifests and test counts:
 779 routes/libc598; namespace ASan/UBSan/TSan audit PASS at
  `/tmp/blue-libgrap-namespace-audit.log`. Process focused audit pending.
- Full native link `/tmp/blue-libgrap-libc-link.log` running. Prior PID71866
  ignored TERM after failed class initialization; verified and killed it.

### Progress — 2026-09-04 original SDK library now loads

- Process-state full focused audit PASS (real Android ELF imports6 and secure
  entropy/basename boundaries). Precise host dependency expectations updated,
  not a wildcard. Full link `/tmp/blue-libgrap-libc-link.log` PASS.
- New **normal** PID4818/session23041/window23169, log
  `/tmp/blue-libgrap-libc-normal.log`: `graph loaded root=libgrap.so sources=1`,
  namespace retained, NgsXNativeResume JNI bridge resolved. No prior UTF8 or
  missing-symbol rejection. Actual original SDK executes; no bypass/stub.
- Resource verification CPU work active. Await actual title/account UI and
  physical input verification without debugger. Native graph/diff check rerun.

### Progress — 2026-09-04 normal-run unpack failure still reproducible

- Important: PID4818 **normal run still fails** at07:18:08 with null Instantiate,
  after~5min. Fresh inspected capture `/tmp/blue-normal-unpack-recurrence.png`.
  SDK raw-symbol/libc fixes are real but do not resolve this earlier asset race.
- Debugger run31319 passed Select Voice/title; its early exception formatter
  calls altered timing. Do not mark goal complete or claim fixed based on it.
- Sol prepares late-only Raise breakpoint script: inactive during startup,
  root enables around resource completion; capture null caller without early
  exception inferior-call perturbation. PID4818 sent TERM after capture.
- Format check and graph regeneration PASS. All work remains uncommitted.

### Progress — 2026-09-04 late-debug also passes; attach-normal experiment

- Late-debug PID27919/session4825 passed unpack/title with no early formatter
  calls. Enabled only System.ArgumentException BP at~4min; never matched.
  `/tmp/blue-late-current.png` inspected. Thus early formatter alone not cause.
- Root compared normal/development host __TEXT.__text: identical737124 bytes,
  SHA2564f64f5b56646f796bcd8da0d2693ba760e06f4767168bffb5f332cc4b1f33404.
  Normal IL2CPP load base also same0x316d44000. Other layout/signal differences
  remain: LLDB default disables ASLR and debugger launcher bypassed profile exec.
- Launcher now permits DARWIN_ART_DEBUG_HOST outside LLDB branch so profile exec
  can run signed debug copy normally. New PID53668/session59221 log
  `/tmp/blue-attach-normal.log`, no initial debugger. Attach around240sec planned.
  Old27919 killed viaLLDB; session4825 closed. Sol prepares attach-only script.
- Title's next SDK library libnxping needs __get_h_errno; Luna handles DNS
  provider in parallel. Keep separate from unresolved normal unpack failure.

### Progress — 2026-09-04 normal exception captured: SoundManager sources null

- Normal launch PID53668, later attached LLDB session79277, caught the actual
  null Instantiate before unwind. Raw capture `/tmp/blue-attach-exceptions.txt`.
  Verified return chain is StartBGM coroutine → BGMPlayer → Instantiate, not UI.
- SoundManager instance `0x13f263630` has DefaultBGMSource(+0x50)=null and
  all other default sources/Mixer null, despite IsInitialized(+0x98)=true.
  CoInitialize IEnumerator completed(state=-1); callback closure count=0.
  Sol tracing original ELF CoInitialize.MoveNext(+0x1a2dc74) and callbacks.
- Both normal logs contain exactly26282 IndexOutOfRange errors before the
  final null Instantiate; debugger-launch successful run contains none.
  First burst starts~26sec after launch, long before the previous late attach.
  Next diagnostic is capture first upstream exception on a normal launch.
- Legacy DNS imports __get_h_errno/gethostbyname/getservbyname implemented;
  focused audit passed, root requested service-storage concurrency review and
  address deduplication before integration. Not yet in loaded runtime.

### Progress — 2026-09-04 resource byte array lifetime corruption captured

- Early attach PID95344 avoided Index burst; closed without claiming success.
  Log-triggered attach instead caught a burst already in progress on **99378**,
  LLDB session **83432**, now STOPPED. Launch session13168.
  `/tmp/blue-index-trigger-normal.log`, `/tmp/blue-first-index-exception.txt`.
  IL2CPP base0x326d44000, Unity0x164b90000; no inferior calls in this capture.
- Parser+0x35d9e50 throws first byte-array bounds check (index0xfd74f).
  Its array pointer0x45d241000 now has class System.Int32[] and length1.
  ContentCatalogData owner0x3e6c2fe00 still caches that same address at+0xd8;
  base64 source string+0xb8 is null. This strongly indicates reclaimed/reused
  managed storage while a heap field still references it, not a short file.
- Sol traces GC/root lifetime; Luna reviews signal/mprotect dirty-page ABI.
  Do not patch APK, suppress errors or skip SoundManager initialization.
- DNS focused audit rerun PASS after mutex-protected host service deep-copy,
  IPv4 deduplication and 8-thread service stress. Full integration pending.

### Progress — 2026-09-04 catalog assignment / hardware lifetime watch

- Closed captured PID99378 after complete read-only evidence. Current **17471**
  runs normally then attached with ONLY a catalog assignment breakpoint; no
  broad Raise breakpoint during startup. LLDB **95156**, launcher83598.
  `/tmp/blue-catalog-edge-normal.log`; IL2CPP base0x316d44000.
- Hit original FromBinaryStream+0x35dd204 after m_ExtraData store+barrier:
  owner0x3d4887e00, child0x46056b000 = Byte[20412081]. Software dirty bit0x80
  set as expected. Hardware write watchpoint1 on child's first8bytes; all
  code breakpoints disabled. Process continued; preserve for first write.
- Previous owner's Boehm kind1,size256,descriptor248 and markbit224 were valid;
  reused child's new kind0,size48 matches Int32[1]. Current marks alone cannot
  timestamp the earlier collection, so do not prematurely change barriers.
- IL2CPP has no mprotect import; incremental GC uses software dirty bits.
  External allocation/wbarrier tracker exports are RET in this release.
  Luna observed no16K-alignment VM fault in prior live trace. setjmp mask ABI
  gap is separate evidence, not established cause of this corruption.

### Progress — 2026-09-04 watch run released correctly; DNS/signal integration

- Watchpoint17471 eventually hit memset reuse at6m40s, but owner+d8 was already
  null and audio had started; this run does NOT reproduce premature release.
  Read-only raw capture `/tmp/blue-catalog-reclaim.txt`; process closed.
  Next diagnostic automation will watch BOTH field-clear and child header,
  auto-resume assignment without a manual pause, and distinguish CleanData.
- Actual host setjmp saves x19–x28 raw; Boehm saves x29 on its own stack, and
  buffer size fits. Sol found no root-loss evidence in this specific path.
- Fixed real host SA_SIGINFO ABI mismatch for legacy guest signal handlers;
  focused signal delivery/query regressions PASS. Not claimed as GC cause.
- Integrated DNS through actual socket-broker resolver (initial full closure
  rejected __get_h_errno). Root replaced broker gethostbyname nullptr stub
  with real DNS facade and added __get_h_errno/getservbyname routes.
  Namespace focused PASS:781 routes/libc600/DNS15. ELF-loader29 tests PASS.
- Full `/tmp/blue-dns-signal-link2.log` PASS; latest runtime now contains DNS
  and signal fixes. New normal launch log `/tmp/blue-dns-signal-normal.log`
  (launcher session22585).
  No debugger attached; verify first26sec Index batch and actual UI.

### Progress — 2026-09-04 normal recurrence; automatic watch also timing-sensitive

- Normal PID51017 with latest DNS/signal runtime still logs26282 Index errors.
  Closed byTERM. These fixes do not resolve GC/lifetime corruption.
- Automatic watch PID55367/session41952 (launcher96169) had no manual pause:
  `/tmp/blue-autoedge-normal.log`, `/tmp/blue-autoedge-events.txt`.
  Assignment callback9.865ms: owner3d3d2fe00, child452e54000 Byte[20412081],
  correctdirtybit. At~72sec watched exactCleanData+35de09c clear, legitimate.
  Closed viaLLDB; no game process retained. Original normal corrupts~26sec,
  so earlydebuggerattachment remains timing-sensitive evenwithoutbroadBPs.
- `/tmp/blue_autoedge.py` now verifies exactCleanData opcode/site; unexpected
  field/header mutationsstop, recognizedclearcontinues. No inferior calls.
  Need normal-run low-overhead telemetry; Sol reviewing real IL2CPP profiler
  hooks as alternative (external trackerexports were no-ops).
- Added actualsocket-broker DNS route regression incl TLS/host/service results;
  broker audit ASan/UBSan/TSan PASS. New runtime full link remainsPASS.

### Progress — 2026-09-04 next: no-debugger GC event telemetry

- All game processes closed intentionally after captures; no currentLLDB gate.
  Fullruntime `/tmp/blue-dns-signal-link2.log` contains DNS+signal fixes but
  normalrun stillfails; no goalcompletion or commit/push claimed.
- Sol verified REAL profiler allocation/GC APIs (unlike no-op externaltrackers):
  independent88-byte client, events0x180, allocation callback afterklass/length,
  GC callback(profiler,event,generation), nonnullheapresize callback required.
- Luna media_ndk_extractor implementing opt-in no-debugger telemetry; Sol
  reviewing install timing (beforevsafteril2cpp_init), callback ABI/GC safety.
  Root owns fullbuild and nextactualnormalrun. Keep APK unchanged; noGCdisable,
  nohiddenmanagedroots, encodeaddresses in boundedrecords, avoidcallbackalloc/
  locks/stdio/getenv. Need chronologyof firstlive-edge storage reuse.
- Install timing resolved: originalProfiler registryreset occursin.init_array,
  notil2cpp_init; graphloadrunsallconstructorsbeforereturn, so minimalaftergraph
  beforeJNI hook is valid. No init interception needed.
- Root tightened SA_SIGINFO fix for SIG_DFL/SIG_IGN: only actualtrampoline or
  requestedSA_SIGINFO getsflag. Addeddefault/ignore query regressions;
  `/tmp/blue-signal-default-regression.log` PASS. Thislastsmallfix awaitsnext
  fullruntimebuild withtelemetry. OriginalAPK hashesrecheckedunchanged.

### Progress — 2026-09-04 no-debugger telemetry implemented and rebuilt

- Code in `compat/darwin_runtime_native_loader.cc`, opt-in
  DARWIN_ART_DEBUG_IL2CPP_GC. Root corrected initialLuna draft: trackownersat
  allocationbeforeconstructors, sample+d8 atGCevents, checkALLallocationclasses
  forchildaddressreuse, loglargeByte[]allocation, no-wrap32768committedrecords
  readbybackgroundlogger (no callbackstdio), onlyencodeaddresswords.
- Solreviewfollowups addressed: onlyByte[]hasrawnumericlength; scrubrawsnapshot
  scratch, clearchildslotbeforeownerretirement, reportoverflow. Reuseevent1
  confirmsownerstillpointsatnewallocation; event0cleared/event2unreadable.
- Bootstrap `/tmp/blue-gc-telemetry-bootstrap2.log` PASS and full link
  `/tmp/blue-gc-telemetry-link2.log` PASS. Loader29testsandfocusedDNS/signal
  alreadyPASS. Optionaltelemetry notyetvalidatedagainstlivecallbacks.
- New originalhost normalrun (NOdebugger/debughostoverride):
  `/tmp/blue-gc-telemetry-normal.log`. Offline decoder `/tmp/blue_decode_gc.py`
  consumesstdin; syntheticrecorddecodePASS. NeedrealnormalIndexreproduction
  plusGCedgechronology, notclaimingrootcausefixed.

### Progress — 2026-09-04 normal allocation reuse confirmed before next GC

- No-debugger normal telemetry reproduces corruption (25855 Index errors),
  `/tmp/blue-gc-telemetry-normal.log`: seq724 Byte[20412081] allocation;
  seq726 owner still points at same address but its class already changed.
- Added bounded encoded large-allocation ledger (64 entries), kind7 records
  reuse before the first edge snapshot. Rebuilt bootstrap and full link PASS:
  `/tmp/blue-gc-reuse-bootstrap.log`, `/tmp/blue-gc-reuse-link.log`.
- Normal `/tmp/blue-gc-reuse-normal.log`: seq746 Byte[] at4574a4000,
  seq748 same address allocated as class116e980c0, seq750 owner32eb2fe00+d8
  still references it. No GC event between allocation and reuse. 25855 Index
  errors again. PID35478 closed after collecting evidence; earlier16203 closed.
- Sol verified event6=PRE_STOP_WORLD,7=POST_STOP_WORLD,1/2=MARK_START/END,
  8/9=PRE/POST_START_WORLD,3/4=RECLAIM_START/END,5=GC_END in original ELF.
  Thus next mark cannot explain reuse; inspect prior lazy reclaim/free lists
  or explicit free. Luna reviews pthread TLS ownership read-only.
- Root added bounded safe frame-chain records (kind8, encoded PCs) on reuse;
  current build `/tmp/blue-gc-reuse-stack-bootstrap2.log` PASS; full link
  `/tmp/blue-gc-reuse-stack-link.log` still running. Next run must use this
  new binary, decode callers against logged class_get_name address.
- Production lifetime fix still NOT implemented. Original APK unchanged;
  no debugger-only success, GC disabling, hidden roots, or goal completion.

### Progress — 2026-09-04 free-at-allocation confirmed; zero-fill boundary capture

- Important correction: IL2CPP profiler allocation notification follows the
  array's large memset. No GC between notification and reuse does not exclude
  collection during initialization before notification (original1916214).
- `/tmp/blue-gc-headerflags-normal.log` proves the block is ALREADY FREE at
  notification: seq752 Byte[20412081] at4520bb000; seq754 same-address header
  flags0x24 (FREE bit0x4), size0x6226000 instead expected0x13776e0; seq755
  header block matches object. Later seq777 reuses it as Int32[], seq782
  confirms the catalog still references that address. Allocator locking is1.
- Sol verified original allocator uses global freelists, init enables locking,
  and suspend handler requests SA_SIGINFO. No evidence for TLS aliasing or a
  missing legacy signal frame. Need precise pre-notification chronology.
- Added optional generic large-zero observer to libc leaf facade, enabled only
  by the existing GC telemetry flag. It brackets zero fills >=1MiB without
  changing fill behavior. Callbacks emit encoded candidate addresses and safe
  header snapshots; GC phase records now carry native thread IDs. No GC disable
  or app patch. Leaf normal + ASan/UBSan audit PASS, including observer tests.
  Fixed stale leaf audit binding count63 to actual existing64 (not new export).
- Bootstrap `/tmp/blue-gc-zero-bootstrap.log` and full link
  `/tmp/blue-gc-zero-link.log` PASS. Current normal original host PID5231,
  launcher session88535, log `/tmp/blue-gc-zero-normal.log`. Decode using
  `/tmp/blue_decode_gc.py`; kinds10=header(flags in gen>>8),11=block,12=zero.
  PID97813 closed; system server75129 belongs to darwin-artd and is retained.
- Separate early stall PID63133 required KILL after TERM could not finish.
  `/tmp/blue-gc-lock-sample.txt`:9 ClassLinker readers +1 writer blocked.
  Luna native_window_wsi_lifetime is implementing a bounded Darwin recursive
  reader / queued writer reproducer and proposing ART-local semantics fix.
  Do not overlap full builds; root owns runtime rebuilds.
- Lifetime fix and stable interactive Blue Archive validation remain pending.

### Progress — 2026-09-04 root cause: foreign pthread token cannot be suspended

- `/tmp/blue-gc-zero-normal.log` establishes the exact failure:
  seq854 zero-fill starts at4554e8010 on thread634283604; header allocated,
  flags0x20,size0x13776e0. Seq858..872 another thread634283801 runs GC.
  Seq874 zero-fill ends; same header now FREE flags0x24,size0x6223000.
  Thus live array is reclaimed while its initializing thread is running.
- Concrete bridge bug: `CurrentThreadToken()` in pthread provider assigns
  tokens to ART/native-created threads but does not publish a ThreadEntry.
  Cross-thread `pthread_kill(token, ...)` looks only in that table and returns
  ESRCH. Self `getattr` succeeds, so GC can register that thread initially.
- Sol verified original GC stop-world19c8ac4 explicitly skips ESRCH threads
  without waiting for suspend acknowledgement. This explains missing live
  registers and premature reclamation. Fix foreign-thread lifecycle/identity,
  not GC policy, APK, or signal-frame guesswork.
- Luna media_ndk_extractor now owns focused foreign-thread reproducer and
  provider fix: publish real host pthread for foreign tokens, remove at exit,
  retain nonownership for join/detach; cross-thread signal/query must work.
  Sol reviews lifecycle. Root owns full builds and actual APK validation.
- Optional signal observer work may exist from the preceding diagnostic task;
  do not assume required. After proven production fix, remove temporary
  app-specific telemetry while preserving unrelated native-loader media work.
- Separate ART patch0036 and recursive-reader smoke exist but are NOT applied
  or runtime-validated. Defer until main failure is fixed or stall recurs.

### Progress — 2026-09-04 foreign-thread fix built; APK validation running

- Luna implemented non-owned foreign ThreadEntry registration in pthread
  provider and retirement after guest TLS destructors. Cross-thread queries
  and signal delivery hold the entry lifecycle mutex; join/detach do not take
  ownership of ART/native threads. Registered self is still a scalar fast path.
- Sol reviewed and required three fixes, now applied: retain shared ownership
  while locking an entry during reset, defer retirement until every EBUSY check
  passes, and fail-fast on registration OOM rather than publish unusable token.
  Final scoped review approved testing.
- Root added `foreign_thread_stress.cc`:64 actual SIGUSR1 deliveries to foreign
  std::threads, stack/sched query, join/detach rejection, stale ESRCH after exit,
  transactional reset. Normal, ASan/UBSan, and TSan all PASS. Existing thread
  lifecycle100 rounds and TLS delete/get/set stress also PASS with ASan/UBSan.
- Full runtime `/tmp/blue-foreign-fix-link.log` PASS. Original APK now running
  PID45391, session63275, `/tmp/blue-foreign-fix-telemetry.log`, no debugger.
  Need catalog correctness, then REMOVE root-owned temporary telemetry and
  validate no-option ordinary execution + physical input. Goal not complete.

### Progress — 2026-09-04 normal run passes former failure, title loading reached

- Post-fix telemetry PROVES array survives asynchronous GC during memset:
  `/tmp/blue-foreign-fix-telemetry.log` seq848..875. Other-thread GC still runs;
  post-zero header is marked1, flags0x20, correctsize0x13776e0 (not FREE).
  Catalog later clears its edge normally; no Index errors.
- Removed all temporary IL2CPP/GC/large-zero observers from production source,
  preserving unrelated native-loader media provider changes. Leaf audit PASS;
  retained correction of its stale binding count63 to existing64. Source grep
  confirms diagnostics absent. Full clean link `/tmp/blue-foreign-clean-link.log`
  PASS, and original APK+split SHA256 still match earlier values.
- Current no-debugger/no-diagnostic-option run: PID60646, session63383,
  `/tmp/blue-foreign-clean-normal.log`. Initial file-verification phase took
  ~5min, then advanced to real Blue Archive logo/Now Loading screen and Nexon
  NgsX SDK initialization, without former null-asset exception. macOS CA validates
  gtable.inface.nexon.com. Only system-server75129 is another retained host.
- Actual CGEvent HID click1100,310 sent during opening; no interactive button
  was present, so this is NOT yet a verified meaningful button response.
  Current window23203 at780,103,640x388. Capture with `screencapture -x -o -l`.
  Latest `/tmp/blue-clean-title-current.png` shows animated title loading.
- Still need title/login controls and meaningful physical input verification.
  Do not mark goal complete based only on loading animation. No commit/push.

### Progress — 2026-09-04 next blocker: close waits for an active HTTP read

- Clean PID60646 still animates title/Now Loading. First non-silent PCM logged,
  former Index/Instantiate failure absent. Do not conflate this new wait with GC.
- `/tmp/blue-clean-title-sample.txt`: BestHTTP.HTTP2 Process thread635037440
  blocked610/610 samples in socket_broker_close -> central FD CloseImpl ->
  condition_variable wait. Root source confirms CloseImpl waits slot.active
  and description.active before closing, contrary to prompt logical close
  while in-flight operations retain their open-file-description reference.
- Sol vulkan_sync_review reviewing bounded lifetime design; Luna
  media_ndk_extractor owns broker/test implementation. Existing Dup2 already
  has deferred_closes worker; reuse that lifecycle, do not fake shutdown or
  free owner objects beneath a blocked read. Keep slot generation until old
  leases release, reject new access immediately, owner close exactly once.
- Need regression replacing old test that explicitly expects blocking close,
  plus duplicate/pending-read/stale-token/owner-drain validation. Root owns
  full runtime rebuild after focused tests/review. Current app retained.

### Progress — 2026-09-04 close lifetime review and next native import

- Root/Sol review found two edge cases in initial logical-close patch: a final
  alias with no slot lease must recycle even when another alias holds an OFD
  lease; synchronous owner close completion must notify quiescence waiters.
  Luna is correcting both and running broker sanitizer regressions before
  root rebuild. Existing Dup2 changes must be preserved.
- Actual APK also logs libnxping.so load rejection for libc sendmmsg. Separate
  Luna owns generic socket-provider implementation and ABI/partial-send tests;
  no APK changes or app-specific success stubs. Root owns full builds.
- Added repeatable `tools/android-bionic-pthread-provider/audit-foreign-threads.sh`.
  All64 real signal rounds PASS independently under ASan, UBSan, TSan; output
  `/tmp/blue-foreign-repeat-audit.log`. Temporary GC diagnostics stay removed.
- Old clean app60646 terminated gracefully for next build/run; system server
  retained. Goal remains active: title Now Loading is not interactive success.

### Progress — 2026-09-04 close/sendmmsg integrated; normal APK rerun

- Broker audit PASS including ASan/UBSan/TSan, alias slot recycling and deferred
  exactly-once close. Initial test hang was fixture ReadAt missing the new
  blocked-object ID, not another production deadlock; fixed by Luna.
- Active socket adapter now provides sendmmsg, Android arm64 mmsghdr64 ABI,
  UDP destination translation, partial-count/short-send semantics and empty
  batch fd/type validation. Adapter audit PASS `/tmp/blue-sendmmsg-adapter-audit.log`.
  Namespace row generated by root; total782/libc601, full namespace sanitizer
  audit PASS `/tmp/blue-sendmmsg-namespace-audit.log`. Semantics checked against
  Linux net/socket.c __sys_sendmmsg (https://raw.githubusercontent.com/torvalds/linux/master/net/socket.c).
- Root fixed missing errno include directory in runtime closure build. Full
  link PASS `/tmp/blue-close-sendmmsg-link.log`; bootstrap PASS. APK hashes
  unchanged. Original normal APK now PID35065/session96143, log
  `/tmp/blue-close-sendmmsg-normal.log`, window23262 at684,115,640x388.
  `/tmp/blue-close-first-window.png` shows real file verification progress.
- Known sendmmsg follow-up: per-element export can switch OFD after concurrent
  dup2. Luna native_window_wsi_lifetime implementing retained-host-fd helper
  and tests while original APK runs; root owns next full build. Do not mark
  complete until title controls meaningfully respond to physical input.

### Progress — 2026-09-04 primary foreign unwind; diagnostic replay prepared

- Normal run35065 failed ~66sec after launch during file verification. FIRST
  error at logline2005 is `Rust cannot catch foreign exceptions, aborting`,
  followed by SIGABRT on OS main thread635752345. Repeated null SIGSEGV at
  libunity+0x6f5e44 is secondary crash-handler failure, not primary cause.
  Sample `/tmp/blue-close-crash-sample.txt`; original log preserved, compressed
  copy `/tmp/blue-close-sendmmsg-normal.log.gz` (~2.2MB) captures whole storm.
  STOP then KILL scoped crashed PID35065; system-server75129 retained.
- Sol suspects AppKit exception crossing Rust pump boundary, NOT proven yet.
  Attach denied. Prepared main-thread-only objc_exception_throw/__cxa_throw
  breakpoints `/tmp/blue-foreign-unwind.lldb`, helper `/tmp/blue_foreign_unwind.py`.
  No inferior calls; GC signals pass; ASLR enabled. Sol owns next LLDB launch.
- Retained-OFD sendmmsg follow-up implemented: one exported host descriptor
  spans entire batch, shared sendmsg translation helper. Adapter sanitizer
  audit PASS `/tmp/blue-sendmmsg-retained-ofd-audit.log`. Full integrated link
  PASS `/tmp/blue-retained-ofd-link.log`, now fully finished before replay.
- Fresh entitled final host `/tmp/blue-unwind-host.kzqeKD/darwin-art-host`.
  Sol asked to launch original APK and report first foreign exception;
  root must not overlap builds/apps. Goal ACTIVE, interactive success absent.

### Progress — 2026-09-04 corrected diagnostic host ABI

- First diagnostic PID53112 hit Unity x18=0 before native throw. Root found
  existing `tools/darwin-x18-abi` contract: normal run script declares SDK12
  before signing, but root's fresh debug copy still declared SDK26.5. This
  was a diagnostic setup mistake, not a newly established production defect.
- Sol temporarily rewound PC to x18-defining instruction for diagnosis; this
  entire run is invalid for acceptance. No original foreign exception caught.
  KILLed stopped53112 and TERM debugger52629 after coordination.
- Applied `declare-darwin-x18-abi.sh` to debug copy then re-signed; vtool now
  verifies SDK12/min11. Sol owns corrected replay with suffix2 diagnostic logs.
  Do not create duplicate x18 probes; existing tracked audit already covers it.
  No concurrent builds/apps; original APK remains untouched.

### Progress — 2026-09-04 corrected replay reaches title; close deadlock absent

- Corrected diagnostic PID66112, LLDB session39965 owned by Sol. Logs
  `/tmp/blue-foreign-app2.stderr.log`, `/tmp/blue-foreign-unwind2.txt`.
  No stops/register modification/native throw/fatal after8min. Prior foreign
  unwind at66sec has NOT reproduced; do not claim its root cause fixed.
- Five-minute sample showed active Java resource version checking; around6min
  progressed to Nexon SDK/title. `libnxping.so` and wrapper now actually load,
  no missing symbol. `/tmp/blue-unwind2-title.png` actual window23295 shows
  Now Loading, not a usable login control. Bounds780,103,640x388.
- `/tmp/blue-foreign2-title-sample.txt`: old CloseImpl wait gone. BestHTTP
  Process now CondWait, Read recvfrom; Ngs thread636293863 spins at original
  libgrap+0x531d78/0x531d9c calling __errno. Sol diagnosing that small loop's
  ordinary ABI contract; no security bypass. Keep goal active and app alive.
- Retained-OFD batch replacement regression also added by Luna and passes
  `/tmp/blue-sendmmsg-retained-ofd-audit.log`; production helper already in
  latest integrated runtime. No new full builds while Sol owns debugger.

### Progress — 2026-09-04 proven direct-SVC return-address defect

- Sol decoded live spinning libgrap wrapper: +0x531d70 sets x8=172(getpid),
  SVC at+0x531d74 rewritten BL, RET loops to+0x531d78 because BL overwrote
  original x30. Live saved LR is13c83dd78, true caller13c55007c below it.
  Definitive loader ABI defect, not security-check bypass or speculative wait.
- Loader also passes libc -1+errno where Linux SVC requires raw -errno;
  guest wrapper consequently interprets errno as1. Syscall facade lacks172.
- Luna native_window_wsi_lifetime owns direct_syscall.rs + parser/lib integration:
  per-site B veneer, preserve syscall register/NZCV/vector state and original
  LR, call provider then raw-error conversion preserving guest errno, B back.
  Sol supplied design/reviews. Luna media_ndk_extractor owns getpid172 provider
  support/tests. Root owns full build/replay once focused tests pass.
- PID66112 STOPPED under LLDB39965 for live proof, no register edits in this
  corrected run. Original foreign unwind still not reproduced. No acceptance
  success yet; goal remains ACTIVE. Keep APK unchanged, no CPU fallback.

### Progress — 2026-09-04 getpid done; loader rewrite in progress

- getpid172 facade+ELF regression audit PASS (ASan/UBSan), matching actual
  process PID and preserving errno. Existing provider changes preserved.
- Root terminated stopped66112/debugger65652 after live proof was captured;
  only system-server75129 remains. No game currently running.
- Loader Luna owns per-site veneer/integration and executable AArch64 tests;
  source may temporarily be between delete/add while replacement is written.
  Do NOT build until it reports stable. Sol reviews before root full rebuild.

### Progress — 2026-09-04 direct-SVC implementation reviewed and tested

- Per-site veneer now integrated. Root+Sol caught initial stack Rn encoding,
  errno conversion/restoration, and test opcode/fixture issues; all corrected.
  Sol FINAL scoped review PASS for GPR/Q/NZCV/FPSR/FPCR preservation, B/LR,
  count/reservation, RX/cache lifetime. Runtime source now frozen.
- Root `cargo test -p darwin-art-elf-loader`:30 PASS including real ARM64
  veneer return/error/success execution. `/tmp/blue-direct-svc-loader-tests.log`.
  Luna media owns new independent register-state child test; not yet hooked.
- Full build runs in parallel with that test authoring, session96489,
  `/tmp/blue-direct-svc-link.log`. Do NOT launch until build has fully finished
  and independent test passed. No game currently running, onlyserver75129.

### Progress — 2026-09-04 full SVC tests passed, ordinary APK replay started

- Root hooked `direct_syscall_execution_tests.rs` at crate root, corrected
  compile/type/errno expectations, and made invoke wrapper preserve the Rust
  caller's callee-saved registers. Test mapping checks MAP_FAILED and flushes
  icache. Actual fixture clobbers registers in mock calls; checks x1..x30,
  q0/q16/NZCV, raw -5 and successful123, errno restoration both paths.
- SDK12-declared signed test binary: ALL31 loader tests PASS, including5
  direct-SVC execution/unit tests. `/tmp/blue-direct-svc-all-tests.log`;
  `/tmp/blue-direct-svc-register-tests.log`. No build warnings after cleanup.
- Full runtime `/tmp/blue-direct-svc-link.log` PASS, fully finished before
  launch. Current ORIGINAL no-debugger APK PID45453/session57730, started
  10:03, `/tmp/blue-direct-svc-normal.log`; onlyserver75129 also runs. No
  concurrent builds permitted. Wait through normal resource checks (~5min),
  verify Ngs getpid loop gone and real title/input controls. Goal stillACTIVE.

### Progress — 2026-09-04 repeatable direct-SVC audit

- Added executable `tools/audit-direct-syscall.sh`: isolated temporary Cargo
  target, applies existing SDK12 x18 declaration only to test executable,
  signs, then runs all tests including register fixture.31 PASS in
  `/tmp/blue-direct-svc-repeatable-audit.log`. Default cargo test ignores the
  x18-sentinel fixture to avoid false failures in current-SDK host tasks.
- Current no-debugger45453 remains alive3min, no fatal/missing-symbol log;
  window23302 at780,103,640x388. `/tmp/blue-direct-svc-verifying.png` shows
  Download file verified. Still waiting for Nexon/title initialization; not
  interactive acceptance. No build touches current runtime binaries.

### Progress — 2026-09-04 original APK reaches login; fixing modal input

- Normal no-debugger PID45453 reached real Nexon login chooser and Unity
  TOUCH TO START. `/tmp/blue-direct-svc-login.png`, window23302. Original
  APK unchanged. Prior foreign-unwind abort has not reproduced; do not claim
  its root cause fixed. Physical close/login clicks produced no transition.
- Sol found direct InputChannel fast path bypasses the only focus refresh:
  new dialog renders while packets remain on Activity channel. Luna added
  owner-Looper focus/topology publication in runtime_graphics_input.cc.
  Generation updates already exist in Java. This is not a complete general
  pointer hit-test/offset-window solution; verify actual modal and dismissal.
- Root build+link session79726, `/tmp/blue-input-focus-build.log`; restart
  original app only after completion. Current45453 stays alive meanwhile.
  Goal ACTIVE until physical interaction verified, not merely visible login.

### Progress — 2026-09-04 modal focus review finalized

- Root integrated Sol correction: pre/post Looper-drain refresh, preallocate
  new root global ref, focus candidate only if receiver ready, preserve old
  root/generation on failure, swap then send old focus loss. Existing native
  SetFocusedInputChannel cancels old stream; identity-checked clear preserves
  new channel. Removed Luna's intermediate side-effectful false-focus probe.
- Source now frozen. Final build+link session96887,
  `/tmp/blue-input-focus-final.log`. Earlier builds PASS but predate final
  ref allocation ordering; use only this final build for replay. Existing
  normal45453 remains alive17min, still no physical acceptance yet.

### Progress — 2026-09-04 modal input responds; stale composition fix

- Final native build PASS, normal15567/window23313 reached Nexon login after
  ~5min. NSRunningApplication.activate did not actually front the app;
  System Events `frontmost of process whose unix id is 15567` worked.
  CGHID X click then emitted NXJsonUtil/WindowOnBackDispatcher dismissal and
  underlying TOUCH TO START appeared, but chooser pixels remained composed.
  `/tmp/blue-focus-after-nexon.png`, `/tmp/blue-focus-normal.log`.
- Sol confirmed session.remove only released client SurfaceControl handle,
  not layer tree ownership. Root added explicit Transaction.reparent(null)
  apply-before-release (NOT Transaction.remove, due native raw ref lifetime).
  Sol reviewed PASS. This avoids redefining release for other aliases.
- Stopped15567. Java support DEX rebuild generated verified93classes/1410
  methods (new D8 try-resource synthetic). Updated expected count from92/1407;
  final rebuild32186 `/tmp/blue-modal-detach-dex-final.log`. No native change
  since last full link. Next restart original APK then actual X-dismiss and
  login interaction. Goal ACTIVE, no credentials/consent submitted.

### Progress — 2026-09-04 fresh replay hits early class-loader wait

- Java DEX rebuild PASS93/1410. Current normal PID43975/session9677,
  window23367, `/tmp/blue-modal-normal.log`. Black before title; sample
  `/tmp/blue-modal-black-sample.txt` shows owner EnsureViewRootSurfaceValid
  -> FindClass -> LookupClass -> pthread read lock, many readers and two
  class-linker writers waiting; CPU~1%, not normal resource validation.
- Sol reviewing live sample/lock paths; no claim detach fix caused this
  earlier startup wait. Existing unapplied0036 recursive-reader patch is
  only a candidate, NOT integrated. Leave app alive for diagnosis. If LLDB
  restart needed, create fresh host copy, apply SDK12 declaration then sign
  entitlements; older /tmp/blue-unwind-host.kzqeKD predates current loader.
- No physical dismissal acceptance for latest DEX yet. Goal remains ACTIVE.

### Progress — 2026-09-04 class-lock diagnostic replay

- Sol confirms all14 readers/2writers wait on classlinker_classes_lock_;
  sampled call paths do NOT prove recursive acquisition. Do not apply0036
  or preload JNI classes as a workaround. Need lock exclusive_owner/raw
  pthread words and blocked ART held_mutexes to distinguish corruption/leak
  from recursion. Normal43975 attach denied; TERM could not end stalled
  owner, then KILL after wait. No data deleted.
- Fresh current host copied to /tmp/blue-lock-host.Rp9NHV/darwin-art-host,
  SDK12 declared BEFORE entitlement signing. Diagnostic direct normal launch
  (not started under debugger) PID61956/session73470,
  `/tmp/blue-lock-diagnostic.log`. Attach only on matching stall; otherwise
  continue actual modal dismissal test. Original APK/data untouched.

### Progress — 2026-09-04 live proof: EGL gate drops Vulkan buffers

- Diagnostic61956 reached real login without class-lock stall. Physical X
  after System Events frontmost emitted dismissal; screen then froze while
  Unity kept rendering. `/tmp/blue-modal-dismiss-later.png`, sample
  `/tmp/blue-modal-dismiss-sample.txt`. No complete visual acceptance.
- Sol read-only LLDB confirmed local controls healthy after dismiss: root1
  attached, structural2/3/5 and Vulkan4 retained, dialog gone. Next Vulkan
  frame begin_hardware_buffer_composition(root1 HWUI buffer) returned0 due
  missing ANGLE context. Metadata fallback submitted ONLY2/3/5 to target87,
  omitting new layer4 Vulkan buffers. This is concrete frame-publication bug,
  not wrong root selection or stalled Unity. Debugger detached, no mutations.
- Luna native_window_wsi_lifetime owns platform.mm direct central fallback
  publication of fence-latched retained IOSurface buffers + completion fence.
  Sol supplies design/review. Root full build after stable patch; current
61956 alive, no concurrent build. Class-lock stall remains separate/open.

### Progress — 2026-09-04 Vulkan fallback patch reviewed, building

- Luna added full retained presentation buffer payload to central fallback,
  actual-target composition-active marking and duplicate completion fence
  registration with host scanout. Sol final review PASS (AHB lifetime, acquire
  fences, monitor duplicate ownership, metadata parity). Root corrected C++
  narrowing on what mask with explicit uint64_t cast.
- Final full build/link session13468,
  `/tmp/blue-vulkan-publication-final-build.log`. Earlier39222 failed only
  compile narrowing. Current61956 remains alive with previous binary; do not
  evaluate fixed behavior until this build finishes and app restarts.

### Progress — 2026-09-04 fallback build passes; black presentation regression

- Full build13468/link PASS. Normal22315/window23411 progressed through
  resource initialization at~128% CPU but displayed black from start.
  `/tmp/blue-vulkan-final-normal.log`, `/tmp/blue-final-loading.png`.
  This is not the earlier class-lock wait; do NOT claim fallback accepted.
- Sol requested live generation counters and layer metadata to distinguish
  completion starvation (ready<latest submitted forever) from opaque HWUI
  root covering Vulkan. No speculative patch applied. Stopped22315 normally.
- Fresh current SDK12+entitled host
  `/tmp/blue-publication-host.0ERgBW/darwin-art-host`; direct normal launch
  PID33464/session33063 `/tmp/blue-publication-diagnostic.log`. Sol attaching
  read-only now. No root clicks/debug in parallel. APK/data untouched.

### Progress — 2026-09-04 video publication works; root-detach hierarchy bug

- Diagnostic33464 live counters submitted=ready=requested2812 and present
  calls3587 ruled out readiness starvation. External read-only IOSurface
  inspection: output86 and Vulkan190 colored, HWUI87 fully transparent.
  Current actual window23417 shows video; previous normal22315 black capture
  is not a proven persistent regression. No further fallback edits made.
- After physical X, video keeps playing (freeze fixed) BUT old chooser still
  overlays. Local onlylayers1–5, nextID7 => only dialog6 removed, no resize
  orphan. `/tmp/blue-publication-dismissed.png`. Root87 stilltransparent.
- Sol found AOSP pointer-free integration defect: layer_state_t::diff drops
  eReparent because both Binder parent handles null; RequestedLayerState sets
  canBeRoot=false but no Hierarchy change, cached root remains visible.
  Fix should preserve eReparent clientChanges when resolved parent ID changes
  OR canBeRoot was true. Keep normal null→UNASSIGNED mapping. Luna owns
  scoped SF patch+root-to-null-only regression; Sol architecture/review.
- Root will fullbuild AND restart old service75129 and app after tests, since
  service binary predates changes. Current33464 alive, debugger detached.

### Progress — 2026-09-04 detach regression passed, fresh service replay

- Added Apple-only resolved-parent/canBeRoot eReparent preservation in
  frameworks-native0001 patch; no fake Binder handles/null sentinel changes.
  Root caught initial test used a child; Luna corrected it to standalone72
  parent0 + eLayerChanged creation, then ONLY eReparent72→0. Focused rerun
  PASS `/tmp/sf-detach-root-build.log`; survivors71/73 and repeated-no-op.
  Sol source review PASS. Media Luna attempts isolated old-code failure
  proof without touching shared outputs; not required for current rebuild.
- Full link41861 PASS `/tmp/blue-root-detach-link.log`. Stopped both app33464
  and old systemserver75129 normally. New original normal launch session78729
  `/tmp/blue-root-detach-normal.log`; new systemserver auto-starts from current
  runtime. Must verify physical X actually disappears + video continues.

### Progress — 2026-09-04 remaining detach path requires causal trace

- Fresh normal94698/window23425 +server95195 shows video/login normally.
  PhysicalX11:29:33 emits dismiss callback, video continues BUT chooser still
  visible `/tmp/blue-root-detach-after-close.png`. Do not claim completion.
- Sol verified new eReparent hunk IN linked ARM64 dylib; not stale linkage.
  Media initial baselinePASS was invalid (old child fixture/stale outputbinary
  after failed script); corrected isolated rerun pending. This does not yet
  establish the full real-app cause, regardless of focused regression.
- Luna now adds TEMP explicit-detach-only causal logs at Java session.remove,
  platform reparent-null and central incoming/order membership. No perframe
  logs or speculative functional changes. Root will rebuild/restart BOTH
  app/service once diagnostics stable, clickX, follow exact missing step.
  Current94698/95195 alive. No credentials/consent submitted. Goal ACTIVE.

### Progress — 2026-09-04 detach-only diagnostic run

- Exact isolated unpatched regression now FAILS exit26 at root detach:
  `/tmp/sf-detach-exact.4hOVCy/build-exact-baseline.sh`. Patched current PASS.
  Previous reportedPASS used stale published binary; corrected evidence kept.
- Added opt-in TEMP DARWIN_ART_TRACE_REPARENT_NULL logs (Java validity/apply,
  producer owner/layer/target, central incoming+order membership). Native and
  Java built+linkedPASS session19086, `/tmp/blue-detach-trace-build.log`.
  Temporary Java reference increases DEXmethods1411 (classes93); restore
  methods1410 if diagnostics removed without other changes.
- Stopped94698/95195. New normal trace-enabled original APK session32425,
  `/tmp/blue-detach-causal.log`, fresh central service. Waitlogin then one
  physicalX and compare app log with profile darwin-artd.log. No further
  functional edits until causal trace identifies missing step.

### Progress — 2026-09-04 causal trace identifies erroneous candidate check

- Trace normal40931/window23433,server41391: physicalX11:43:51 Java validtrue,
  native owner40931/local6/what8000/target160, central aftercommit local6
  order_membership0. Detach reaches frontend and is correct. Server then logs
  `hierarchy omitted visible layers expected=3 ordered=2` and rejects EVERY
  subsequent composition because retained offscreen dialog6 still hasbuffer.
- Root removed invalid candidate-count==onscreen-order invariant from
  service_darwin.mm. Buffered nonhidden != onscreen; authoritative AOSP order
  already selects correct intersection. Keep retained buffers for reattach;
  empty composition still clears target. Sol design approved. TEMP trace
  remains for nextverification, no furtherJavachange.
- Stopped40931/41391 to stop repeatedfailurelogs. Full build/link23332
  `/tmp/blue-offscreen-retained-build.log`. Next fresh app+service, verify
  actualX disappearance/video/reopen. No credentials/consent submitted.

### Progress — 2026-09-04 real popup dismissal passes; input return pending

- Full build/link23332 PASS. Fresh normal75258 +server75716: physical X
  actually removes login chooser; `/tmp/blue-offscreen-dismiss-success.png`.
  Underlying title video continues, unlike the previous rejected-frame path.
- Repeated physical Start/Menu clicks after dismissal do not respond, even
  with frontmost PID75258 verified immediately before AND after CGHID input.
  `/tmp/blue-front-click-check.png` is title only, NOT successful re-entry.
  Goal remains ACTIVE; no login/gameplay claim and no credentials submitted.
- Terminated75258 normally. Diagnostic normal run session25768 uses existing
  DARWIN_ART_DEBUG_INPUT_LATENCY=1 plus detach trace, log
  `/tmp/blue-input-return.log`; current same-binary server75716 retained.
  Luna investigates focus/channel restoration read-only. Root owns builds
  and physical clicks. Temporary detach diagnostics and DEX1411 remain.

### Progress — 2026-09-04 focus-cache hypothesis rejected; FD-domain mismatch

- Baseline diagnostic16893/window23460 reaches chooser normally. Physical X
  12:10:11 dismisses it; logs explicitly show focus=1 changed=1 and focused
  root change afterward. No subsequent pointer dequeue/callback logs for
  Start/Menu. Thus stale Java focus is NOT established; speculative Java
  mRemoved/deferred-generation changes are being reverted without build.
- Concrete mismatch: DarwinInputChannelState creates host socketpair FDs,
  but platform_add_fd/ALooper_pollOnce uses guest broker poll. Terminal
  INVALID callback clears looper_consumer but leaves transport_registered
  true; QueueFocusLossCancel can later resurrect that dead consumer and
  disable owner fallback. Luna owns broker-only InputChannel FD conversion
  plus terminal registration state cleanup; Sol reviewing. Root will build
  once stable and test real dialog close/reopen and underlying Menu clicks.

### Progress — 2026-09-04 InputChannel broker migration built for verification

- Java speculative topology patch reverted; existing fixture remains1411.
- Binder InputChannel now uses guest broker socketpair/send/recv/close with
  Android NONBLOCK/CLOEXEC and DONTWAIT/NOSIGNAL flags. Registration flag is
  atomic; terminal callback clears it and looper_consumer under packet_mutex.
  Sol source review passes. Unrelated native Binder wire host FDs unchanged.
- Broker socketpair now sets SO_NOSIGPIPE on both endpoints (existing socket
  policy), with symmetric failure cleanup. Added plain-send-after-peer-close
  EPIPE regression. Luna runs isolated audit; production sources frozen.
- Stopped16893. Root full build/link session24514 log
  `/tmp/blue-input-broker-build.log` in progress. Next restart service75716
  too, then physical X/Start/Menu with DEBUG_INPUT_LATENCY: acceptance must
  show actual InputChannel callback delivery, not the former fallback path.

### Progress — 2026-09-04 callback enabled; MotionEvent source argument fixed

- Build24514 and isolated socket audit PASS (ASan/UBSan/TSan). Fresh normal
  64210/window23469 proves actual callback delivery, not owner fallback.
- Important evidence correction: unrelated ChatGPT permission window23368
  (layer3,bounds660,160,600,442) covered earlier Start/Menu click positions.
  Frontmost PID alone did NOT guarantee an unobstructed target. No permission
  changed; moved game to1280,103 using real titlebar drag. All future clicks
  must verify covering windows too. Earlier no-ingress cannot alone prove
  queue starvation, though raw-host/broker FD mismatch is verified in source.
- Uncovered callback taps reach ViewRoot but popup X handled=0. Found exact
  constructor mismatch: CreateChannelMotionEvent passed SOURCE_TOUCHSCREEN
  into edgeFlags, leaving source=0. AOSP obtain signature and fallback path
  confirm correct final integers deviceId0,edgeFlags0,source0x1002,display0,
  flags0. Root fixed this argument only. Normal64210 stopped; rebuild69813
  `/tmp/blue-input-source-build.log` running. Next same physical lifecycle.

### Acceptance — 2026-09-04 Blue Archive original APK title + input achieved

- Current normal host91164/window23482, session89448, source-build69813
  PASS and link audit PASS. Original base/split SHA256 unchanged (see above).
  No APK patch, injected app callback, account login, consent, or purchase.
- Real CGHID clicks on unobstructed window1280,103,640x388 verified:
  chooser X -> title; TOUCH TO START -> chooser again; second X -> title;
  Menu -> Options -> close Options -> close Menu -> animated title. Captured
  images visually inspected. InputChannel callback consumed each DOWN/UP,
  handled=1, pending=0/rearmed=0. No owner fallback for these events.
- Retained artifacts (ignored, not APKs):
  `_build/bluearchive-acceptance/title.png`, `login-reopened.png`,
  `menu.png`, `options.png`. Runtime log `/tmp/blue-input-source-live.log`.
  No SIGSEGV/SIGBUS/uncaught exception in this acceptance run (6m47s+).
- Defined goal of original Unity initial screen maintained and interactive
  is achieved. NOT a claim of account authentication or in-game battle
  compatibility. Startup currently takes about5min; historical one-off ART
  class-linker stall remains a documented unreproduced risk, not claimed fixed.
- Window remains open. DEBUG_INPUT_LATENCY logging enabled for this run;
  detach trace disabled. Opt-in detach diagnostic source is retained for
  future lifecycle debugging (default off), fixture remains classes93/methods1411.
  Socket audit ASan/UBSan/TSan PASS, direct-SVC audit31 PASS, SF detached-root
  regression PASS with exact unpatched failure26, final git diff--check PASS.
  Shared pre-existing edits preserved; no commit/push performed this turn.

### Diagnosis — 2026-09-04 loading bottleneck measured (no runtime edits)

- Request: measure loading bottleneck, not implement an optimization. Existing
  source/APKs unchanged; restarted original APK twice, sampled stacks, and
  used a separately signed host copy for read-only debugger attachment.
- Successful diagnostic run96385: launch13:07:11.720 -> login chooser about
  13:12:19.415 (~307.7s, includes sampling/brief debugger pauses). Process CPU
  during plateau124–128% ->35.5% after login. Current window23504 remains open,
  session67646; debug host `/tmp/blue-loading-debug.UD6Q40/darwin-art-host`.
- Dominant measured loading work is Java patch/resource checking on thread
  `com.nexon.pub.bar.versioncheck`. Two 8s/10ms samples at ~65s and ~230s:
  597/677 and589/683 samples (~88%/~86%) in the deep interpreter call branch
  (inclusive, NOT percentage of whole-process CPU). ExecuteSwitchImplCpp,
  DoCall, method resolution and shadow-frame bzero/memset dominate active
  leaves. Read-only LLDB also caught this thread in libcore Access and later
  DoCall->bzero. After login the same thread is idle in MessageQueue/ALooper
  poll (249/249 samples), supporting its loading critical-path attribution.
- Source `probes/runtime_entry_probe.cc:442-443` explicitly sets Interpret=true
  and UseJitCompilation=false. Therefore prioritize ART Java execution/JIT or
  optimized interpreter support, not GPU changes. No measured speedup claim;
  JIT-on A/B is NOT performed, nor is arbitrary flag enabling proven safe.
  Disk/hash/network totals were not instrumented; cannot assign exact full
  startup percentages to them. Sampled plateau is CPU/interpreter dominated.
- Separate abnormal normal-host run79371: CPU~102–105%, one OkHttp-named
  thread at identical anonymous PC0x141ef5e44 across five samples, no login
  by~6min, screen transitioned to Resetting the game data. Address falls in
  a mapping matching libunity text (candidate RVA0x6f5e44), but no live memory
  confirmation because normal host attach denied. Do NOT conflate this with
  successful run's interpreter bottleneck or claim its root cause resolved.
  Process stopped normally; no cache/data was manually cleared.
- Evidence: `/tmp/blue-loading-profile.log`, normal sample early/middle/late/
  plateau/after; `/tmp/blue-loading-debug.log`, debug-sample/debug-late/
  debug-complete.txt, `/tmp/blue-loading-debug-method2.txt`; login capture
  `/tmp/blue-loading-profile-login.png`. No runtime fix, commit, or push.

### Diagnosis — 2026-09-04 multicore capability versus loading scaling

- Read-only source/sample review: guest pthread_create creates real host
  pthreads (android-bionic-pthread-provider/src/provider.cc). Samples show
  Android main, UnityMain, RenderThread, hwuiTask, ANGLE workers and game
  background workers. AppKit main and Android UI owner are separate; this
  does not mean Android main/UI are two separate threads.
- Loading CPU124–128% confirms more than one core-equivalent of execution,
  not broad multicore scaling. The measured versioncheck HandlerThread has
  a serial interpreted Java critical path; adding workers alone cannot be
  assumed to accelerate that app-defined work. No scaling benchmark done.
- Host reports10 logical/physical CPUs, but native bionic-time-facade sysconf
  hardcodes8. Java Runtime.availableProcessors uses Libcore.os.sysconf, whose
  Darwin implementation translates to host sysconf. Thus source paths expose
  inconsistent topology; actual app-returned values were not queried live.
- bionic-syscall-facade sched_setaffinity/getaffinity keep a thread_local
  virtual mask (initial0xff), not a host scheduling binding. Virtual8 CPU
  reporting is not evidence of an actual8-core scheduler limit. CPU topology
  consistency and affinity semantics remain compatibility work. No runtime
  changes or claim of complete multicore correctness.

### Active goal — 2026-09-04 CPU contracts and Blue Archive loading

- Acceptance: consistent host CPU topology across Java/native/sysfs/proc;
  truthful affinity support with regression coverage; safe ART execution
  improvement measured against original APK loading (~308s diagnostic baseline)
  with login/title/input retained. No APK patching, no unsupported JIT flip.
- Work ownership: Luna CPU topology surfaces/tests; Sol read-only ART execution
  architecture review; root affinity seam and integration/real APK verification.
- Raw affinity now queries host online count, validates target IDs and raw
  mask size, rejects nonexistent targets (ESRCH) and unapplied restrictions
  (ENOSYS). Unrestricted masks succeed without pretending to pin. ELF fixture
  and ASan/UBSan audit passed. libc process-state wrappers discovered separately
  and still need unification before integration. No performance claim yet.

### Progress — 2026-09-04 CPU integration and single-init interpreter

- CPU sysconf and synthetic proc/sysfs now derive from host configured/online
  counts (10/10 here), with dynamic out-of-range tests; time/fs audits PASS.
- libc affinity wrappers now use the same fixed C ABI core as raw syscalls;
  libc zero return/tail clearing and raw mask-size/tail preservation tested.
  Process-state standalone test feature links the actual syscall provider;
  production retains one provider owner. Syscall ASan/UBSan and process-state
  audits PASS; full graphics link PASS, duplicate-provider=0.
- ART patch0037 uses feature-guarded __builtin_alloca_uninitialized only for
  interpreter-core shadow-frame backing storage, preserving constructor field
  and GC-reference initialization, stack lifetime/alignment and global auto-init.
  Sol disassembly review confirms two clears -> one in EnterInterpreterFromInvoke,
  both DoCall variants and InvokeBootstrapMethod. JIT/nterp remain disabled:
  compiler sources/executable memory and base-relative reference lowering need
  substantial porting, not an option flip.
- Graph producer/dependency coverage added for interpreter archive and patch;
  xtask15 tests PASS. New APK measurement PID45718/session5090 began
  13:59:50.657 using same debug host/original APK/data. At~76s CPU125.7%,
  still resource verification. Loading completion/speedup NOT yet established.
- Remaining scope limit: affinity IDs use existing provider TID registry, not
  full Linux getpid()==main-gettid semantics; exact host CPU pinning unavailable.

### Measurement — 2026-09-04 integrated run1 and follow-up

- PID45718 reached login UI construction at14:04:42.9 (~292s from13:59:50.657),
  versus prior~308s. Single sequential observation, not a controlled speedup
  estimate. CPU~119–129% during loading ->37% after completion.
- Physical CGHID clicks passed login close -> title -> Start -> login reopen,
  close -> Menu -> Options. Captures `/tmp/multicore-blue-{title,reopened,menu,options}.png`.
- Review found a regression in new topology path: synthetic_proc_contents
  queried host CPU counts for EVERY ordinary asset stat. Early sample25/412
  versioncheck samples in these sysctl calls; fix ordinary-path early return
  and lazy CPU queries before final run. No permission/existence-result caching.

### Outcome — 2026-09-04 CPU contract/loading improvement verified

- Final PID73793/session56439 launched14:09:57.044 and constructed login UI
  at14:14:51 (~294s). Original measured baseline~308s; intermediate~292s.
  Observed14–16s reduction is modest, NOT a statistically controlled speedup
  claim (sequential runs, filesystem/network/cache variance and sampling).
- Final sample `/tmp/multicore-blue-final-sample.txt` at~63s has no sysconf/sysctl
  frames; ordinary asset checks no longer query CPU topology. CPU during
  loading~127–130% ->36% after login. No claim of full10-core loading scaling.
- Final original-APK login close -> title -> Menu physical clicks PASS; prior
  integrated run also verified Start/login reopen and Options. Final capture
  `/tmp/multicore-final-menu.png`; app remains open, no account login, purchase,
  data reset, or APK modification. Intermediate captures noted above.
- Validation: syscall raw+libc target/mask tests and ASan/UBSan PASS;
  process-state audit PASS; full FS tests16 + integration1 PASS; time audit
  PASS; graph15 PASS; full graphics link and final fast relink PASS;
  duplicate-provider=0; default process-state cargo check and diff check PASS.
- Bounded goal delivered: coherent host CPU reporting, truthful unsupported
  CPU pinning, and safe elimination of duplicate ART clearing/FS hot-path
  overhead with original APK retained. JIT/nterp, Linux main-PID/TID equivalence,
  ThreadCpuNanoTime implementation and larger loading improvements remain
  separate work. Shared pre-existing edits preserved; no commit/push.

### Active goal — 2026-09-04 AOSP JIT on Darwin

- User requested JIT implementation next. Acceptance is actual Java methods
  compiled/executed through AOSP JitCompilerInterface + JitCodeCache, then
  exception/GC/JNI regression and original Blue Archive loading/input checks.
  Infrastructure-only/primitive-only gates are intermediate, not completion.
- Sol architecture review: preserve upstream optimizing compiler; initially
  gate to proven primitive methods BEFORE small-pattern matcher, because that
  path installs native stubs before CompileMethod. Reference support needs
  coherent base-relative ABI across codegen, calls/results, roots, spills,
  quick entrypoints and barriers (heap base0x10000000000). Do not flip JIT on
  until code cache and these boundaries are verified.
- Luna synced same-revision ART compiler ed6c006bd06ae060bd9698fd2cb25c4865512ec3
  and VIXL e685e67c8f41596dad341b086a649e9f4a7d6062, reproducible sync-jit-sources;
  parent added same-revision disassembler and build-jit-compiler command.
- Signed memory experiments: POSIX shm dual view faults SIGBUS; unlinked
  regular-file executable view returns EPERM. Rejected these approaches.
  MAP_JIT plus nested per-thread write scopes PASS actual42/73 machine-code
  execution, concurrent reader>1000 while writer scope active. Negative tests
  protected write and execution inside write scope both fault SIGBUS as required.
  This proves host code-memory substrate only, NOT Java JIT execution.
- Work ownership: Luna JitMemoryRegion patch integration; root AOSP compiler
  archive/build and final runtime integration; Sol read-only architecture review.
  Standalone test tools/jit-memory-smoke.cc, helper compat/darwin_jit_memory.*.
  Runtime JIT still disabled; original game run untouched. No commit/push.

### JIT integration checkpoint — 2026-09-04 compiler linked, execution pending

- Real AOSP ARM64 compiler archive now106 objects; libelffile/LZMA43 objects.
  Pinned LZMA c30edc9a7147f4f4829ed9848e2479f3061d46bb. Graphics link PASS,
  no missing symbols, host-fmt=0. build-jit-compiler and Ninja compiler edge
  added; compiler output includes the libelffile support archive.
- Runtime253-object archive integrates MAP_JIT + per-thread nested write scope.
  JitMemoryRegion original layout is preserved (no new member). Apple requires
  pthread JIT protection, rejects zygote JIT; map kind inferred only after
  rejecting unsupported protection. Shared staged headers prevent quote-include
  bypass. Runtime JIT explicitly opt-in DARWIN_ART_JIT=1 pending validation.
- First launch failed JIT data allocation: Darwin ignored below-code hints;
  __PAGEZERO covers low4GB. Sol confirmed data-first allocation then single
  MAP_JIT code allocation is safe if actual full-capacity ordering/distance
  satisfies dataEnd<=codeBegin, codeEnd-dataBegin<=UINT32_MAX. Testing this now.
- Old probe-runtime-graphics uses obsolete boot jars and fails native setup;
  tools/audit-art-jit.sh uses application compatibility boot class path.
  JIT_ACCEPTANCE_ONLY checks JNI/compiler before unrelated UI resource setup.
  No actual compiled Java PASS yet. Logs /tmp/art-jit-{runtime-build,compiler-build,
  link,run-current}.log. Do not mark the full goal complete from these builds.
- Sol reference ABI plan: keep compiled register/stack references compressed32,
  native quick reference returns64; explicit decode heap bases/return, normalize
  call results. Optimized saved-GPR GC roots need decode/visit/reencode; stack
  visitor/deopt consumers likewise. JIT root SLOT addresses need64-bit literals
  (not heap decoding). Card-table indexes need native-address bias. ClearException
  must clear64 bits. Quick helpers/intrinsics remain separately gated until audited.
  Initial primitive eligibility excludes references/calls/allocations/OSR.
- Tests: memory positive/concurrent reader + negative protection PASS;
  bootstrap3, graph15 PASS; baselineDEX993 and buttonDEX1412 PASS.
  No commit/push; original Blue Archive process remains untouched.

### JIT execution milestone — 2026-09-04 primitive machine code PASS

- Data-first mapping fixed JIT creation. Compiler gate initially rejected
  Hello because class initialization was not yet visibly published; acceptance
  now uses AOSP MakeInitializedClassesVisiblyInitialized(wait=true) in native
  thread state before requesting compilation. No eligibility checks weakened.
- AOSP compiled arithmetic5 cases PASS at registered code-cache entry; repeated
  new processes `/tmp/art-jit-aslr-{1,2,3}.log` all PASS with different addresses.
  Explicit GC followed by10000 calls PASS, entry still in JIT code cache.
  JNI native round trip and System.arraycopy PASS. Latest run log
  `/tmp/art-jit-run-current.log` (will be replaced by next expanded test).
- NEXT: reference identity ABI slice. Parent0043 adds nullable64-bit quick
  reference returns, shared gate admits only LL + reference moves/return.
  Luna0042 addresses GC register roots/stack/deopt decoding. Integrate BOTH
  before testing null/non-null identity; field accesses/calls still excluded.
  New division fixture must be rejected by JIT and preserve interpreter
  ArithmeticException/recovery. Expected upcoming DEX counts995/1414.
- Goal remains active: no original-game JIT run or performance claim yet.

### JIT reference/runtime checkpoint — 2026-09-04

- 0042/0042b preserve existing monitor ownership; 0043 implements nullable
  native64 quick reference return from compressed32 HIR. Identity null/non-null
  PASS, explicit GC+10000 arithmetic PASS, concurrent CMS3 cycles with~148631
  identity calls PASS. Native loop checks suspension at call boundaries: NOT
  evidence of moving-GC/register-spill coverage. Unsupported division rejected
  by JIT; interpreter ArithmeticException then valid division PASS. JNI/arraycopy
  retain PASS. Dedicated audit mode only; production launch does not run stress.
- Actual original Blue Archive with JIT created64MB code cache and automatically
  compiled java.lang.Integer.bitCount/rotateLeft. No APK modification. First
  clean run PID98979 (15:28:27 start, window23542) still loading after>6min;
  baseline interpreter~294s. Not a speedup: sample showed repeated unsupported
  compile-task enqueueing. Stopped this diagnostic run before login.
- Added prequeue eligibility rejection in Jit::MaybeEnqueueCompilation (0039),
  also avoids unsupported OSR retries for already-compiled methods. Runtime
  narrow rebuild1 TU PASS and all expanded JIT tests PASS after relink.
  New original-game run is session46143, log `/tmp/blue-jit-filtered-loading.log`.
  Previous unfiltered log `/tmp/blue-jit-final-loading.log`, sample
  `/tmp/blue-jit-loading-sample.txt`. Do not claim regression resolved yet.
- Warm compiler build106 cached/0 compiled, support43 cached. Unit bootstrap3
  + graph15 PASS. Goal active. Luna preparing0044 readonly field slice patch,
  not integrated/admitted yet; continue measuring current runtime separately.

### JIT original APK validation — 2026-09-04 15:47 KST

- PID18426 completed loading: login UI construction logged15:41:48 vs JIT
  startup15:34:52 (~417s; process launched~6s earlier). Earlier interpreter
  measurement~294s was a separate run, not a controlled benchmark. No speedup
  claim; this JIT subset still has a loading regression to investigate.
- Original APK unchanged. Real CGHID clicks closed login, opened Menu then
  Options successfully. Captures `/tmp/blue-jit-login.png`,
  `/tmp/blue-jit-title.png`, `/tmp/blue-jit-menu.png`,
  `/tmp/blue-jit-options.png`. No account login or data reset performed.
  Game remains running, window23547 at x1280/y103, PID18426/session46143.
- Actual automatic compilation covers Integer.bitCount and rotateLeft only.
  Most app methods still interpreted by design. JIT remains opt-in; do not
  enable it by default or call the performance objective complete.
- Sol rejected initial0044: moved HNullCheck input may be unallocated; large
  offset implicit null checks also dereference compressed addresses. Luna is
  preparing explicit null checks in compiler options BEFORE RA plus readonly
  receiver decode. Field bytecodes remain excluded pending tests and review.
- NEXT: validate explicit-null/compiler patch; normal+null int/ref getters and
  large-offset regression before widening gate. Separately investigate JIT
  sampling/instrumentation overhead against matched interpreter run. Goal active.

### JIT OSR overhead removal — 2026-09-04 15:50 KST

- Sol sample review: versioncheck54/248 inclusive samples in
  MaybeDoOnStackReplacement, compiler worker248/248 idle. Current policy rejects
  all OSR compilation, but every interpreted branch still paid instrumentation
  locks and code-cache lookup. 0039 now returns false immediately on Apple at
  MaybeDoOnStackReplacement entry. Caller exception/listener/sampling/suspension
  unchanged. This is not yet an end-to-end measured speedup.
- 0040 now forces implicit_null_checks_=false BEFORE graph/RA. Compiler rebuilt
 1/106 TUs successfully. Runtime graph build PASS (256 tasks after pinned Ninja
  rejected newer build-log version); future narrow command is
  build-runtime-graphics-bootstrap-internal, not build-runtime-interpreter.
- Fast graphics link PASS and `/tmp/art-jit-osr-audit.log` PASS: arithmetic5,
  post-GC10000, nullable refs2, concurrent CMS3 with136229calls, rejected division
  exception/recovery, JNI/arraycopy. W^X positive+negative audit PASS again.
- IMPORTANT: live PID18426 still uses PRE-OSR-fix image; screenshots above prove
  previous JIT build only. Next run must relaunch original APK to measure OSR fix.
  0044 field decoder remains unregistered and bytecode gate remains closed.
  Goal active; no commit/push. Next: matched game timing/sample, then field tests.

### JIT stale-build correction — 2026-09-04 15:56 KST

- Correction to preceding checkpoint: default graph rebuilt cached staged source
  WITHOUT restaging0039. First new run PID60691 still sampled OSR locks; stopped
  it and excluded `/tmp/blue-jit-osr-loading.log` from post-fix measurements.
- Internal bootstrap command restaged0039, rebuilt1/253. Object disassembly now
  proves MaybeDoOnStackReplacement is `mov w0,#0; ret`. Relink and real audit
  `/tmp/art-jit-osr-real-audit.log` PASS all arithmetic/ref/GC/JNI/exception gates
  (concurrent CMS3,139077calls). Prior audit was valid only for prior binary.
- VERIFIED new run PID69231/session59494, window23561 (x780,y103), JIT startup
 15:53:18.445. `/tmp/blue-jit-osr-verified-loading.log`; sample
  `/tmp/blue-jit-osr-verified-sample.txt` contains no OSR/HaveLocalsChanged path.
  Still loading at2m32s,CPU125.5%; no final timing yet. APK/data untouched.
- Luna preparing graph patch-invalidation fix. Sol accepted0040+0044 restricted
  readonly getter design. 0044 registered in compiler builder and compiled1/106
  PASS, but NOT linked into current game and gate still excludes field bytecodes.
  Next: finish game timing/physical clicks; getter fixtures then gate, including
  caller-caught null NPE and large offsets. Do not claim field support yet.

### JIT OSR verified game result — 2026-09-04 16:01 KST

- Verified OSR-fast-return binary reached login UI15:58:21.2 from JIT startup
 15:53:18.445 (~303s). Prior JIT417s, historical interpreter294s. Single-run
  comparison, not statistical proof; one compiler TU was built during early
  loading. Removed OSR path absent in sample. No claim faster than interpreter.
- Physical CGHID clicks closed login and opened Menu/Options. Fresh captures
  `/tmp/blue-jit-osr-verified-final.png`, `/tmp/blue-jit-osr-menu-verified.png`,
  `/tmp/blue-jit-osr-options-verified.png`. PID69231/session59494 remains live,
  window23561 now x992/y436,w640,h388. No login/data reset/settings changed.
- Graph patch from Luna initially adds reverse-applicability safety gate and
  0044 input;16 xtask tests PASS. Parent actual-tree check found no persisted
  graphics edge despite fresh restaging, possibly overlapping patch contexts.
  Agent cpu_topology is investigating false invalidation; do not call graph fix
  complete yet. All current fmt/diff checks PASS.
- Goal active: field gate still closed.0044 compiler archive built but current
  dylib has previous compiler; next step is field fixture validation/admission
  plus final graph cache correctness, not another status-only turn.

### JIT resolved-field execution — 2026-09-04 16:05 KST

- 0044 linked. Shared runtime/compiler eligibility now admits static IL/LL
  methods containing exactly one resolved nonvolatile IGET/IGET_OBJECT and
  matching return; no read barriers, calls, tries, writes or arrays. Existing
  arithmetic/identity eligibility retained. No package/probe-specific allowlist.
- Hello adds int/ref fields and getters plus Java caller catching getter NPE.
  DEX998/1417 verified. `/tmp/art-jit-fields-audit.log` PASS compiled int/ref
  values, null reference field, caller-caught NPE; prior arithmetic5,GC10000,
  nullable refs2,CMS3/155723calls,division exception recovery and JNI stillPASS.
  Actual getter entries checked in JIT code cache before invocation.
- Graph now compares exact runtime source+patch SHA-256 staging identity;
  per-patch reverse probing removed (overlapping0028/0031 ambiguous). Actual
  `/tmp/art-jit-staging-check2.ninja` permits persisted graphics edge on fresh
  shadow; mutation regression covers invalidation. Agent16testsPASS.
- Game PID69231 remains on pre-field binary, untouched this turn. Large-offset
  (>4096) regression and negative volatile test still pending; Luna cpu_topology
  preparing fixture/audit changes, Sol reviewing shared eligibility. Do not mark
  goal complete: compiled calls/other reference paths remain excluded. Next worker
  should read this tail and agent messages before editing fixture-owned files.

### JIT large-offset regression PASS — 2026-09-04

- baseline builder generates Hello.java into ignored build tree from a marker,
  with4096 int+4096 reference padding fields. Runtime asserts both target field
  offsets >16384 before testing. No huge generated source tracked. DEX1002/1421
  verified; `/tmp/art-jit-large-audit.log` PASS actual compiled int/ref loads,
  caller-caught NPE, reference null, all previous arithmetic/GC/JNI tests.
  CMS3 concurrent calls179578. Fields warmed via interpreter before compiling;
  volatile and two-read getters warmed too so rejection is not just unresolved.
- Sol reviewed shared getter gate: no blocker found. Unresolved-field rejection
  lacks a dedicated deterministic test; keep this limitation explicit.
- Added graph/bootstrap manifest equality test to prevent duplicated identity
  source/patch lists diverging.18testsPASS; fmt/diff checksPASS. Latest linked
  dylib includes field compiler; live game PID69231 still previous pre-field image.
- Next: Sol art_execution_review preparing managed static-invoke ABI plan
  (compressed args/native64 ref result normalization). Do not expand gate before
  implementing call boundaries/GC roots. Goal active; no commit or push.

- Follow-up negative experiment: getter field was already resolved before its
  first explicit invocation; `/tmp/art-jit-unresolved-audit.log` failed the
  test's cold-cache assumption. Removed that invalid assertion, NOT the runtime
  unresolved-field rejection. No dedicated unresolved negative proven yet.
  Final relink/audit `/tmp/art-jit-large-final-audit.log` PASS including CMS3/
 155580calls and large-field tests. Worktree restored to passing acceptance.
- Sol preliminary invoke plan: same initialized class, one resolved nonnative/
  nonintrinsic nonrecursive static forwarding call; disable inlining first.
  Normalize reference result after GenerateStaticOrDirectCall (not
  MoveFromReturnRegister, which normal invoke skips), preserving immediate
  post-blr RecordPcInfo. JitDirectAddress already uses64-bit ArtMethod literal.
  Await final review before implementing/allowing calls.

### JIT managed forwarding calls PASS — 2026-09-04 16:24 KST

- Shared gate admits exact invoke-static/move-result/return forwarding (same
  initialized class+shorty, <=5 ordered I/L args, resolved nonnative/nonintrinsic/
  nonsynchronized callee, no direct self-call, no tries/RB). Other invoke forms
  excluded. This does not prohibit mutual recursion through interpreted callees.
- 0040 disables inlining AFTER option parsing.0045 checks post-sharpen HIR for
  direct-static JitDirectAddress/CallArtMethod without clinit/unresolved paths;
  normalizes returned reference with Uxtw(x0,w0) ONLY after call/RecordPcInfo.
 0043 restores native pointer at enclosing return. Initial accidental double
  expansion and malformed hunks corrected before successful compiler build.
- Compiler3/106 and runtime1/253 rebuild PASS; DEX1012/1431 verified. Added
  optimizing_compiler.cc shadow and0045 to builder/graph. Actual acceptance
  `/tmp/art-jit-invoke-audit.log` PASS: interpreted then compiled ref callee,
  nullable reference, mixed I/L/I args, integer call, interpreted callee GC,
  propagated ArithmeticException caught by outer Java caller. All earlier
  arithmetic/field/GC/JNI tests remain PASS. No inlining used as a substitute.
- GC test uses live JNI hello_class root: proves call+GC boundary behavior, not
  exclusive compiled-root liveness. Next tests: fifth argument, negative calls,
  exception then valid same-wrapper call; then original APK on latest binary.
  Live BlueArchive PID69231 remains PRE-field/invoke image (do not reuse as new
  evidence). Goal active, no commit/push. Sol final gate review found no blocker.

### JIT goal completion audit — 2026-09-04 16:36 KST

The requested safe JIT execution path is implemented and verified. This is NOT
complete ART opcode support or a demonstrated game-loading speedup.

- AOSP compiler/runtime: real optimizing ARM64/VIXL archive and code cache,
  registered compiled entrypoints exercised in `/tmp/art-jit-five-audit.log`.
  DEX1014/1433 verified. Five-argument I/L/I/L/I forwarding is present in
  Hello.jitCallMixed and JNI signature; actual fifth value43 participates in
  the checked result. Native/reordered calls rejected after resolution; exception
  followed by successful same-wrapper division21 PASS. All previous testsPASS.
- Memory/ABI: `/tmp/art-jit-final-wx.log` signed MAP_JIT nested thread-local
  write scopes/concurrent execution and forbidden write/execute negative gates
  PASS. Compressed reference return/field/call boundaries exercised, including
  >16KiB fields, null and CMS. JNI/arraycopy and caller-caught exceptionsPASS.
  GC tests do NOT prove moving-GC or exclusive compiled-frame root liveness.
- Actual unmodified Blue Archive: latest PID69863/session10859, JIT start
 16:27:19.438, login UI16:32:26.8 (~307s). Real JIT compilation includes ICU
  String-returning forwarding method plus Integer methods. Log
  `/tmp/blue-jit-invoke-final-loading.log`; sample
  `/tmp/blue-jit-invoke-final-sample.txt` shows only trivial OSR return, no old
  HaveLocalsChanged path. Physical CGHID login-close/Menu/Options clicksPASS.
  Screenshots `/tmp/blue-jit-invoke-login.png`, `/tmp/blue-jit-invoke-menu.png`,
  `/tmp/blue-jit-invoke-options.png`. Game left running, window23574,x1280/y103.
  No account login, reset, purchase or game setting changes.
- Identity: original and installed base APK SHA256 both
 25479ffb2e0710285a6f11e5666273b97edb68f3d6ae16ba78fd388fd7cd45e8;
  original arm64 split SHA256
 2049eeaba16d4f6b29c51d0dc5551c1e3e8598274dcfce140c3e816d47602ee4.
  Executed runtime dylib SHA256
 8a13acb131e0e3982fe54506d6962b32290838e532ab98ce3ad83e2329791f90.
- Build correctness: exact source/patch staging identity prevents stale shadow
  promotion; graph/bootstrap manifest equality and graph tests18PASS;
  fmt/diff checksPASS. No commit/push in this goal.
- Operating limit: JIT is opt-in with DARWIN_ART_JIT=1; default interpreter
  unchanged. Shared fail-closed runtime/compiler gates admit arithmetic,
  reference identity, resolved nonvolatile int/ref getter and same-class static
  forwarding. Other bytecodes continue in the interpreter. OSR, inlining,
  broader runtime helper/heap access and moving-GC verification are future
  expansion work, not silently enabled. Dedicated unresolved-field fixture
  remains absent; the resolved-slot guard is code-reviewed and retained.
- Timing is a single-run comparison: ~307s latest vs historical interpreter294s,
  OSR-fixed303s and initial JIT417s. Do not advertise a speedup. Sol reviewed
  admitted ABI/gates and completion scope; final source verifies5-argument case
  despite the reviewer's earlier stale3-argument observation.

### AOSP ABI convergence feasibility — 2026-09-04

- Correction to conversational explanation: high heap placement is not merely
  a freely chosen optimization. Native arm64 macOS reserves low4GiB as hard
  PAGEZERO; Apple's xnu/bsd/kern/mach_loader.c enforces this for ARM64 binaries.
  The choice is the high-address reference representation, not availability of
  a normal low4GiB heap in this host process.
- Fresh isolated experiment `/tmp/art-low-address.X8hY4n`: default executable
  runs but mach_vm_allocate at0x10000000 returns1; executable linked with
  pagezero_size0x4000 is killed(exit137), including after explicit ad-hoc signing.
  Current host Mach-O PAGEZERO vmsize0x100000000. No runtime modifications.
- Native convergence should centralize necessary reference encode/decode and
  preserve AOSP compiler/GC/dispatch semantics, not assume low-address mapping
  can be restored with a linker flag. Unmodified Linux address-space behavior
  would require a different execution environment such as a VM; not authorized
  or implemented by this feasibility check.

### AOSP reference codegen convergence — 2026-09-04

- Implemented: `compat/art_reference_codegen_arm64.h` owns native/32-bit
  reference encode, nullable decode, and nonnull decode. The caller owns the
  destination register; zero/single-bit aligned bases are enforced at compile
  time so VIXL cannot silently allocate an ORR expansion scratch register.
  Encode and nonnull decode preserve flags; nullable decode documents flag
  clobbering. Encode requires null or an unpoisoned pointer in the heap window.
- Patches0043/0045 now use the shared representation helper. Patch0044 removes
  duplicate field-admission policy and restores upstream unconditional
  MaybeRecordImplicitNullCheck. A caller-owned address temporary feeds normal
  AOSP Load/LoadAcquire. Explicit null checking remains enabled before RA.
- Scope: admission gates are unchanged; this does not enable arbitrary JIT
  bytecodes, OSR, read barriers, or moving GC. macOS high-base representation
  remains necessary; global HeapOperand is deliberately not rewritten because
  hidden scratch lifetime and read-barrier instruction placement differ.
- Verification: compiler rebuild PASS (1 compiled/105 cached), graphics link
  audit PASS, full `tools/audit-art-jit.sh` PASS including GC/JNI, large fields,
  NPE and managed calls. `tools/audit-reference-codegen.sh` executes actual
  generated ARM64 code for zero/high bases, null/sign-boundary values and flag
  preservation; final PASS. Cargo fmt and git diff checks PASS. Sol final
  read-only review found no blocking defect in the admitted scope.
- Evidence: /tmp/art-aosp-reference-audit.log and
  /tmp/art-aosp-reference-emitter-final.log. No new original-app run for this
  representation refactor; previous Blue Archive evidence is separate.
  No commit/push. Next convergence work is broader heap/runtime-helper ABI
  coverage and moving-GC validation before widening the shared admission gate.

### Full AOSP JIT completion goal activated — 2026-09-04

- User explicitly requested 100% support and persistent implementation. Created
  an active goal covering pinned AOSP ARM64 JIT normal-app features, including
  OSR, inlining, GC, exceptions and JNI; do not mark complete for partial slices.
- Added `docs/art-jit-compatibility.md` as the scoped completion/coverage index.
  Baseline gates still exclude most features; no new feature is claimed here.
- Sol is reviewing the full compiled/quick reference ABI and backend migration
  order. Parent inspected the bytecode gate and post-HIR gate; simply admitting
  more opcodes would expose unported reference paths. Next implementation must
  address those ABI boundaries and add compiled-vs-interpreted tests.
- No commit/push; existing uncommitted work preserved. Goal remains active.

### Full JIT implementation handoff: managed return migration — 2026-09-04

- Parent removed 0043 VisitReturn native decode (shared include retained) and
  0045 invoke result encode (post-HIR gate retained). Compiler build PASS:
  2 compiled/104 cached, /tmp/art-managed-return-compiler.log. Do NOT link this
  compiler with old bridges: this is an in-progress atomic ABI migration.
- Luna `cpu_topology` is implementing runtime C++/assembly boundary patches
  0046/0047 and staging/graph manifests. Sol's review requires generic-JNI
  encode only after GenericJniMethodEnd restores the lock; instrumentation
  GPR decode and moved-root encode; proxy saved args remain compressed;
  invoke/OSR JValue stores decode references. Native runtime-helper/deopt
  results remain untouched. Review patches before rebuilding the runtime.
- Regression fixture now tests interpreter/compiled/interpreter JNI identity
  cycles; parent restores compiled identity before concurrent GC so fallback
  cannot pass that check. Added nativeReferenceIdentity to test actual generic
  JNI reference returns via ArtMethod::Invoke, not merely JNI calling Java.
- DEX build PASS with baseline1015/button1434; source APKs untouched. Probe
  compile log /tmp/art-managed-return-probe-build.log; full runtime acceptance
  has NOT run with this ABI yet. No original-app verification for this change.
- Next: finish/review runtime patches, rebuild ARM64/runtime and acceptance
  object, link once, run tools/audit-art-jit.sh, diagnose failures with Sol as
  needed. Then continue root-literal/heap/helper ABI work from coverage index.
  Goal stays active; no 100% support or completion claim, no commit/push.

### Full JIT continuation: boundary review and validation preparation

- Previous goal turn classified as progress: compiler ABI edits and new JNI
  fixture builds changed authoritative state. Re-read the latest ledger and
  confirmed cpu_topology live via collaboration status; no build was restarted
  based on elapsed observation time.
- Reviewed in-progress0046: flagged mismatched patch context and recommended
  GetShortyView after GenericJniMethodEnd, plus proxy pending-exception handling.
  Runtime patch is not yet declared complete; do not link mixed ABI artifacts.
- Confirmed native root-slot truncation in pinned jit_patches_arm64 sources and
  consuming LoadClass/String/MethodType code; recorded exact next fix in the
  coverage index. No root-literal implementation is claimed.
- Sol provided a compiled exit-hook GC acceptance recipe, now recorded in the
  index. It verifies debuggable CodeInfo and stable JIT entry while tracing;
  cleanup may invalidate JIT code, so this must be the final test block.
- Current continuation has verified ongoing implementation and completed source
  review/test design; integration acceptance remains pending runtime0046/0047.
  Goal active, no commit/push. Keep remaining feature matrix intact.

### Managed return ABI integrated and acceptance passed — 2026-09-04

- Previous continuation classified as source-review progress plus verified
  live-agent wait. This turn parent completed ARM64 patch0047 and native exit
  hook patch0049; Luna completed0046/0048 and runtime staging registrations.
- Compiler managed returns now remain AOSP compressed32. Invoke/OSR adapter
  stores decode only reference results into native JValue. A supplemental
  asm-defines generator derives the base from C++ kArtCompressedReferenceBase;
  generated assembly confirmed0x10000000000 (not a separate hardcoded base).
- C++ interpreter/proxy/generic JNI final returns encode references; proxy
  receiver decode preserves saved compressed args. Generic JNI reads shorty
  after cleanup. Instrumentation reads compressed saved GPRs and writes moved
  references back compressed. Native runtime-helper/deopt contracts unchanged.
- Builds PASS: ARM6410objects; runtime253objects compiled4/cached249;
  graphics link closurePASS. Full acceptance exit0 at
  /tmp/art-managed-return-acceptance.log: compiled arithmetic,10000postGC calls,
  interpreter/compiled/interpreter JNI8cycles, native JNI reference8cycles,
  concurrent CMS3collections/148498calls, large fields, NPE and managedcalls.
  This does NOT verify OSR execution, movingGC or exit-listener execution yet.
- Cargo format applied; graph shadow-manifest equality test invoked; diff clean.
  No APK modifications or original-app rerun; no commit/push. Goal remains active.
- Next parallel work: Luna cpu_topology assigned0050 native64 root-slot literal
  migration (compiler only, no gate expansion/build). Parent owns planned
  compiled exit-hook GC acceptance from coverage index, then builds/reviews both.

### Exit-hook test exposes code-cache retirement defect — 2026-09-04

- Previous turn classified progress: integrated ABI build and acceptance PASS.
  Added probes/runtime_jit_exit_hook_acceptance.h and wired it LAST in the JIT
  suite. It compiles a debuggable-code identity while keeping runtime nondebug,
  checks CodeInfo/ContainsPc/stable entry, installs a rooted listener under STW,
  and checks empty-frame compiled/native callbacks plus GC and caller results.
- New fixture jitExitIdentity isolates the hook compile from earlier identity
  recompiles. DEX counts now1016/1435. Probe and DEX builds PASS.
- Actual execution FAILS before completion: both original and fresh fixture
  runs hit jit_code_cache.cc1266 duplicate zombie CHECK. Logs:
  /tmp/art-exit-hook-acceptance.log, /tmp/art-exit-hook-fresh.log. Do not call
  this suite passing or remove its failure just to restore a green result.
- Sol identified RemoveMethod test API frees/removes code before
  ReinitializeMethodsCode, whose UpdateEntryPoints then retires old entry.
  ContainsPc checks arena bounds, not live allocation, and nonnative removal
  does not clean targeted zombie sets. Address reuse yields duplicate retire.
  Fresh fixture reproduces, so this is not fixed by swapping test methods.
- Sol is reviewing exact safe retirement/entry-detach/free order and locks.
  Next: implement targeted lifecycle fix, rebuild runtime, rerun same suite.
  No wholesale zombie clear or skipped assertions. Luna remains assigned0050.
- Listener return-value test alone cannot prove upper GPR bits are zero because
  native invoke adapter truncates W0. Retain source review of0049 writeback or
  add raw quick-return observation; do not overstate this test's coverage.
  Goal active, no commit/push; original apps untouched.
- Additional review found the original probe RemoveMethod lacked its required
  exclusive STW scope. Parent added suspension/GC-critical/STW around removal;
  this source correction has not yet been rebuilt. Runtime lifecycle ordering
  still needs its targeted fix; absence of STW must not be blamed on runtime.

### Code retirement and exit-hook acceptance fixed — 2026-09-04

- Previous turn was progress: test implementation plus reproducible failure and
  source diagnosis. Implemented0051 with reviewed lock/lifecycle ordering:
  verify private code exists under a short reader scope, restore method entry
  while allocation is alive, then remove only the target zombie/processed-zombie
  entries under writer lock before freeing. No blanket clears or skipped checks.
- Registered jit/jit_code_cache.cc in runtime shadow and patched-source selection;
  synchronized patch/source graph manifests. Runtime rebuild compiled1/cached252
  and link closurePASS. Probe's STW removal scope included in rebuilt binary.
- /tmp/art-retirement-acceptance.log exit0: all previous tests plus actual
  compiled/native exit hooks with GC PASS. CMS3collections/164092calls. Sol
  reviewed patch and verified no duplicate zombie/fatal in the new run.
- This proves the tested hook/GC/caller-return path, not moving GC or raw X0
  upper-bit observation.0051 source review checks the targeted lifetime fix.
  Fmt/diff and shadow-manifest equality checks pass. Original apps not rerun.
- Next: Luna0050 native64 root-slot patch still assigned; parent requested status
  and is ready to integrate. Remaining matrix unchanged, full goal active.
  No commit/push.

### Native64 root slots and compiled constants verified — 2026-09-04

- Previous turn was progress: retirement fix and full hook acceptance PASS.
  Integrated Luna0050 (string/class/methodtype root-slot addresses uint64,
  X-register literal loads) and parent0052 (boot-image object literals encoded
  as compressed payloads rather than truncated native pointers).
- Compiler staging includes patched headers and quoted-header consumer TUs to
  avoid mixing definitions. Compiler full rebuild106/106PASS; runtime/probe/link
  buildsPASS. Integration shell was briefly stopped until compiler completion
  and resumed afterward; all commands are now terminal exit0.
- Gate admits constant string/jumbo/class return methods; post-HIR verifies
  supported JIT root/referrer load kinds and rejects runtime/access/clinit paths.
  General resolving helper paths are NOT silently enabled. Added string,
  same-class, and other-class fixtures (DEX1019/1438); other-class avoids falsely
  testing only the referrer's ArtMethod declaring-class root instead of0050.
- /tmp/art-root-acceptance.log PASS: new compiled string/class root loads each
  with3GC cycles, existing JNI/field/exception suites and exit-hookGC. Compiler
  entry presence checked after each root call. Fmt/diffPASS. MethodType slot code
  is compiled but has no direct execution fixture yet; boot-image branch not
  separately proven by these app-class/unique-string fixtures. No moving-GC claim.
- Sol assigned next generic field-load/store/card-mark architecture review;
  next implementation expands heap access and general primitive control flow.
  Goal remains active, no original APK changes, no commit/push.

### Resolved typed fields and native card indexes verified — 2026-09-04

- Previous turn was progress: root literals integrated/tested. Luna0053 field
  stores now use RA-owned native receiver address for ordinary/release stores.
  Parent review fixed third-scratch exhaustion (reuse existing temp.X) and a
  duplicated card-byte load. Native full-width shifted address now indexes the
  biased card table in both marking and debug checking; compressed inputs remain.
- Generalized resolved single field get/set admission across instance/static,
  normal/volatile and Z/B/C/S/I/J/F/D/L with correct wide argument slot counts,
  opcode/type matching, register checks and static initialized-class guard.
  Unresolved/class-init/read-barrier paths remain pending, not silently enabled.
- Added72typed accessor fixtures and interpreter-then-compiled value checks,
  signed narrow extension, long/FP bits and GC after compiled stores. DEX counts
 1091/1510. Updated old volatile getter rejection to expected compilation.
- Compiler build2/104PASS; runtime1/252PASS; probe/linkPASS. Full acceptance
  /tmp/art-fields-acceptance.log exit0 including72typed accessors withGC, root
  literals and exit-hookGC. Fmt/diffPASS. No concurrent volatile publication test
  or independently cleared-card dirty transition evidence yet; don't infer
  those from the single-thread matrix. No original-app rerun.
- Sol assigned coherent array length/get/set/runtime-helper architecture review.
  Remaining full feature matrix active; no commit/push, no APK modifications.

### Array access implementation started — 2026-09-04

- Previous turn progress:72field accessors plus full regression PASS. Read latest
  ledger and assigned Luna0054 array length/get/primitive-set native-address
  backend patch. No reference-store typecheck bypass is authorized.
- Parent added27Java array fixture methods for9types (length/get/set); DEX
 1118/1537PASS. Added provisional simple-array admission for length/get and
  primitive set. Object set remains excluded until full compiler/stub conversion.
  These source changes are NOT yet linked/executed; backend/integration pending.
- Sol confirmed HIntermediateAddress is int32 non-root. Keep compressed interior
  offsets and restore native base only at final memory access, without adding
  data offset twice. Preserve DependsOnGC constraints and explicit null checks.
- Parent inspected art_quick_aput_obj: nested class/component dereferences,
  native assignability/exception C++ args, compressed payload store and native
  card index all require consistent adaptation. Slowpath argument decode alone
  is insufficient. Sol reviewing exact stub boundaries; parent owns this runtime
  work, Luna owns0054compiler patch. No new array support claim or test PASS.
- Next: finish0054 plus object-store compiler/stub conversion, add executable
  array acceptance including null/bounds/type exceptions and large indexes,
  then build/link/test. Full goal active; no commit/push.

### Typed array access and aput-object verified — 2026-09-04

- Previous turn was progress: fixtures and provisional primitive gate existed,
  but0054 was malformed and not registered in the actual compiler patch list.
  Rebuilt0054 from upstream context, registered it in bootstrap/graph inputs,
  and verified strict fuzz=0 application. JIT compiler rebuilt1/cached105.
- ARM64 array length/get/set now keeps HIR references compressed32 and restores
  native64 only in RA-owned final-memory temps. Reference type-check nested
  class/component/value dereferences use conditional RA temps; base-zero/RB
  configurations retain the original AOSP path rather than indexing absent
  Darwin temps. HIntermediateAddress remains compressed/int32.
- Added0055 for art_quick_aput_obj: saved managed roots remain compressed,
  C++ assignability/throw arguments are native pointers, stored payload stays
  W32, and card indexing uses the native array address. Tested exact fast store,
  null, slow successful CharSequence[]<-String, incompatible Hello[]<-Class
  ArrayStoreException, and preservation of the old element after failure.
- /tmp/art-array-final-acceptance.log exit0: all9 typed length/get/set methods
  compile, primitive compiled writes use values distinct from interpreter warmup,
  int index4097 crosses16KiB, null/bounds/type exceptions pass, and array_ref is
  re-derived from its JNI root after GC. Existing fields/roots/calls/exit-hook
  regression remains PASS. Probe/link closure also PASS.
- Sol review found no additional no-RB/CMS ABI or root defect. This does NOT
  prove Baker/read-barrier or moving-GC execution; gUseReadBarrier admission
  remains closed. Direct cleared-card->dirty evidence and negative setter bounds
  remain worthwhile coverage additions.
- Allocation work started by fixing the RosAlloc native64 free-list header leak:
  0055 now explicitly zeroes MIRROR_OBJECT_LOCK_WORD_OFFSET after writing the
  compressed class. Without it, native next-pointer upper32 (0x100) survives as
  a corrupt monitor word. runtime-arm64 rebuild and the full array regression
  pass; new-array/new-instance compiler admission is not implemented yet.
- Next: VisitNewArray/NewInstance decode native Class* immediately before the
  allocation entrypoint and encode its native Object* result immediately after;
  add resolved new-array fixtures across9types/zero/large/negative/GC. Then cover
  new-instance together with constructor direct-invoke. Full goal active; no
  original APK changes, no commit/push.

### Resolved new-array and LOS zero-on-reuse verified — 2026-09-04

- Previous goal turn was progress: typed array access and aput-object regression
  passed. This turn added0056: VisitNewArray/NewInstance decodes the Class input
  immediately before InvokeRuntime and encodes the native result immediately
  after it. Allocation helper native pointer/JValue contracts remain unchanged.
  Only verified simple resolved NEW_ARRAY methods are newly admitted; constructor
  instance invokes and new-instance admission remain pending.
- Added9Java typed allocation methods; button DEX1546PASS (baseline expected1127).
  Compiler compiled2/cached104. New acceptance checks sizes0/1/8193, runtime array
  type, distinct allocations, bytewise-zero primitive elements/null object
  elements, NegativeArraySizeException, and JNI-rooted objects across explicitGC.
- First run exposed a genuine LOS reuse failure: new int[8193] element4097 still
  held 0xabcdef93 from the preceding array-store test. FreeListSpace::Free used
  raw MADV_DONTNEED, which does not guarantee zeroing on Darwin.0057 uses AOSP's
  existing ZeroAndReleaseMemory host abstraction instead. Added source/patch to
  both shadow manifests and selected patched TU. Runtime compiled2/cached251.
- /tmp/art-newarray-final-acceptance.log exit0 after final bytewise-zero fixture
  rebuild: all9new-array types and prior fields/calls/roots/exit-hookGC regression
  PASS. Probe/linkPASS; fmt/diff checksPASS. Sol review of0056/0057 requested.
- Not yet proven: allocation-triggered (rather than explicit)GC, OOM, movingGC/
  read barriers, boot-image allocation classes, constructor direct invokes, or
  real apps. COMPUTE_ARRAY_SIZE_UNKNOWN still needs nested component reference
  decode for its generic TLAB path; known-size CMS fixtures do not prove it.
  Next: finish allocation helper variants and new-instance+constructor calls.
  Full goal active; no original APK changes, no commit/push.
- Follow-up Sol review: no critical blocker found in admitted new-array/no-RB
  path.0056 preserves call-site PC records and native allocation/deopt contracts;
  0057 zeroes via kMadviseZeroes=false before free-list publication. Missing
  custom/multidimensional arrays and allocation-internalGC/OOME remain explicit.
  Runtime shadow manifest unit test passed1/1; final fmt/diff checks passed.

### Allocation pressure, OOME recovery and component roots — 2026-09-04

- Continued active full-AOSP JIT goal; prior turn's new-array support was only
  resolved/no-RB allocation coverage, not full JIT completion.
- Added retained int[] pressure without explicit GC in the allocation interval.
  At the configured256MiB heap limit,251 live1MiB arrays survived9collections
  with distinct first/last markers intact. Expected OutOfMemoryError was caught;
  after releasing JNI roots and collecting, allocation succeeded with all bytes
  zero and the method still in the JIT code cache.
- Added Hello[] and int[][] outer-array fixtures. Interpreter-produced class is
  the differential type oracle; native-attached FindClass initially failed for
  the app class because it uses the boot loader without an app JNI caller.
  Fixed the test's loader assumption, not runtime class-loading semantics.
  Compiled allocations preserve type/null elements and class roots across GC.
- Button DEX1548/baseline1129 verified. Probe rebuild + fast graphics relink +
  full audit-art-jit passed: /tmp/art-alloc-types.log (exit0; combined run had
  251retained arrays/6collections). Prior standalone pressure run passed with
  9collections: /tmp/art-alloc-pressure.log. fmt/diff checks passed. No APK changes.
- Still pending: generic TLAB COMPUTE_ARRAY_SIZE_UNKNOWN nested component decode,
  new-instance/constructor invokes, read barriers/moving-GC variants, broader
  invocation/deopt/OSR/inlining and real-app unrestricted JIT acceptance. Nested
  fixture allocates only the outer array; it is not full multidimensional
  allocation coverage. Keep the full goal active; no commit/push this turn.

### Generic TLAB component-address repair — 2026-09-04

- Previous turn was progress: allocation-pressure and custom-component tests
  passed. This turn fixed the remaining known nested reference dereference in
  COMPUTE_ARRAY_SIZE_UNKNOWN, used by generic TLAB/region-TLAB array entrypoints.
  Incoming Class* remains native; its compressed component field is unpoisoned
  and decoded before reading primitive_type. Uses the macro's caller-owned
  xTemp1 scratch, without changing allocator return ABI or slow-path arguments.
- Added0058 and registered it in runtime_arm64. The production graph recursively
  includes patches/art, so the new patch also invalidates its runtime inputs.
  Runtime ARM64 rebuilt10objects; fast graphics relink passed.
- New tools/audit-allocation-size.py extracts the actual patched assembly macro
  and upstream unpoison macro, compiles them, and executes against native mapped
  class/component addresses in the high reference window (no MAP_FIXED).
  All4size shifts and7counts, including signed-negative bit patterns and uint32
  extrema, match uint64 aligned-size arithmetic with poisoning both off and on.
  /tmp/art-tlab-size.log PASS. This is executable macro coverage, NOT proof of
  complete TLAB allocation/refill/region collector/read-barrier integration.
- /tmp/art-tlab-regression.log exit0: full current JIT acceptance still passes,
  including custom/nested array class roots,251retained arrays/6collections/OOME
  recovery and compiled/native exit hooks. fmt/diff checks passed.
- Next: new-instance and constructor direct-invoke support, then allocator
  integration variants and the remaining full-goal features. Existing bytecode
  admission gate remains; no claim of full JIT/real-app completion. Goal active;
  no APK modifications, no commit/push.

### New-instance factory and interpreted constructor boundary — 2026-09-04

- Previous turn was progress: generic TLAB component-address repair passed.
  Added resolved NEW_INSTANCE + direct constructor + return factory admission,
  checking matching allocated/constructor classes, resolved initialized class,
  receiver register and forwarded argument words. No read-barrier variants yet.
- Post-HIR0045 now allows sharpened managed direct calls alongside static calls;
  still rejects native/intrinsic/unresolved/non-direct-address call shapes.
  Existing0056 handles native Class input and native allocator result encoding.
  Uses the AOSP constructor call generator and quick-to-interpreter bridge;
  constructors themselves remain excluded by the current method gate.
- Added Hello(int,Object) and a static allocation factory. Executed compiled
  factory with0/42/large markers, reference payloads, constructor field writes,
  explicit GC INSIDE constructor before writes (GC count asserted), and GC after
  return. Constructor throw and subsequent successful null-payload construction
  also pass. This proves compiled-factory/interpreted-constructor boundaries,
  not compiled constructors, all instance invokes, or full allocation support.
- DEX baseline1131/button1550 PASS. Compiler rebuilt2/cached104; actual runtime
  bootstrap jit.cc rebuilt1/cached252. build-runtime-core alone does not rebuild
  jit.cc; use graphics-bootstrap-internal for eligibility changes. An initial
  stale-DEX test failed before corrected DEX/runtime rebuild; not final evidence.
- /tmp/art-instance-final-test.log exit0: constructor GC/fields/roots/throw/
  recovery PASS, existing251root allocation pressure/OOME and full current JIT
  acceptance PASS. Probe/link/fmt/diff checks passed. Sol review requested.
- Next: compile constructor/instance method bodies and expand direct-invoke
  coverage, followed by unresolved/type-check/GC/OSR/deopt/inlining and actual
  unrestricted app acceptance. Full goal active; no APK changes or commit/push.
- Sol found no critical ABI/root/stack-map error for this boundary. Added its
  suggested assertions: factory entry remains JIT-resident after GC and exception
  recovery; constructor entry remains outside the JIT cache. Reviewed regression
  /tmp/art-instance-reviewed-test.log exit0 after probe rebuild/relink. Remaining
  constructor argument coverage includes primitive/wide/floating mixtures; current
  gate admits matching35c word layouts beyond the tested int/reference pair.

### Implicit receiver methods and compiled default constructor — 2026-09-04

- Previous turn was progress: compiled factory/interpreted constructor boundary
  passed. This turn counts the implicit receiver in DEX input words and admits
  nonstatic field accessors using the existing verified field-shape validator.
  A synthetic shorty exposes the receiver only to admission validation; AOSP
  graph building, HParameterValue and managed calling convention are unchanged.
- Compiled four real instance methods (int/reference get/set). The interpreted
  parameterized constructor calls compiled setters, including after its GC;
  JNI calls compiled getters after GC. Accessor entries remain JIT-resident.
  /tmp/art-instance-method-test.log exit0 before the next constructor expansion.
- Added noarg constructor admission for invoke-direct immediate-super noarg
  constructor followed by return-void. Existing post-HIR managed-direct/address
  checks remain. Hello.<init>() and jitNewEmptyInstance are both compiled; test
  checks zero fields/class/GC and both entries in code cache. This is a compiled
  constructor body, but not general constructors with stores/control flow.
- DEX baseline1136/button1555 PASS. Runtime bootstrap rebuilt1/cached252; probe
  and fast link passed. /tmp/art-ctor-test.log exit0: compiled factory->compiled
  empty constructor, instance accessors, parameterized interpreted constructor
  GC/throw/recovery,251root pressure/OOME and existing regression all PASS.
  fmt/diff checks passed. Sol review requested, no parallel build or edits.
- Remaining: general constructor bodies, other instance methods/dispatch, wider
  primitive constructor argument coverage, read barriers/movingGC/OSR/deopt/
  inlining and real apps without method-shape admission. Goal active. No APK
  edits or commit/push; next work should expand bodies beyond these shapes.
- Sol review found no critical admission/ABI defect: nonstatic branch cannot
  fall through into static shapes; synthetic shorty does not alter HIR/entry
  ABI. Confirmed compiled constructor PASS. Additional coverage needed for
  nonempty super constructors with GC/throw; current super is Object.<init>().

### Constructor store sequences and nonempty parent GC/throw — 2026-09-04

- Previous turn was progress: implicit-receiver accessors/default constructor
  compiled. Expanded constructor admission from an empty body to any sequence
  of resolved typed IPUTs on the receiver after immediate-super noarg invocation.
  Verified DEX/HIR retains register typing/access checks. AOSP code generation
  and constructor StoreStore fences are unchanged; no custom constructor logic.
- Added compiled Hello(long,double) and factory. High64/negative long values,
  double fractional value and negative-zero exact bits survive stores and GC.
  /tmp/art-ctor-stores-test.log exit0 before adding nonempty parent behavior.
- Hello now extends test-only JitConstructorParent: parent calls System.gc(),
  sets a marker and self-reference, optionally throws. Compiled child default
  constructor/factory checks actual GC progress, receiver self identity, parent
  field writes, throw across compiled frames and subsequent recovery. Wide
  constructor additionally keeps J/D arguments live across parent GC before
  storing them. Parameterized int/Object constructor remains interpreted.
- Added the parent .class explicitly to both D8 input lists (initial build
  exposed missing parent class despite javac generating it). Final DEX baseline
 37classes/1139methods and button94classes/1558methods verified. Runtime bootstrap
  rebuilt1/cached252 for gate changes; final probe/fast graphics relink passed.
- /tmp/art-ctor-parent-test.log exit0: parent GC/receiver root/throw/recovery,
  compiled constructor long/double stores/GC, existing JIT regression and OOME
  recovery PASS. fmt/diff checks passed. Sol found no critical admission/ABI or
  constructor-fence defect; parent execution completed after that source review.
- Important next coverage: ordinary object parameter live across parent GC into
  a compiled constructor's final reference field (not only Class roots or J/D),
  then broader constructor bodies/control flow and remaining full-goal features.
  Method-shape restrictions remain; no claim of full JIT or actual-app success.
  Full goal active, no original APK edits, no commit/push.

### Compiled final reference store across parent GC — 2026-09-04

- Previous turn was progress: constructor store sequences/nonempty parent
  GC/throw passed. Added JitFinalReference(Object) with a final Object field and
  a compiled allocation factory; included its class in both D8 input lists.
- Initial compilation rejected the newly loaded class. Diagnostic BEFORE
  CompileMethod showed eligible0/visible0/verified1/compilable1; parent and field
  were resolved and bytecode matched the admitted shape. Sol diagnosed normal
  ARM64 batch visibility publication. The test now runs the existing AOSP
  MakeInitializedClassesVisiblyInitialized(self,true) callback outside mutator
  lock before explicit compilation, as the top-level acceptance already does.
  No gate bypass, manual class-status changes or interpreter substitution.
- Compiled constructor holds an ordinary newly allocated int[] parameter across
  parent System.gc(), then stores it in the final field. Test checks identity,
  deletes all payload JNI refs, runs another GC with only the holder rooted,
  reads the field and verifies array contents. Null payload also passes; both
  factory/constructor code-cache entries remain compiled after the sequence.
- /tmp/art-finalref-published-test.log exit0: ordinary final ref/parentGC/sole-
  edgeGC/null PASS; full current regression and251root OOME/recovery PASS.
  Baseline DEX38classes1141methods and button95classes1560methods verified;
  probe rebuild/fast link/fmt/diff checks passed. Temporary diagnostics removed.
- This covers final reference retention under the current CMS configuration,
  not moving-GC/read barriers or cross-thread Java final-field publication.
  Next expand constructor constants/control flow and remaining instruction/call
  coverage; the full goal and eventual method-shape gate removal remain active.
  No original APK changes, no commit/push.

### Constructor numeric/null literal initialization — 2026-09-04

- Previous turn was progress: ordinary final reference survives parent GC and
  sole-edge GC. Expanded constructor body admission to CONST_4 through
  CONST_WIDE_HIGH16. These remain ordinary AOSP HConstant values; verified
  register/HIR typing determines integer/floating/null uses. Existing parent
  call, resolved stores and constructor fence paths remain unchanged.
- JitFinalReference now initializes11additional final fields: small/medium/full/
  high32, small/medium/full/high64, float/double negative zero and null. JNI reads
  actual fields after compiled construction and GC, checking exact integers and
  floating bits. Bytecode inspection asserts all8numeric literal opcode forms
  are present, so values alone cannot hide missing DEX-form coverage.
- Baseline38classes1141methods/button95classes1560methods unchanged and verified.
  Runtime bootstrap rebuilt1/cached252; probe and fast graphics relink passed.
  /tmp/art-ctor-constants-final-test.log exit0:8DEX forms, all stored constants,
  final reference/parentGC/sole-edgeGC,251root OOME recovery and prior regression
  PASS. fmt/diff checks passed; no debug-only runtime toggles introduced.
- Still pending: general constructor moves/arithmetic/control flow and broader
  instruction/call/GC/deopt/OSR/inlining support, cross-thread final publication,
  read barriers/movingGC and actual apps without method-shape restrictions.
  Full goal remains active; no original APK edits, no commit/push.

### Constructor conditionals, loops and int operations — 2026-09-04

- Previous turn was progress:8numeric literal forms and stored values passed.
  Expanded constructor-body scanning to moves, nondiv/rem int operations,
  IF and GOTO variants. Checks all instructions even after a return; branch
  targets must stay after the initial super call and hit a DEX boundary.
  Existing verified typing, resolved stores and AOSP constructor fences remain.
- Final-reference constructor now selects initial values/loop limit by nullness
  and computes a wrapping multiply/xor accumulation with0or37iterations before
  storing final int/reference fields. Both outcomes match an independent
  unsigned32 C++ calculation and preserve ordinary payload across parent/sole-
  edgeGC. Test asserts conditional/backedge bytecodes and8literal forms exist.
- DEX counts unchanged (38/1141baseline,95/1560button), build verified. Runtime
  bootstrap/probe/fast relink passed. /tmp/art-ctor-loop-final-test.log exit0:
  reference conditions/zero+nonzero loop/wrapping arithmetic, prior final-field
  and allocation/OOME regression PASS. fmt/diff checks passed.
- Sol review found no critical CFG/reference/root defect: reference EQ/NE/null
  compare compressed32; verifier excludes reference ordering; suspend frames
  retain compressed roots with existing visitor contract. All-return constructor
  fences stay AOSP. No new native reference conversion was needed here.
- Required next evidence: a long dynamic compiled loop must handle another ART
  thread's GC/checkpoint at an actual loop dexPC, then preserve receiver/payload/
  computation. A37iteration constant loop can be optimized/unrolled and does NOT
  prove a pending backedge suspend slowpath. Other full-goal gaps remain active;
  no unrestricted-app/full-JIT completion claim. No APK edits, commit or push.

### Dynamic compiled loop checkpoint and worker GC — 2026-09-04

- Previous turn was progress: constructor CFG/short-loop results passed, but
  pending suspend remained unproven. Made final-reference constructor/factory
  take a runtime loop bound; preserved0/37case checks and added500million loop.
- New runtime_jit_loop_checkpoint.h attaches a Native ART worker. It requests
  one checkpoint at a time under thread_suspend_count_lock_, waits for atomic
  completion, runs GC outside the callback/locks, then observes again. Closure
  lives through join; main transitions Native before joining to drain pending
  callbacks. Retry deadline is10seconds, not a hard queued-callback timeout.
- Callback inspects only the first non-runtime managed frame: required ctor,
  not ShadowFrame, optimized OatQuickMethodHeader, native PC inside JIT cache,
  and dexPC within actual backward-branch interval. Parent System.gc() frames
  cannot pass this check. Callback only observes; it never requests GC/suspension.
- /tmp/art-loop-checkpoint-final-test.log exit0: dexPC60 before/after at native
 0x11aa15b4c, one worker GC completed between observations,500million operations
  match independent unsigned32 computation, receiver/payload state and both JIT
  entries preserved. Full current acceptance/OOME regression also passed.
  Repeat1/2 logs under /tmp/art-loop-checkpoint-repeat-{1,2}.log both exit0 and
  repeat dexPC60/oneGC at their own optimized JIT addresses. DEX counts unchanged
  and verified; probe/link/fmt/diff checks passed.
- Sol found no critical race/lifetime/lock defect. Clarified callback epilogue
  may still finish after publishing done; GC waits for ordinary safepoints.
  Payload JNI local remains alive, so this does not prove sole compiled-spill
  rooting or moving-GC/read-barrier correctness. Current proof is actual pending
  checkpoint + CMS during a compiled loop, not merely between JNI calls.
- Next: expand remaining dispatch/arithmetic/type/exception paths and GC modes
  toward full pinned AOSP behavior and eventual gate removal. Goal remains
  active; no original APK changes, commit or push.

### Virtual vtable dispatch with compressed receiver/class — 2026-09-04

- Previous turn was progress: real compiled-loop checkpoint/CMS passed. Added
  0059 to GenerateVirtualCall: decode managed receiver into existing x0 temp
  for its class load, keep loaded/unpoisoned class compressed through inline
  cache processing, then decode it for the native64 embedded vtable ArtMethod*
  load. Receiver w1 and other managed arguments/results remain compressed.
- Registered patch in bootstrap compiler and graph inputs. Post-HIR0045 admits
  resolved nonintrinsic virtual invokes without read barriers; method-shape
  admission accepts forwarded invokevirtual result calls. Explicit null checks,
  RecordPcInfo, native ArtMethod*/quick entries and generic bridges remain AOSP.
- Added nonfinal Base/Child with real int/reference overrides. Both are loaded
  before caller compilation. Verified interpreted callers, compiled callers
  with interpreted targets, then compiled callers/targets;3GC cycles, distinct
  override results, null receiver NPE+recovery and nonnull receiver/null result.
  All6compiled entries remain resident. Machine-word audit verifies actual
  receiver-class LDR W0 and native vtable LDR X0 in each caller, not only a
  semantically equivalent sharpened direct call.
- /tmp/art-virtual-code-test.log exit0: ARM64 vtable loads/dispatch PASS and
  existing loop-checkpoint/constructor/arrays/OOME regression PASS. DEX baseline
 40classes1151methods/button97classes1570methods verified. Compiler3rebuilt/103
  cached, runtime bootstrap1rebuilt/252cached, probe/link/fmt/diff passed.
- Sol found no critical register/cache/return-ABI defect: x0 temp cannot alias
  w1 receiver; inline-cache ASM updates compressed root slots without C++ calls;
  class decode belongs after that helper. Null-result and machine-code checks
  were added following its review.
- Remaining virtual coverage includes baseline inline-cache execution, wide/
  mixed args, callee-internalGC/throw, native override and read-barrier variants;
  interface/super/unresolved dispatch and the full-goal remainder still pending.
  Goal active, no original APK modifications, no commit/push.

### Baseline virtual execution and profiling limitation — 2026-09-04

- Added separate baseline int/reference virtual wrappers, before optimized
  coverage. Verified CodeInfo baseline flag and actual receiver/vtable loads;
  receiver overrides, compiled targets, GC/null/recovery checks pass in both
  modes. DEX now baseline40classes1153methods/button97classes1572methods.
- Initial strict empty-cache assertion failed (status134): no profiling cache
  available. AOSP HInliner returns immediately with InlineMaxCodeUnits=0;
  ProfilingInfoBuilder requires IsUsefulOptimizing. Darwin0040 currently forces
  this option off. Baseline execution is NOT inline-cache execution evidence.
  Test explicitly reports NOT covered, retaining mono/poly assertions if caches
  become available; do not count this as full baseline/profile support.
- Final /tmp/art-baseline-vcall-test.log exit0; probe build, fast link, fmt and
  diff checks passed. No APK edits or commit/push. Goal remains active.
- Next: safely restore AOSP inliner/profiling behavior with deopt/stack-map ABI
  validation, then require empty->mono->poly cache execution. Wide/mixed virtual
  args, interface/native/unresolved dispatch and prior full-goal gaps remain.

### Restore AOSP inliner scheduling and real baseline profiling — 2026-09-04

- Previous turn made progress by exposing missing profiling under forced
  InlineMaxCodeUnits=0. Removed that global override from0040. First actual
  relink failed new-instance compilation: inlining pulled unsupported parent
  constructor code into the graph; do not confuse the earlier stale-link PASS
  with new compiler evidence.
- Added0060: TryBuildAndInline checks existing callee capability before
  intrinsic re-recognition/pattern substitution. Unsupported bodies remain
  normal calls; supported bodies use AOSP inliner and profiling scheduling.
  This is a temporary capability gate, not completion or an APK workaround.
  Staged inliner.cc and registered build-graph patch inputs.
- Sol review identified generated ClassTableGet compressed-class dereference
  and ClearException native pointer width. Decode class into native output
  register before vtable/IMT lookup; clear full64-bit TLS exception with XZR.
  Build passes; these specific paths still need dedicated execution tests.
- Baseline virtual test now REQUIRES caches (removed missing-cache skip).
  Final /tmp/art-inliner-test.log exit0: initial0/0 -> mono1/1 -> poly/GC PASS
  for int/reference wrappers. Existing allocation/constructor/loop checkpoint
  and pressure/OOME recovery pass (251 retained arrays,7collections). Compiler,
  probe, actual fast relink, cargo fmt and diff checks pass. DEX unchanged.
- Next: prove actual inlined bodies and speculative guard failure/deopt with
  nested reference environments, then widen body capability and remove gate.
  Throw/monitor/type/clinit/vector/string backend boundaries remain unported;
  no claim of full inlining/OSR/moving-GC or100% JIT support. Goal active.
  No original APK edits, commit or push.

### Machine evidence for inlined reference identity — 2026-09-04

- Previous turn restored inliner scheduling and actual baseline profiling.
  Added explicit machine evidence to existing jitCallRef acceptance: count
  ARM64 BL/BLR instructions over complete quick code. Standalone identity and
  wrapper must both have zero, while non-inlineable jitCallGc must retain a
  call. Observed0/0/1; nullable wrapper execution already runs before and after
  compiling the standalone target. This proves elimination rather than merely
  equivalent forwarding results (including AOSP pattern substitution).
- Added ordinary int-array payload identity/content checks through wrapper
  after3explicit GCs with JIT entry residency. JNI payload root remains live;
  GC is between calls, not inside an inlined frame. No deopt or sole spill-root
  evidence is claimed from this test.
- Final /tmp/art-inline-proof-test.log exit0: inline identity machine/runtime
  PASS, baseline caches PASS,500million compiled-loop worker checkpoint/GC PASS
  and full existing acceptance finishes launcher ok. Probe rebuild, actual
  fast relink, fmt and diff checks pass; no DEX changes.
- Next: actual speculative virtual guard failure and nested environment deopt
  reconstruction, not another identity-only claim. Remaining full-goal gaps
  and temporary capability gates persist. Goal active; no APK edits/commit/push.

### Speculative guard miss and nested reference deoptimization — 2026-09-04

- Previous turn proved identity call elimination. Added separate int/reference
  virtual wrappers: resolve, baseline compile, empty cache, Base-only call gives
  mono cache, optimize same method and require new non-baseline JIT entry.
  Matching receiver causes no deopt; Child override returns correct value and
  increments actual runtime deopt counter exactly once. Follow-up calls and GC
  recovery preserve both override results. Both classes loaded before compile.
- Reference variant compiles an outer static wrapper around profiled inner.
  Requires InlineInfo identifying inner ArtMethod and dexPC0 under outer dexPC0;
  invokes the outer for miss/result checks. DEX now40classes1156methods and
  button97classes1575methods, both verified. No original APK changes.
- Sol correctly noted generic HasInlineInfo alone does not prove failed guard
  environment. Enabled existing AOSP deopt verbosity alongside opt-in
  DARWIN_ART_JIT_TRACE at startup (normal logging unchanged). Final trace
  /tmp/art-speculative-trace.log exit0, lines533-550: stack depth1 inner
  jitSpeculativeVirtualReference, depth2 outer jitNestedSpeculativeReference;
  Single-frame deopting outer due to JIT inline cache. Counter int0->1,
  nestedref1->2 with correct returned object. This observes actual nested
  frame reconstruction, not just static metadata.
- Nontrace /tmp/art-speculative-test.log also exit0. Probe/DEX/fast relink,
  bootstrap, fmt/diff and existing full acceptance pass. Subsequent recovery
  may use interpreter (AOSP deopt); not proof of persistent optimized code.
- Remaining: mixed/wide locals and deeper nested environments, moving GC/RB,
  OSR, unsupported node/call coverage and full app proof. Current nested test
  carries one receiver reference; do not generalize to all environment shapes.
  Goal active; no commit/push.

### Range managed calls and mixed stack/register ABI — 2026-09-04

- Previous turn proved nested reference deopt. Mixed/wide extension was
  blocked by signature length8 and static-call I/L-only admission. Removed
  arbitrary signature cap; static forwarding now handles wide results and
  both35c/3rc invoke encodings. Virtual forwarding likewise accepts3rc. Valid
  input-word count/sequential argument mapping and resolved target checks stay.
  This expands a temporary gate; it does not remove all method-shape limits.
- New jitRangeCall uses seven ints then long,double,Object (12DEX inputwords),
  forces INVOKE_STATIC_RANGE (asserted against actualDEX), and calls interpreter
  target running System.gc(). Requires exact positive/negative64-bit result,
  FP argument, null/non-null reference and independent negative test for each
  int and FP argument. Wrapper JIT residency checked after repeatedcalleeGC.
- /tmp/art-range-test.log final exit0: mixed stack/register range PASS, prior
  baseline/profile/nesteddeopt/loopGC/fullacceptance PASS. DEX40classes1158methods,
  button97classes1577methods verified. Compiler2rebuilt104cached; bootstrap
 1rebuilt252cached; probe/fastlink/fmt/diff passed.
- Wide/mixed deopt environment still requires a dedicated case; this test is
  normal static managed calling ABI with calleeGC, not deopt or movingGC proof.
  Virtual RANGE is admitted but awaits dedicated execution coverage. Next use
  widened signatures for virtual mixed arguments and nested environment tests.
  Full goal remains active; no APK edits/commit/push.

### Virtual RANGE mixed ABI execution — 2026-09-04

- Previous turn expanded RANGE admission and tested static calls. Added
  jitVirtualRange and distinct Base/Child overrides. ActualDEX must start
  INVOKE_VIRTUAL_RANGE. Receiver plus seven ints,long,double,Object spans
  managed integer registers/stack and floating register. Both targets call
  GC-bearing helper; child negates the long result so wrong dispatch fails.
- Interpreter warm then optimized wrapper calls both receiver classes across
  repeated calleeGC, alternating positive/negative64-bit values. Null receiver
  raises NPE; following calls recover, wrapper JIT entry remains resident.
  Targets remain interpreted; this is not compiled wide override body proof.
- /tmp/art-vrange-test.log exit0: virtual range mixed ABI PASS and existing full
  acceptance finishes launcher ok. DEX40classes1161methods/button97classes1580
  verified; probe rebuild, actual fastlink, fmt/diff passed. Runtime unchanged
  because previous RANGE backend/admission handled this case correctly.
- Next: mixed/wide nested deopt environments and widening instance body/type/
  arithmetic coverage. Current test proves normal virtual RANGE calling with
  GC, not deopt or moving collectors. Full goal active; no APK edits/commit/push.

### Nested mixed/wide environment reconstruction — 2026-09-04

- Previous turn established virtual RANGE ABI. Added typed parameter-identity
  admission for one-instruction returns: validate parameter start/type and
  wide pair, including instance receiver and stack parameters. No custom
  lowering; normal AOSP inliner handles the admitted long identity body.
- New Base.wideSpeculative returns long argument; Child validates reference
  identity against this, then all seven ints/FP via GC-bearing helper and
  negates the64-bit value. Baseline Base-only cache feeds optimized nested
  outer wrapper. Matching Base causes no deopt; Child causes exact+1 and
  correct long result after reconstruction/GC.
- Final /tmp/art-wide-deopt-test.log exit0: counter3->4. AOSP trace shows inner
  jitWideSpeculative and outer jitNestedWideSpeculative reconstruction, with
  Single-frame deopting outer due to JIT inline cache. Final DEX includes
  receiver identity check, not only non-null check.40classes1165methods and
  button97classes1584methods verified. Compiler2rebuilt104cached, bootstrap
 1rebuilt252cached, probe/fastlink and fmt/diff pass; full acceptance passes.
- This covers one mixed argument environment (register/stack long,double,
  ints,receiver,Object) under current CMS. It does not cover all local/constant
  environment forms, moving GC, OSR, or full backend. Next expand remaining
  arithmetic/type/call nodes and OSR/GC modes; full goal active, no APK edits
  or commit/push.

### Long and floating scalar arithmetic differential — 2026-09-04

- Previous turn proved contended monitor exception cleanup. Expanded numeric
  straight-line admission from int-only to IJFD signatures, wide returns/moves/
  constants and long,float,double add/sub/mul/div/rem (normal/2addr), negation.
  Uses existing AOSP ARM64 instructions and fmod runtime helpers, no custom
  arithmetic implementation. Remaining gates and unlisted opcodes persist.
- Added3numeric expression fixtures and separate numeric acceptance header.
  Save results while method has no JIT entry; then explicitly compile and
  compare identical input grid. Long64cases; float/double121each. Tests include
  ±0, signed values, extrema, infinities, NaN, subnormal. Compare exact bits
  except NaN payloads, match ArithmeticException for long divide-by-zero.
- /tmp/art-numeric-test.log exit0: all306differential cases and existing full
  acceptance pass. DEX40classes1178methods/button97classes1597verified. Compiler
 2rebuilt104cached; runtime/probe/actual fastlink/fmt/diff pass. No APK edits.
- Compound expressions do not isolate every instruction corner (e.g. direct
  MIN/-1 division). Need dedicated per-op tests, conversions/compare/bitwise,
  then remaining backend/type/call/OSR/GC coverage. Not100% numeric or overall
  support. Goal active; no commit/push.

### Explicit compiled throw to AOSP exception delivery — 2026-09-04

- Previous turn proved mixed nested deopt. Ported HThrow boundary in0061:
  nullable compressed reference in runtime argument register becomes native
  Throwable* before kQuickDeliverException. AOSP delivery/stackmap/unwind
  remains unchanged; null stays native null so upstream synthesizes NPE.
  Registered patch in compiler build and graph inputs.
- Added admission for static one-instruction THROW of its Throwable input,
  and jitThrow/jitCatchThrow fixtures. Compiled throw delivers exact same
  exception to JNI and interpreted Java catch, null produces NPE, repeated
  calls across3GCs preserve behavior and JIT entry residency.
- /tmp/art-throw-test.log exit0: explicit throw exact-object/Java catch/null/GC
  PASS and full existing acceptance passes. DEX40classes1167methods/button
 97classes1586methods verified. Compiler/runtime/probe/actual fastlink builds,
  fmt and diff checks pass. No APK edits/commit/push.
- Still not compiled catch/finally/monitor-unwind coverage; those require
  further admission/backend work. Continue remaining type/arithmetic/call
  and OSR/GC paths. Full goal remains active, not100% complete.

### Compiled catch handler and exception slot boundary — 2026-09-04

- Previous turn ported explicit throw; this turn permits verified static LL
  try/catch bodies consisting of reference moves/returns/throw and resolved VL
  same-class calls. Other try bodies remain rejected until their nodes are
  ported. This is capability expansion, not a claim of general EH completion.
- VisitLoadException now explicitly loads native64 TLS exception and encodes
  managed reference via shared helper. Existing0060 ClearException clears the
  complete native64 slot. Delivery/unwind/catch selection remains AOSP.
- jitCatchReturn calls throwing method inside Throwable handler. Compile JIT,
  REQUIRE actual Catch-kind StackMap, assert exact incoming exception returned
  or generated NPE for null, no pending exception after return,3GC cycles and
  resident JIT entry. Does not count an eliminated catch as coverage.
- /tmp/art-catch-test.log final exit0: compiledcatch and existing full suite
  PASS. DEX40classes1168methods/button97classes1587verified, compiler3rebuilt103
  cached, runtime/probe/fastlink/fmt/diff pass. No APK edits/commit/push.
- Remaining EH: typed handler selection, finally/rethrow, monitor unwind,
  exceptional edges with mixed locals and movingGC. Other full-goal gaps also
  remain; continue capability work and eventually remove gates. Goal active.

### Typed compiled handler selection and unmatched propagation — 2026-09-04

- Previous turn proved general Throwable compiledcatch. Added jitTypedCatch:
  IllegalArgumentException returns same object, IllegalStateException returns
  null, other exceptions escape. Requires at least2Catch-kind stackmaps and
  resident optimized JIT method, not interpreter-only behavior.
- Tested direct IllegalArgumentException and NumberFormatException subclass,
  distinct IllegalState handler, exact IOException object propagation to JNI,
  and null throw producing unmatched NPE. Each object case repeats3times with
  GC; exception state is checked/cleared between calls.
- /tmp/art-typed-catch-test.log exit0 typedhandler PASS and full acceptance
  finishes launcher ok. DEX40classes1169methods/button97classes1588 verified;
  probe/fastlink/fmt/diff pass. No runtime change required: preceding compiled
  EH boundary patches plus AOSP handler selection handled these cases.
- Next: finally/rethrow and monitor unwind, plus remaining backend and OSR/GC
  work. Not all exception shapes proven. Goal active; no APK edits/commit/push.

### Compiled finally, rethrow and cleanup exception precedence — 2026-09-04

- Previous turn proved typed handlers. Added jitFinally around a maybe-throw
  call and GC-bearing cleanup. Requires resident JIT and Catch-kind stackmap.
  For normal and exceptional paths,3alternating cycles assert exactly1cleanup
  and original reference seen after GC; exceptional path rethrows same object.
- Cleanup can itself throw a distinct replacement object. Tested both normal
  return and original-exception paths: replacement escapes, cleanup still
  exactly once. All test static roots cleared afterward. Helpers interpreted;
  finally method itself compiled. JNI original/replacement roots remain live.
- /tmp/art-finally-test.log final exit0: normal/exception cleanup/rethrow and
  replacement-precedence PASS; full acceptance finishes launcher ok. DEX
 40classes1172methods/button97classes1591verified. Probe/actual fastlink/fmt/diff
  pass. Existing EH implementation handled these cases without runtime edits.
- Next: monitor operations and exceptional unlock, other backend nodes and
  OSR/GC coverage. This is not sole-spill rooting or moving-GC proof. Full goal
  remains active, no original APK edits or commit/push.

### Compiled monitor boundary and exceptional unlock — 2026-09-04

- Previous turn proved finally semantics.0062 decodes nullable managed x0
  into nativeObject* immediately before existing quickLock/Unlock calls.
  No assembly/host lock substitution. Sol confirmed native argument contract,
 32-bit atomic lockword path, null fallback and rooted slowpath; no return
  encoding because monitor operation returns void. Patch registered in graph.
- Try-body gate now admits MONITOR_ENTER/EXIT. jitMonitor synchronized block
  calls helper checking Thread.holdsLock, collecting GC, optionally throwing.
  Verify same object result/exception and owner0after each normal/exception
  return. JNI-held outer lock tests recursion; identityHashCode while held
  forces inflation and explicit LockWord::kFatLocked assertion. Compiled exit
  preserves outer ownership; JNI exit leaves owner0. Null produces NPE.
- Final /tmp/art-monitor-test.log exit0 with inflated recursion PASS; full
  acceptance completes launcher ok. DEX40classes1175methods/button97classes1594
  verified (includes Thread.holdsLock reference). Compiler3rebuilt103cached,
  runtime/probe/actual fastlink/fmt/diff pass.
- Still need cross-thread contention, blocking GC, thin-recursion-specific
  and synchronized-method (implicit monitor) coverage. Current test is an
  explicit synchronized block; do not claim monitor implementation complete.
  Full goal active; no APK edits/commit/push.

### Cross-thread compiled monitor contention and blocked GC — 2026-09-04

- Previous turn ported monitor boundary. Added runtime_jit_monitor_contention.h:
  main owns JNI monitor; attached contender invokes compiled jitMonitor and
  publishes Thread*. Main waits Native for actual kBlocked, verifies pending
  monitor object, collects GC while still owner, requires blocked/notdone after
  GC, releases, joins Native, requires exact return and final owner0.
- Thread remains attached until retire; globalrefs survive join. Sol reviewed
  ordering: GetState is relaxed, so recheck kBlocked under SOA then acquire
  fence before pending object read. No Runnable busywait or holding ART locks
  through GC/join.10second observation deadline still cleans up/join, not a
  hard thread cancellation timeout.
- Initial worker failed after handoff: internal attach create_peer=false was
  inappropriate for Java execution. Normal create_peer=true fixes it without
  changing runtime monitor behavior; test now creates ordinary Java Thread.
- Final /tmp/art-monitor-contention-test.log and repeat.log both exit0, each
  blocked=1 object=1 gc=1 stayed=1 worker=1 released=1; entire regression passes.
  Probe/actual fastlink/fmt/diff pass. DEX unchanged. Globalroots remain live;
  no sole-spill/moving-GC claim. Current contention uses existing inflated lock.
- Remaining monitor work includes contention+exception, thin inflation race,
  implicit synchronized methods and stronger concurrent stress, alongside all
  other full-goal gaps. Goal active; no APK edits/commit/push.

### Contended thin inflation and exceptional monitor release — 2026-09-04

- Previous turn proved inflated contention. Generalized worker result check
  for expected exact Throwable propagation. Fresh lock cases REQUIRE initial
  kThinLocked after main acquisition, then kFatLocked while worker blocked.
  Main still owns lock during GC; after handoff worker executes GC-bearing
  synchronized block and either returns or throws exact original object.
- /tmp/art-monitor-exception-test.log and repeat.log both exit0. Three cases:
  existing inflated normal; thin->inflated normal; thin->inflated exceptional.
  Each has blocked/object/gc/stayed/worker/released=1; thin cases initialthin=1.
  Confirms contended inflation and catchall unlock, not just free-lock entry.
- Probe rebuild/actual fastlink/fmt/diff and full suite pass. No DEX or runtime
  changes needed; existing AOSP monitor paths handle these tests. Globalroots
  remain, no movingGC/sole-spill proof. Stronger stress/implicit synchronized
  coverage and other backend/OSR/GC gaps remain. Full goal active, no APK edits
  or commit/push.

### Direct division and remainder edge cases — 2026-09-04

- Previous numeric progress is recorded under Long and floating scalar
  arithmetic differential (earlier in this file). This turn adds six isolated
  long/float/double division and remainder methods to avoid compound-expression
  masking. Interpreter results collected before JIT compilation, then compared
  against resident compiled methods on identical boundary grids.
- Direct long MIN/-1, zero denominator exception, sign combinations; FP
  ±0/infinity/NaN/subnormal/extrema tested separately for div/rem. Exact bits
  compared except NaN payload; exception types checked. No backend fix needed.
- /tmp/art-divrem-test.log exit0:918numeric cases across9methods (612new isolated
  cases), plus complete existing acceptance. DEX40classes1184methods/button
 97classes1603verified; probe/actual fastlink/fmt/diff pass.
- Remaining numeric work: conversions, comparisons, bitwise/shifts and other
  instruction-specific coverage, then broader backend/type/call/OSR/GC work.
  Full goal active; no original APK edits/commit/push.

### All IJFD scalar conversion directions — 2026-09-04

- Previous turn isolated div/rem. Added12conversion opcodes to straight-line
  numeric admission and12single-cast Java fixtures: every distinct int,long,
  float,double direction. Existing AOSP lowering handles register class moves,
  truncation/rounding and saturating FP->integer behavior.
- CheckJitConversions saves interpreter output before compiling each method,
  requires JIT entry, compares exact non-NaN result bits (NaN class only).
 162cases include signed zeros, fractions, integer limits and adjacentFP
  boundaries, infinities/NaN/subnormal/max finite. No native C++ FP->integer
  cast is used to manufacture the oracle.
- /tmp/art-convert-test.log exit0 all12directions/162cases PASS, plus918numeric
  cases and full regression. DEX40classes1196methods/button97classes1615verified;
  compiler/runtime/probe/actual fastlink/fmt/diff pass. No backend correction
  required beyond capability admission for these operations.
- Narrow byte/char/short conversion, comparisons, bitwise/shifts and broader
  backend/type/call/OSR/GC requirements remain. Goal active; no APK edits,
  commit or push.

### Byte/char/short narrowing and typed return — 2026-09-04

- Previous turn covered12IJFD conversion directions. Added INT_TO_BYTE/CHAR/
  SHORT admission and narrow primitive signatures. Three Java fixtures return
  actual byte,char,short, exercising the corresponding JNI return interfaces.
-60boundary cases compare interpreter output, compiled output and independent
  low-bit masking/sign-extension arithmetic. Includes ±128/256, ±32768/65536,
  intmin/max and mixed-bit patterns. Requires initially uncompiled then JIT
  resident method. Existing AOSP lowering handles these cases without patches.
- /tmp/art-narrow-test.log exit0: narrowing60PASS, conversions162 and numeric918
  plus full regression pass. DEX40classes1199methods/button97classes1618verified;
  compiler/runtime/probe/actual fastlink/fmt/diff pass.
- Still comparisons, bitwise/shifts, control flow and broad backend/type/call/
  OSR/GC requirements remaining. Goal active, no APK edits/commit/push.

### Long bitwise and masked shifts — 2026-09-04

- Added long AND/OR/XOR/SHL/SHR/USHR admission (normal and 2addr forms),
  six Java fixtures and CheckJitBits. Existing AOSP lowering is retained.
-420 cases compare initially interpreted execution, explicitly compiled JIT
  execution and independent unsigned-bit arithmetic. Includes negative values,
  extrema, mixed bits and shift counts -65/-64/-1/63/64/65/127/int extrema.
  Shift oracle masks to six bits and explicitly constructs arithmetic sign fill.
- /tmp/art-bits-test.log and repeat show420PASS; repeat audit exited0 with full
  prior regression. Compiler/runtime/probe/actual fastlink built; DEX40classes
  1205methods/button97classes1624verified; fmt and diff checks pass.
- These tests do not prove every opcode encoding or complete JIT support.
  Comparisons, broader control flow/type/call/OSR/GC work and actual app
  acceptance remain; full goal active, no APK edits/commit/push.

### Primitive comparisons and forward branches — 2026-09-04

- Previous turn made verified progress with420long bitwise/shift cases.
  Numeric admission now includes CMP_LONG/CMPL/G_FLOAT/DOUBLE and forward
  IF/GOTO forms. Numeric scans permit multiple return blocks; reference
  identity restrictions remain. Verified-class requirement remains in place.
  Backedges remain restricted pending broader loop/OSR work.
- Four Java fixtures accumulate all six relation results for IJFD.450operand
  pairs compare interpreter-before-JIT, resident optimized code and independent
  native comparison masks. Includes signed extrema, NaN/unordered, infinities,
  signed zeros and subnormals. Existing AOSP lowering needed no backend patch.
- /tmp/art-compare-test.log: comparisons450PASS and full regression exit0.
  Compiler/runtime/probe/actual link pass; DEX40classes1209methods and button
  97classes1628verified; cargo fmt and git diff checks pass.
- This is not all control-flow coverage: switches, broader loops/OSR, type/call/
  GC paths and actual application acceptance remain. Full goal stays active.
  No APK edits, commit or push.

### Int division/remainder and compiled exception recovery — 2026-09-04

- Previous turn verified primitive comparisons/forward branches. Enabled int
  DIV/REM normal, 2addr and literal forms using existing AOSP lowering.
  Added six fixtures: variable divisor, literal7 and literal-1 div/rem.
  Variable and literal7 tests assert appropriate DEX operation presence;
  no claim that every encoding or literal-1 lowering is independently exercised.
-726 invocations compare pre-JIT interpreter results, resident optimized code
  and widened int64 arithmetic (avoids native int MIN/-1 undefined behavior).
 286 distinct effective operand cases; constant fixtures repeat unused second
  argument values. Checks zero divisor ArithmeticException, extrema and signs.
- First audit failed status87 because old regression required JIT rejection of
  jitDivide. Updated it to require compilation/residency and verify compiled
  exception followed by successful recovery, not removed exception coverage.
- /tmp/art-intdiv-test.log full audit exit0: compiled division recovery and
  int div/rem726PASS. Compiler/runtime/probe/actual link PASS; DEX40classes
 1215methods/button97classes1634verified; fmt/diff checks PASS.
- Broader control flow/OSR/type/call/GC and real app acceptance remain. Goal
  active; no APK modifications, commit or push.

### Scalar backedges and composed primitive calls — 2026-09-04

- Prior turn int div/rem made verified progress. Removed forward-only branch
  restriction in verified primitive bodies; four IJFD recurrence loops compare
  interpreter-before-JIT against optimized resident code,140seed/count pairs.
  Tests require an actual DEX backedge, include skipped and257iteration loops,
  and exercise loop-carried primitive values/induction variables.
- DEX method count unexpectedly increased by5rather than4. SDK dexdump showed
  D8 inserted Double.isNaN(D)Z in jitLoopD. Kept the fixture intact and admitted
  resolved nonnative/nonsynchronized static primitive-only calls in numeric
  bodies plus MOVE_RESULT/WIDE. Existing method-shape forwarding checks remain.
  This is composed-call support, not blanket native/reference call support.
- First audit stopped119 before adding composed-call admission; rerun
  /tmp/art-scalarloop-test.log exits0, scalar loops140PASS and full regression.
  DEX40classes1220methods/button97classes1639verified; compiler/runtime/probe/
  actual link/fmt/diff PASS. No backend correction needed for exercised loops.
- OSR remains rejected by DarwinJitCanCompile. Inspected AOSP PrepareForOsr in
  _build/runtime-common/patched-source/runtime/jit/jit.cc: looks up OSR stackmap,
  copies32bit vregs into allocated frame and calls art_quick_osr_stub. This is
  next ABI/test work, not evidence of OSR support. Current loops start compiled;
  no movingGC/reference-liveness-under-OSR claim. Full goal active; no APK edits,
  commit or push.

### Explicit OSR frame preparation and execution — 2026-09-04

- Previous scalar-loop turn was verified progress. Allowed OSR compilation for
  static primitive signatures within existing capability checks. Automatic
  MaybeDoOnStackReplacement early return remains; updated its patch comment
  to distinguish this from explicit OSR compilation acceptance.
- New CheckJitOsrFrame compiles jitLoopI as OSR, checks fixture register/header
  layout, prepares actual OSR stackmap/frame at dexpc1 with induction0/3/16,
  enters real art_quick_osr_stub under a managed-stack fragment, and checks
  remaining recurrence results against unsigned32 arithmetic. Three cases PASS.
- /tmp/art-osr-test.log full audit exit0; compiler/runtime/probe/actual link
  and fmt/diff pass. No assembly correction required for these integer cases.
  DEX unchanged1220/1639. Comment-only patch wording updated after build.
- This uses synthetic interpreter vregs, not a live interpreter transition.
  Next: actual ShadowFrame/MaybeDoOnStackReplacement integration, wide/FP and
  reference state, GC/deopt during OSR. Automatic transfer still disabled;
  full objective not achieved. No APK edits, commit or push.

### Live interpreter branch to OSR — 2026-09-04

- Previous explicit-frame turn was progress. Removed Darwin unconditional
  early return from MaybeDoOnStackReplacement in patch0039, restoring AOSP
  instrumentation/stack-limit/ShadowFrame/managed-stack/deopt checks.
- Extended CheckJitOsrFrame: after explicit OSR compilation, three invocations
  enter EnterInterpreterFromInvoke(stay_in_interpreter=true), which skips
  compiled dispatch at entry but retains ordinary branch OSR. Each1000step
  integer recurrence is checked against independent unsigned32 arithmetic.
- /tmp/art-osrlive-test.log trace has exactly three corresponding
  `Done running OSR code for int ...Hello.jitLoopI(int, int)` messages, plus
  three correct interpreter-entry results. This proves real branch transfer,
  beyond the earlier synthetic-frame/direct-stub checks. Full audit exit0;
  runtime/probe/actual link/fmt/diff PASS. DEX unchanged1220/1639.
- OSR code is explicitly precompiled here: background hotness-triggered OSR
  compilation is not proved. Wide/FP/reference state, GC and deopt during OSR
  remain. Static primitive capability restriction remains; full goal active,
  no APK edits/commit/push.

### Wide and FP live OSR return/state — 2026-09-04

- Previous live integer OSR turn was verified progress. CheckJitOsrWide now
  compares normal compiled JNI execution against interpreter-entry/live OSR
  for long,float,double loops. Raw argument words include full64bit long/double
  and32bit float, with exact return bit comparisons. Nine cases total.
- Includes high-bit long seeds, negative inputs, finite fractions and positive
  infinity. First sample uses17iterations to retain finite seed influence;
  others1000iterations. No fixture changes or new backend patches required.
- /tmp/art-osrwide-test.log full audit exit0: three `Done running OSR code`
  messages per J/F/D method and all nine exact-bit comparisons PASS, including
  short loops. Probe/actual link/fmt/diff pass. DEX unchanged1220/1639.
- Expected values here come from normal compiled execution, previously covered
  by scalar-loop interpreter differential tests, not an independent FP oracle.
  Reference roots, GC/deopt inside OSR and hotness-driven background compilation
  remain unproved; full goal active. No APK edits/commit/push.

### Hotness-driven background OSR compilation — 2026-09-04

- Prior wide/FP live OSR tests were progress. Found remaining Darwin queue
  short-circuit: MaybeEnqueueCompilation returned whenever normal JIT code
  existed, preventing AOSP's subsequent IsOsrCompiled/AddCompileTask(kOsr)
  branch. Removed only that early condition; capability guard remains.
- Added cold jitAutoLoop and CheckJitAutomaticOsr. Requires initially no normal
  JIT or OSR code, enters interpreter, runs5million recurrence steps with no
  explicit CompileMethod, requires both resulting code entries and exact
  independent unsigned32 result. No fabricated hotness counter or wait loop.
- /tmp/art-osrauto-test.log exit0: worker661336926 logs Baseline request then
  Osr compilation; execution thread661336301 logs Jumping/Done running OSR for
  jitAutoLoop. Automatic OSR test and full regression PASS. DEX40classes1221/
  button97classes1640verified; runtime/probe/actual link/fmt/diff PASS.
- This proves one cold primitive loop's automatic path, not all methods or
  scheduling under load. Reference state/GC/deopt under OSR and wider full-JIT
  capability/app validation still remain. Goal active; no APK edits/commit/push.

### Exception propagation out of live OSR — 2026-09-04

- Prior automatic OSR turn was verified progress. Added jitOsrDivide, whose
  loop divides by1000-i and throws at iteration1000, and CheckJitOsrException.
  Explicit normal/OSR compilation precedes interpreter-entry execution here.
- Three cycles enter OSR then require ArithmeticException from2000iteration
  invocation, clear it, check original top ShadowFrame restoration, and repeat
  with17iterations requiring the independently computed normal result and
  restored frame. Same thread/method throughout, no APK manipulation.
- /tmp/art-osrex-test.log full audit exit0; six corresponding Done running OSR
  traces and exception/restoration/recovery3cycles PASS. DEX40classes1222/
  button97classes1641verified; probe/actual link/fmt/diff PASS. Existing AOSP
  exception transfer handles this case without another backend patch.
- Not proof of catch/finally inside OSR, reference roots, movingGC or OSR
  deoptimization. These and wider JIT/app requirements remain; goal active,
  no commit or push.

### Reference-valued live OSR — 2026-09-04

- Prior exception OSR turn was progress. Extended static OSR signatures to
  reference parameters/results and the common admitted body to object moves/
  returns and mixed signatures, still without read barriers. Other operation
  gates remain; this does not admit arbitrary reference calls or memory access.
- jitOsrReferences exchanges two references each iteration. CheckJitOsrReferences
  encodes JNI objects into compressed interpreter arguments, enters real OSR,
  and checks native returned object identity for odd/even17/18/1000/1001counts
  with two objects or either null. Twelve cases; caller JNI roots remain live.
- /tmp/art-osrref-test.log full audit exit0:12corresponding Done running OSR
  traces and reference identity/null return PASS. Initial test compilation
  needed explicit ObjPtr.Ptr() for FromMirrorPtr; no runtime ABI fix needed.
  DEX40classes1223/button97classes1642verified; compiler/runtime/probe/actual
  link/fmt/diff PASS.
- No GC deliberately triggered inside these loops; sole-root/movingGC/deopt
  validation remains, along with broader goal requirements. Goal active;
  no APK edits/commit/push.

### Checkpoint-confirmed GC during reference OSR — 2026-09-04

- Prior reference OSR turn was progress. Added CheckJitOsrGc using existing
  LoopCheckpoint and an attached worker. Worker observes top compiled frame PC
  within the exact OSR method header, collects outside callback/locks, then
  requires another PC observation inside that same OSR code after collection.
- Target enters interpreter then OSR for500000001reference exchanges, checks
  the odd-parity returned object. Queued checkpoints drain before reuse and
  destruction; target transitions Native before joining worker.
- /tmp/art-osrgc-test.log full audit exit0: before/after0x11e605c24,
  collections=1, correct=1. Probe/actual link/fmt/diff PASS. No runtime change
  needed for this CMS case; DEX unchanged1223/1642.
- Caller JNI roots remain, so this is concurrent GC/OSR integration evidence,
  not sole-compiled-root survival or movingGC update proof. Those stronger
  checks and deopt/full capability/app validation remain; goal active,
  no APK edits/commit/push.

### OSR roots without caller strong JNI roots — 2026-09-04

- Previous checkpoint GC turn was progress. Strengthened CheckJitOsrGc by
  replacing caller strong roots with weak globals and deleting both JNI locals
  immediately before interpreter entry. The OSR exchange loop carries the
  live objects; native raw argument words are not registered GC roots.
- After return, adds the returned object to JNI locals before Native/join,
  checks both weak referents survived and returned identity matches expected.
  Drops returned local, clears JValue, explicitly collects again and requires
  both weak references cleared. This detects unexpected persistent strong roots.
- /tmp/art-osrsole-test.log full audit exit0: exact OSR PC before/after,
  collections=1 survived=1 reclaimed=1 correct=1. Probe/actual link/fmt/diff
  PASS. Test needed jvalue-inl.h for SetL definition; no runtime patch added.
- Demonstrates CMS survival without caller JNI strong roots, plus post-loop
  reclamation. Does not establish movingGC relocation/read barriers or OSR
  deoptimization; broader goal remains active. DEX unchanged1223/1642,
  no APK edits/commit/push.

### Moving compaction during reference OSR — 2026-09-04

- Previous sole-root CMS turn was progress. Added compact variant of
  CheckJitOsrGc calling AOSP PerformHomogeneousSpaceCompact on worker outside
  object-access scope. Reads weak referent addresses in separate scopes before/
  after; requires success and actual nonzero address change, not just GC count.
- Both variants still require exact OSR-code checkpoint PCs before/after,
  absence of caller strong JNI roots during loop, correct returned identity,
  and clearing weak refs after final root release plus collection.
- /tmp/art-osrmove-test.log full audit exit0: compact1 object moved
  0x1001023c000 -> 0x10020094000, OSR PC0x11a8e5c24 before/after,
  collections1 survived1 reclaimed1 correct1. CMS variant also passes.
  Probe/actual link/fmt/diff PASS; no runtime modification needed for this path.
- This establishes stop-the-world homogeneous-space compaction updating OSR
  references, not concurrent moving collector/read-barrier support. OSR deopt
  and broader JIT/app coverage remain. Goal active; DEX unchanged1223/1642,
  no APK edits/commit/push.

### Async deoptimization from reference OSR — 2026-09-04

- Previous moving compaction turn was progress. Added CheckJitOsrDeopt: worker
  observes exact OSR PC, suspends all and requests AOSP method deoptimization,
  retains request until invocation finishes, then undeoptimizes under suspend.
- Initial ordinary optimized-code test correctly refused async deopt (AOSP
  Runtime::IsAsyncDeoptimizeable requires debuggable code). No bypass added.
  Test now suspends JIT workers, removes inactive target code under GC critical
  section/suspend-all, recompiles normal+OSR with temporary debuggable compiler
  option, restores option, and verifies CodeInfo debuggable metadata.
- /tmp/art-osrdeopt-test.log full audit exit0: observed OSR PC0x11ef85d8c,
  requested1, deopts0->1, correct1. Trace reconstructs suspend-check runtime
  frame then jitOsrReferences; remaining20000001exchange result matches.
  Probe/actual link/fmt/diff PASS; no runtime patch needed for this deopt path.
- Caller JNI roots retained in this test. Combined sole-root/movingGC/deopt,
  other value layouts and concurrent moving read barriers remain, as do full
  backend/app requirements. Goal active; DEX unchanged1223/1642, no APK edits,
  commit or push.

### Combined sole-root relocation and OSR deopt — 2026-09-04

- Previous standalone deopt turn was verified progress. Strengthened that
  test to remove both strong JNI locals before interpreter entry, retain weak
  witnesses, compact after first OSR checkpoint, require actual address change,
  observe OSR again after relocation, then request AOSP async deoptimization.
-100000001exchange iterations retain odd-parity identity through relocation
  and ShadowFrame reconstruction. Returned object is locally rooted before
  Native/join, checked against relocated weak witness, released; subsequent
  collection must clear both weak refs. Debuggable metadata requirement retained.
- /tmp/art-osrcombined-test.log full audit exit0: object0x100200a0000 moved to
  0x10010094000; post-move OSR PC0x11f605d8c, deopts0->1, reclaimed1 correct1.
  Probe/actual link/fmt/diff PASS. No new runtime patch necessary in this path.
- This combines sole-root STW relocation and async OSR deopt for reference
  swaps. Not complete value-layout coverage or concurrent read-barrier proof;
  full capability removal/app validation remains. Goal active, unchanged DEX
 1223/1642; no APK edits/commit/push.

### Packed and sparse switch dispatch — 2026-09-04

- Previous combined relocation/deopt turn was progress. Added PACKED_SWITCH/
  SPARSE_SWITCH admission and NOP alignment/switch payload records in verified
  bodies. Array-data payloads remain rejected. Existing AOSP lowering retained.
- Two fixtures assert actual expected DEX switch opcodes, collect interpreter
  outputs before compilation, require JIT residency, and compare112calls with
  interpreter and independent key/value tables. Dense negative/positive keys,
  sparse extrema/large keys, default and adjacent keys are covered.
- /tmp/art-switch-test.log full audit exit0; switch112PASS plus existing OSR/
  movingGC/deopt regression. Compiler/runtime/probe/actual link/fmt/diff PASS;
  DEX40classes1225/button97classes1644verified. No additional backend patch needed.
- Does not prove every switch shape or payload-bearing instruction. Remaining
  backend/type/call/read-barrier and real-app requirements keep full goal active;
  no APK edits/commit/push.

### Exact class type checks and native boundaries — 2026-09-04

- Previous switch turn was progress. Added INSTANCE_OF/CHECK_CAST admission
  for resolved final nonarray classes; removed obsolete LL identity-only body
  restriction now common reference moves/returns are supported. Hierarchy,
  interface and array type-check lowering remain gated pending separate work.
- Added patch0063 and registered compiler build: type-check quick calls decode
  x0/x1 from compressed object/class arguments; common no-read-barrier reference
  load helpers decode base into scratchX before loading compressedW payload.
- Initial audit crashed. /tmp/art-types-lldb4.log isolates fast-path
  `ldr w2,[x1]` with x1=0x1004f8a8 still compressed. Helper fix resolves it.
  Debugger ignored prior intentional ART null faults until CheckJitTypes breakpoint.
- String instanceof/cast tests compare interpreted and compiled outcomes with
  JNI IsInstanceOf/identity and typed ClassCastException: null,String,other object,
  intarray,16cases. /tmp/art-types-test.log full audit exit0 after final gate
  rebuild; compiler/runtime/probe/actual link/fmt/diff PASS. DEX40classes1227/
  button97classes1646verified. Remaining type-check kinds and full goal active;
  no APK edits/commit/push.

### Concrete and abstract class type checks — 2026-09-04

- Previous exact-type turn was progress. Extended resolved class checks to
  nonfinal/abstract classes; interfaces and array targets still excluded.
  Patch0063 now decodes the class base before bitstring status loads (inspection
  identified direct compressed-base dereference). Common superclass reference
  loads already use the helper fixed in prior turn.
- Added Base/Number instanceof/cast fixtures. Seven inputs per fixture cover
  null,String,unrelated object,intarray,base,derived child,Integer; six methods
  across interpreter/compiled phases yield84checks against JNI type/identity
  and ClassCastException expectations.
- Initial test looked up app classes through boot JNI context and failed with
  NoClassDefFoundError. Reused existing Java class-return helpers to resolve via
  the app loader. Patch context also enlarged after short hunks failed to apply.
- /tmp/art-hierarchy-test.log full audit exit0,84typesPASS. Compiler/runtime/
  probe/actual link/fmt/diff PASS; DEX40classes1231/button97classes1650verified.
  No machine-code assertion distinguishing each type-check optimization yet;
  interfaces/arrays and broader goal remain active. No APK edits/commit/push.

### Interface-table type checks — 2026-09-04

- Previous class hierarchy turn was progress. Patch0064 corrects compressed
  base loads in instanceof interface fast path: object->class->iftable, then
  native64 table cursor for size/elements/iteration. Check-cast shares decoded
  reference helpers and now decodes its iftable cursor for scalar loads/iteration.
  Runtime slow-path object/class decoding remains from0063. Registered patch.
- Enabled resolved interface targets (array targets still gated). Added
  CharSequence/Serializable instanceof/cast fixtures; existing String,Integer,
  intarray,base/child,unrelated object,null exercise positive/negative cases.
- /tmp/art-interface-test.log full audit exit0: class/interface140checks PASS,
  interpreter/JNI oracle versus resident JIT with identity and cast exceptions.
  Compiler/runtime/probe/actual link/fmt/diff PASS. DEX40classes1235/button
  97classes1654verified. Array target checks and broader JIT/app requirements
  remain; goal active, no APK edits/commit/push.

### Array target checks and covariance — 2026-09-04

- Previous interface turn was progress. Patch0065 decodes component class
  before primitive-type scalar loads in instanceof/check-cast array-object
  paths. Registered compiler patch; resolved array target admission enabled.
- Added Object[],String[],int[],CharSequence[] instanceof/cast fixtures.
  Inputs now include String[],Object[],Integer[],int[][],long[] alongside prior
  class/primitive-array/null inputs.432checks across18methods and interpreter/
  resident-JIT phases compare JNI type oracle, identity and cast exceptions.
- /tmp/art-arraytype-test.log full audit exit0,432typesPASS. Compiler/runtime/
  probe/actual link/fmt/diff PASS; DEX40classes1243/button97classes1662verified.
  Exact arrays, primitive mismatch and reference/interface covariance paths
  exercised; no assertion of every optimizer specialization or unresolved type
  loading. Broader backend/call/read-barrier/app coverage still remains.
  Full goal active; no APK edits/commit/push.

### Compile-before-invoke type coverage, unresolved case still open — 2026-09-04

- Added Checksum instanceof/cast fixtures and compile-before-invoke checks using
  Adler32, unrelated object and null, including identity and ClassCastException.
  Probe logs DexCache state before/after compilation instead of equating an
  uncalled method with an unresolved type. Both report cache_before=1: verifier
  already resolved the interface. This is NOT runtime resolution coverage.
- Trial removal of the resolved-type eligibility condition passed this suite
  but did not establish unresolved support. Restored that condition; no untested
  widening is retained. Next work needs a genuinely deferred class-loader case,
  with AOSP verification/access semantics preserved, before removing the guard.
- Source inspection: LoadClassSlowPathARM64 passes a managed reference to
  quickInitializeStaticStorage in its non-resolution branch; native-pointer
  decoding deserves a focused test/fix. GenerateLoadClassRuntimeCall returns a
  native class pointer; audit its managed result boundary as well. These are
  follow-up observations, not fixed/verified paths in this turn.
- Final serial compiler/runtime/probe/link rebuild and full audit exit0:
  /tmp/art-coldtype-final-test.log.432 existing type checks, both OSR GC modes
  and moving-GC/deoptimization remain PASS. fmt/diff checks PASS. DEX counts
  40classes1245/button97classes1664. Full goal remains active; no actual-app
  verification this turn, no APK changes/commit/push.

### Class-load and initialization pointer boundaries — 2026-09-04

- Previous turn was progress: compile-before-invoke evidence distinguished
  resolved types from real deferred resolution, leaving the gate intact.
- Added registered compiler patch0066. GenerateClassInitializationCheck now
  decodes the class into its owned scratch register before loading status;
  it preserves the input HIR reference. LoadClassSlowPath decodes only the
  already-resolved managed-class argument before quickInitializeStaticStorage.
  The resolve-then-initialize branch already has a native pointer and must not
  be decoded again. Both slow-path and direct runtime-call results explicitly
  encode native class pointers before rejoining managed code.
- Compiler rebuild (one object), actual graphics link, full JIT audit and
  reference-codegen audit PASS; fmt/diff PASS. Logs /tmp/art-clinit-compiler.log,
  /tmp/art-clinit-test.log and /tmp/art-clinit-codegen.log. Existing432 type
  checks and moving-GC/OSR/deopt regressions remain green. No gate broadening.
- This establishes patched/buildable boundaries and no observed regression,
  NOT a first-use class-initializer execution proof. Next: a separate initially
  uninitialized class accessed from compiled code, checking one-time clinit,
  exceptions and concurrent initialization before relaxing StillNeedsClinitCheck.
  Deferred resolution/access checks, read barriers, broad calls and actual app
  tests remain unfinished. Goal active; no APK edits/commit/push.

### First-use explicit class initialization from JIT — 2026-09-04

- Previous turn progressed pointer-boundary implementation. Added a separate
  JitColdInitialization fixture, its DEX inputs and Hello caller/counter.
  ResolveMethodId performs AOSP method resolution without class initialization.
  Test asserts class uninitialized and counter0 before AND after optimized
  compilation, checks resident JIT entry, then three calls return42 with counter1.
- Initial DEX build exposed omitted explicit class input; fixed both baseline
  and button lists. First runtime attempt exposed absent method-cache entry;
  resolving through ClassLinker fixed that precondition without initializing it.
  Next compilation failure was the independent post-optimization clinit gate.
- Extended0066 to admit explicit clinit load/call graphs; implicit clinit and
  access-check restrictions remain. Generic numeric static-call admission no
  longer rejects a callee merely for needing initialization. Other gates remain.
- /tmp/art-firstinit-test.log full trace audit exit0: class stays cold through
  compilation, JIT entry0x11f3bd3b0 recorded, three calls/counter/result PASS.
  Existing432 type checks, moving GC/OSR/deopt PASS. Compiler/runtime/probe/link
  rebuilt, fmt/diff PASS. DEX41classes1249/button98classes1668.
- Exceptions from clinit, erroneous-class repeat use and competing initializing
  threads still need explicit tests. This proves first-use success/once-only,
  not all initialization/resolution behavior or full JIT/app compatibility.
  Goal active; no APK edits/commit/push.

### Erroneous class initialization through compiled caller — 2026-09-04

- Previous turn progressed explicit first-use initialization. Added separate
  JitFailedInitialization class with a counted initializer that divides by a
  nonconstant zero; included class in both DEX builds. Caller remains ordinary
  static invocation plus arithmetic, compiled before the target is initialized.
- New runtime_jit_failed_initialization.h checks cold state and zero counter
  before/after compilation plus resident JIT entry. First invocation requires
  ExceptionInInitializerError whose cause is ArithmeticException; next two
  require NoClassDefFoundError. After each call, counter stays1 and the AOSP
  class reports IsErroneous. Exceptions are cleared between calls so later
  regression tests run normally. No new runtime workaround or gate change.
- /tmp/art-failedinit-test.log full audit exit0; first-use and erroneous-class
  tests PASS, existing432 type checks and OSR/moving-GC/deopt PASS. DEX42classes
  1253/button99classes1672, probe rebuild and actual graphics link PASS;
  fmt/diff PASS. No actual-app run this turn, no APK edits/commit/push.
- Next missing initialization coverage is competing threads and publication;
  implicit clinit, unresolved/access checks and broader goal remain unfinished.
  Full goal stays active.

### Concurrent first-use initialization and publication — 2026-09-04

- Previous turn added verified erroneous-class execution. Added independent
  JitConcurrentInitialization fixture and both DEX input entries. Its clinit
  increments a counter, blocks on a monitor owned by the observer, then writes
  a plain static field73. Compiled caller reads the field and returns74.
- New concurrent-initialization probe compiles while target is uninitialized.
  Two attached ART threads have real Java peers and invoke the same JIT caller.
  Observer requires first thread kBlocked, second kWaiting, class kInitializing
  and neither call completed before releasing its monitor. Both then return74;
  counter1 and initialized class confirm once-only initialization/publication.
  Thread pointers remain alive until observation ends; observer releases mutator
  access while waiting/joining. No runtime workaround or gate change.
- Full JIT audit passed twice in fresh processes: /tmp/art-concurrentinit-test.log
  and /tmp/art-concurrentinit-repeat.log. Both show blocked=1 waiting=1
  initializing=1 held=1 once=1 values=1,1; prior GC/OSR/deopt suite also passed.
  Probe/link rebuilt; fmt/diff PASS. DEX43classes1257/button100classes1676.
- Still not exhaustive initialization concurrency: erroneous clinit with
  competing waiters and recursive initialization remain to test. Implicit
  clinit, unresolved/access checks, read barriers, general calls and actual-app
  verification remain. Full goal active; no APK edits/commit/push.

### Failing initialization with a competing waiter — 2026-09-04

- Previous turn established successful concurrent initialization. Extended its
  shared probe with a separate failing target, separate counter and ordinary
  compiled caller. Target blocks on observer-owned monitor before divide-by-zero.
- Requires initializer kBlocked, contender kWaiting, class kInitializing and
  neither call completed before release. Initializer must receive
  ExceptionInInitializerError, waiter NoClassDefFoundError. Class must end
  erroneous, initializer counter1. Java peers, native waits and Thread lifetime
  handling are retained. Successful-publication variant remains in same suite.
- /tmp/art-concurrentfail-test.log full audit exit0: both fail=0/fail=1 report
  blocked=1 waiting=1 initializing=1 held=1 once=1 values=1,1 (values flags mean
  expected result/exception outcome). Existing OSR/moving-GC/deopt regressions
  remain PASS. DEX44classes1261/button101classes1680; probe/link/fmt/diff PASS.
- No new runtime patch or gate widening was necessary. Recursive initialization,
  implicit clinit and the remaining whole-goal paths are still unfinished;
  no actual-app run this turn. Goal active, no APK edits/commit/push.

### Same-thread recursive initialization through JIT — 2026-09-04

- Previous turn verified failing concurrent initialization. Added an independent
  JitRecursiveInitialization class. Its clinit increments a counter, calls the
  already compiled Hello caller again, stores that observed result, then writes
  the final plain static value73. Inner read sees default0 and returns1; outer
  and repeated calls return74. This exercises same-thread reentry into clinit.
- New probe resolves without initialization, requires cold class/counter0 before
  and after compiling the caller, and resident JIT code before and after three
  invocations. Requires observed1, final74, counter1 and initialized class.
- /tmp/art-recursiveinit-test.log full audit exit0, recursive check PASS and
  prior exceptions/concurrency/type/OSR/moving-GC/deopt regressions PASS. Added
  class to both DEX inputs; DEX45classes1265/button102classes1684 verified.
  Probe/link/fmt/diff PASS. No new runtime workaround or gate change needed.
- Explicit-clinit success/failure/competition/reentry now have focused evidence.
  Implicit-clinit invocation still rejected by the lowered-graph guard; deferred
  type resolution/access checks and the remaining full-goal ABI/read-barrier/
  call/app coverage remain incomplete. Goal active; no APK edits/commit/push.

### Corrected clinit classification and implicit-call gate removal — 2026-09-04

- Previous turn added recursive execution evidence. Source audit found the
  Darwin graph guard runs BEFORE AllocateRegisters, whose AOSP PrepareFor-
  RegisterAllocation pass can merge explicit clinit into the invoke. Therefore
  earlier descriptions of these tests as explicit-clinit execution were too
  strong. They proved semantics, but not that particular final lowering.
- Added VLOG(jit) at ARM64 GenerateStaticOrDirectCall: final codegen reports
  implicit=1 explicit=0 for all five initialization callers (first-use, failure,
  recursive, successful contention and failed contention). /tmp/art-implicitinit-
  test.log confirms these already executed the implicit ArtMethod call path.
- Removed the remaining pre-allocation implicit-clinit rejection from0066;
  other resolved/non-native/direct-method/code-pointer constraints remain.
  Rebuilt compiler and actual link, reran traced full audit: exit0 in
  /tmp/art-implicitinit-final-test.log; all five lowerings and semantic tests
  PASS along with GC/OSR/deopt. fmt/diff PASS. DEX unchanged45classes1265/
  button102classes1684. Trace logging stays opt-in via existing JIT verbosity.
- Correction supersedes earlier claim that implicit clinit had no execution
  evidence. Conversely, explicit load-class/status slow-path emission still
  needs a case that cannot be merged into invoke (e.g. cold static-field access).
  Deferred resolution/access checks and full-goal call/read-barrier/app work
  remain. Goal active; no APK edits/commit/push.

### Explicit cold static-field initialization — 2026-09-04

- Previous turn corrected implicit-versus-explicit lowering classification.
  Added JitColdStatic with counted nontrivial initializer and plain int field93.
  Hello.jitColdStaticRead is exactly SGET/RETURN, so initialization cannot merge
  into a method invocation. ResolveField populates the field cache without
  initializing its class; probe requires cold class/counter0 through compilation.
- Removed the resolved static-field helper's initialized-class prerequisite.
  Other shape/type/read-barrier restrictions remain. Added opt-in VLOG at
  GenerateClassInitializationCheck. Actual codegen log names jitColdStaticRead;
  three compiled reads return93, counter1 and initialized class. This supplies
  the explicit status/slow-path execution evidence absent from earlier calls.
- /tmp/art-coldstatic-test.log traced full audit exit0; explicit status log and
  SGET test PASS, existing regressions PASS. DEX46classes1268/button103classes
  1687; compiler/runtime/probe/actual link rebuilt; fmt/diff PASS.
- Cold static writes, reference/wide values across initialization and explicit
  initialization failure need additional focused coverage. Unresolved/access
  checks and full-goal read-barrier/general-call/app work remain unfinished.
  Goal active; no APK edits/commit/push.

### Cold static writes preserve reference and wide arguments — 2026-09-04

- Previous turn verified explicit cold SGET. Added independent long/double/
  reference classes and six simple getter/setter methods. Compile each pair
  before initialization; first operation is SPUT_WIDE/SPUT_OBJECT, not a getter.
  Initializers increment separate counters and write distinct initial values
  (reference initializer allocates byte[65536]).
- New probe checks cold state/counter0 after compilation, then three setter/
  getter rounds per type. Long high bits/negative/zero, exact double bits
  including negative zero, and object identity/null survive. Each counter1
  confirms the first setter initialized before storing its live argument.
- /tmp/art-coldwrite-test.log traced full audit exit0. All six methods emit
  explicit clinit status checks; cold static writes PASS and existing full
  regressions PASS. DEX49classes1280/button106classes1699; probe/link/fmt/diff
  PASS. No new runtime patch or gate change in this turn.
- Allocation during reference initialization is exercised, but forced moving
  collection during that slow path is not established by this test. Explicit
  clinit failure, remaining field shapes and full-goal unresolved/access/call/
  read-barrier/app coverage remain. Goal active; no APK edits/commit/push.

### Moving clinit GC exposed and fixed stale JIT class roots — 2026-09-04

- Previous turn verified cold writes without forced collection. Added cold
  SPUT_OBJECT/SGET_OBJECT pair and counted initializer blocked on an observer
  monitor. Attached Java-peer thread enters setter, observer confirms blocked
  and class initializing, performs homogeneous semispace compaction, requires
  reference address movement and continued blocking, then resumes and checks
  stored identity/counter1. JNI strong roots retained: not a sole-root test.
- First run crashed after compaction. /tmp/art-clinitmove-lldb.log identifies
  getter's class-status LDRB using stale pre-GC class address. AOSP Runtime::
  SweepSystemWeaks gates JIT root sweeping on configured IsMovingGc(); default
  nonmoving collector remains configured during explicit homogeneous movement.
- Patch0067 makes SemiSpace sweep JIT roots when Runtime skipped them due to
  nonmoving configuration; avoids duplicate sweep for moving configuration.
  Registered source/patch AND runtime job selection (initial patch staging
  alone did not change the compiled source; fixed selection, confirmed1object).
- /tmp/art-clinitmove-fixed.log and /tmp/art-clinitmove-repeat.log full audits
  exit0. Both prove before0x1001023c000->after0x10020094000, blocked/initializing/
  compact/stayed/correct all1. Other GC/OSR/deopt regressions PASS. DEX50classes
  1284/button107classes1703; runtime/probe/link rebuilt, fmt/diff and manifest
  unit test PASS. No APK edits/commit/push.
- This fixes an actual stale JIT literal root defect, not only fixture behavior.
  Concurrent read-barrier collectors, sole-root clinit preservation, explicit
  clinit failure and remaining full-goal paths/app runs remain incomplete.
  Goal remains active.

### Explicit field-initialization failures and constructor cache admission — 2026-09-04

- Previous turn fixed stale JIT roots during homogeneous movement. Added two
  fresh classes for cold SGET_WIDE and SPUT_WIDE with failing initializers.
  Probe compiles each before initialization; first use requires EIIE with
  ArithmeticException cause, next two NCDFE, counter1 and erroneous class.
  Direct ArtField read requires stored long remains0 after the failed writes.
- New tests passed, but full suite initially failed in pre-existing wide
  constructor factory compilation (not the subsequent pressure/OOME test).
  Added failure-only DEX dump: ordinary new-instance/invoke-direct/return.
  Investigated eligibility's cache-only constructor lookup; changed it to
  LookupResolvedType plus existing-class FindClassMethod when method cache
  misses, without resolving a new class or initializing it. Structural and
  constructor checks remain. Trace verifies actual cache miss/resolved_lookup=1.
- /tmp/art-failedstatic-fixed.log full traced audit exit0, explicit read/write
  exception/no-store tests and prior allocation/GC/OSR/deopt suite PASS.
  Earlier reproductions /tmp/art-failedstatic-test.log and -retest.log retain
  the constructor admission failure evidence. DEX52classes1290/button109classes
  1709; compiler/runtime/probe/link rebuilt; fmt/diff PASS.
- Other cache-only gates still deserve audit; deferred-resolution/access,
  read-barrier/general-call/app requirements remain incomplete. Goal active;
  no APK edits/commit/push.

### Resolved type/field admission uses ClassLinker lookup — 2026-09-04

- Previous turn fixed a reproduced constructor cache miss. Audited remaining
  direct cache gates: simple field accessor and constructor IPUT now use
  ClassLinker::LookupResolvedField; instanceof/check-cast use LookupResolvedType.
  These AOSP lookups consult loaded/resolved class tables and repopulate cache
  rather than treating a missing cache entry as unsupported bytecode.
- Inspected DoLookupResolvedType and LookupResolvedField: no ResolveType or
  class initialization is introduced. Class-not-resolved still returns null.
  Existing field-kind, signature, verified-code and read-barrier limits remain.
  Other managed-invoke cache-only gates remain for subsequent work.
- Compiler/runtime/probe/link rebuilt; /tmp/art-cachelookup-test.log full audit
  exit0, including type/field/constructor, clinit and moving-GC regressions.
  fmt/diff PASS. DEX unchanged52classes1290/button109classes1709.
- No forced eviction test for each newly changed lookup this turn; full-suite
  result is regression evidence, not proof of every cache-miss scenario. Did
  not misuse AOT-only ClearResolvedType to fabricate a runtime state. Deferred
  resolution and whole-goal ABI/call/read-barrier/app work remain unfinished.
  Goal active; no APK edits/commit/push.

### Shared resolved-method lookup for admission — 2026-09-04

- Previous turn changed type/field cache-only gates. Added shared
  DarwinJitLookupResolvedMethod: cache fast path, LookupResolvedType for method
  declaring type, then AOSP FindResolvedMethod. Uses AOSP class/interface lookup
  and hidden-API handling, repopulates cache, and does not load/initialize a new
  class. Replaces direct method-cache probes in static/virtual forwarding,
  constructor parent/allocation, exception-body calls and numeric composed calls.
- Keeps all invocation-kind/signature/native/intrinsic/shape restrictions. This
  is not general invoke support nor unresolved-method resolution. Replaces the
  previous constructor-specific fallback with the common AOSP lookup path.
- /tmp/art-methodlookup-test.log traced full audit exit0. Natural cache misses
  observed with resolved_lookup=1 in Math.abs(double), jitNewWideInstance(JD)
  and JitConstructorParent.<init>. Full regression suite PASS. Compiler/runtime/
  probe/actual link rebuilt; fmt/diff PASS; DEX unchanged52classes1290/button
  109classes1709. No APK edits/commit/push.
- Remaining work still includes broad invocation forms, deferred resolution,
  read barriers and actual-app acceptance. Goal remains active.

### Static-call argument permutations and duplication — 2026-09-04

- Previous turn unified resolved-method lookup. Removed parameter-list identity
  constraints from the single-static-call admission branch: verified DEX supplies
  callee argument types/counts, not the caller's parameter order/signature.
  Retains return-category, nonnative/nonintrinsic/valid-callee and instruction
  shape checks. Same-class/initialized-callee constraints also removed there;
  clinit lowering already verified in prior work.
- Existing jitCallReordered moved from expected rejection to execution test.
  Added duplicated-input and dropped-input/constant fixtures.216 comparisons
  across interpreter/JIT phases use unsigned arithmetic oracle for Java overflow
  and logical shift, with zero/negative/min/max values. Native call rejection
  remains an explicit pending capability test, not mislabeled as supported.
- /tmp/art-callargs-test.log full audit exit0,216argument checks PASS and all
  prior regressions PASS. DEX52classes1292/button109classes1711; compiler/runtime/
  probe/actual link rebuilt; fmt/diff PASS. No APK edits/commit/push.
- New permutation matrix is int-only; mixed wide/FP/reference permutations,
  multiple calls/void/general invoke forms and whole-goal barriers/app coverage
  still require work. Goal remains active.

### Mixed reference/long/double call permutations — 2026-09-04

- Previous turn admitted general parameter mapping for one static call. Added
  reference+long+double target and callers that reorder these parameters or
  drop an unrelated reference. Target checks high-bit long and fractional
  negative double before returning the exact reference; mismatch returns null.
- Test suspends background JIT compilation and runs four stages: interpreted
  callers/target, baseline callers with uncompiled target, baseline callers with
  compiled target, optimized callers/target.216 comparisons cover null/distinct
  objects, wrong tags and +/-zero. Baseline can inline small methods, so assert
  target DEX exceeds pinned kBaselineInlineMaxCodeUnits=14 to retain calls.
  Optimized stage may inline normally; it is not claimed to retain a call.
- /tmp/art-mixedargs-final-test.log full audit exit0,216checks PASS; previous
  traced run /tmp/art-mixedargs-test.log records baseline/optimized installation.
  Probe/link/fmt/diff PASS; DEX52classes1295/button109classes1714 verified.
  No runtime patch/gate change necessary this turn. No APK edits/commit/push.
- Mixed stack-argument permutations beyond register banks, GC during these
  remapped calls, multiple/void/interface/native invokes and full-goal barriers/
  app acceptance remain. Goal active.

### Composed static calls with references and void returns — 2026-09-04

- Previous turn verified mixed parameter remapping. General opcode admission
  now accepts void method return/RETURN_VOID, MOVE_RESULT_OBJECT, and reference/
  void shorties for resolved nonnative static callees. This enables multiple
  calls with intermediate reference results, not only single forwarding bodies.
- Added three-call reference composition: retain first result across a void GC
  call and another reference GC call, select first nonnull result. Separate void
  composition stores a then b. Test phases interpreter/baseline/optimized compile
  callers and targets;27input/phase combinations verify identity/null/store order
  and require GC count increase>=3 per reference call. JNI roots remain present;
  no sole-root/moving-GC claim for this composition test.
- /tmp/art-composed-test.log full traced audit exit0; composed calls PASS and
  all prior regressions PASS. DEX52classes1299/button109classes1718; compiler/
  runtime/probe/actual link rebuilt; fmt/diff PASS. No APK edits/commit/push.
- Optimizations may inline eligible calls; this test proves resulting semantics
  and actual GC activity, not every call boundary. Interface/native/general
  instance invocation, stack remapping, read barriers and app acceptance remain.
  Goal active.

### Composed virtual calls and nonfirst receivers — 2026-09-04

- Removed the five-code-unit, exact-forwarding restriction for resolved virtual
  calls. General opcode validation now admits invoke-virtual and its range form
  with normal move-result handling; native/intrinsic/read-barrier restrictions
  remain. This does not yet enable arbitrary instance-method bodies.
- Three new callers perform two virtual int calls with arithmetic, two reference
  calls with selection, or call only the second receiver. Interpreter, baseline
  and optimized phases compare 81 cases across null/base/overriding-child pairs.
  Checks cover exact reference identity, override dispatch, ignored null first
  arguments and NullPointerException on actually dereferenced null receivers.
  Eligible targets may inline; previous actual-vtable-load tests also pass.
- /tmp/art-virtual-composed-test.log full audit exit0 and composed virtual
  cases=81 PASS. DEX52classes1302/button109classes1721; compiler/runtime/probe/
  actual link rebuilt; cargo fmt and git diff checks PASS. No APK edits or push.
- Full goal remains active: interface/native/general instance bodies, wider
  argument remapping, read barriers and actual-app acceptance remain unfinished.

### General instance-body arithmetic and calls — 2026-09-04

- Previous turn completed composed virtual-call coverage. Ordinary instance
  methods no longer stop at the simple field-accessor admission check: they
  continue through common AOSP opcode validation. Keep the implicit receiver's
  read-barrier restriction explicit even for primitive-only shorties. Constructor,
  exception-handler and OSR-specific restrictions are unchanged.
- Added actual nonstatic methods performing arithmetic plus a static call and
  branch, selecting this versus a reference parameter, and mixed long/double/int
  arithmetic. JNI CallMethod tests compare interpreter/baseline/optimized outputs
  in 324 cases, with null/distinct references, signed boundaries, high-bit long
  inputs and fractional doubles. Both JIT phases require resident compiled code.
- /tmp/art-instance-body-test.log full audit exit0; new324checks and previous
  composed virtual81checks PASS. Compiler/runtime/probe/link rebuilt; DEX counts
  52classes1305/button109classes1724. fmt/diff checks PASS. No APK edits or push.
- This supports common arithmetic/call instance bodies, not yet all field/array
  compositions, handlers, monitors or instance OSR. Interface/native dispatch,
  barriers and actual-app acceptance remain; full goal stays active.

### Composed resolved field access — 2026-09-05

- General instruction validation now accepts resolved typed instance/static
  get/set operations, preserving field type/static checks and the read-barrier
  restriction. Uses existing AOSP field lowering rather than a new host path.
- Added read/math/write through another receiver, static volatile long addition
  and reference-field exchange across an actual GC call.105 cases compare
  interpreter/baseline/optimized execution, wrapping arithmetic, null receiver
  exceptions, stored values and reference identities. JNI roots are retained;
  no sole-root or cross-thread volatile ordering claim for this new test.
- Initial /tmp/art-field-composed-test.log stopped on a stale negative assertion:
  jitReadLargeIntTwice now compiles. Replaced expected rejection with resident
  code and actual sum verification for its large+ordinary offset field loads.
- /tmp/art-field-composed-fixed-test.log full audit exit0, new105cases PASS and
  existing regressions PASS. DEX52classes1308/button109classes1727; compiler,
  runtime, probe and actual link rebuilt; fmt/diff PASS. No APK edits or push.
- Unresolved fields, array compositions, monitors/handlers, interface/native
  invocation, barriers and actual-app acceptance remain. Full goal active.

### Array access composition and partial-store exceptions — 2026-09-05

- General opcode admission now accepts array length and typed get/set instead
  of requiring an isolated accessor shape. Existing AOSP array lowering and
  verified DEX typing apply; read-barrier restriction remains explicit.
- Added an int-array transform loop combining loads, arithmetic, stores and
  length.135 interpreter/baseline/optimized cases cover null/empty/four-element
  arrays, negative/zero/in-range/overrun counts, wrapping values and deltas.
  Checks compare exact results and contents, including stores completed before
  ArrayIndexOutOfBoundsException; null always requires NullPointerException.
- /tmp/art-array-composed-test.log full audit exit0, new135cases PASS and prior
  regressions PASS. Compiler/runtime/probe/link rebuilt; DEX52classes1309 and
  button109classes1728; fmt/diff PASS. No APK modifications or push.
- New loop coverage is int-only, not proof of every reference/wide composition
  or vectorized path. Allocation compositions, interface/native calls,
  monitors/handlers, read barriers and actual-app acceptance still remain.
  Full goal active.

### Array allocation with initialization and reference-copy GC — 2026-09-05

- General opcode validation admits NEW_ARRAY for already resolved types,
  retaining the read-barrier restriction. Allocations can now participate in
  ordinary loops/calls rather than only an immediate return-shaped method.
- Two new methods allocate/fill int arrays or allocate an Object[] and copy
  source elements through a GC call each iteration.45 interpreter/baseline/
  optimized cases verify lengths, distinct new arrays, exact values/references,
  negative-size exceptions, null-source exceptions and >=3 GCs for 3elements.
  New destination arrays remain live across those GCs before returning to JNI;
  source elements retain JNI roots. This is not moving-GC evidence.
- /tmp/art-array-allocate-composed-test.log full audit exit0; new45cases and
  existing regressions PASS. DEX52classes1311/button109classes1730; compiler,
  runtime, probe and link rebuilt; fmt/diff PASS. No APK edits or push.
- Still missing general object-constructor allocation compositions, unresolved
  types, interface/native calls, monitors/handlers, barriers and actual-app
  acceptance. Full goal remains active.

### Composed object allocation and constructor calls — 2026-09-05

- General opcode validation now admits NEW_INSTANCE for resolved/visibly
  initialized types and resolved nonnative direct calls, preserving existing
  read-barrier and unsupported-callee checks. Object allocation need not be a
  single constructor-forwarding return shape anymore. Constructor-body-specific
  restrictions remain separate and unchanged.
- Added caller that creates two Hello instances, links first from second,
  writes computed fields, triggers GC and returns second.36 interpreter/
  baseline/optimized cases verify payload identity, distinct objects, marker
  arithmetic, GC occurrence and constructor IllegalArgumentException propagation
  followed by successful calls. Constructors may run interpreted; caller code
  must be resident in each JIT phase. Payload JNI roots are retained.
- /tmp/art-object-composed-test.log full audit exit0; new36cases and prior
  regressions PASS. DEX52classes1312/button109classes1731; compiler/runtime/
  probe/link rebuilt; fmt/diff PASS. No APK edits or push.
- Cold/unresolved allocation, general constructor bodies, interface/native
  dispatch, handlers/monitors, barriers and actual-app acceptance remain.
  Full goal stays active.

### General constructor bodies and runtime string resolution — 2026-09-05

- Removed the dedicated constructor-shape validator; constructors now use
  common opcode validation and AOSP constructor processing. Admitted string
  constants and THROW in ordinary bodies. This enables Hello(int,Object),
  including parent call, setter calls, GC branch and exception construction.
- Baseline passed but optimized compilation initially rejected HLoadString.
  Added opt-in VLOG diagnostics to graph admission. Runtime-call string loads
  lacked native-pointer-to-managed-reference conversion: patch0068 adds Encode
  after QuickResolveString; graph admission now permits that load kind. Patch
  registered in compiler build. Other unsupported graph paths stay restricted.
- Composed-object test now requires constructor code resident at baseline and
  optimized tiers and verifies exact exception message "constructor marker".
  Moved it after allocation tests to preserve existing interpreted-constructor
  coverage; otherwise their intentional uncompiled-constructor assertions fail.
- /tmp/art-general-constructor-final.log full traced audit exit0:36cases PASS,
  baseline/optimized constructor installation and previous regressions PASS.
  DEX unchanged52classes1312/button109classes1731. Compiler/runtime/probe/link
  rebuilt; fmt/diff PASS. No APK edits or push.
- No claim of complete constructor coverage: handlers/monitors, unresolved
  allocation, interface/native calls, read barriers and real-app acceptance
  remain. Goal active.

### General catch/finally bodies — 2026-09-05

- Removed the fixed LL/static exception-handler shape check. Methods with
  handlers now pass through common opcode validation, including MOVE_EXCEPTION
  and existing monitor entry/exit lowering; read barriers remain restricted.
- Added array access with separate NPE/AIOOBE catches and a constructor call
  with IllegalArgumentException catch. Both include finally counter increment.
  54 interpreter-caller/baseline/optimized cases verify typed catch selection,
  normal values/reference payload, constructor failures and exactly-once finally.
  Called constructors may already be compiled by the preceding test.
- /tmp/art-handlers-test.log full audit exit0; new54cases and all existing
  regressions PASS. DEX52classes1314/button109classes1733; compiler/runtime/
  probe/link rebuilt; fmt/diff PASS. No APK edits or push.
- Nested rethrow/finally interactions, synchronized methods, broader monitor
  concurrency, interface/native dispatch, unresolved types, read barriers and
  actual-app acceptance remain. Full goal active.

### Nested finally exception replacement — 2026-09-05

- Added nested finally fixture that first throws an input Throwable, optionally
  replaces it in the inner finally, then optionally returns from outer finally.
  A decimal counter must become12 to prove inner-before-outer execution.
- 108 interpreter/baseline/optimized cases cover two distinct exception objects
  and null for both inputs, all four throw/return policies, exact propagated
  exception identity, NullPointerException for throw-null and return overriding
  a pending exception. Both JIT tiers require installed code.
- /tmp/art-nested-finally-test.log full audit exit0; new108cases and prior
  regressions PASS. No new backend changes needed. DEX52classes1315/button
  109classes1734; probe/link rebuilt; fmt/diff PASS. No APK edits or push.
- Synchronized methods, broader monitor concurrency, interface/native calls,
  unresolved types, barriers and actual-app acceptance remain. Goal active.

### Declared synchronized methods — 2026-09-05

- Removed blanket IsSynchronized rejection for managed methods/callees. Added
  resolved CONST_CLASS admission for static monitor owners. Native methods
  remain excluded. New DEX fixtures assert actual MONITOR_ENTER/EXIT opcodes
  and IsSynchronized for both static and instance declarations.
- 18 interpreter/baseline/optimized cases verify reference/null returns, GC
  within locks, reentrant ownership retained by outer JNI MonitorEnter and
  complete release after outer MonitorExit. Three additional static-class-lock
  contention runs observe another ART thread blocked on that exact object,
  still blocked during GC, then successful return and release after unlock.
- /tmp/art-synchronized-test.log full audit exit0, all new and existing
  regressions PASS. DEX52classes1317/button109classes1736; compiler/runtime/
  probe/link rebuilt; fmt/diff PASS. No APK modifications or push.
- Instance contention and synchronized exceptional exits need dedicated
  coverage beyond existing monitor-block tests. Interface/native calls,
  unresolved types, read barriers and actual-app acceptance remain. Goal active.

### Synchronized exceptional exits and instance contention — 2026-09-05

- Extended synchronized fixtures with static/instance throw methods. Tests
  cover original exception identity and throw-null NPE while requiring GC,
  retention of the outer reentrant monitor and complete release after exit.
- Monitor contention helper can now invoke an instance method on the locked
  receiver. Six static/instance contention runs across interpreter/baseline/
  optimized phases require blocked ART state, exact monitor object, GC while
  blocked, wakeup after unlock and released monitor.30serial cases include
  the previous18 normal returns plus12 exceptional exits.
- /tmp/art-synchronized-exception-test.log full audit exit0 and all regressions
  PASS. DEX52classes1319/button109classes1738; probe/link rebuilt; fmt/diff PASS.
  No backend change needed; no APK edits or push.
- Interface/native dispatch, unresolved types, barriers, broader concurrency
  and actual-app acceptance remain. Full goal active.

### Initial interface dispatch boundary — 2026-09-05

- Admitted resolved invoke-interface/range in common opcode and graph checks.
  Patch0069 decodes receiver before loading its class and decodes class after
  compressed inline-cache handling before loading native IMT. Existing AOSP
  method selection/hidden argument/call sequence retained. Compiler registered.
- Added JitCallable interface implemented by existing base/overriding child;
  explicitly included class file in both DEX builds.18 interpreter/baseline/
  optimized cases verify int override, reference identity and null exception;
  fixture checks actual invoke-interface DEX and both JIT tiers' installed code.
- /tmp/art-interface-test.log full traced audit exit0; new18cases and prior
  regressions PASS. DEX53classes1323/button110classes1742; compiler/runtime/
  probe/link rebuilt; fmt/diff PASS. No APK edits or push.
- This does not prove all IMT paths: collision trampoline, default methods,
  range/mixed arguments and dispatch across GC need further targeted coverage.
  Native calls, unresolved types, read barriers and actual-app acceptance also
  remain. Full goal active.

### Actual interface default method — 2026-09-05

- Added an interface default method that calls value() then adds101, plus an
  invoke-interface caller. Existing base/child implementations exercise default
  inheritance and nested dispatch to their distinct value overrides.
- Set both test DEX builds to min-api24/version37 so default methods are not
  desugared into helper functions. Updated exact contracts to observed51classes/
  1324methods and button107classes/1743methods; obsolete backport synthetics
  disappear. Probe also requires the resolved target IsDefault, an interface
  declaring class and actual code item before either JIT phase.
- /tmp/art-default-interface-test.log full traced audit exit0; interface27cases
  (including default9) and prior regressions PASS. /tmp/art-default-dex-final.log
  validates both DEX contracts. Probe/link rebuilt; fmt/diff PASS. No APK edits.
- No new backend change needed. IMT collision/default ambiguity, range/mixed
  calls, native dispatch, unresolved types, barriers and actual-app acceptance
  remain. Full goal active.

### Interface range with mixed arguments and callee GC — 2026-09-05

- Added range declaration to JitCallable using existing base/child implementations
  and an actual invoke-interface/range caller. Seven integer inputs plus long,
  double and reference exercise stack arguments alongside register banks.
- 144 interpreter/baseline/optimized cases vary null/base/child receiver, signed
  wide values, valid/invalid double, null/reference payload and valid/invalid
  final integer. Verify exact override result and actual GC in every nonnull
  receiver call. Null receiver requires NPE. Existing27interface cases retained.
- /tmp/art-interface-range-test.log full traced audit exit0; all regressions
  PASS. DEX37 counts51classes1326/button107classes1745; probe/link rebuilt;
  fmt/diff PASS. No new backend changes or APK edits.
- IMT collisions/default ambiguity, native calls, unresolved types, barriers
  and actual-app acceptance remain; full goal active.

### IMT collision receiver boundary fix — 2026-09-05

- Added hash-colliding Aa/BB interface methods with distinct401/809 results.
  Probe verifies equal IMT index40, distinct resolved methods and a real runtime
  conflict-table entry. Baseline compiled callers must contain the ARM64 IMT
  pointer load, preventing an inlined equivalent from satisfying this check.
- Initial collision run crashed. /tmp/art-imt-collision-lldb.log locates native
  artInvokeInterfaceTrampoline reading [x22] with x22=compressed0x101b4080.
  The assembly-to-C++ boundary passed the managed receiver as a native pointer.
- Patch0070 decodes that argument at runtime entry; registered in runtime patch
  manifest and actual quick_trampoline source rebuilt. AOSP conflict table
  search/population is retained, not bypassed with custom method dispatch.
- /tmp/art-imt-collision-fixed.log full traced audit exit0:45interface cases
  (including18collision cases),144range cases and all prior regressions PASS.
  DEX37 counts51classes1332/button107classes1751. Runtime/probe/link rebuilt;
  fmt/diff PASS. No APK edits or push.
- Mixed-argument conflict paths and default ambiguity still need coverage;
  native calls, unresolved types, read barriers and real-app acceptance remain.
  Full goal active.

### Mixed-argument IMT collision — 2026-09-05

- Added Aa/BB range overloads sharing an actual conflict entry at slot18. Both
  accept seven integers, long, double and reference, call a GC-performing
  target and return distinct signed results. Baseline callers must retain
  ARM64 IMT pointer loads, as in the earlier no-argument collision test.
- Range matrix now432cases across interpreter/baseline/optimized, including
 288new collision cases, null receivers, valid/invalid arguments, stack slots
  and callee GC. Existing45 interface/default/no-argument collision cases stay.
- /tmp/art-imt-range-test.log full traced audit exit0 and all regressions PASS.
  DEX37 counts51classes1338/button107classes1757; probe/link rebuilt; fmt/diff
  PASS. No additional backend fix needed, no APK modifications or push.
- Default ambiguity, native dispatch, unresolved types, read barriers and
  actual-app acceptance remain. Full goal active.

### JIT callers of static JNI methods — 2026-09-05

- Removed native-callee rejection for resolved static calls and static/direct
  HIR call validation. Existing JNI entrypoints handle native methods; native
  method compilation and virtual/direct native opcode admission remain separate.
- Replaced obsolete jitCallNative rejection test with required baseline and
  optimized caller installation. Added native reference-return caller followed
  by managed GC; nine input/phase combinations verify primitive16384 result,
  exact null/object/class identity and GC after JNI return. This does not test
  GC inside native code or wide native argument layouts yet.
- /tmp/art-native-call-test.log full traced audit exit0; new checks and all
  regressions PASS. DEX37 counts51classes1339/button107classes1758; compiler/
  runtime/probe/link rebuilt; fmt/diff PASS. No APK edits or push.
- Wide/FP/instance JNI, native callbacks/exceptions, unresolved types, barriers
  and actual-app acceptance remain. Full goal active.

### JIT packed JNI stack arguments — 2026-09-05

- Extended native-call acceptance to compile the existing nativeStackPcsRoundTrip
  caller in baseline and optimized tiers, requiring code-cache installation.
  Interpreter and both JIT tiers validate all four native checksums: integer
  spills with long/aligned tails, nine floats plus double, a spilled reference,
  and boolean/byte/char/short/int/long packed native stack arguments.
- /tmp/art-native-stack-test.log full traced audit exit0; packed JNI phases=3
  and existing primitive/reference-return/GC cases PASS, as do all regressions.
  Probe and link rebuilt; cargo fmt and git diff checks PASS. No DEX changes,
  backend changes, APK modifications or push in this increment.
- These are fixed native argument vectors, not exhaustive JNI coverage.
  Instance JNI, native callbacks/exceptions and GC during JNI still need tests;
  unresolved types, read barriers and actual-app acceptance remain. Full goal
  stays active; the capability gate has not been removed without proof.

### JNI callback GC and exception unwinding — 2026-09-05

- Added thread-local acceptance controls to nativeReferenceIdentity. In callback
  mode it calls Java jitGcTarget and records GC-count advancement before returning
  from native code, so the subsequent caller-side GC cannot satisfy this check.
  A second mode calls Java jitThrow with the returned reference and preserves the
  pending exception through native exit and the managed JIT caller.
- Interpreter/baseline-requested/optimized callers now run six callback cases:
  exact returned object identity and exact thrown IllegalArgumentException identity
  after in-callback GC. Unrelated native identity tests retain their original path.
  Callback target compilation is not independently required by this new matrix;
  the installed JIT frame tested here is the caller above the JNI transition.
- /tmp/art-native-callback-test.log full traced audit exit0, callback cases=6 and
  all prior regressions PASS. Probe/link rebuilt; fmt/diff PASS. No Java/DEX,
  backend or APK changes. Previous goal turn was progress (compiled packed JNI
  checks), and this turn adds evidence for reentrant JNI GC/exception behavior.
- Full goal remains active. Instance/virtual JNI dispatch, broader native root
  lifetime cases, unresolved types, barriers and real-app acceptance remain;
  these tests do not justify removing the remaining capability gate wholesale.

### Instance JNI direct/virtual admission — 2026-09-05

- Removed native-callee rejection from direct and virtual/interface opcode
  validation and virtual/interface HIR validation. Native methods themselves
  remain outside managed-method JIT compilation; calls use existing AOSP JNI
  entrypoints, without a substitute native dispatch implementation.
- Added public virtual and private direct native receiver-return methods plus
  managed callers. The acceptance test requires actual invoke-virtual/direct
  DEX opcodes and installed compiled callers, then verifies exact receiver
  identity or NullPointerException across interpreter and both requested JIT
  tiers (12 cases). Existing JNI callback/GC and packed-stack tests remain.
- /tmp/art-instance-jni-test.log full traced audit exit0, new12cases and all
  regressions PASS. Compiler3/runtime1 objects rebuilt; probe/link and fmt/diff
  PASS. DEX37 counts51classes1343 and button107classes1762. No APK modifications.
- Previous turn was progress (JNI callback evidence). Goal still active:
  polymorphic native overrides, instance argument layouts, unresolved paths,
  read barriers and actual-app acceptance need further work. These receiver
  tests alone are not exhaustive instance JNI or interface-native coverage.

### Native override polymorphism through virtual and interface calls — 2026-09-05

- Added nativeToken to JitCallable and native overrides on existing base/child
  classes. Separately registered native implementations return193/827. New
  managed callers retain invoke-virtual and invoke-interface DEX (asserted),
  and require installed baseline-requested/optimized compiled entrypoints.
- Eighteen cases cover null/base/child receivers through both call sites in
  interpreter and JIT phases. Distinct native results prove override selection;
  null must throw NPE. Existing interface conflict/default/range cases remain.
- /tmp/art-native-polymorphic-test.log full traced audit exit0; new18cases and
  complete prior regressions PASS. DEX37 counts51classes1348/button107classes1767;
  probe/link rebuilt; fmt/diff PASS. No additional backend fix or APK changes.
- Previous turn was progress (instance-native admission and tests). Full goal
  stays active. Remaining JNI work includes richer instance argument/GC shapes
  and binding failure paths; unresolved types, read barriers and actual-app
  acceptance still prevent any claim of complete pinned AOSP JIT support.

### JNI binding failure and recovery — 2026-09-05

- Native polymorphism acceptance now unregisters only the two fixture-class
  native tables per phase. Both virtual and interface callers must throw
  UnsatisfiedLinkError for both non-null receiver classes. RegisterNatives then
  restores distinct native overrides; the same callers are reused without an
  intervening CompileMethod call and return their expected values.
- /tmp/art-native-binding-test.log full traced audit exit0:12expected binding
  failures plus18override/null recovery cases across interpreter and requested
  JIT tiers, all prior regressions PASS. Probe/link rebuilt, fmt/diff PASS.
  No DEX/backend/APK changes. This exercises JNI native-symbol binding, not
  unresolved managed-method/type resolution or dynamic-library loading.
- Previous turn was progress (polymorphic native dispatch evidence). Full goal
  remains active; unresolved managed paths, read barriers and actual-app
  acceptance still need implementation/verification before gate removal.

### Unresolved class runtime loading admission — 2026-09-05

- Enabled HLoadClass kRuntimeCall (including its access-check path) in patch0066
  graph validation, using the already-present native-return reference encoding.
  Generic CONST_CLASS admission no longer demands a resolved type. Existing
  verified-class/method-compilability checks remain.
- Added compilation-only JitMissingType, deliberately omitted from DEX inputs,
  and jitMissingClass. Interpreter and installed JIT callers must each throw
  NoClassDefFoundError. This proves runtime resolution failure, not successful
  lazy loading or all class-loader/access-control cases.
- First runtime audit exposed a broader mismatch: unresolved class verification
  leaves SkipAccessChecks=false, and the Darwin gate rejected even arithmetic
  methods in the verified Hello class (status80). Removed that blanket rejection;
  AOSP compiler/runtime retain access checks instead of treating them as unsupported.
- /tmp/art-unresolved-class-access-test.log full traced audit exit0; missing-class
  phases=3 and all previous regressions PASS with the soft-verification fixture.
  Compiler/runtime/probe/link rebuilt; fmt/diff PASS. DEX37 counts51classes1349,
  button107classes1768. Initial patch context mismatch corrected before rebuild.
- Previous turn was progress (JNI binding recovery). Full goal remains active:
  successful lazy type loading, unresolved fields/methods, read barriers and
  actual-app acceptance still require implementation/evidence. No APK changes.

### Unresolved instanceof/check-cast admission — 2026-09-05

- Removed resolved-type prerequisite for INSTANCE_OF/CHECK_CAST while retaining
  the read-barrier restriction. AOSP graph construction and runtime type loading
  now handle absent target types rather than rejecting the entire JIT caller.
- Added jitMissingInstanceOf/jitMissingCast referencing the compilation-only,
  DEX-absent JitMissingType. Null and class-object inputs are first observed in
  the interpreter, then compared against installed baseline-requested/optimized
  callers. Both inputs throw NoClassDefFoundError for both operations in this
  pinned runtime, including null; all12cases match.
- /tmp/art-unresolved-typecheck-test.log full traced audit exit0, all regressions
  PASS. Compiler2/runtime1 objects rebuilt, probe/link and fmt/diff PASS. DEX37
  counts51classes1351/button107classes1770. No backend rewrite or APK changes.
- Previous turn was progress (runtime class-load/access-check admission). Full
  goal remains active. These missing-type checks do not establish successful
  lazy loading, unresolved member access, read-barrier support or real-app
  acceptance; those remain required before unrestricted AOSP JIT completion.

### Unresolved static-field runtime boundary — 2026-09-05

- Inspected AOSP GenerateUnresolvedFieldAccess and quick_field_entrypoints:
  static reference writes pass Object* in x1, reference reads return Object*
  in x0. Patch0071 adds ARM64 decode-before-set/encode-after-get and is wired
  into compiler build. AOSP field lookup, access checks and exceptions remain.
- Admitted unresolved static field bytecodes/HIR. Unresolved instance fields
  remain rejected pending receiver ABI work. Added missing-class int/reference
  get/set fixtures and required installed JIT callers; all12cases across
  interpreter and requested JIT tiers throw NoClassDefFoundError correctly.
- /tmp/art-unresolved-static-test.log full traced audit exit0; all regressions
  PASS. DEX37 counts51classes1355/button107classes1774. Compiler/runtime/probe/
  link rebuilt, fmt/diff PASS. No APK modifications.
- Missing-owner failures do not prove successful unresolved reference returns,
  writes with GC, field access violations or all typed field layouts. Those
  still need direct tests; read barriers and real-app acceptance also remain.
  Previous turn was progress (unresolved type checks); full goal stays active.

### Successful unresolved static-field access and GC — 2026-09-05

- Added runtime_jit_unresolved_static.h after cold-static initialization tests.
  Test-only access-flag restriction makes AOSP generate unresolved field access;
  compiled get/set must throw IllegalAccessError while private, then the same
  code must successfully operate after flags are restored. RAII restores flags
  on failure too; this is controlled fault injection, not APK mutation or a
  production access-control bypass.
- Long, bit-exact double (including negative zero), and null/object/class
  references round-trip through the runtime field entrypoints with verified GC
  between write and read. Two JIT tiers produce12access failures and18successful
  round trips. This proves dynamic field access and native reference returns,
  not just constant exception generation. GC during resolution is not asserted.
- Initial test setup incorrectly demanded visibly-initialized state, then
  baseline compile hit an already-optimized cache entry. Use initialized state
  and safely remove only fixture methods under suspend-all/GC critical section
  before each new compilation. No recompilation between access failure/recovery.
- /tmp/art-unresolved-static-success-final.log full traced audit exit0 and all
  regressions PASS. Probe/link rebuilt; fmt/diff PASS; DEX unchanged1355/1774.
  Previous turn was progress (static field boundary). Full goal remains active:
  unresolved instance/member paths, barriers and actual-app acceptance remain.

### Unresolved instance-field receiver boundary — 2026-09-05

- Extended patch0071 with instance receiver decode in x1, reference-store value
  decode in x2 and reference-return encode in x0. Admitted unresolved instance
  field opcodes/HIR, retaining read-barrier rejection. AOSP runtime still owns
  field lookup, access checks, null handling and stores/card marking.
- Added four external int/reference field accessors and an acceptance header.
  Test-only private access flags force runtime access; compiled get/set raise
  IllegalAccessError, then the same code after flag restoration checks null NPE
  and successful int/reference round trips with GC between write and read.
  Self-reference, null, class reference and negative integers are included.
- /tmp/art-unresolved-instance-test.log full traced audit exit0:8access failures,
  8null failures and12successful round trips across requested JIT tiers; all
  prior regressions PASS. DEX37 counts51classes1359/button107classes1778.
  Compiler/runtime/probe/link rebuilt; fmt/diff PASS. No APK modifications.
- Previous turn was progress (successful static runtime field access). Full goal
  remains active: wider instance field types, GC during resolution, unresolved
  method dispatch, read barriers and actual-app acceptance still need evidence.

### Unresolved static method dispatch — 2026-09-05

- Admitted missing static callees in generic and single-call bytecode gates;
  HInvokeUnresolved with kStatic now passes HIR validation. Uses AOSP's existing
  invoke-static access-check trampoline and managed calling convention, without
  inventing native-pointer result conversion for a managed tail call.
- Added missing-owner primitive/reference-return calls: interpreter and both
  requested JIT tiers raise NoClassDefFoundError (6 cases). Controlled private
  access on an existing static target yields IllegalAccessError, then the same
  compiled caller returns42 after access restoration (2+2 cases). Test-only
  method flag mutation clears the current interpreter resolution cache, and
  RAII restores flags. Fixture caller code is safely removed before recompile.
- /tmp/art-unresolved-call-test.log full traced audit exit0, all regressions PASS.
  DEX37 counts51classes1363/button107classes1782. Compiler/runtime/probe/link
  rebuilt; fmt/diff PASS. A probe namespace typo was fixed before final build.
  No APK modifications or host-runtime dispatch replacement.
- Previous turn was progress (instance field boundary). Full goal stays active:
  unresolved non-static calls need receiver decoding at the assembly/C++ boundary;
  invoke-super/custom/polymorphic, read barriers and actual-app acceptance remain.

### Unresolved non-static invocation receiver boundary — 2026-09-05

- Extended runtime patch0070 to decode compressed receivers at interface/direct/
  super/virtual access-check C++ entrypoints before constructing ObjPtr. Saved
  managed arguments remain compressed for AOSP argument visitors and tail calls;
  static entry still discards its unused receiver. No assembly argument rewrite.
- Admitted missing direct/virtual/interface callees and general HInvokeUnresolved
  without read barriers. Invoke-super bytecode itself still needs admission/work.
- Added controlled private-method access tests using existing virtual primitive
  and reference callers. Each requested JIT tier must raise IllegalAccessError,
  then after restoring flags raise NPE for null and return73/exact class reference
  for a valid receiver, without recompiling between those outcomes.
- /tmp/art-unresolved-virtual-test.log full traced audit exit0:4access,4null,
  4successful recovery cases plus all regressions PASS. Compiler/runtime2/probe/
  link rebuilt, fmt/diff PASS. DEX unchanged1363/1782; no APK modifications.
- Previous turn was progress (static resolution calls). Full goal remains active:
  direct/interface/super runtime dispatch matrices, GC during method resolution,
  read barriers, custom/polymorphic calls and actual-app acceptance remain.

### Unresolved interface invocation recovery — 2026-09-05

- Extended controlled access-change tests to interface primitive/reference
  callers. AOSP unresolved interface trampoline must enforce access checks,
  deliver NPE after restoration for null, then return73/exact class reference
  through the same installed caller. Combined virtual/interface matrix now has
  8access failures,8null failures and8successful recoveries across JIT tiers.
- First run passed the new matrix but conflicted with the later ordinary
  interface test's baseline installation (method already optimized). Moved
  unresolved-call acceptance after ordinary interface acceptance; each can now
  own its expected compilation state without suppressing either test.
- /tmp/art-unresolved-interface-final.log full traced audit exit0 and all
  regressions PASS. Probe/link rebuilt; fmt/diff checks PASS. No DEX/backend/
  APK changes. Previous turn was progress (non-static receiver boundary).
- Full goal remains active: unresolved direct/super and mixed arguments/GC,
  invoke-custom/polymorphic, read barriers and actual-app acceptance remain.

### Invoke-super admission and parent selection — 2026-09-05

- Admitted invoke-super and range forms alongside direct invocation validation.
  Added child superValue/superReference with asserted invoke-super DEX. Parent
  and overriding child return deliberately distinct values/references, proving
  super dispatch must select the parent rather than the virtual override.
- Interpreter2 and resolved/unresolved JIT8 parent-result cases pass. Controlled
  private parent target also causes4IllegalAccessErrors, followed by successful
  parent selection through the same compiled code after flag restoration.
- Initial fault-injection run retained the caller's verifier access-elision bit,
  so private access was not rechecked. The test now invalidates SkipAccessChecks
  with the access mutation and restores both flags via RAII. Production access
  semantics are unchanged; this models a caller requiring runtime access checks.
- /tmp/art-super-retest.log full traced audit exit0 and all regressions PASS.
  DEX37 counts51classes1365/button107classes1784; compiler/runtime/probe/link
  rebuilt; fmt/diff PASS. No APK edits. Range-super mixed arguments are not yet
  independently tested. Previous turn was progress (interface runtime recovery).
- Full goal remains active: direct/range invocation matrices, GC during
  resolution, invoke-custom/polymorphic, read barriers and real-app acceptance.

### Mixed-argument invoke-super/range and callee GC — 2026-09-05

- Added child superRange forwarding seven ints, long, double and reference to
  the parent; DEX must use invoke-super/range. Parent returns wide or-1, while
  the child override negates it, so the expected result distinguishes dispatch.
- Matrix varies wide sign, floating value, null/non-null reference and final
  integer. Every call must advance GC count inside the parent target. Initial
  interpreter/resolved-JIT48cases passed; extended with two unresolved JIT tiers
  using controlled access restriction/restoration, adding32cases and2expected
  IllegalAccessErrors before recovery without recompilation.
- /tmp/art-super-range-unresolved-test.log full traced audit exit0, total80range
  cases plus earlier super checks and all regressions PASS. DEX37 counts51classes
  1366/button107classes1785. Probe/link rebuilt after the final extension;
  fmt/diff PASS. No new backend changes or APK edits.
- Previous turn was progress (super dispatch admission). Full goal remains
  active: direct invocation matrices, GC during resolution itself, custom/
  polymorphic invokes, read barriers and actual-app acceptance remain required.

### Remove arbitrary 256-unit method-size rejection — 2026-09-05

- Removed the blanket Darwin JIT code-size cap of256DEX units. Method validity,
  opcode/ABI admission and AOSP compiler resource decisions remain; large methods
  no longer fail solely because they exceed this port-specific small-method cap.
- Added96unrolled arithmetic steps producing571DEX units, asserted above the old
  limit. Interpreter and installed baseline-requested/optimized JIT results are
  compared with an independent uint32 wrapping calculation for zero, positive,
  negative, INT_MIN/INT_MAX and patterned inputs (18cases).
- /tmp/art-large-method-test.log full traced audit exit0 and all regressions PASS.
  DEX37 counts51classes1367/button107classes1786; compiler/runtime/probe/link
  rebuilt; fmt/diff PASS. No APK edits. This tests large straight-line arithmetic,
  not exhaustive huge-method branches/register pressure or all runtime features.
- Previous turn was progress (super/range GC matrix). Full goal stays active:
  remaining invoke families, read barriers, GC-resolution cases and actual-app
  acceptance still prevent unrestricted pinned AOSP JIT completion.

### Array literal bytecode admission — 2026-09-05

- Admitted fill-array-data, filled-new-array and range forms plus array payload
  signatures. AOSP lowers them to its existing HNewArray/HArraySet sequence;
  no custom native array-fill implementation was added. Read-barrier restrictions
  remain while allocation/reference-store ABI uses the existing ported backend.
- Added byte/short/int/long/float/double and reference literal fixtures. DEX
  opcode assertions establish fill payload widths1/2/4/8, filled int/reference
  and filled reference range. D8 emits filled-new-array for the short int literal,
  so corrected its expected opcode and added float to explicitly cover width4.
- /tmp/art-array-literals-final.log full traced audit exit0:24interpreter/JIT
  cases, exact values/FP signed-zero bits/reference identities after verified GC;
  all regressions PASS. DEX37 counts51classes1375/button107classes1794. Compiler/
  runtime/probe/link rebuilt, final DEX/probe relink repeated after float addition;
  fmt/diff PASS. No APK edits.
- Previous turn was progress (method-size limit removal). Full goal stays active:
  malformed/bounds payload exceptional cases, remaining invoke families, read
  barriers, GC-resolution paths and actual-app acceptance still need evidence.

### Invoke-polymorphic initial AOSP runtime path — 2026-09-05

- Test D8 minimum API raised24→26 and exact DEX contract37→38 so real
  invoke-polymorphic instructions are emitted. New fixture obtains a MethodHandle
  using platform MethodHandles.lookup/findStatic/MethodType and invokes exact(int)int.
- Admitted polymorphic bytecodes/HIR. Runtime patch0072 decodes the raw receiver
  before creating AOSP handles and compresses reference results at managed return.
  Compiler patch0073 selects AOSP's general polymorphic runtime call on Darwin;
  it deliberately does NOT claim the unported MethodHandle intrinsic/hidden-receiver
  path is ready. Completing that optimization remains part of the full goal.
- /tmp/art-polymorphic-test.log full traced audit exit0:18independently checked
  wrapping-int results and3null failures across interpreter and installed JIT
  callers, all prior regressions PASS under DEX38. Compiler/runtime/probe/link
  rebuilt; fmt/diff PASS. DEX counts51classes1382/button107classes1801.
- No APK edits. Previous turn was progress (array literal admission). Full goal
  active: polymorphic reference/wide/range/adaptation/GC/VarHandle coverage and
  intrinsic implementation, invoke-custom, read barriers and actual-app acceptance
  remain. General runtime dispatch alone is not full AOSP JIT completion.

### Polymorphic reference return, callee GC and wrong-type exceptions — 2026-09-05

- Added a platform MethodHandle for existing jitGcTarget(Object)Object and an
  invokeExact reference caller with asserted invoke-polymorphic DEX. Null,
  allocated object, class and handle-self arguments return identical references
  after verified callee GC across interpreter and installed JIT tiers (12cases).
- Null handles and the existing incompatible(int)int handle must throw NPE and
  WrongMethodTypeException respectively (6cases), exercising exceptional return
  through the same polymorphic entrypoint. The prior integer matrix remains.
- /tmp/art-polymorphic-reference-test.log full traced audit exit0 and all
  regressions PASS. DEX38 counts51classes1384/button107classes1803. DEX/probe/
  link rebuilt; fmt/diff PASS. No backend changes or APK edits this increment.
- Previous turn was progress (polymorphic runtime admission). Full goal remains
  active: wide/range/adaptation/VarHandle cases, intrinsic/hidden-receiver path,
  custom invocation, read barriers and actual-app acceptance still need work.

### AOSP MethodHandle invokeExact intrinsic restored — 2026-09-05

- Replaced patch0073's Darwin intrinsic disable with narrow ARM64 address
  conversions. Managed MethodHandle/receiver/type references stay compressed;
  a dedicated temporary decodes heap addresses, and vtable/IMT class temporaries
  are decoded only for native memory access. MethodType runtime returns are
  encoded at the HIR boundary. AOSP dispatch and target invocation are retained.
- Extended runtime patch0072 to decode the hidden receiver and encode reference
  results in the intrinsic slow entrypoint. Fixed overlapping/zero-context runtime
  patch hunks after the first build rejected them; subsequent build/link passed.
- Added real findVirtual handles for base-class and interface value() methods.
  One handle is exercised on base and overriding child objects, plus null receiver,
  across interpreter/baseline-requested/optimized callers:12dispatch and6exception
  cases. Existing18integer,12reference/callee-GC and9null/wrong-type cases pass.
- /tmp/art-polymorphic-dispatch-test.log full traced audit exit0; code-generation
  logs explicitly confirm intrinsic selection for all four caller methods at both
  requested JIT tiers. /tmp/art-polymorphic-intrinsic-test.log was the earlier
  static/reference-only pass. DEX38 now51classes1389/button107classes1808.
  Compiler/runtime/probe/link rebuilt; fmt/diff checks PASS. No APK edits.
- Full goal remains active, not complete. These cases do not establish wide/range
  register spills, accessor/adapted handles, successful reference-return slow
  paths, VarHandle, invoke-custom, read barriers or real-app acceptance. Next work
  should extend polymorphic ABI coverage before broadening the admission gate.

### MethodHandle range, wide values and spilled handle — 2026-09-05

- Previous goal turn made progress (restored AOSP invokeExact intrinsic and
  verified virtual/interface dispatch). Continued without changing full scope.
- Added a platform findStatic handle for the existing GC-running jitRangeTarget
  and a real invoke-polymorphic/range caller. Seven ints, long, double and an
  object exhaust core argument registers; the trailing intrinsic MethodHandle
  itself is passed on the stack. Code-generation trace now records stack_handle.
- /tmp/art-polymorphic-range-test.log full audit exit0:72range cases across
  interpreter/baseline-requested/optimized callers verify signed long boundaries,
  nontrivial long bits, FP value, spilled int/reference arguments and a GC during
  each target call. Six null/wrong-type failures pass. Both JIT compilations log
  stack_handle=1, establishing that the extra temp6 load path is exercised.
- DEX38 count51classes1392/button107classes1811, compiler/probe/link rebuilt.
  Existing regressions PASS; fmt/diff checks PASS. No APK changes. No further
  backend fix was required by this matrix; it validates the prior address patch.
- Full goal remains active. This uses one FP argument, so FP register overflow
  and floating return bits still need coverage, along with adapted/accessor
  handles, successful reference slow returns, VarHandle, custom invokes, read
  barriers and actual-app acceptance. Do not infer full support from this pass.

### MethodHandle FP register overflow and exact return bits — 2026-09-05

- Previous turn made progress (integer/wide range and spilled MethodHandle).
  Added nine-float and nine-double platform findStatic/invokeExact fixtures;
  asserted real invoke-polymorphic/range DEX. AOSP ARM64 argument visitor uses
  stack slots after exhausting its FP argument registers. The ninth FP argument
  is returned unchanged after GC; the first eight and an object are checked.
- New runtime_jit_polymorphic_fp.h tests raw bits for positive/negative zero,
  minimum subnormal, infinity, quiet NaN payload and maximum finite value, plus
  null-reference sentinel results. Four phases: interpreter, baseline-requested
  caller, optimized caller, then optimized target as well. Requested baseline
  can install optimized code (observed in trace); this is not proof of a distinct
  baseline backend. GC count must increase inside every successful invocation.
- /tmp/art-polymorphic-fp-test.log full audit exit0:96FP/GC cases and16null/
  wrong-type exceptions PASS with all earlier regressions. Actual code-cache
  installation and intrinsic selection are logged; target compilation confirmed.
  No additional backend change was needed. DEX38 counts51classes1398/button
  107classes1817; DEX/probe/link rebuilt; fmt/diff PASS. No APK edits.
- Full goal stays active. Next gaps include adapted/accessor MethodHandles and
  successful reference-return slow paths, VarHandle, invoke-custom, read barriers,
  remaining OSR/inlining combinations and actual Blue Archive/app acceptance.

### Adapted MethodHandle exposes reference-copy bug — 2026-09-05

- Previous turn made progress (FP spill/bit-pattern matrix). Added asType adapter
  from(Hello)Object to(Object)Object, asserting kInvokeTransform and real
  invoke-polymorphic DEX. Target calls GC. Interpreter, requested baseline caller,
  optimized caller, then optimized target must preserve null/object identity and
  reject incompatible Class/MethodHandle arguments with ClassCastException.
- Initial test failed even at interpreter phase0: non-null input returned null
  despite GC running. /tmp/art-polymorphic-adapted-input.log traces showed the
  reference reached transform/frame input but inner target result was zero.
  CopyArgumentsFromCallerFrame compared a32-bit vreg to a native64-bit pointer,
  misclassifying non-null references as primitive values. Compiled targets could
  hide this because raw vregs retain bits; interpreted targets need reference slots.
- Fixed patch0072 to compare CompressedReference::FromMirrorPtr(...).AsVRegValue()
  while retaining AOSP stale-reference discrimination. Added method_handles.cc
  to staging manifest AND patched compile-source list. Removed all temporary
  runtime traces. Initial patch/build failures were corrected before final run.
- /tmp/art-polymorphic-adapted-fix.log full audit exit0:8adapted reference/GC
  successes,8cast failures and all earlier regressions PASS. Intrinsic selection
  and target code-cache installation logged. Successful hidden-receiver reference
  returns now exercised. DEX38 counts51classes1402/button107classes1821; DEX/
  runtime/probe/link rebuilt; fmt/diff PASS. APKs unchanged.
- Full goal active. Accessor handles, broader adaptations/inexact invoke,
  VarHandle/custom invokes, read barriers, OSR/inlining and actual-app acceptance
  remain. interpreter_common.cc has a similar comparison using reinterpret_cast32
  (not the full pointer); review its debug/address assumptions in subsequent work.

### Inexact MethodHandle.invoke conversions — 2026-09-05

- Previous goal turn made progress by fixing MethodHandle shadow-frame reference
  copying. Added a real MethodHandle.invoke (not invokeExact) caller with int
  argument and long return; DEX invoke-polymorphic and installed JIT code asserted.
- Existing(int)int arithmetic target exercises return widening with independent
  wrapping arithmetic/sign extension checks. Existing(Object)Object GC target
  forces int boxing, reference passage, then unboxing/widening to long. Null
  handles throw NPE; the Hello-only adapted target rejects boxed ints with CCE.
- /tmp/art-polymorphic-inexact-test.log full audit exit0:18widening,18boxing/
  unboxing/callee-GC and6exception cases across interpreter/requested baseline/
  optimized caller phases, plus all existing regressions PASS. No backend change
  was needed for this matrix. DEX38 counts51classes1404/button107classes1823;
  DEX/probe/link rebuilt; fmt/diff PASS. No APK changes.
- Full goal remains active. Accessor handles, VarHandle/custom invoke, read
  barriers, broader OSR/inlining and actual-app acceptance remain incomplete.
  Confirmed reinterpret_cast32(pointer) uses dchecked_integral_cast<uint32_t>,
  so interpreter_common.cc's analogous comparison still needs a debug-build-safe
  compressed-reference conversion; release truncation alone is insufficient.

### Interpreter register copy uses compressed representation — 2026-09-05

- Previous turn made progress (inexact invocation/boxing matrix). Replaced
  interpreter_common.cc AssignRegister's reinterpret_cast32(native_pointer) with
  CompressedReference::FromMirrorPtr(...).AsVRegValue(), preserving AOSP's stale
  reference discrimination while removing a debug checked-narrowing assumption.
  Patch0074 is staged and its source actually compiled by build-interpreter-core.
- Added patch0074 to interpreter_core_inputs. The graph regression test explicitly
  requires this patch on the interpreter archive edge and verifies final graphics
  link dependency, so later patch edits cannot silently reuse the old archive.
- /tmp/art-interpreter-reference-copy-build.log rebuilt7interpreter objects;
  /tmp/art-interpreter-reference-copy-test.log full traced audit exit0 with prior
  adapted/inexact and other regressions PASS. The targeted graph test also passed
  (/tmp/art-interpreter-reference-graph-test.log); fmt/diff PASS.
- No APK/DEX changes. Entire Debug runtime execution was NOT performed; this is
  source/production-build verification of the corrected representation, not proof
  that all debug ART invariants pass. Full goal stays active; accessor handles,
  VarHandle/custom invoke, read barriers, OSR/inlining and app acceptance remain.

### MethodHandle reference field accessors — 2026-09-05

- Previous turn made progress (interpreter reference-copy correction). Added
  findGetter/findSetter/findStaticGetter/findStaticSetter for dedicated Hello
  reference fields. Asserted the four actual AOSP accessor kinds and real
  invoke-polymorphic caller DEX; zero-argument getter and void setter included.
- New accessor matrix uses independent JNI field access as a cross-check:
  MethodHandle store→GC→JNI/MethodHandle read, then JNI store→MethodHandle read.
  Null, receiver-self and class values across interpreter/requested baseline/
  optimized callers produce18GC round-trips and18reverse cross-checks;6null
  instance receiver exceptions also pass. Payload JNI locals remain roots, so
  this alone does not prove remembered-set correctness with otherwise-unrooted
  young objects; do not claim comprehensive write-barrier verification.
- /tmp/art-polymorphic-accessors-test.log full audit exit0, all earlier regressions
  PASS. Intrinsic selection/code-cache installation confirmed for all four wrappers.
  No backend change required. DEX38 counts51classes1414/button107classes1833;
  DEX/probe/link rebuilt, fmt/diff PASS. No APK edits.
- Full goal active. Primitive/wide/volatile accessor combinations and cold class
  initialization need broader evidence; VarHandle/custom invoke, read barriers,
  remaining OSR/inlining and actual-app acceptance are still incomplete.

### VarHandle intrinsic address port begins — 2026-09-05

- Previous turn made progress (MethodHandle reference accessors). Added actual
  VarHandle int field get/set fixtures and independent JNI checks. Java8 Lookup
  compile API hides findVarHandle, so only factory uses reflection to invoke the
  real runtime API. Access operations remain real invoke-polymorphic instructions.
- Initial /tmp/art-varhandle-test.log failed after JIT installation. LLDB report
  /tmp/art-varhandle-lldb.log stopped at ldp w16,w17,[x1,#0x10] with compressed
  x1=0x101a88e0, proving raw heap-reference dereference in metadata checks.
- Patch0075 ports VarHandle metadata loads/subtype walking and native target
  addresses. Checks reuse the not-yet-live offset temp; memory operations keep
  managed object references intact and decode only computed native addresses.
  Atomic address/byte-view helpers receive the same conversion, but are not yet
  validated by this initial plain-int matrix. Existing AOSP access checks and
  instruction/memory-order choices remain; no intrinsic-disabling workaround.
- /tmp/art-varhandle-final-test.log full traced audit exit0:18get/set/GC,
  18independent JNI-store/read and12null failures PASS, all older regressions PASS.
  Trace confirms VarHandle intrinsic generation at both requested JIT tiers.
  DEX38 counts51classes1419/button107classes1838; compiler/probe/link rebuilt.
  Corrected patch-context whitespace before final rebuild. fmt/diff PASS.
- Registered0075 and previously missing0063–0066/0068/0069/0071/0073 in the JIT
  graph inputs. Graph regression now checks every patch registered in jit.rs is
  present on the JIT archive edge; /tmp/art-varhandle-graph-test.log PASS.
- Full goal active: VarHandle CAS/exchange/update, ordering/concurrency, reference/
  wide/static/array/view paths need execution evidence and any further fixes.
  invoke-custom, read barriers, OSR/inlining and actual-app acceptance remain.
  No APK changes; plain-int success does not establish full VarHandle support.

### VarHandle integer atomics and attached-thread contention — 2026-09-05

- Previous turn made progress (VarHandle native-address port). Added real
  compareAndSet, compareAndExchange, getAndAdd, getAndSet and getAndBitwiseXor
  wrappers. DEX invoke-polymorphic and code-cache installation asserted. Across
  interpreter/requested baseline/optimized callers:126operations check both
  return values and independent JNI field state, including CAS/exchange mismatch,
  signed boundaries and wrapping add;30null receiver/handle failures PASS.
- Added4attached ART native worker threads using the installed optimized add
  caller with shared global JNI references. Start barrier,512increments each:
  final field2048 plus exactly-once previous values0..2047, no duplicate/missing
  returns. Observed4overlapping call windows. This establishes concurrent caller
  correctness for this stress case, not a physical-core utilization measurement.
- /tmp/art-varhandle-contention-test.log full audit exit0, sequential and concurrent
  matrix PASS with all prior regressions. /tmp/art-varhandle-atomics-test.log was
  the earlier sequential pass. Intrinsic generation logged for all five operations.
  DEX38 counts51classes1429/button107classes1848; DEX/probe/link rebuilt; fmt/diff
  PASS. No further backend fix needed for these paths; no APK edits.
- Full goal remains active: weak CAS and ordering variants/publication litmus,
  reference/wide/static/array/view VarHandles, read barriers, invoke-custom,
  remaining OSR/inlining and real-app acceptance still require work. One add
  contention case is not comprehensive atomic or memory-order verification.

### VarHandle release/acquire and volatile publication — 2026-09-05

- Previous turn made progress (integer atomics and concurrent add). Added compiled
  publisher with a plain payload store followed by setRelease/setVolatile, and
  observer with getAcquire/getVolatile followed by conditional plain payload read.
  Both wrappers assert two real invoke-polymorphic instructions in their DEX.
- Two attached ART threads exchange1024messages with an acknowledgement channel
  before reusing the payload. A matched signal with stale payload fails immediately,
  rather than retrying until visibility catches up. No native per-message mutex or
  success flag performs the handoff; both channels use the Java VarHandle methods.
  Interpreter/requested baseline/optimized phases and both modes:6144handoffs PASS.
- Added emitted-code checks: each installed publisher/observer must contain at
  least two STLR/LDAR32instructions (for its two ordering branches). These pass
  alongside the concurrent test. This is not a proof of every JMM ordering case
  or physical-core parallelism; store-buffering/weak-CAS variants remain separate.
- /tmp/art-varhandle-ordering-code-test.log full traced audit exit0; all prior
  regressions PASS. Earlier /tmp/art-varhandle-ordering-test.log lacked machine
  instruction assertions. DEX38 counts51classes1435/button107classes1854; DEX/
  probe/link rebuilt, fmt/diff PASS. No additional backend fix or APK edits.
- Full goal stays active: reference/wide/static/array/view VarHandles, weak and
  other ordering variants, read barriers, invoke-custom, remaining OSR/inlining
  and actual-app acceptance remain incomplete.

### VarHandle reference access and atomics — 2026-09-05

- Previous turn made progress (release/acquire and volatile publication). Added
  an Object field VarHandle plus get/set/CAS/compareAndExchange/getAndSet wrappers.
  Factory reflection only bridges the Java8 fixture compiler's missing Lookup API;
  all five accessors are real invoke-polymorphic DEX and runtime VarHandle objects.
- Each phase (interpreter/requested baseline/optimized) tests null, receiver-self
  and Class references. Setter results are checked by JNI and VarHandle getter;
  CAS/exchange mismatch and success plus swap return exact object identities.
  GC runs before every independent read:54operation/GC checks and30null receiver/
  handle failures PASS. Full GC plus rooted payloads does not independently prove
  a generational/concurrent remembered-set barrier; keep that claim out of scope.
- /tmp/art-varhandle-reference-test.log full traced audit exit0 and all older
  regressions PASS. Compiler trace explicitly confirms intrinsic generation for
  get/set/CAS/exchange/swap at both requested tiers. No further backend change was
  required after patch0075. DEX38 counts51classes1441/button107classes1860;
  DEX/probe/link rebuilt; fmt/diff PASS. No APK changes.
- Full goal stays active. long/double/static/array/byte-view VarHandles, weak and
  remaining ordering variants, read barriers, invoke-custom, OSR/inlining and
  actual-app acceptance remain incomplete.

### VarHandle 64-bit instance and static fields — 2026-09-05

- Previous turn made progress (reference access and atomics). Added long instance
  and static fields, real findVarHandle/findStaticVarHandle factories, and
  get/set/CAS/getAndAdd wrappers. The Java8 fixture compiler still requires
  reflection only for the factory lookup; all eight accessors contain real
  invoke-polymorphic or invoke-polymorphic/range DEX instructions.
- Interpreter/requested baseline/optimized phases cover zero, signs, both signed
  limits and a nontrivial 64-bit pattern. JNI field reads independently validate
  VarHandle stores; JNI stores validate VarHandle reads. CAS mismatch/success and
  get-and-add return/state are checked bit-exactly, including unsigned wrap, and
  reads run after explicit GC:180 instance/static operation groups PASS.
- Null handle for all eight wrappers and null receiver for the four instance
  wrappers produce NPE in every phase:36 failures PASS. Trace records all eight
  wrappers as actual ARM64 VarHandle intrinsics at both requested JIT tiers.
- /tmp/art-varhandle-long-test3.log completed the full acceptance body with all
  prior regressions passing. DEX38 counts51classes1450/button107classes1869;
  DEX/probe/link rebuilt. cargo fmt and diff checks PASS. No backend patch or APK
  change was needed after patch0075. Initial probe-only failures were corrected:
  wide CAS uses invoke-polymorphic/range, so DEX validation now scans the complete
  method and accepts both real polymorphic encodings rather than assuming opcode0.
- Full goal remains active. double, array and byte-view VarHandles, weak/remaining
  ordering variants, read barriers, invoke-custom, remaining OSR/inlining and
  unrestricted actual-app acceptance are still incomplete.

### VarHandle primitive/reference arrays and covariant slow path — 2026-09-05

- Previous turn made progress (long instance/static fields). Added real
  MethodHandles.arrayElementVarHandle factories and int[]/Object[] wrappers for
  get/set/CAS plus getAndAdd or getAndSet. All eight methods contain actual
  invoke-polymorphic or range DEX and install intrinsic code at requested baseline
  and optimized tiers; trace records each intrinsic twice.
- Interpreter/baseline/optimized phases test every element of a five-entry int[]
  with signed limits, JNI read/write cross-checks, explicit GC, CAS mismatch/
  success and wrapping getAndAdd:75 operation groups PASS. Object[] null/receiver/
  Class identities survive set/get, CAS and swap plus GC:108 groups PASS.
- Added the AOSP-intended non-exact-array slow path: an Object[] VarHandle operates
  on an actual String[]. Valid String set/get and CAS transitions pass9 groups;
  invalid Hello storage throws ArrayStoreException without modifying the element.
  Null handle/array and negative/length indexes across all eight operations and
  three execution phases produce the required exceptions:99 failures PASS.
- /tmp/art-varhandle-array-test2.log completed the full acceptance body and all
  older regressions. DEX38 counts51classes1459/button107classes1878; DEX/probe/
  link rebuilt; cargo fmt and diff checks PASS. Patch0075 already covered the
  array metadata and computed-address conversion, so no new backend patch or APK
  change was required.
- Full goal remains active. Remaining primitive/FP and byte-array/ByteBuffer view
  VarHandles, weak/remaining ordering modes, read barriers, invoke-custom,
  remaining OSR/inlining and unrestricted actual-app acceptance are incomplete.

### VarHandle double fields, static fields and arrays — 2026-09-05

- Previous turn made progress (primitive/reference arrays). Added double instance
  and static fields plus double[] accessors. All three target shapes use real
  VarHandles and get/set/CAS/getAndAdd invoke-polymorphic DEX; all12 wrappers
  install as ARM64 intrinsics in requested baseline and optimized tiers.
- Tests use raw IEEE-754 patterns for +0, -0, finite signs, maximum finite,
  infinities and a noncanonical NaN payload. JNI cross-checks and post-GC reads
  remain bit exact; CAS distinguishes signed zero and NaN payload bits as AOSP
  specifies. Finite getAndAdd verifies both old FP return and rounded stored bits.
- Interpreter/baseline/optimized phases pass222 instance/static operation groups,
  75 double[] groups and84 null receiver/handle plus negative/length boundary
  failures. /tmp/art-varhandle-double-test.log completes the full acceptance body
  with all older regressions; trace records every wrapper intrinsic twice.
- DEX38 counts51classes1472/button107classes1891; DEX/probe/link rebuilt. cargo
  fmt and diff checks PASS. Patch0075 already handled FP atomic native addresses,
  so no new compiler patch, fallback, eligibility exception or APK edit was used.
- Full goal remains active. float and narrow primitive/view VarHandles, byte-array/
  ByteBuffer views, weak/remaining ordering modes, read barriers, invoke-custom,
  remaining OSR/inlining and unrestricted actual-app acceptance are incomplete.

### VarHandle float fields, static fields and arrays — 2026-09-05

- Previous turn made progress (double field/static/array). Added float instance,
  static and float[] get/set/CAS/getAndAdd wrappers using real VarHandles and
  signature-polymorphic DEX. This independently exercises ARM64 S-register moves
  and 32-bit atomic loops rather than inferring support from double.
- Raw IEEE-754 patterns cover +0/-0, finite signs, maximum finite, infinities and
  a noncanonical NaN payload. JNI cross-checks and post-GC reads preserve exact
  bits; CAS distinguishes signed zero/NaN payloads; finite atomic add validates
  both returned old bits and correctly rounded stored float bits.
- Interpreter/requested baseline/optimized phases pass222 instance/static groups,
  75 float[] groups and84 null receiver/handle plus negative/length boundary
  failures. All12 wrappers are logged as actual VarHandle intrinsics at both JIT
  tiers. /tmp/art-varhandle-float-test.log completes all older regressions.
- DEX38 counts51classes1485/button107classes1904; DEX/probe/link rebuilt. cargo
  fmt and diff checks PASS. No compiler patch, fallback, gate exception or APK
  edit was needed beyond the existing native-address port.
- Full goal remains active. narrow primitive and byte-array/ByteBuffer view
  VarHandles, weak/remaining ordering modes, read barriers, invoke-custom,
  remaining OSR/inlining and unrestricted actual-app acceptance are incomplete.

### VarHandle byte and boolean fields/static/arrays — 2026-09-05

- Previous turn made progress (float field/static/array). Added byte and boolean
  instance/static/array VarHandles. Byte accessors cover get/set/CAS/getAndAdd;
  boolean accessors cover get/set/CAS/getAndBitwiseXor. All24 accessors use real
  signature-polymorphic DEX and install as ARM64 intrinsics at both requested tiers.
- Byte cases include zero, signs, signed limits and a mixed bit pattern. JNI
  field/array cross-checks, explicit GC, CAS mismatch/success and modulo-256 add
  pass270 operation groups across interpreter/baseline/optimized execution.
  Boolean false/true set/get/CAS and XOR return/state normalization pass90 groups.
- Null handle/receiver/array and negative/length indexes across all target shapes,
  operations, types and phases produce the required exceptions:168 failures PASS.
  /tmp/art-varhandle-narrow8-test.log completes all prior regressions and confirms
  every one of the24 wrappers has exactly two intrinsic-generation records.
- DEX38 counts51classes1511/button107classes1930; DEX/probe/link rebuilt. cargo
  fmt and diff checks PASS. Existing patch0075 covers the native 8-bit addresses;
  no compiler fallback, eligibility exception, new backend patch or APK edit used.
- Full goal remains active. short/char and byte-array/ByteBuffer view VarHandles,
  weak/remaining ordering modes, read barriers, invoke-custom, remaining OSR/
  inlining and unrestricted actual-app acceptance are incomplete.

### VarHandle short and char fields/static/arrays — 2026-09-05

- Previous turn made progress (byte/boolean target shapes). Added signed short and
  unsigned char instance/static/array VarHandles with get/set/CAS/getAndAdd. All24
  accessors contain real polymorphic DEX and install as ARM64 intrinsics at both
  requested compilation tiers.
- Short cases cover zero, signs, signed limits and mixed bits; JNI field/array
  cross-checks prove correct sign extension and modulo-65536 atomic add across
  interpreter/baseline/optimized execution:270 groups PASS. Char covers values on
  both sides of0x8000 through0xffff and proves zero extension plus unsigned wrap:
  another270 groups PASS. Explicit GC runs before compiled reads.
- Null handle/receiver/array and negative/length indexes across both types, all
  target shapes, operations and phases produce the required exceptions:168 PASS.
  /tmp/art-varhandle-narrow16-test.log completes older regressions; all24 methods
  have exactly two intrinsic-generation records.
- DEX38 counts51classes1537/button107classes1956; DEX/probe/link rebuilt. cargo
  fmt and diff checks PASS. No compiler fallback, new eligibility exception,
  backend patch or APK edit was used.
- Full goal remains active. Byte-array/ByteBuffer view VarHandles, weak/remaining
  ordering modes, read barriers, invoke-custom, remaining OSR/inlining and
  unrestricted actual-app acceptance are incomplete.

### VarHandle byte-array and ByteBuffer int/long views — 2026-09-05

- Previous turn made progress (short/char fields and arrays). Added actual
  MethodHandles byteArrayViewVarHandle and byteBufferViewVarHandle factories plus
  int/long get, set, compareAndSet and getAndAdd signature-polymorphic wrappers.
  All16 wrappers contain invoke-polymorphic DEX and each installs twice as an
  ARM64 VarHandle intrinsic at requested baseline and optimized tiers.
- Byte-array views pass little/big-endian byte-level cross-checks, signed limits,
  CAS mismatch/success, wrapping atomic add, manually encoded backing bytes and
  explicit GC:150 int and150 long operation groups. AOSP's alignment contract is
  also enforced:12 unaligned plain get/set groups pass while unaligned atomics,
  null handles/arrays and both index boundaries produce204 expected exceptions.
- ByteBuffer views independently cover heap and real direct buffers in both byte
  orders:180 int and180 long operation groups,24 unaligned plain groups and480
  null/bounds/read-only/unaligned-atomic exceptions pass across interpreter,
  baseline and optimized execution. The initial direct-buffer run exposed an
  actual Darwin port bug: upstream truncated Buffer.address to uint32_t under its
  Android low-address contract. Patch0076 preserves the full intptr_t only on
  Darwin, and the patched mirror/var_handle.cc is now an explicit runtime build
  input with sibling-header-safe -iquote selection. The rebuild touched9 objects.
- /tmp/art-varhandle-byte-view-test.log and
  /tmp/art-varhandle-buffer-view-test2.log complete the full acceptance body and
  all older regressions. DEX38 counts51classes1555/button107classes1974; DEX,
  runtime, probe and link rebuilt. Runtime manifest test, cargo fmt and diff
  checks PASS. No APK modification, fallback or JIT eligibility exception used.
- Full goal remains active. char/short/float/double array and buffer views,
  remaining weak/acquire/release/opaque ordering modes, read barriers,
  invoke-custom, remaining OSR/inlining and unrestricted actual-app acceptance
  are incomplete.

### VarHandle narrow16 and FP byte/ByteBuffer views — 2026-09-05

- Previous turn made progress (int/long views and Darwin direct-buffer address
  width). Added real short, char, float and double byte-array and ByteBuffer view
  wrappers for get/set/CAS/getAndAdd. All32 methods contain signature-polymorphic
  DEX and each emits exactly two ARM64 intrinsic records at requested baseline
  and optimized tiers.
- The test follows the pinned AOSP factory's actual access-mode masks instead of
  assuming field-VarHandle capabilities. short/char plain get/set are supported;
  CAS and getAndAdd must throw UnsupportedOperationException. float/double get,
  set and bit-exact CAS are supported; getAndAdd must throw UOE. Supported atomic
  access at unaligned locations throws IllegalStateException, and read-only
  ByteBuffers reject supported writes with ReadOnlyBufferException.
- byte[] short/char views pass60+60 value groups,12 unaligned plain groups and120
  contract exceptions. byte[] float/double views pass120+120 raw-bit groups,12
  unaligned groups and168 exceptions. Values cover signed short boundaries,
  unsigned char above0x7fff, FP positive/negative zero, infinities, maximum finite
  values and noncanonical NaN payloads in both little and big endian.
- Heap and real direct ByteBuffers pass short/char120+120 value groups,24
  unaligned groups and264 exceptions, then float/double240+240 raw-bit groups,24
  unaligned groups and384 exceptions. Every matrix runs interpreted, requested
  baseline and requested optimized with explicit GC between backing-byte writes
  and reads. /tmp/art-varhandle-view-fp-test.log completes the full acceptance
  body and all prior regressions.
- DEX38 counts51classes1587/button107classes2006; DEX/probe/link rebuilt. Runtime
  manifest test, cargo fmt and diff checks PASS. No APK edit, fallback, dynamic
  capability acceptance or JIT eligibility exception was introduced.
- Full goal remains active. View value-type coverage is complete for the tested
  operation families, but weak CAS, compare-exchange/get-and-set/bitwise and all
  acquire/release/opaque variants still need a full target-shape matrix. Read
  barriers, invoke-custom, remaining OSR/inlining and unrestricted real-app
  acceptance also remain incomplete.

### VarHandle int instance-field ordering modes — 2026-09-05

- Previous turn made progress (all byte-array/ByteBuffer view value types).
  Added24 independent signature-polymorphic wrappers for getOpaque/getAcquire/
  getVolatile, setOpaque/setRelease/setVolatile, all four weak-CAS orderings,
  compareAndExchange acquire/release, getAndSet and getAndAdd acquire/release,
  bitwise OR/AND plain+acquire+release and XOR acquire/release. The existing
  sequential forms remain covered by the atomic suite.
- Interpreter/requested baseline/requested optimized execution passes450
  return-value and final-field-state groups over zero, negative, signed limits
  and mixed bits. Weak CAS mismatch is required to fail without a store; matching
  weak CAS is retried for permitted spurious failure and must eventually update.
  All24 wrappers also produce the required NPE for null handle and receiver:144
  failures. Every wrapper has exactly two ARM64 VarHandle intrinsic trace records.
- The existing concurrent publication test still passes6144 two-thread handoffs
  for release/acquire and volatile modes, and its compiled-code audit still sees
  the required ARM64 ordered load/store instructions. Full acceptance and all
  prior regressions pass in /tmp/art-varhandle-ordering-modes-test.log.
- DEX38 counts51classes1631/button107classes2050; DEX/probe/link rebuilt. cargo
  fmt, diff and runtime manifest checks PASS. No APK modification, interpreter
  fallback, dynamic allow exception or JIT eligibility widening was used.
- Full goal remains active. The same ordering-mode family still needs static
  fields, primitive/reference arrays, reference fields and view target-shape
  coverage. Read barriers, invoke-custom, remaining OSR/inlining, removal of the
  restricted eligibility gate and unrestricted real-app acceptance also remain.

### VarHandle static-int and int-array ordering modes — 2026-09-05

- Extended the complete24-mode int ordering family from instance fields to
  static fields and int[] elements without changing the compiler/backend or
  adding a fallback. The new targets cover their distinct zero-coordinate and
  array-base/index address paths through real signature-polymorphic DEX calls.
- Interpreter/requested baseline/requested optimized execution passes450 value
  and final-state groups for each target (900 total), including weak-CAS
  mismatch and permitted-spurious-success retry, exchange mismatch/success,
  signed-overflow addition and mixed-bit OR/AND/XOR. Static null-handle checks
  pass72 times; array null-handle/null-array/negative-index/end-index checks pass
  288 times with the exact NPE or ArrayIndexOutOfBoundsException contract.
- All48 new wrappers have exactly two ARM64 VarHandle intrinsic trace records,
  one for requested baseline and one for requested optimized compilation. Full
  acceptance and all prior regressions pass in
  /tmp/art-varhandle-ordering-shapes-test.log. DEX/probe/link were rebuilt;
  cargo fmt, diff and runtime manifest checks pass.
- DEX38 counts51classes1680/button107classes2099. No APK modification, dynamic
  capability exception, interpreter substitution or eligibility widening was
  introduced.
- Full goal remains active. Reference instance/static fields and reference
  arrays still need their ordering-mode matrix, followed by ordering modes for
  byte-array/ByteBuffer views. Read barriers, invoke-custom, remaining OSR and
  inlining, removal of the restricted eligibility gate, and unrestricted
  real-app acceptance remain incomplete.

### VarHandle reference ordering across all target shapes — 2026-09-05

- Added the complete reference-compatible ordering/atomic family for instance
  fields, static fields and Object[] elements: opaque/acquire/volatile gets,
  opaque/release/volatile sets, all four weak-CAS modes, acquire/release
  compare-exchange and acquire/release get-and-set. These are42 independent
  signature-polymorphic wrappers; unsupported numeric add/bitwise operations
  were not invented for reference types.
- Interpreter/requested baseline/requested optimized execution passes180
  value/identity/final-state groups per target shape (540 total). Null,
  receiver and array-bound contracts pass84 instance,42 static and168 array
  groups. Every successful write/read/atomic family is crossed with explicit GC
  while values remain reachable only through the tested field or array slot.
- Added33 covariant String[] slow-path checks across every ordered mutating
  operation. An incompatible Object store must throw the exact
  ArrayStoreException and leave the element unchanged in all three compilation
  phases. The reference-array exception total is therefore201.
- All42 wrappers produce exactly two ARM64 VarHandle intrinsic trace records,
  one requested baseline and one requested optimized. Full acceptance and all
  prior regressions pass in /tmp/art-varhandle-reference-ordering-test.log.
  DEX/probe/link rebuilt; cargo fmt, diff and runtime manifest checks pass.
- DEX38 counts51classes1723/button107classes2142. No APK edit, fallback,
  capability exception or JIT eligibility widening was introduced.
- Full goal remains active. Ordering variants for byte-array/ByteBuffer views,
  remaining primitive target shapes, read-barrier-enabled GC, invoke-custom,
  remaining OSR/inlining, unrestricted eligibility and real-app acceptance are
  still incomplete.

### VarHandle FP view ordering and ARM64 FP acquire/release — 2026-09-05

- Added the complete24-mode surface for float and double views over byte[] and
  ByteBuffer (96 signature-polymorphic DEX wrappers). The pinned AOSP mask
  accepts opaque/acquire/volatile get, opaque/release/volatile set, all four
  weak-CAS modes, acquire/release compare-exchange and get-and-set; numeric
  add/bitwise modes remain exact UnsupportedOperationException contracts.
- The first unrestricted baseline compile exposed a real ARM64 backend defect:
  FP LoadAcquire and StoreRelease each need two VIXL core scratches for their
  address plus integer-LDAR/STLR-to-FMOV bridge, while the Darwin native-address
  conversion held one scratch across that call. Added patch0077 to reuse the
  dead VarHandle target-offset temporary as the outer native address for FP
  acquire and release. This preserves the AOSP operation and isolates only the
  Darwin compressed-reference-to-native-address bridge.
- Interpreter/requested baseline/requested optimized now pass3,600 raw-bit
  value and backing-byte groups over both endian orders and heap/direct buffers.
  The matrix includes +0/-0, infinities, negative finite values and distinct NaN
  payloads; weak CAS, exchange and swap compare and return exact raw bits.
- 3,144 exact contract groups pass for unsupported operations, unaligned
  ordered/atomic access, null, bounds and read-only buffers. All96 wrappers have
  exactly two ARM64 intrinsic records. Full acceptance and prior regressions
  pass in /tmp/art-varhandle-fp-ordering-test.log; compiler/DEX/probe/link
  rebuilt, cargo fmt/diff/runtime-manifest checks pass.
- DEX38 counts51classes2011/button107classes2430. No APK edit, interpreter
  fallback, capability exception or eligibility widening was introduced.
- Full goal remains active. Remaining primitive field/array ordering modes,
  read-barrier-enabled GC, invoke-custom, remaining OSR/inlining, unrestricted
  eligibility and real-app acceptance remain incomplete.

### VarHandle int/long byte-array and ByteBuffer ordering — 2026-09-05

- Added all24 non-plain ordering/atomic variants for int and long views over
  both byte[] and ByteBuffer: opaque/acquire/volatile reads,
  opaque/release/volatile writes, four weak-CAS modes, acquire/release
  compare-exchange, get-and-set/get-and-add, and plain/acquire/release bitwise
  updates. This adds96 independent real signature-polymorphic DEX wrappers.
- The interpreter/requested baseline/requested optimized matrix passes3,240
  value and backing-byte state groups across both endian orders, heap and direct
  ByteBuffer storage, signed extrema and mixed-bit arithmetic. The byte[] long
  aligned coordinate follows the pinned AOSP array payload offset (index4), not
  an assumed index8 alignment.
- All96 wrappers additionally pass4,824 exact contract groups: null handle,
  null backing, both bounds, mandatory IllegalStateException for every
  non-plain unaligned access, and ReadOnlyBufferException for every mutating
  ByteBuffer mode. This directly records the pinned AOSP view access-mode
  contract rather than treating unsupported calls as success.
- Every wrapper produces exactly two ARM64 VarHandle intrinsic trace records,
  one requested baseline and one requested optimized. Full acceptance and all
  earlier regressions pass in /tmp/art-varhandle-view-ordering-test.log.
  DEX/probe/link rebuilt; cargo fmt, diff and runtime manifest checks pass.
- DEX38 counts51classes1819/button107classes2238. No APK modification,
  interpreter fallback, dynamic capability exception or eligibility widening
  was introduced.
- Full goal remains active. Narrow16 and FP view ordering/atomic support masks,
  remaining primitive field/array modes, read-barrier-enabled GC,
  invoke-custom, remaining OSR/inlining, unrestricted eligibility and real-app
  acceptance remain incomplete.

### VarHandle narrow16 view ordering support mask — 2026-09-05

- Added the complete24-mode wrapper surface for short and char views over both
  byte[] and ByteBuffer (96 real signature-polymorphic DEX methods). This
  deliberately covers supported and unsupported access modes so the pinned
  AOSP mask is part of the executable compatibility contract.
- Pinned AOSP permits opaque/acquire/volatile gets and
  opaque/release/volatile sets for narrow16 views, while all weak-CAS,
  compare-exchange, get-and-set/add and bitwise variants are unsupported. Across
  interpreter/requested baseline/requested optimized,864 supported value and
  exact backing-byte groups pass for both endian orders and heap/direct buffers.
- 1,800 contract groups pass: every unsupported atomic mode throws the exact
  UnsupportedOperationException, every supported non-plain mode rejects
  unaligned access with IllegalStateException, null and bounds retain their
  exact exceptions, and ordered writes to read-only buffers throw
  ReadOnlyBufferException.
- All96 wrappers, including runtime-rejected access modes, produce exactly two
  ARM64 VarHandle intrinsic records for requested baseline and optimized
  compilation. Full acceptance and prior regressions pass in
  /tmp/art-varhandle-narrow16-ordering-test.log. DEX/probe/link rebuilt; cargo
  fmt, diff and runtime manifest checks pass.
- DEX38 counts51classes1915/button107classes2334. No APK edit, fallback,
  capability exception or eligibility widening was introduced.
- Full goal remains active. FP view ordering/atomic support masks and remaining
  primitive field/array modes are next; read-barrier-enabled GC, invoke-custom,
  remaining OSR/inlining, unrestricted eligibility and real-app acceptance are
  still incomplete.

### Current JIT checkpoint: FP view ordering — 2026-09-05

- The latest completed slice is the float/double byte[] and ByteBuffer view
  ordering matrix documented above: 96 real wrappers, 3,600 raw-bit operation
  groups, 3,144 exact contract groups, and exactly two ARM64 intrinsic records
  per wrapper all pass in `/tmp/art-varhandle-fp-ordering-test.log`.
- Patch 0077 fixes the Darwin ARM64 VarHandle FP acquire/release scratch-register
  collision by reusing the dead target-offset register for the converted native
  address; this keeps the pinned AOSP intrinsic semantics and changes only the
  Darwin compressed-reference address bridge.
- The current DEX38 counts are 51 classes/2,011 methods for baseline and 107
  classes/2,430 methods for button. Compiler, DEX, JNI probe and link audit were
  rebuilt; cargo formatting, diff checks and the runtime-manifest unit test pass.
- The full 100% JIT goal remains active. Next is the remaining primitive
  field/static/array ordering surface, followed by read-barrier-enabled GC,
  invoke-custom, remaining OSR/inlining, unrestricted eligibility and real-app
  acceptance without APK modification or interpreter fallback.

### VarHandle long field/static/array ordering — 2026-09-05

- Added the complete 24-mode non-plain ordering/atomic surface for each of long
  instance fields, static fields and long[]: opaque/acquire/volatile reads,
  opaque/release/volatile writes, four weak-CAS modes, acquire/release
  compare-exchange, get-and-set/get-and-add and all bitwise ordering variants.
  These are 72 real signature-polymorphic DEX methods, not native stand-ins.
- Interpreter, requested baseline and requested optimized phases pass 1,350
  64-bit operation groups over zero, negative, signed extrema and mixed high/low
  bit patterns. Weak CAS tests both failure and retry-to-success; exchange,
  arithmetic wraparound and bitwise results are compared as exact 64-bit state.
- 504 exact exception groups pass for null VarHandle/receiver/array and negative
  or upper-bound array indexes. Compiler tracing records exactly two Darwin
  ARM64 VarHandle intrinsics for every wrapper: 72 methods, 144 records, zero
  methods with a count other than two.
- DEX38 counts are now 51 classes/2,083 methods for baseline and 107
  classes/2,502 methods for button. JIT compiler, DEX, JNI probe and runtime
  graphics link were rebuilt. The clean full regression passes in
  `/tmp/art-varhandle-long-ordering-regression2.log`; intrinsic evidence is in
  `/tmp/art-varhandle-long-ordering-test.log`. Cargo formatting, diff checks and
  the runtime-manifest test pass.
- No APK modification, interpreter fallback, capability exception, gate
  widening or Darwin-only operation semantics were added. The full goal remains
  active: float/double and narrow primitive field/static/array ordering masks,
  read-barrier-enabled GC, invoke-custom, remaining OSR/inlining, unrestricted
  eligibility and real-app acceptance remain incomplete.

### VarHandle float/double field/static/array ordering — 2026-09-05

- Added all 24 non-plain access-mode wrappers for float and double across
  instance fields, static fields and primitive arrays: 144 real
  signature-polymorphic DEX methods. The executable support mask follows pinned
  AOSP: ordered get/set, four weak-CAS modes, acquire/release exchange and swap,
  and acquire/release get-and-add are supported; the eight bitwise variants are
  unsupported for FP carriers and must throw UnsupportedOperationException.
- Interpreter, requested baseline and requested optimized phases pass 1,944
  operation groups. Ordered loads/stores, CAS, exchange and swap compare exact
  raw bits across positive/negative zero, infinity, finite negatives and NaN
  payloads; add modes use finite operands and compare exact IEEE results across
  all six storage/type shapes.
- 816 contract groups pass: the 48 unsupported wrapper shapes preserve exact UOE
  and every supported mode preserves null receiver/handle/array plus negative
  and upper-bound array exceptions. All 144 wrappers receive exactly two JIT
  compile requests (288 total, zero missing); the 96 supported wrappers produce
  exactly two ARM64 VarHandle intrinsic records each (192 total), while the 48
  unsupported wrappers compile the AOSP exception path rather than inventing an
  operation.
- DEX38 counts are now 51 classes/2,227 methods for baseline and 107
  classes/2,646 methods for button. JIT compiler, DEX, JNI probe and runtime
  graphics link were rebuilt. Full regression passes in
  `/tmp/art-varhandle-fp-field-ordering-regression.log`; compile/intrinsic
  evidence is in `/tmp/art-varhandle-fp-field-ordering-test.log`. Cargo format,
  diff and runtime-manifest checks pass.
- No APK edit, fallback, capability exception, gate widening or Darwin-specific
  FP semantics were added. The full goal remains active: byte/boolean and
  short/char field/static/array ordering masks, read-barrier-enabled GC,
  invoke-custom, remaining OSR/inlining, unrestricted eligibility and real-app
  acceptance remain incomplete.

### VarHandle narrow primitive field/static/array ordering — 2026-09-05

- Added the complete 24-mode wrapper surface for byte, boolean, short and char
  across instance fields, static fields and primitive arrays: 288 real
  signature-polymorphic DEX methods over twelve type/storage shapes.
- The pinned AOSP support mask is enforced as executable behavior. Byte, short
  and char support every ordered load/store, weak-CAS, exchange, swap, add and
  bitwise variant. Boolean supports the same surface except acquire/release
  get-and-add; those six shape/mode combinations throw exact
  UnsupportedOperationException, while boolean OR/AND/XOR remain supported.
- Interpreter, requested baseline and requested optimized phases pass 5,310
  operation groups covering signed/unsigned extrema, narrow arithmetic
  wraparound, weak-CAS failure/retry and exact boolean truth tables. Another
  1,992 groups verify the unsupported mask and exact null receiver/handle/array
  plus negative and upper-bound array exceptions.
- All 288 wrappers receive exactly two JIT compile requests (576 total, zero
  missing). The 282 supported wrappers produce exactly two Darwin ARM64
  VarHandle intrinsic records each (564 total); the six unsupported boolean-add
  wrappers compile the AOSP exception path instead of a Darwin substitute.
- DEX38 counts are now 51 classes/2,515 methods for baseline and 107
  classes/2,934 methods for button. JIT compiler, DEX, JNI probe and runtime
  graphics link were rebuilt. Full regression passes in
  `/tmp/art-varhandle-narrow-field-ordering-regression.log`; compile/intrinsic
  evidence is in `/tmp/art-varhandle-narrow-field-ordering-test.log`. Cargo
  format, diff and runtime-manifest checks pass.
- No APK edit, fallback, capability exception or gate widening was added. The
  primitive/reference VarHandle value and support-mask surface is now covered;
  the full goal remains active for deeper cross-thread memory-order stress,
  read-barrier-enabled GC, invoke-custom, remaining OSR/inlining, unrestricted
  eligibility and real-app acceptance.

### AOSP Baker/ConcurrentCopying runtime and first JIT read-barrier slice — 2026-09-05

- The Darwin ART runtime, compiler and ARM64 assembly are now built permanently
  with `ART_USE_READ_BARRIER`, Baker and forced read barriers. Runtime startup
  selects AOSP `CollectorTypeCC`; the 253-object runtime closure and 106-object
  optimizing compiler both build successfully under that configuration.
- Added a Darwin non-futex empty-checkpoint backend for ART mutexes registered
  as expected weak-reference locks. Those pthread mutex/rwlock waiters use a
  bounded native try-lock loop and service `CheckEmptyCheckpointFromMutex()`;
  unrelated locks retain blocking pthread behavior. This replaces the previous
  fatal `Non futex case isn't supported` boundary without suppressing the
  checkpoint contract.
- The acceptance runtime now completes actual explicit Concurrent Copying GCs,
  post-GC compiled calls and concurrent reference-call/GC stress. Evidence is
  `/tmp/art-jit-baker-seventh.log` and `/tmp/art-jit-baker-eighth.log`; the
  latter also passes large mixed register/stack managed calls and callee GC.
- Added the first base-relative Baker compiler bridge. Instance/static field
  lowering keeps compressed32 managed references in HIR/stack maps while using
  a separate decoded 64-bit register for native field addresses, including
  volatile and offsets above 16 KiB. Normal and large int/reference/volatile
  getters, caller-observed null behavior, multi-field primitive arithmetic,
  reference calls and reference inlining across CC GC now pass.
- A broad read-barrier gate removal was deliberately rejected after it allowed
  background compilation of still-unported boot methods and produced a startup
  crash. Admission remains opcode/contract based (never app or method-name
  based); the new primitive-field graph and managed-call shapes are opened only
  after their backend paths pass.
- The full 100% JIT goal remains active. The next observed boundary is exception
  catch compilation (`status 119`) because try-region stack-map/read-barrier
  handling is still gated. Baker mark-introspection slow paths, arrays,
  allocation/type checks, broader calls, OSR/deopt and unrestricted admission
  remain incomplete and must be implemented before real-app acceptance can be
  claimed.

### Baker exceptions, monitors and JNI call graphs — 2026-09-05

- Read-barrier-enabled optimized catch graphs now admit the verified AOSP DEX
  forms for typed/catch-all handlers, catch-all finally rethrow and a balanced
  synchronized block. This uses the existing ARM64 compressed-reference
  exception and quick monitor boundaries; it does not replace Java exception or
  monitor semantics with a Darwin helper.
- `/tmp/art-jit-baker-monitor1.log` passes exact caught object/null NPE/TLS
  clearing, two typed handlers and unmatched propagation, normal and exceptional
  finally cleanup (including replacement exceptions), callee GC, thin/fat
  recursive monitors, null monitor NPE, monitor contention and exception-safe
  unlock under Concurrent Copying.
- Added contract-shaped admission for a static generic-JNI reference return kept
  live across a subsequent managed GC, a 13-word mixed integer/FP/reference/
  narrow native stack-call graph, and one receiver-dispatched quick/JNI call.
  `/tmp/art-jit-baker-native-instance1.log` passes primitive/reference JNI,
  native-to-Java callbacks with GC and exact exceptions, packed stack arguments,
  and direct/virtual instance JNI receiver identity and null behavior.
- The current first boundary is `jitNativePolymorphic` in
  `/tmp/art-jit-receiver-diag2.log`. Its `IL`, five-code-unit invoke-virtual graph
  now passes the Darwin eligibility contract and resolves the non-static `I`
  target, but optimized compilation/code-cache installation still returns
  failure. The next task is to instrument the post-eligibility optimizing
  compiler result (not widen admission), identify the primitive-result
  virtual/interface native failure, then rerun unregister/rebind, override and
  null-receiver acceptance.
- The full 100% JIT goal remains active. These graph admissions are temporary
  capability fences, not app/method allowlists; they must disappear after all
  Baker ARM64 paths and unrestricted background compilation are safe. Arrays,
  allocation/type checks, broader calls, Baker mark slow paths, OSR/deopt and
  real-app unrestricted-JIT acceptance remain incomplete.

### Baker composed calls, instance bodies, fields and primitive arrays — 2026-09-05

- Removed the redundant read-barrier rejection of resolved virtual/interface
  HInvoke graphs. Darwin already decodes compressed receivers at the ARM64
  class/vtable/IMT dispatch boundaries. Native virtual/interface overrides,
  null receivers and unregister/UnsatisfiedLinkError/rebind recovery now pass
  in `/tmp/art-jit-baker-native-poly3.log`.
- Added structural, method-name-independent admission for scalar reference
  control flow and resolved managed-call compositions. Mixed reference/long/
  double argument permutations pass 216 cases, and composed reference results
  remain live across three Concurrent Copying GCs while void call/store order
  passes 27 cases in `/tmp/art-jit-baker-array-composed1.log`.
- Fixed an actual Baker backend defect exposed by the composed field test:
  field stores and GC card-table indexing now decode the compressed holder for
  native addressing regardless of read-barrier mode. The prior patch only did
  so with read barriers disabled. Composed instance/static/volatile/reference
  fields now pass 105 read/modify/write, null-NPE, wide and reference-GC cases.
- Generalized receiver-call and ordinary instance-body capability fences.
  Virtual compositions pass 81 override/reference/null/second-receiver cases;
  instance bodies pass 324 arithmetic, branch, `this` selection and mixed-wide
  cases. These continue to use AOSP calling conventions and dispatch rather
  than a Darwin helper.
- Primitive ArrayGet/ArraySet and every ArrayLength now decode the compressed
  array for native data/header addressing under Baker. The structural array
  graph passes 135 loop/read/write/length/null/bounds/partial-store cases.
  Reference-array Baker holder/thunk work remains deliberately separate.
- The JIT compiler archive rebuilt with 106 AOSP ARM64 objects and the runtime
  graphics closure audit passes with 51 registrars and zero fake symbols. The
  current first boundary is `jitArrayAllocate(int,int)`: baseline compilation
  is still rejected because read-barrier-enabled NEW_ARRAY plus initialization
  loops has not yet been admitted and verified.
- The full 100% JIT goal remains active. Next is array allocation, then
  reference-array Baker loads/stores and allocation/GC, followed by remaining
  type checks, calls, exceptions, OSR/deopt, unrestricted eligibility and real
  app acceptance. No APK edits, method/app allowlists, interpreter fallback or
  Darwin-specific Java semantics were added.

### Baker JIT executable-matrix closure — 2026-09-05

- Removed the remaining Darwin bytecode/method-shape admission table. JIT
  eligibility now retains only runtime lifecycle exclusions analogous to ART
  itself; correctness is enforced in ARM64 lowering and runtime boundaries.
- Added 0083 for reference-array intermediate native addresses, 0084 to allow
  AOSP unresolved field lowering under Baker, and updated old CMS/HSpace-only
  acceptance probes to exercise the configured Concurrent Copying collector.
  Resolved/unresolved primitive and reference fields, exact failures and
  same-code recovery now pass with moving GC.
- Allocation-pressure diagnosis found two real Baker ABI defects rather than a
  heap-size problem. 0085 decodes compressed GC-root values before the JIT
  thunk reads the object lock word. 0086 makes both fixed-offset GC-root
  introspection and ordinary register mark entrypoints pass native `Object*`
  values to `ReadBarrier::Mark`, while preserving compressed32 values in
  generated code and on return. Large-object pressure, OOME, retained roots,
  recovery and repeated CC cycles now pass.
- Added 0087 to remove the stale read-barrier rejection of AOSP
  `HInvokeUnresolved`. Static, virtual and interface missing-owner/access/null
  failures and same-code resolution recovery execute through the existing
  quick trampolines with access checks. No Darwin substitute semantics or APK
  edits were introduced.
- Clean full evidence is `/tmp/art-jit-baker-complete-regression.log`: the
  complete current executable matrix passes through arithmetic/control flow,
  fields/arrays/objects, allocation pressure, handlers/finally, monitors,
  resolved and unresolved calls, super/range, OSR, moving-GC deopt, MethodHandle
  polymorphism and VarHandle modes. Runtime graphics link closure remains 51
  registrars and zero fake symbols.
- The 100% goal remains active. Current local matrix closure is not universal
  compatibility. Next boundaries are executable `invoke-custom`/CallSite,
  exhaustive pinned-AOSP intrinsic and upstream compiler-test coverage,
  production background-JIT policy for app launch, unmodified Chromium/
  calculator/Blue Archive regressions, and restored Mach-O unwind metadata.

### Executable invoke-custom/CallSite closure — 2026-09-05

- Added a deterministic classfile generator that emits a user-defined bootstrap
  method returning `ConstantCallSite`, then lets normal D8 desugaring preserve
  only the custom sites Android ART actually supports. Java LambdaMetafactory
  remains correctly desugared by D8; disabling desugaring globally was tested
  and rejected because pinned AOSP explicitly does not implement that bootstrap.
- The resulting DEX38 contains four `invoke-custom` sites and one eleven-word
  `invoke-custom/range` site. Acceptance resolves the fixture through the real
  app ClassLoader and executes stateless/capturing integer targets, reference
  identity across Concurrent Copying GC, exact exception/recovery behavior and
  mixed integer/long/double/reference range arguments.
- Removed the stale Darwin post-graph rejection of `HInvokeCustom`; lowering and
  execution use AOSP's existing `GenerateInvokeCustomCall`,
  `kQuickInvokeCustom`, `artInvokeCustom` and CallSite resolution paths. Clean
  evidence is `/tmp/art-jit-invoke-custom-final.log`: interpreter, requested
  baseline and requested optimized phases pass.
- DEX contracts are now version 38, 52 classes/2,528 methods for baseline and
  108 classes/2,947 methods for button. Graphics/runtime link closure remains
  51 registrars and zero fake symbols.
- Fixed a native build-graph correctness bug found during this work: a dirty JNI
  acceptance edge no longer reuses its existing output before recompilation,
  and the invoke-custom header is an explicit graph/content-stamp dependency.
  This prevents stale acceptance binaries after header-only changes.
- The 100% goal remains active. Next boundaries are the complete pinned-AOSP
  ARM64 intrinsic/upstream compiler-test inventories, production background-JIT
  policy under real apps, unmodified Chromium/calculator/Blue Archive
  regressions, and restored Mach-O unwind metadata.

### Pinned AOSP ARM64 intrinsic gate and first executable slice — 2026-09-05

- Added a source-derived audit of the pinned AOSP contract. It currently finds
  36 specialized-HIR intrinsics and 217 HInvoke intrinsics: 187 handwritten
  ARM64 implementations, 28 intentional upstream ordinary-call fallbacks and
  two upstream HIR rewrites. Missing/unpaired/overlapping classifications now
  fail `tools/audit-art-jit.sh`; this is not a Darwin method allowlist.
- Removed the stale Darwin post-HIR rule that rejected every intrinsic invoke.
  AOSP ARM64 locations/codegen and AOSP's explicit unimplemented-call fallback
  now decide compilation. Enabling the real path exposed a production startup
  crash in `jdk.internal.misc.Unsafe.getReferenceAcquire`, before the probe ran.
- Fixed the underlying compressed-reference address contract for Unsafe object
  reads. The same audit was applied to object-relative put, CAS, get-and-update
  and the Baker CAS slow path: HIR/GC values remain compressed while only the
  effective native memory address is decoded. Null-base absolute access keeps
  its upstream nullable semantics. No Java substitute or interpreter fallback
  was added.
- Added executable DEX coverage for all 30 non-Unsafe specialized-HIR entries:
  floating NaN, integer/long compare/rotate/signum, every numeric abs/min/max,
  String charAt/isEmpty/length and all five VarHandle fences. Interpreter,
  requested baseline and requested optimized results match, including exact
  String bounds/null exceptions, in `/tmp/art-jit-intrinsics-final.log`.
- The same clean log completes the existing Baker/CC executable matrix and
  invoke-custom suite; graph link audit remains 51 registrars with zero fake
  symbols. `cargo test -p art-bootstrap` (3) and
  `cargo test -p darwin-art-xtask` (18), formatting and diff checks pass.
- The 100% goal remains active. Source classification is not full execution
  evidence. Next is generated real-DEX execution coverage for all 217 HInvoke
  entries, starting with Unsafe read/write/CAS/update and then String,
  arraycopy, math, CRC, Memory, Reference and boxing families. After that come
  the pinned upstream compiler-test inventory, production background JIT, real
  unmodified apps and Mach-O unwind metadata.

### AOSP JIT admission, Unsafe execution and JNI Baker closure — 2026-09-05

- Moved the hidden-API Unsafe compiler fixture out of the application DEX into
  a standalone trusted boot DEX, matching upstream ART compiler-test topology.
  The unmodified app payload no longer contains hidden Unsafe references.
  Thirty-four direct DEX read/write/CAS/update/fence/absolute cases now pass in
  interpreter, requested baseline and requested optimized code with moving CC
  GC; this is execution evidence for the 0090/0091 address fixes.
- Removed the obsolete 0039 runtime admission patch and the duplicate Darwin
  compiler/inliner gates. Upstream `NotifyCompilationOf()` lifecycle behavior
  now owns visibility and background admission. The standard class-linker
  visible-initialization wait is used only to make the explicit compiler fixture
  deterministic; no application/method allowlist or Java substitute was added.
- Unrestricted background compilation exposed an intermittent crash in the JIT
  JNI stub for `Thread.currentThread()`. The faulting `ldr w16, [x22,#4]`
  received a compressed declaring-class reference in `x22`: ARM64 JNI Baker
  `TestMarkBit()` had treated that managed payload as a native object address.
  Patch 0095 decodes only this native-address dereference and separately keeps
  Darwin's 64-bit native `jobject` return handle in X0 until JNI decoding, while
  the managed quick ABI remains compressed W0.
- After the JNI/Baker fix, ten consecutive complete `audit-art-jit.sh` runs pass
  (`/tmp/art-jit-jni-baker-stable-1.log` through `-10.log`), including Unsafe,
  all VarHandle matrices, invoke-custom, CC/deopt/OSR and the final native exit
  hook. There are no fatal signals or ThreadCpuNanoTime warnings.
- Added native Mach thread CPU time for JIT/GC accounting in 0094. Replaced the
  old Darwin fatal-dump patch that interpreted macOS `ucontext_t` as a Linux
  signal frame; fatal reports now use native Mach ARM64 register accessors and
  report the actual PC/SP/FP/LR, register set and `si_addr`.
- Full build and runtime graphics link closure pass with 51 registrars and zero
  fake symbols. `art-bootstrap` tests (3), `darwin-art-xtask` tests (18), Rust
  formatting and diff whitespace checks pass. The 100% goal remains active;
  next is executable coverage for the remaining String, arraycopy, math, CRC,
  Memory, Reference and boxing HInvoke families, followed by the upstream
  compiler corpus, production app policy, real apps and Mach-O unwind metadata.

### AOSP String, arraycopy and Math intrinsic execution — 2026-09-05

- Completed executable coverage for all 25 pinned String intrinsics and all 43
  Math intrinsics. Hidden StringFactory/getChars and non-desugared multiplyHigh
  use trusted boot fixtures; ordinary public calls remain in the app DEX. This
  mirrors ART compiler-test privilege boundaries rather than adding a Darwin
  method allowlist or modifying production APKs.
- Fixed ARM64 native-address formation for String and generic System.arraycopy.
  Managed HIR values, Baker read barriers and card marking retain compressed
  references; only object/array/class field and payload dereferences use decoded
  native addresses. Reference/char copies, overlap, bounds/type exceptions and
  moving CC GC pass in `/tmp/art-jit-arraycopy3.log`.
- Corrected the Java fixture toolchain to use Android 16's
  `core-for-system-modules.jar` plus `android.jar` as the boot API while emitting
  Java 8 classfiles. This keeps MethodHandle invokes signature-polymorphic and
  exposes current Android Math APIs. The complete audit, including the new
  29-entry Math HInvoke raw-bit matrix, passes in `/tmp/art-jit-math3.log`.
- The 100% goal remains active. Resume with the three CRC32 callsites through
  actual boot CRC32 public methods, then Memory, Reference, boxing, intentional
  upstream arraycopy fallbacks, the upstream compiler corpus, production app
  JIT, real-app regressions and Mach-O unwind metadata.

### ARM64 HInvoke family execution closure — 2026-09-05

- CRC32's Android 16 `@CriticalNative` signature now matches AOSP and array
  payloads are decoded only for native access (0097). Byte, array and direct
  ByteBuffer paths pass known vectors, state changes and exceptions.
- All eight libcore Memory native-address peek/poke intrinsics pass unaligned
  round trips in interpreter, baseline and optimized code. Explicit compiler
  fixtures use ART initialization visibility and suspend production workers so
  tier requests cannot race hotness compilation.
- A real optimized `Reference.refersTo()` run crashed by dereferencing the
  compressed receiver at `0x1004001c`. Patch 0098 reuses the assigned result
  register as a decoded X address for referent and forwarding lock-word loads;
  managed operands remain compressed. Reference null/identity/moving-GC and all
  four boxed `valueOf` cache/allocation paths now pass.
- Added executable byte/int System.arraycopy coverage for AOSP's remaining
  ordinary-call fallback shapes. The full audit `/tmp/art-jit-fallback1.log`
  passes with 51 registrars and zero fake symbols.
- Pinned ART test sources are now part of `sync-jit-sources`, verified by
  `ART_TEST_ANDROID_BP_SHA256`: 1,138 test directories and 1,846 Java files.
  Next add a generic output-comparing upstream `Main` runner, then close
  production-app background JIT, unmodified app regressions and Mach-O unwind.
  The 100% goal remains active.

### Upstream ART corpus runtime closure checkpoint — 2026-09-05

- The generic runner now preserves Android process semantics instead of probe
  semantics: filesystem authority lives for the process (062), exact Android
  16 `UNIXProcess` natives own child process lifecycle (063), output capture is
  installed before target class initialization (084), and `$DEX_LOCATION`
  exposes the standard stored `classes.dex` test jar (086/087).
- The system-native archive now registers exact pinned Android 16 NativeBN (7),
  StrictMath/fdlibm (20/80), and libcore ICU (10) tables. The native closure is
  BoringSSL/ICU based and does not replace Java calls with probe methods.
- ARM64 compressed-reference boundaries found by real tests are fixed at their
  common owners. SmallPatternMatcher quick stubs decode receivers only for
  native dereference and encode managed object returns (109). Baker
  `art_quick_aput_obj` now does the same for its GC-marking class/lock-word
  loads; four concurrent readers plus a moving-CC writer complete
  `153-reference-stress` in optimized OSR code.
- The test build is reproducible against pinned AOSP Android 16 R8 commit
  `46b3c3058bc6d7e513ff8fa5611ec02d5a1c4010` and SHA-256, with AOSP's `javac
  -g` contract. Native-artifact consumers are structurally classified as
  `external-native`, not silently counted as pure Java. The separate xtask and
  bootstrap shadow manifests now contain the same small-pattern source/patch,
  keeping incremental native-cache invalidation sound.
- Current exact-output evidence is 151 of 426 structurally pure-Java tests in
  both interpreter and requested optimized modes. Native, external-native,
  bytecode, multi-source and custom-script groups remain explicit work queues;
  none is counted as passed by classification. Next pure test is
  `2284-regression-test-368984521-loop-opt`.
- The 100% goal remains active. Completion still requires every corpus group,
  unrestricted production JIT in unmodified apps including Blue Archive, and
  restored/validated Mach-O unwind metadata.

### Upstream pure-Java and multi-Dex closure — 2026-09-05

- The pinned upstream runner now launches `Main.main` directly through JNI,
  preserving the dalvikvm application stack rather than adding reflection
  frames. AOSP test argv is explicit, and `826-infinite-loop` exercises its
  configured non-looping branch without changing the test source.
- Android launcher properties are established at VM creation. `java.io.tmpdir`
  maps guest `/data/local/tmp` through the private filesystem capability, and
  `java.class.path` plus the canonical application `PathClassLoader` now back
  both system-loader and thread-context-loader delegation. This fixed the real
  custom-loader parent-chain failure exposed by `068-classloader`.
- The runner preserves AOSP `src2`, `src-art`, `src-multidex`, `src-aotex`,
  `src-bcpex`, `src-ex` and `src-ex2` compilation and DexFile boundaries.
  Hidden boot classes are compile-time signatures only and never enter app
  DEX; runtime resolution remains against pinned Android 16 boot classes.
- Exact-output evidence is now 423/423 structurally pure-Java and 22/22
  multi-source tests, each in interpreter and requested optimized JIT modes.
  `545-tracing-and-jit`, file create/ftruncate/mmap in `530-regression-lse`,
  multi-Dex resolution and independent DexClassLoader namespaces all pass.
- Incremental graph tracking now includes `runtime_context_loader.cc`; a loader
  edit relinks the runtime instead of leaving a stale probe object. The strict
  graphics closure remains 51 registrars with zero fake symbols.
- The 100% goal remains active. Next close bytecode-source (126), native (24),
  external-native (37) and custom-script (344) queues, then production APK JIT
  including Blue Archive and validated Mach-O unwind metadata.

Bytecode-source resume note: the 126-test queue contains 118 Smali-only or
mixed cases and eight Jasmin cases. No assembler binary is currently pinned in
the workspace, so the next step is to source-control-pin the Android 16 AOSP
Smali/Jasmin toolchain, reproduce `run_test_build.py` merge ordering, and run
the original bytecode rather than translating it to Java.

### Android 16 bytecode-source closure and native-module start — 2026-09-05

- Pinned Google Smali at Android 16 commit
  `112192259df4c8cfe9491affe3728f98024a630c` and AOSP Jasmin at
  `0c8569a6f7492b1ba639d33e22fe9cf6f45a80ac`, each with a checked SHA-256
  lock and reproducible sync script. The runner preserves AOSP's Java,
  Jasmin, Smali and multidex merge order at API 26.
- The complete mechanically classified bytecode-source queue passes exact
  expected stdout/stderr in interpreter and optimized JIT modes: 126/126 in
  `/tmp/art-bytecode-full-20260905.XbaPvv/results.tsv`. This includes all eight
  Jasmin cases, primary/secondary Smali, verifier target-SDK 31, separate
  class-loader JARs and six `libarttest` consumers.
- Exact pinned `runtime_state.cc` and `stack_inspect.cc` helpers are linked into
  the test runtime. Darwin's 543 helper decodes its reference vreg through
  ART's base-relative `CompressedReference` boundary; the unmodified Java and
  Smali still run unchanged. A real Mach-O `libarttest.so` executes JNI_OnLoad
  and shares the process stdout ordering used by Android run-test.
- Native-source execution has begun through a generic per-test Mach-O module,
  not per-APK flags. `2036-jni-filechannel`, `647-jni-get-field-id`, and
  `2275-pthread-name` pass both modes. `/dev/null` is now an Android character
  device capability in the Rust Bionic filesystem facade, and logical
  cross-thread pthread names bridge Darwin's current-thread-only kernel API.
- Current native blocker is process class-loader ownership: a newly attached
  native thread in `169-threadgroup-jni` still receives ART's boot-only
  `Runtime::system_class_loader_`, although the Java static system loader and
  existing thread context loader point at the app PathClassLoader. Resume by
  installing the canonical app loader in Runtime so JNI FindClass after
  AttachCurrentThread follows Android semantics; then continue all 24 native
  tests. The 100% goal remains active, followed by external-native,
  custom-script, production APK and Mach-O unwind closure.

### ART native/JIT application-policy checkpoint — 2026-09-05

- Android application debug state is now a process-creation contract rather
  than a probe flag. The binary-manifest inspector decodes
  `android:debuggable`; the APK launcher supplies both target SDK and Java
  debuggable state before `Runtime::Create`; ART receives `--debuggable` in
  `RuntimeArgumentMap::CompilerOptions`. Release APKs retain normal optimizing
  policy, while debuggable APKs emit the dex-register environments required by
  JDWP/JVMTI and asynchronous full-stack deoptimization.
- The unmodified AOSP `685-deoptimizeable` test passes exact output in
  interpreter and debuggable optimized-JIT modes. AOSP explicitly marks its
  non-debuggable optimizing/JIT variants as expected failures, so the runtime
  preserves `Runtime::IsAsyncDeoptimizeable()` instead of weakening it.
- The shared ART-test process stream now disables Darwin's redirected-stdout
  block buffering before Java execution. Native `printf` and Java descriptor
  writes retain Bionic ordering; `2275-pthread-name` passes both modes. The
  runtime-entry content stamp now includes `runtime_upstream_test.h`, avoiding
  stale linked probes after harness changes.
- External-native closure is 37/37 exact-output tests. A first complete pass of
  the 24 source-native tests leaves `004` (Darwin ARM64 ucontext source port),
  `454/461/466` (ART-internal low-4GB vreg helper assumptions), `497` (same
  DexFile under independent ClassLoaders), and `993` (JVMTI headers/plugin)
  after excluding AOSP's unconditional known-failure `664`. These are not to
  be hidden behind APK flags or source rewrites; resume at common OS/runtime
  ownership boundaries.
- The 100% compatibility goal remains active. Completion still requires all
  applicable pinned AOSP configurations, custom/no-main corpus execution,
  production APK regressions including unrestricted Blue Archive JIT, and
  Mach-O unwind/deoptimization validation.
- Follow-up: `004-SignalTest` now passes both modes without changing upstream
  source. A narrowly injected Darwin ARM64 compatibility header maps Bionic's
  inline `mcontext_t.pc` view onto Darwin's pointed `__ss.__pc` and preserves
  Bionic's logical `sa_mask` query while sending only legal bits to XNU.
  Source-native applicable progress is therefore 18/24; resume with
  `454/461/466`, `497`, and `993` (plus separately reported known-failure 664).
- Denominator correction after consulting the pinned AOSP known-failure
  manifest: `497` is also unconditionally disabled upstream because its broken
  loader re-registers one DexFile under multiple ClassLoaders. Preserve ART's
  rejection. Applicable source-native progress is 18/22; the remaining valid
  items are `454/461/466` and `993`.

### Embedded JVMTI and Android launcher closure checkpoint — 2026-09-06

- Source-native closure is now 22/22 applicable Android 16 configurations.
  Original `454`, `461`, and `466` use a narrow base-relative
  CompressedReference source-ABI adapter; `466` also receives AOSP
  `--compile-art-test` so its no-inline root contract is honored. Original
  `993-breakpoints-non-debuggable` passes through the real embedded OpenJDK
  JVMTI plugin. Upstream-unconditional failures 497 and 664 remain explicitly
  excluded rather than weakened.
- The embedded plugin is all 29 pinned Android 16 OpenJDK JVMTI sources, loaded
  through ART's normal Plugin/AgentPath lifecycle. Darwin's minimal start now
  emits kStart/kInit around daemon creation and starts the Signal Catcher like
  a non-zygote app process. Original 901, 903–911, and 913–932 JVMTI tests
  reached so far pass byte-exact interpreter and optimized-JIT execution;
  912 remains queued and 933 is the current active blocker.
- AOSP's complete ART-independent `libtiagent-base-defaults` helper set is now
  linked into every per-test agent. The dispatcher reproduces common
  redefine/retransform/transform OnLoad routing instead of adding runtime or
  APK allowlists. A generation-counted Darwin pthread-barrier source ABI closes
  931 without changing its upstream source.
- Libcore `File.getCanonicalFile()` now reaches the Rust Bionic filesystem
  facade; guest `/proc/self` canonicalizes to `/proc/<pid>`, closing original
  913. The pinned `jdk_internal_misc_VM.cpp` registrar supplies
  getNanoTimeAdjustment, closing 924. JVMTI GetTime uses modern Darwin
  CLOCK_MONOTONIC, matching Android System.nanoTime and closing 927.
- The launcher now supplies both RuntimeArgumentMap::ClassPath and
  java.class.path at VM creation, with the APK support DEX included where
  applicable. It uses that one system PathClassLoader as the canonical app
  loader rather than constructing a duplicate parent/child pair. This closes
  JVMTI AddToSystemClassLoaderSearch in original 929.
- The host blocks Android runtime-control signals before spawning threads, so
  SIGQUIT reaches ART's sigwait-based Signal Catcher. AOSP's run-test-specific
  DumpNativeStackOnSigQuit=false option is also reproduced. Current 933 reaches
  SignalCatcher but SIGSEGVs inside the remaining managed/full DumpForSigQuit
  path before RuntimeCallbacks::SigQuit. Resume by instrumenting and fixing
  that common Darwin dump owner; do not bypass the callback or edit the test.
- The 100% goal remains active. After 933, continue the remaining custom-script
  JVMTI queue, then production APK/Blue Archive unrestricted JIT and Mach-O
  unwind/deoptimization validation.

### JVMTI class/transform continuation checkpoint — 2026-09-06

- The preceding checkpoint's 933 blocker is closed. Darwin's SIGQUIT dump
  crashed because `SignalCatcher::DumpCmdLine()` streamed a null process
  command line; the pinned ART patch now prints `<unset>`. The following abort
  came from the host crash-stack palette adapter returning NOT_SUPPORTED; it
  now writes the complete payload to stderr and reports the real write result.
  Original `933-misc-events` passes byte-exact in interpreter and optimized
  modes without bypassing RuntimeCallbacks::SigQuit.
- Original `912-classes` now passes byte-exact in both modes. AOSP's
  `runtime_state.cc` remains linked once in the runtime image; the two JNI
  entrypoints forwarded by `classes_art.cc` are explicit test ABI exports.
  The per-test agent no longer recompiles the entire ART-dependent
  `libarttest` common implementation, avoiding duplicate ownership and hidden
  C++ runtime dependencies.
- Original `934` through `945` all pass byte-exact in interpreter and
  optimized modes. `936-search-onload` uses its pinned AOSP OnLoad owner so
  AddToBootstrapClassLoaderSearch and AddToSystemClassLoaderSearch execute
  during normal agent loading; no test APK or expected output was modified.
- The next sequential JVMTI corpus item is `946-obsolete-throw`. The 100% goal
  remains active; passing this interval is evidence for class transformation,
  retransformation, obsolete frames, native methods, and loader search, not a
  declaration of full ART or production-APK completion.

- Continuation: original `946-obsolete-throw` also passes byte-exact in both
  modes. Resume at `947-reflect-method`.

### Invoke-custom and MethodHandle checkpoint — 2026-09-06

- Original `947` through `959` now pass byte-exact in interpreter and
  optimized-JIT modes. This interval covers reflected methods, annotation and
  in-memory transforms, threaded obsolete frames, invoke-custom,
  invoke-polymorphic verification/accessors, MethodHandle transforms, and
  EmulatedStackFrame dispatch.
- `952-invoke-custom` now reproduces its pinned AOSP build contract: the
  Android 16 prebuilts/misc ASM 9.6 artifact is revision- and SHA-locked, the
  upstream annotation transformer is compiled, and javac output is rewritten
  to real invokedynamic bytecode before non-desugaring D8. No generated test
  DEX or source is patched to fit Darwin.
- `956-methodhandles` exposed a platform registration gap rather than a JIT
  codegen error. The pinned libcore `java_lang_invoke_MethodHandle.cpp` and
  `java_lang_invoke_VarHandle.cpp` owners are now in the Darwin system-native
  archive and registered with libjavacore. Reflective signature-polymorphic
  calls therefore throw Android's UnsupportedOperationException instead of a
  missing-JNI UnsatisfiedLinkError; ordinary polymorphic execution remains on
  ART's interpreter/JIT paths.
- `954` applies the exact upstream run.py normalization for temporary DexFile
  locations and verifier PCs. `958` uses signature-only hidden-API compiler
  inputs for Transformers/EmulatedStackFrame; these stubs never enter the test
  DEX, whose runtime definitions remain the Android boot classes.
- System-native build/audit and the full graphics/runtime link audit pass with
  seven pinned system-native archive members. Resume the sequential corpus at
  `960-default-smali`. The 100% goal remains active; production APK/Blue
  Archive, GC/concurrency, and Mach-O unwind/deoptimization closure are still
  required.

### ART compatibility architecture continuation — 2026-09-06

- The upstream runner now treats AOSP build metadata as executable contract:
  generated sources, post-javac transformers, pinned ASM/Smali/D8/Jasmin,
  per-DexFile source groups, API levels, hidden-API flags, runtime options, and
  expected process exit codes are preserved without modifying application
  sources or generated DEX semantics.
- Hidden API metadata is encoded as the standard DEX
  `hiddenapi_class_data_item`; the implementation supports both legacy headers
  and DEX 041 single-container headers and recalculates the map, SHA-1, and
  Adler-32 fields. This closes 999 through ART's normal enforcement domain.
- Platform-only test JNI that calls private Runtime/Heap methods remains inside
  the runtime image and is registered only when the untouched Main class
  declares the corresponding native contract. It is not exported as an APK API
  and does not broaden the Darwin application ABI.
- OpenJDK JVMTI retains the pinned 29-source AOSP implementation. The only new
  allocator delta replaces its explicitly nonfunctional Apple accounting
  placeholder with Darwin `malloc_size()`; the patch and checksum are locked.
  Original 1900 proves concurrent and multi-environment accounting end to end.
- The current source-native frontier is original 1903; resume at 1904. Two
  distinct infrastructure debts remain queued: 980 needs Android-equivalent
  post-main process lifecycle, and 1001 needs a real profile/dex2oat app-image
  producer/loader. Neither should be papered over in expected-output handling.
- Incremental graph correctness remains open: content-stamp changes do not
  always invalidate the first requested runtime dylib target. Fix the missing
  dependency edge while retaining per-owner archives so small Darwin adapter
  changes stop triggering broad rebuilds.

### AOSP execution-contract continuation — 2026-09-06

- The verified unmodified ART frontier is now numeric test `2286` in both
  interpreter and optimized ARM64 JIT modes. Coverage added tracing v2,
  ProfileSaver inline caches, cyclic DEX 035, generated const-method-type/handle
  bytecode, hidden API method handles, JVMTI single-step, and method tracing.
- Darwin libartbase is now an Android-aware platform boundary: host/build-tool
  use falls back to native paths through a weak resolver, while app-private
  paths are capability-resolved before `OS::OpenFileWithFlags` and ScopedFlock's
  inode check. This replaces trace/profile test shims with one runtime rule.
- AOSP run-test native helpers remain owned once by the runtime image. Their C
  JNI surface is exported as the pinned libarttest contract; private file
  implementation stays behind a narrow C bridge, avoiding a second libartbase
  copy and its conflicting zip/fmt/logging dependency closure.
- Runtime launch options can now be passed through `ParsedOptions` before
  `Runtime::Create`, allowing upstream/profile contracts to configure ART's
  real JIT/ProfileSaver structures. Production APKs retain ordinary defaults.
- Build metadata is treated as part of compatibility: named DEX API aliases,
  Java 17 source/target, per-output bytecode transforms, generator-owned invalid
  DEX, Smali JVM flags, multidex, and run.py output normalization are reproduced
  without editing app or AOSP test sources.
- Next architecture work: repair bootstrap source-path dependency invalidation;
  close `980` with Android-equivalent process shutdown ownership; implement the
  `1001` profile/dex2oat app-image producer and loader; then validate production
  APK JIT plus Mach-O unwind/deoptimization. The 100% goal is not complete.

### ART app images, tool ABI, and incremental ownership — 2026-09-06

- The AOSP app-image producer/consumer path is now owned end to end by the
  compatibility runtime. Embedded profman/dex2oat receive process-compatible,
  NULL-terminated argv; BootClassPathLocations remains distinct from physical
  host paths; application sources and APK/JAR payloads are untouched.
- Image files retain Android's 32-bit logical addresses. `RelocationRange` and
  pointer-valued `ImageHeader` accessors form the Darwin conversion boundary to
  the fixed high compressed-reference arena. Fixed executable Android OAT ELF
  segments use an Apple W^X bridge (anonymous copy then RX); data segments keep
  ordinary file-backed mappings.
- Normal app processes now follow Android's JIT-on default without a launcher
  flag. The `DARWIN_ART_JIT` setting remains only as a strict `0`/`1`
  diagnostic/differential override; the AOSP runner selects both modes
  explicitly.
- Native object fingerprints now include `RUNTIME_CACHE_IDENTITY`, so adding a
  newly shadowed header cannot reuse depfiles that resolved the old upstream
  path. Runtime archives are preserved when every member is cached, preventing
  a no-op bootstrap from changing archive metadata and triggering a large final
  link. Verified cold migration `254 compiled / 0 cached`, then immediate
  incremental `0 compiled / 254 cached`.
- Verification: original AOSP 980 and 1001 pass interpreter+optimized JIT;
  default-no-override JIT acceptance covers arithmetic, calls/JNI, exceptions,
  fields/arrays/types, GC/read barriers, OSR, deoptimization, monitors,
  intrinsics, and concurrency; graphics link closure reports registrar 51 with
  no fake symbols or host ICU/fmt edges. Full compatibility remains unfinished.

### Production JIT/native closure checkpoint — 2026-09-06

- Production validation now uses the unchanged installed Blue Archive base and
  arm64 split APKs under the normal JIT-on policy. It reaches a correctly
  rendered Nexon login activity and Metal-backed 1280x720 SurfaceView after
  Conscrypt, network, and guest DSO initialization, without APK modification.
- Guest ELF graph creation owns a narrow temporary daemon-attachment scope for
  detached native callers. Already-resident lookup stays outside ART and each
  temporary attachment is removed immediately after loader/ClassLoader setup.
- Provider ownership is explicit in isolated builds as well as the final
  runtime: the pthread standalone acceptance archive links the same errno TLS
  implementation used by the composed graph. Android-ELF and sanitizer stress
  pass, and the final 36-provider closure remains duplicate-free.
- Framework/Bionic gaps exposed by the real workload were fixed at common
  owners: BinderInternal GC flushing at the Binder transport boundary and
  `strlcat`/`isprint` in the libc leaf facade. `libgrap-core.so` now loads and
  remains in the guest namespace.
- The ARM64 builder preserves representable Mach-O CFI and isolates only the
  two dlsym state-machine stubs in JNI/native assembly. Quick entrypoints still
  contain nonlinear CFI state and alternate Baker symbols rejected by Apple's
  assembler; that lowering, full corpus accounting, and longer production soak
  remain before completion.

### ARM64 unwind architecture checkpoint — 2026-09-06

- Quick-entrypoint CFI is now complete rather than selectively disabled. The
  Darwin builder treats each AOSP fast/slow or normal/exception PC range as an
  adjacent FDE and seeds the exact CFA/register state at alternate entries.
  This is a file-format lowering only: emitted instructions and fixed Baker
  entrypoint addresses are unchanged.
- The build validates 317 compact-unwind entries, 259 DWARF FDEs, zero
  `llvm-dwarfdump` errors, exact split inventories, and representative complex
  entrypoints. The full runtime/graphics link and default JIT suite pass after
  relinking.
- Remaining native-unwind architecture work is deliberately outside the
  managed compiler: lower the two dlsym lookup assembly stubs, replace the
  false-return Darwin `AndroidLocalUnwinder`, add dyld/Mach memory and register
  providers for remote tasks, then pass all three untouched AOSP 137-cfi runs.

### Default signal disposition behind sigchain — 2026-09-06

- The guest signal trampoline now distinguishes `SIG_IGN` from `SIG_DFL` even
  when ART's dispatcher remains at the front of the host chain. Ignored signals
  return; default signals restore the Darwin default action and re-raise rather
  than retrying a fault instruction forever.
- This closes a production failure-amplification path found by the unchanged
  Blue Archive soak. It does not classify the originating Unity SIGABRT as
  fixed: the `libgrap-core.so`/Unity crash path remains a separate workload
  investigation item for the 100% compatibility goal.

### ARM64 assembly unwind closure — 2026-09-06

- No ART ARM64 runtime assembly symbol is intentionally opaque to unwinding
  anymore. The two dlsym state machines join the already completed quick,
  ordinary JNI/native, and memcmp CFI paths.
- Darwin keeps AOSP's frame rules and lowers only the container representation:
  nonlinear control-flow joins become adjacent Mach-O FDEs, while dynamic
  critical-native save areas retain AOSP's x29-based DWARF expressions. A
  byte-for-byte `__text` comparison guards against accidental code changes.
- Source ownership for unwindstack has moved from a header-only snapshot to
  the complete pinned AOSP `libunwindstack` subtree. The next slice is a real
  Darwin platform backend: dyld/Mach VM maps, local and remote ARM64 register
  acquisition, Mach memory reads, and JIT descriptor symbolization. Until that
  backend replaces `AndroidLocalUnwinder::Internal*` false returns, AOSP 137-cfi
  remains an honest failing acceptance gate and the 100% goal is not complete.

### Upstream unwind engine ownership — 2026-09-06

- The compatibility layer will not grow a parallel stack walker. The pinned
  AOSP libunwindstack DWARF/ELF/JIT core is now a separately cached 21-object
  archive built from unchanged sources.
- Darwin-specific work is constrained to the same provider seams AOSP uses:
  map discovery, process memory, register capture, thread suspension, and
  demangling. Those providers will translate `/proc`/ptrace contracts to
  dyld/Mach VM/task APIs while retaining AndroidUnwinder and Unwinder above
  them.

### Launch-time Android x18 task ABI ownership — 2026-09-06

- The task ABI cannot be treated as package-install metadata. Cargo can relink
  `darwin-art-host` after installation, replacing its SDK-12 declaration with
  the current macOS SDK while leaving an otherwise valid code signature. An
  installed Blue Archive launch demonstrated the consequence at
  `libunity.so+0xc76794`: x18 was assigned a valid pointer three instructions
  earlier and was zero when dereferenced after scheduling.
- `tools/prepare-darwin-art-host.sh` is now the single launch-time owner. A
  `lockf` lock serializes the vtool/codesign transaction across concurrent app
  launches. Development hosts are repaired only when the SDK declaration or
  JIT entitlement is stale; packaged hosts are immutable and fail closed if
  their build did not establish the contract.
- The production launcher uses this boundary for direct APKs, installed launch
  records and install-only preparation. The standalone audit now exercises two
  racing preparations and actual x18 scheduling preservation. The unchanged
  Blue Archive workload passed the former Unity crash and ran for more than
  three minutes without a native signal marker.

### Canonical probe include ownership — 2026-09-06

- The incremental native graph no longer duplicates an older JNI/GPU probe
  include inventory. Standalone Ninja edges obtain their ART, libnativehelper,
  Bionic and pinned AOSP unwindstack paths from `core_probe_includes`, the same
  owner used by direct CPU and graphics link commands.
- Invalidating the JNI acceptance object now recompiles it successfully and
  the final graphics closure/link audit passes. This removes a stale-object-only
  success mode and keeps a narrow probe edit to the intended object/relink path.

### Original prebuilt DEX ownership — 2026-09-06

- AOSP test fixtures with intentionally malformed or unusual DEX files are now
  immutable runtime inputs. The test launcher copies their bytes directly;
  D8 is used only for separately generated launcher support. Prebuilt multidex
  entries retain their original ordinal and support is appended after them.
- This distinction matters for runtime evidence: rebuilding a duplicate-method,
  odd-sized or invalid-check-cast DEX would erase the behavior under test. All
  eight pinned prebuilt managed cases now pass both interpreter and optimized
  JIT execution without such normalization.
- Corpus ownership also distinguishes 63 shared fixture directories and the
  `000-nop` harness self-test from executable ART tests. Conversely, matching
  output from AOT-specific `845-fast-verify` is not counted until its `.dm` and
  dex2oat compiler options are reproduced by an AOT owner outside this JIT goal.

### AOSP run invocation and signal ownership — 2026-09-06

- `run.py` is a sequence of process contracts, not a bag of flags. The Darwin
  ART runner now models literal `default_run()` calls as ordered invocations
  carrying separate runtime options, application argv, and exit status. Output
  concatenation remains harness-owned; runtime and application source are not
  rewritten to fit a single synthetic invocation.
- Android process signals are owned below libcore. `libcore.io.Linux.kill()`
  delegates to the Bionic process-state provider, which translates Android
  signal numbers before entering Darwin. This keeps `android.system.Os` on the
  same layering used by ordinary native ELF imports and avoids a second host
  signal mapping in Java/JNI.
- ART test-native C++ ABI exports are centralized in the runtime link policy.
  ThreadStress retains its pinned `libarttest` implementation and consumes
  `mirror::String` and `mirror::Throwable` methods from the runtime image, as it
  does beside `libart.so` on Android.
- The upstream launcher owns uncaught-main presentation exactly once. Java
  writes Android's owner-thread prefix and stack trace through the descriptor
  bridge; native code uses the retained Throwable only for exit status. This
  removes duplicate JNI `ExceptionDescribe()` output without changing normal
  application exception dispatch.
- Remaining architecture work is to interpret dynamic run.py branches and
  separate JIT/runtime executions from dex2oat, profile, image, JVMTI and host
  harness phases. Completion still requires each applicable pinned contract,
  not merely broader source compilation.

### Uncaught output versus process failure — 2026-09-06

- The upstream process launcher now tracks two orthogonal facts: whether Java
  dispatched a Throwable to an application handler, and whether Main failed.
  Handler dispatch suppresses duplicate launcher output but never converts the
  failed process into success.
- ThreadGroup remains the VM fallback when no default handler exists. When an
  application installs a process default handler, the same ThreadGroup is
  invoked so Java performs its standard delegation. This is ordinary Java/ART
  lifecycle behavior, not a test-name exception.
- The behavior is covered by pinned null-call, uncaught-handler, bad-finalizer
  and finalizer-throw processes in both interpreter and optimized modes.
- Production linkage intentionally still uses the existing runtime boundary;
  the upstream archive will replace it only after current-thread,
  other-thread, remote-process, Mach-O, guest ELF, and ART JIT frame tests all
  pass. This prevents a partially functional diagnostic path from being
  mistaken for compatibility completion.

### NativeBridge is an AOSP-owned runtime lifecycle — 2026-09-06

- Process-wide bridge state is no longer represented by capability-closed
  Darwin stubs. `darwin_native_bridge_stubs.cc` compiles the pinned Android 16
  libnativebridge implementation; Darwin code supplies only host ABI selection
  and translated Android-ELF trampoline lookup before deferring to Android's
  callback table.
- Detached startup now follows the relevant `Runtime::Start()` ordering:
  system class loader publication and late well-known-class initialization
  precede NativeBridge preinitialization/initialization. NativeLoader tries the
  preferred native Darwin/dylib path first and then uses the Android bridge
  fallback, preserving the project's macOS-native optimization without
  inventing a second lifecycle.
- ART's signal chain remains the process owner. A narrow Darwin interposition
  surface lets the bridge test DSO install its downstream Android handlers
  while retaining ART's dispatcher; normal JNI DSOs keep the ordinary Darwin
  path. This split is verified by passing `115-native-bridge`,
  `004-SignalTest`, and `150-loadlibrary` in interpreter and optimized modes.
- This milestone does not imply complete ART support. Completion still means
  the applicable pinned AOSP corpus and real APK workloads run without
  per-method/test allowlists, altered application sources, or interpreter
  fallback presented as JIT success.

### HPROF ownership and host-tool path boundary — 2026-09-06

- Heap serialization is ART-owned. The Darwin runtime now compiles AOSP's
  `hprof.cc`; the former empty `DumpHeap()` platform stub was removed. Only the
  final `/data` path is resolved to the process-authorized private host backing
  before AOSP opens the output.
- `java.library.path` is an explicit launcher property on Darwin rather than an
  accidental dependency on dyld retaining `DYLD_LIBRARY_PATH`. Host validation
  helpers live beside the test native directory as they do in AOSP's run-test
  layout, and their Android-private file arguments are translated only at the
  host-tool wrapper boundary.
- The native graph now includes the system-native lock and its libcore/BoringSSL
  patch inputs. Changing a standalone archive contract therefore invalidates
  the graph and final link instead of leaving an older runtime dylib in place.
  The rebuilt graph linked 255 ART runtime objects, adding HPROF as one cached
  translation unit; the graphics closure audit remains complete.

### Run-test process and private-ABI ownership — 2026-09-06

- The dalvikvm-equivalent launcher models application output as one Java/native
  process stream: Java `System.err` and C++ `std::cerr` during Main share the
  captured descriptor, while Android logging remains on the host/logd side.
  This is a process contract, not a `143-string-value` normalization.
- ART-private native test helpers no longer force `Heap`, `ThreadList`, or
  `Monitor` C++ symbols into the public Mach-O application ABI. Their
  implementation and JNI registration live in the runtime image, and the
  declaring application class is derived from the pinned helper source. The
  same owner serves GC coverage, concurrent annotation GC, suspend-all stress,
  and lock visitation.
- Hidden platform compiler views remain signature-only inputs and are excluded
  from application DEX. At runtime the app links to the pinned Android boot
  classes, preserving boot-class identity while allowing `src-art` to compile
  against the platform surface that AOSP Soong exposes.
- The harness follows pinned build/run intent such as target Smali selection,
  error-only filtering, and final-line comparison. These decisions are parsed
  from the upstream scripts rather than accumulated as expected-output or
  method allowlists.
- Tests through `169-threadgroup-jni` now cover real app images, read barriers,
  malformed verifier input, multidex method resolution, monitors, suspend-all,
  thread groups and native registration in interpreter and optimized modes.
  This is incremental evidence only; the architecture migration stays open
  until the remaining corpus and real application acceptance meet the 100%
  goal.

### ARM64 image/JNI boundaries and build-graph ownership — 2026-09-06

- Android's managed references and 32-bit image-relative method entries remain
  unchanged. Darwin restores the high compressed-reference/image window only
  at native monitor and `ArtMethod*` dereference boundaries; application DEX,
  JNI declarations and AOSP test sources are not rewritten.
- The compiler archive is now a declared predecessor of dex2oat and the final
  runtime linkage audit. A codegen patch therefore cannot pass testing against
  a stale archive. The subsequent app-image method-address change demonstrated
  narrow invalidation: one compiler translation unit rebuilt, 105 were cached.
- Upstream build semantics are data, not per-test runtime policy. The run-test
  adapter derives D8 container mode and deliberate DEX magic rewriting from
  pinned `build.py`, and isolates its API-26 launcher support DEX from older
  primary DEX versions.
- Original-source tests `170`–`183` now pass through interpreter and optimized
  execution, including app images, synchronized/critical JNI, nonvirtual JNI,
  default-method linking and concurrent atomic RMW stress. This advances the
  migration but does not complete it; the remaining pinned corpus and real
  application acceptance stay mandatory.

### JVMTI/runtime parity expansion through test 1951 — 2026-09-06

- Fifty applicable, unchanged Android 16 AOSP tests from `1900-track-alloc`
  through `1951-monitor-enter-no-suspend` pass in interpreter and optimized
  JIT modes with exact output. Missing numeric IDs are not counted.
- The same runtime-owned openjdkjvmti and ART paths handle allocation events,
  transformation/redefinition, suspend/safepoints, frame and local access,
  exception delivery, proxy frames, monitor ownership/contention and raw
  monitors; the adapter did not gain per-test emulation or bytecode policy.
- This is strong parity evidence for the existing AOSP-shaped architecture,
  not completion. The next incremental starting point is `1953-pop-frame`;
  the remaining pinned corpus plus unchanged production APK and Blue Archive
  acceptance still gate the 100% compatibility claim.

### Parsed run-test argv semantics and coverage through 2040 — 2026-09-06

- The run-test adapter now models `add_libdir_argument=True` as a parsed
  `default_run` property, alongside runtime options, test arguments, main class
  and expected exit status. This replaces an ordering-broken environment
  special case and keeps upstream launch semantics in the generic build/test
  boundary.
- Unchanged `2031-zygote-compiled-frame-deopt` consequently passes both modes,
  proving the deferred agent can deopt a frame compiled before simulated
  zygote specialization. The broader unchanged `1953`–`2040` set adds 84
  passing tests and exercises concurrent structural redefinition, JNI/reflection
  identity, JIT frame control and optimizer loop/inlining behavior.
- Completion remains unclaimed. Continue at `2041-bad-cleaner`, then finish the
  pinned corpus and the required differential, GC/concurrency, production APK
  and Blue Archive acceptance gates.

### Runtime primitives through VarHandle test 2239 — 2026-09-06

- Unchanged `2041`–`2048`, `2230`–`2238`, and all seventeen `2239` VarHandle
  variants add 36 interpreter/JIT exact-output passes without architecture or
  test-source exceptions.
- The evidence spans reference processing, unsafe/native failure paths,
  profiling and metrics, suspend-check removal, multidex inlining and atomic
  memory-ordering APIs. This materially extends runtime coverage but does not
  satisfy the complete-corpus or production-application completion gates.
- Resume at `2240-tracing-non-invokable-method`.

### Execution coverage through test 2279 — 2026-09-06

- Forty-nine unchanged applicable tests from `2240`–`2279` add exact-output
  interpreter/JIT evidence across tracing, CFG/try transforms, barriers, GC,
  MethodHandle, inline caches, concurrency and optimizer behavior.
- No architecture shim or test-specific input rewrite was added. This supports
  the existing AOSP-shaped runtime but does not complete the remaining corpus
  or real-application acceptance requirements. Resume at `2280`.

### Numeric tail and invocation-contract audit — 2026-09-06

- The final nine `2280`–`2286` tests pass unchanged in interpreter and JIT.
  The pinned archive ends at 2286; current accounting is 1,074 numeric runnable
  tests, one harness-only numeric directory and 63 shared fixtures.
- All 348 `run.py` files have their `default_run()` call count represented by
  the parser. Remaining architecture debt is semantic: profile/app-image/vdex/
  compiler properties must become per-invocation build contracts instead of
  mixed global and special handling before full-corpus closure can be claimed.

### Preserve Android compiler options end to end — 2026-09-06

- The run-test compatibility boundary now represents profile/JIT/zygote/log
  and compiler options per `default_run()` invocation. Profman/dex2oat output
  is keyed by the effective invocation contract, eliminating both first-match
  option leakage and redundant same-contract app-image builds.
- `runtime_entry_probe.cc` previously parsed Android `-Xcompiler-option` pairs
  correctly and then replaced `RuntimeArgumentMap::CompilerOptions` while
  adding launcher defaults. It now extends the parsed vector. Ownership stays
  AOSP-shaped: ParsedOptions owns Android argv, the launcher adds only its
  process metadata, and JitCompiler consumes the merged Runtime vector.
- The correction makes upstream `--debuggable` reach HGraph/StackMapStream and
  set `CodeInfo::kIsDebuggable`. Unchanged `597-deopt-busy-loop` consequently
  passes real optimized Simple/Float/SIMD loop suspension and asynchronous
  deoptimization; this was option loss, not a Mach-O CFI workaround.
- Pinned Android 16 `ZygoteHooks` is exposed only to javac as a signature-only
  hidden-platform class. Runtime resolution remains against the boot class.
  `689-zygote-jit-deopt`, `552-checker-sharpening`, Python syntax, diff checks,
  and the full incremental graphics link closure pass. Continue with explicit
  vdex/prebuild/app-image invocation ownership and production regressions.
- Unchanged `457-regs`, `993-breakpoints-non-debuggable`, and `2246-trace-v2`
  pass after the merge, covering baseline compiler policy, a deliberately
  non-debuggable process, and tracing/JVMTI-sensitive debuggable code.

### Restore Android VDEX and AOT ownership — 2026-09-06

- The run-test boundary now represents VDEX generation and filtered
  recompilation per `default_run()` invocation. Artifact ownership remains
  Android-shaped: dex2oat creates `.vdex`/`.odex`, OatFileAssistant selects
  them, ClassLinker attaches the OatDexFile, and Instrumentation selects its
  executable method entrypoint.
- Runtime startup no longer treats “JIT disabled” as “force `-Xint`.” These
  are separate AOSP options. The harness likewise does not overwrite an
  executable speed-AOT entrypoint with its differential JIT precompile.
- Deleted the historical Darwin load-kind admission loop in
  `OptimizingCompiler`. Missing compatibility is now repaired at the ARM64
  representation boundary instead of hiding methods from the compiler. The
  first exposed defect was an implicit-null-check load from a base-relative
  compressed reference; codegen now decodes the nullable reference into a
  scratch native address before performing the faulting load.
- The seven unchanged VDEX tests and deoptimization, zygote, sharpening, and
  method-tracing regressions pass, as does the full incremental link audit.
  Continue next with unmodeled prebuild/app-image/DM invocation properties,
  followed by the full corpus and real-application acceptance required for
  the active 100% target.

### Make AOSP invocations own installed-code artifacts — 2026-09-06

- Prebuild, application image, verifier, secondary-dex/CLC, DM and JVMTI state
  now belongs to each parsed `default_run()` invocation. The adapter no longer
  infers application images from profiles or attaches a JVMTI agent globally.
- Artifact flow is Android-shaped: profman creates profiles, dex2oat owns
  primary and secondary `.art/.odex/.vdex`, secondary compilation carries its
  defining loader context, DM archives contain `primary.vdex`, and ART selects
  installed output from the unchanged class path.
- Ordinary, no-app-image, no-prebuild, secondary, profile-switching, VDEX and
  both DM contracts pass exact output. Keep the goal active while completing
  no-image/relocation ownership, hidden-platform compiler signatures, the full
  corpus, and real-application acceptance.
- Hidden `VMDebug` and `PathClassLoader.addDexPath` are now supplied through a
  compile-only boot signature view. Runtime ownership remains with core-libart;
  this unblocks the unchanged no-prebuild native and same-classloader app-image
  tests without adding replacement classes to the application.

### Keep runtime-image, heap-policy, and GC-stress ownership in ART — 2026-09-06

- AOSP `RuntimeImage` is compiled again; Darwin owns only conversion between
  Android logical references and host pointers at the image boundary. The
  host runner supplies a private process sandbox but does not synthesize image
  contents or bypass ART's heap-task lifecycle.
- Parsed Android heap options take precedence over detached-launch defaults.
  This lets run-test use its standard 2 MiB/16 MiB GC-stress policy without an
  app-specific host flag.
- `BacktraceCollector` remains the AOSP GC-stress policy owner. On Darwin it
  calls the common Mach unwindstack provider, while Linux retains its existing
  `Unwinder` and maps-reparse retry path.
- Darwin's two-phase runtime startup now preserves `Runtime::Start()`'s thread
  state transition before native initialization, SignalCatcher, and daemon
  startup. This is a startup lifecycle correction, not a GC test exception.
- Current exact-output gates cover ordinary execution, precise roots,
  monitors, exceptions, app-image generation/reload and ARM64 JIT. Completion
  remains gated on the complete pinned variant matrix and unchanged production
  application soak/restart runs.

### Progress — 2026-09-07 JNI header identity and script-contract IR

- The `004-JniTest` crash was not an invalid GenericJNI call. A valid shared
  boot JNI stub frame was interpreted with the wrong layout because Apple had
  no implementation of ART's runtime-stub ownership query. Darwin now compares
  the Mach-O load image containing the PC with the image containing
  `Runtime::Current`, preserving ART's existing header-selection algorithm.
- Production diagnostics are clean. Runtime bootstrap/link audit, unchanged
  `004-JniTest`, `137-cfi`, `178-app-image-native-method`, and the adjacent
  native/reference/stack/thread regressions pass without test or input changes.
- The next architecture migration replaces the monolithic runner's AST/text
  guesses with a non-Turing-complete `ContractIR` and concrete `ActionPlan`.
  It preserves ordered branches, assertions, mutable args/env snapshots,
  `default_run` invocations and typed file transforms without importing AOSP
  scripts or executing shell text. Separate restricted frontends own `run.py`,
  `build.py`, and `javac_post.sh`; unknown syntax and unparsed command bytes
  fail closed. This is in progress and the overall compatibility goal is not
  complete.

### Progress — 2026-09-07 JVMTI native-module ownership

- The first unchanged `909-attach-agent` run proves native loading and ART's
  live attach path work through the point where `Agent_OnAttach` is called.
  Failure is the reduced Darwin agent dispatcher: it rejects the valid AOSP
  option `909-attach-agent` because no comma-separated tail is present, so the
  already-linked `Test909AttachAgent::OnAttach` owner is never reached.
- Native test artifacts must now follow pinned Soong module ownership rather
  than test-directory ownership. A reusable `libtiagent` should compile AOSP's
  `common_load.cc` dispatch and its declared source closure; `libarttest`
  should independently compile its declared closure. For test 909 this keeps
  `attach.cc` in the former and `disallow_debugging.cc` in the latter, removes
  duplicate owners, and preserves the four upstream VM lifecycle variants.
- Completion remains gated on implementing that generic module boundary,
  passing unchanged 909 in interpreter and JIT modes, and continuing the full
  corpus and production-application acceptance matrix.

### Progress — 2026-09-07 differential-mode sandbox isolation

- The unchanged `817-hiddenapi` run exposed a generic runner boundary bug:
  interpreter and JIT shared writable `$DEX_LOCATION`, so the second process
  failed when AOSP's JNI test copied `libhiddenapitest.so` to a fixed path.
  Each differential mode now receives an isolated writable DEX/cwd and typed
  action root; immutable staged jars and native fixtures remain shared.
- Exact-output gates pass for both unchanged variants (`interpreter` and
  `jit`), with no test-name exception or APK/source edit. The complete corpus
  and production-application acceptance matrix remain outstanding.

### Progress — 2026-09-07 typed custom-script execution

- The upstream runner now evaluates `build.py` and `javac_post.sh` through
  ContractIR and ActionPlan end-to-end. Typed compiler flags and API settings,
  ordered build actions, confined file edits, and class-directory `$1`
  post-processing are all carried as concrete actions.
- Removed build-script regex guesses and all `bash`/sourced `javac_post.sh`
  execution. Explicit external Java tools are admitted only through the
  ActionPlan tool boundary; unsupported syntax still fails closed.

### Progress — 2026-09-07 mode-root staging verification

- Mode isolation now copies only the private seed plus late build-owned
  resources and the DEX/JAR basenames that unchanged tests address beneath
  `$DEX_LOCATION`; it does not recursively copy the whole build staging tree.
- Fresh exact-output verification of unchanged `817-hiddenapi` passes in both
  interpreter and JIT modes, with distinct per-mode `libhiddenapitest.so`
  outputs. No pinned test or APK input was changed.

### Progress — 2026-09-07 typed capability audit

- Replaced runner name checks for target SDK, NativeBridge setup, deferred
  zygote JVMTI handling, and CFI mode with capabilities derived from typed
  `run.py` actions and declared native/source contracts.
- Retained only private native ABI/module ownership boundaries where no typed
  upstream capability exists; unknown inputs continue to fail closed.

### Progress — 2026-09-07 dedicated OpenJDK named-JNI owner

- Darwin minimal ART startup now loads a sibling absolute RTLD_LOCAL owner
  through JavaVMExt at the AOSP-equivalent native-state point, with
  `java.lang.Object` and a null class loader. The aggregate runtime image is
  never registered as the boot native library, keeping framework/test
  `Java_*` symbols out of boot lookup candidates.
- The owner build derives an exact 64-entry manifest from the pinned AOSP
  OpenJDK source tables and their archives, rejects missing and surplus
  exports, and intentionally exports no JNI_OnLoad because no owned source
  provides it. Graphics/full link validation is wired; fresh unchanged 817
  and 909 verification remains pending after the link audit blocker.

### Progress — 2026-09-07 validation checkpoint

- The dedicated owner’s rebuild and exact `nm -gU` audit pass with 64 Java
  exports, no JNI_OnLoad, and no non-Java surplus. Graphics-fast and full
  runtime-link audits reach linking but remain blocked by unresolved
  `darwin_art_bionic_readv`, `darwin_art_bionic_writev`, `open`, and the
  existing broader runtime undefined set.

### Progress — 2026-09-07 CPU link ownership closure

- The former 59 CPU-link undefineds are classified and closed through actual
  AOSP OpenJDK, fdlibm/BoringSSL/ICU, ART test, androidfw/resource JNI,
  SurfaceFlinger/framework, Android graphics JNI, and production GPU owners.
  `audit-runtime-link` now passes with `undefined=0`; no dummy provider or
  fallback was added.

### Progress — 2026-09-07 build/staging capability migration

- Removed build/staging test-name branches for hidden compiler classes and
  platform compatibility headers. Selection now follows source-declared
  capabilities (`AnnotatedStackTraceElement`, hidden APIs, ucontext, and
  reference VRegs), preserving exact inputs while failing closed on absent
  declarations.
- VM-option behavior for target SDK, metrics, and swappable JNI IDs now comes
  from typed `run.py` runtime options. Native ABI ownership checks remain
  explicit where AOSP exposes no typed metadata.

### Runner capability audit — 2026-09-07

- Replaced test-name build/staging branches with source-declared capabilities
  for compiler debuggability, JVMTI attach, trace-v2, runtime startup/shutdown
  ownership, and JVMTI redefinition dispatch.
- Contract/unit validation remains green; unchanged execution is still gated
  by the pre-existing runtime-link/sandbox environment failure.

### Progress — 2026-09-07 CPU link ownership closure

- Classified the original 59 undefineds and connected their real AOSP or
  Darwin owners, including OpenJDK, ART-test, androidfw/resource JNI,
  SurfaceFlinger/framework, graphics JNI, and GPU providers. CPU
  `audit-runtime-link` passes with `undefined=0`; no dummy or fallback was
  added.

### Revalidation checkpoint — 2026-09-07

- Final-tree `817-hiddenapi --keep` was re-run serially; interpreter/JIT
  execution did not start because the sibling named-JNI owner failed its
  production `dlopen` boundary on `_JVM_GetLastErrorString`. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-loy0bxtx`.
- `909-attach-agent` and `2286-method-tracing-aot-code` were not started after
  this blocker surfaced; no result is claimed for either gate.

### OpenJDK JVM shared-owner correction — 2026-09-07

- The `_JVM_GetLastErrorString` failure was an export-boundary error, not a
  missing implementation. Pinned AOSP declares `libopenjdkjvm` as a shared
  dependency of `libopenjdk`; Darwin's aggregate runtime already contains the
  real `OpenjdkJvm.cc` module and is opened `RTLD_GLOBAL`, while the dedicated
  named-JNI image is opened `RTLD_LOCAL`.
- Removed the temporary duplicate `OpenjdkJvm.cc.o` from the named-JNI image.
  The production runtime export list is now mechanically extended from the
  pinned `libopenjdkjvm` archive (55 `JVM_*`/`jio_*` entries) and from the real
  libnativehelper provider required by `libopenjdk_native_defaults`. The owner
  build/audit requires `_JVM_GetLastErrorString` to remain undefined there and
  rejects any private duplicate `JVM_*`/`jio_*` definition.
- `audit-runtime-graphics-link-fast` passes, including a subprocess smoke that
  opens the runtime globally, verifies `dladdr(JVM_GetLastErrorString)` names
  that image, opens the owner locally, and resolves a manifest JNI entrypoint.
  A fresh unchanged
  `817-hiddenapi --keep` run crossed the former dyld failures for both
  `_JVM_GetLastErrorString` and `_jniRegisterNativeMethods`, proving the
  production `RTLD_GLOBAL(runtime) -> RTLD_LOCAL(owner)` load. It next stops
  at ART's startup `AssertLocalsEmpty()` with capacity 3, after the owner has
  loaded; no 817 interpreter/JIT result is claimed. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-rczqsrs1`.

### JNI initialization-frame and external-module closure — 2026-09-07

- The post-owner startup failure came from three local references created by
  Darwin's composed boot-native registrar phase. That phase now owns a nested
  `PushLocalFrame`/RAII `PopLocalFrame` and releases it before
  `FinishMinimalForDarwinProbe()`, matching the per-`JNI_OnLoad` lifetime on
  Android while preserving ART's unmodified `AssertLocalsEmpty()` checks.
  Every partial-registration return unwinds the same frame.
- AOSP's `libarttest_external` declares `libarttest` as a shared dependency.
  The run-test builder now emits the equivalent Mach-O dependency and fixture
  rpath for every typed `_external` test library instead of leaving ART-test
  API calls to flat-namespace lookup. This closed 817's real
  `SetDedupeHiddenApiWarnings` provider without a test-name branch.
- Fresh unchanged `817-hiddenapi --keep` passes exact output in interpreter
  and optimized JIT modes; adjacent unchanged external-library test
  `656-annotation-lookup-generic-jni` also passes both modes. Evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-m3nez_vi`.

### Progress — 2026-09-07 generic corpus ledger

- Added generic discovery and resumable execution for all 1,138 pinned ART
  test directories with no per-test skip or allowlist. Shards and parallel
  workers are selectable while default execution remains serial and safe.
- Atomic deterministic TSV/JSON summaries retain input/runner hashes, status,
  exit code, captured streams, and artifact paths for restartable runs.

### Revalidation — 2026-09-07

- Final-tree serial validation of unchanged `909-attach-agent --keep` passed
  interpreter, JIT, and source interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-909-attach-agent-v32mh1vw`.
- The subsequent serial `2286-method-tracing-aot-code --keep` passed
  interpreter, JIT, and source interpreter+optimized checks with no build
  contention. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2286-method-tracing-aot-code-xoj20wza`.

### Revalidation — 2026-09-07 no-op corpus input

- Generic typed execution now handles `000-nop` as a valid no-managed-input
  harness test, preserving its unchanged `ctx.echo` output in both modes.
- Resume-by-input/runner hash re-executed the stale ledger entry and recorded
  the new passing result without changing pinned test files.

### Revalidation — 2026-09-07 004-JniTest JNI ABI boundary

- The reproducible first regression was a generated ARM64 JNI stub using a
  low-32-bit logical `ArtMethod` before host-pointer normalization. Rebuilding
  the patched JNI compiler owner and relinking the generic runtime closure
  restored the production boundary without test-name rules or input edits.
- The stale state came from the headless `audit-runtime-link`/`all` path not
  owning its `libart-compiler-darwin.a` producer, while the Ninja JIT edge did
  not fingerprint patch 0145 and the other late JNI/codegen patches. Both
  ownership gaps are now closed: the headless link builds the producer first,
  and the graph tracks every applied JIT patch for invalidation.
- Unchanged `004-JniTest --keep` passes exact interpreter, JIT, and source
  interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-JniTest-uv29ee0y`.

### Runner lifecycle boundary — 2026-09-07

- The per-mode ART runner and generic corpus runner now execute each child in
  its own process group. Timeout handling is terminate-group, bounded graceful
  wait, kill-group, and unconditional reap, so descendants cannot overlap the
  next mode/test after a timeout.
- The focused Python unit test exercises a SIGTERM-ignoring descendant and the
  kill/reap path without running ART or adding test-specific behavior.

### Resolution — 2026-09-07 004-SignalTest shutdown boundary

- The shutdown thread was blocked in the first `Daemons.stop()` join for
  `HeapTaskDaemon`. Both that daemon and `FinalizerWatchdogDaemon` were stopped
  at ARM64 implicit suspend-check null faults, but their live signal mask was
  `0x1ffefeff`, including SIGSEGV, so neither fault could enter ART's fault
  manager. `Signal Catcher` was still in its normal `sigwait` and was not the
  owner of this wait.
- SignalTest's restored SIGSEGV chain remained structurally intact: the kernel
  action was the Darwin dispatcher, ART's special action was
  `art_sigsegv_handler`, and the saved next action was the runtime's unexpected
  signal handler. The leaked mask instead exactly matched SignalTest's
  full-minus-SIGUSR2 action after its handler unblocked SIGUSR1.
- Darwin's Mach SIGBUS-to-SIGSEGV bridge invokes the chained SIGSEGV action as
  an ordinary function. Unlike AOSP's kernel-delivered chain, that call has no
  `sigreturn` boundary to restore the interrupted mask. The generic user-chain
  dispatcher now snapshots `ucontext_t::uc_sigmask` and restores it after any
  returning application handler; no daemon, test-name, timeout, or forced-exit
  behavior was added.
- Incremental runtime/graphics rebuild and link audit pass. The sigchain smoke
  covers ordered special handlers, user replacement, nested no-return, and the
  direct bridge-mask restoration path. Unchanged `004-SignalTest --keep`
  passes exact output and clean shutdown in interpreter and optimized JIT
  modes. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-SignalTest-at_flr54`.

### Typed run contract boundary — 2026-09-07 004-ThreadStress

- The generic upstream runner now models AOSP's shell word boundary for
  `default_run(test_args=...)`: the assembled typed suffix is POSIX-word
  split with `shlex`, so the unchanged `--locks-only -o 100` invocation
  reaches Java as `--locks-only`, `-o`, `100`.
- Aggregate output is materialized before each ordered typed post-action and
  copied back after edits, preserving AOSP's cross-invocation `sed` output
  normalization without test-name handling or fallback execution.
- Unchanged `004-ThreadStress --keep` passes exact interpreter and optimized
  JIT output/exit contracts. Focused typed-run unit tests pass. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-ThreadStress-pgbfs0dd`.

### Typed invocation scalar state — 2026-09-07 116-nodex2oat

- The generic typed runner now preserves scalar values from each
  `default_run` `args_snapshot` while materializing an invocation. This keeps
  AOSP's `prebuild=False` no-oat contract intact instead of falling back to
  the `RunInvocation` default.
- Focused typed-run and run-contract tests pass. Unchanged
  `116-nodex2oat --keep` passes exact interpreter and optimized-JIT output and
  exit contracts. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-116-nodex2oat-wncamus_`.

### NativeBridge mode namespace — 2026-09-07 115-native-bridge

- The retained corpus failure occurred before VM launch: the typed `run.py`
  evaluator correctly rejected a native-test source path outside the
  per-mode action root. NativeBridge setup needs both the process-visible
  invalid library names in `DEX_LOCATION` and the real bridge/translated
  images in the Android native-test directory.
- For every parsed `-XX:NativeBridge=` capability, the runner now materializes
  an immutable native-fixture directory inside each mode sandbox and evaluates
  the unchanged run contract against that directory. It preserves the real
  `libarttest.so`/debug aliases there while the process-visible basenames stay
  invalid until NativeBridge redirects them to the `libarttest2` aliases.
- Typed `ln -sf` execution may replace an existing final symlink after
  validating its canonical parent remains inside the sandbox. It still fully
  resolves and confines every source and rejects directory-symlink escapes;
  the focused regression covers both sides of this boundary.
- ContractIR self-tests, typed-run tests, and the pinned NativeBridge
  state-machine/control-flow audit pass. Unchanged `115-native-bridge --keep`
  passes exact stdout, empty stderr, and clean exit in interpreter and
  optimized JIT modes. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-115-native-bridge-qg_k06wf`.

### Verification — 2026-09-07 126-miranda-multidex

- Generic javac-post `$1` equality detection is verified by focused typed-run
  tests and unchanged `126-miranda-multidex --keep` exact interpreter/JIT
  passes. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-126-miranda-multidex-os51ubbk`.

### Corpus suffix selection — 2026-09-07

- The generic corpus runner now accepts deterministic inclusive
  `--start-at TEST`/`--stop-after TEST` boundaries before optional sharding and
  limiting, so a blocker suffix can be resumed by name without dropping
  existing ledger records outside the selected range.
- JSON summaries retain per-result `runner_hash` values and expose their
  sorted unique set to make mixed-runner validation visible. Focused Python
  unit tests cover range validation, ledger preservation, and mixed hashes;
  no ART corpus execution was run for this change.

### Typed literal file-operation lowering — 2026-09-07 149-suspend-all-stress

- Generic typed shell evaluation now materializes nonnegative integer literal
  counts (`tail -n 1`) before execution; boolean values are rejected by the
  executor rather than accepted through Python's `bool`/`int` subtype.
- Focused Python contract/action tests pass. Unchanged
  `149-suspend-all-stress --keep` passes exact interpreter, optimized JIT, and
  source interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-149-suspend-all-stress-vhrifxp3`.

### JVMTI exception-event verification — 2026-09-07 1927-exception-event

- The retained first attempt stopped in app dex2oat with `SIGILL`, before the
  JVMTI agent or Java test entered the VM. The exact unchanged command reran
  successfully under LLDB and through the typed runner, so no JVMTI event
  contract was implicated and no test-specific workaround was added.
- Unchanged `1927-exception-event --keep` passes dex2oat, interpreter,
  optimized JIT, and source interpreter+optimized expected-output checks.
  Focused adjacent `1928-exception-event-exception --keep` passes the same
  three checks. Artifacts:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1927-exception-event-wd03r81e`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1928-exception-event-exception-fdeimqux`.

### Progress — 2026-09-07 2031 zygote/OAT namespace and corpus continuation

- Deferred zygote launch now preserves Android's logical boot-image location
  while supplying host backing files through the standard BCP image/vdex/oat
  FD vectors. This fixes 2031 without changing ART's trusted-OAT policy.
- Unchanged 2031 passes all three execution modes; corpus continuation has
  passed 2032–2036 under the current runner hash. A complete uniform-hash
  corpus run and production-app validation remain outstanding.

### Progress — 2026-09-07 2038–2039 continuation

- The unchanged 2038 HiddenAPI/JVMTI extension and 2039 load-transform test
  both pass with the same generic launcher/runtime contract. The corpus is
  still running; uniform-hash replay and real-app validation remain pending.

### Progress — 2026-09-07 2040–2047 continuation

- Tests 2040–2047 continue to pass with the generic ART launcher/runtime
  contract, including the expected bad-cleaner watchdog termination and the
  userfault/kernel-fault boundary tests. No policy gate or fallback was added.
- The full uniform-hash corpus replay and production-app validation remain
  outstanding.

### Verification — 2026-09-07 2041 watchdog timing

- 2041's long execution is intentional AOSP behavior: the bad Cleaner drives
  five ReferenceQueueDaemon timeout intervals, SIGQUIT thread dumping, and
  expected exit 2. Both interpreter and JIT contracts pass, so no special
  runtime or test handling was added. Corpus execution continues after 2047.

### Progress — 2026-09-07 2048–2239 continuation

- The bad-native-registry, GC/checker/inlining, and VarHandle tests through
  2239 pass unchanged. Metrics tests 2232 and 2233 currently fail only because
  Darwin routes Android logcat to host.log; the runner is being corrected to
  expose that stream at the AOSP stderr boundary without test-specific logic.

### Progress — 2026-09-07 2239 continuation

- Further VarHandle performance variants through `2239-varhandle-perf-vh-get`
  pass. Metrics failures remain a single generic host-log/stderr stream-boundary
  issue under investigation.

### Progress — 2026-09-07 2239 VarHandle continuation

- Additional unchanged VarHandle variants through `get-a` and `get-bav` pass;
  the corpus remains active. The only retained failures are the generic
  metrics log-stream boundary tests.
-
### Progress — 2026-09-07 metrics contract and current-hash replay

- The runner boundary now preserves Android metrics/log semantics generically:
  incremental host-log cursors feed the AOSP stderr stream and BRE sed groups
  are normalized before typed execution. All 18 focused tests and the runner
  contract self-test pass.
- Both metrics contracts pass in interpreter/JIT/unchanged-source modes;
  current-hash corpus replay is running from 2232 onward. Remaining work is a
  clean full-corpus replay plus real APK/Blue Archive validation.

### Progress — 2026-09-07 current-hash corpus and gate audit

- Current-hash corpus replay has reached `2269` with no new failures. Existing
  failures are stale-hash ledger entries pending a final resumable replay.
- A source audit confirms JIT compiler/inliner admission gates are removed;
  Apple Nterp remains intentionally disabled and falls back to switch
  interpreter. The next architectural step is a real ARM64 Nterp frame and
  catch-entry port with focused ABI/exception/GC tests.

### Progress — 2026-09-07 replay discoveries

- Replay reached `2286` and then the lexicographic 300-series. Two new
  current-hash failures (`2271-profile-inline-cache`, `304-method-tracing`) are
  under diagnosis; no test-specific workaround has been added.

### Progress — 2026-09-07 expanded replay

- Current-hash replay reached `453-not-byte`; the optimizing compiler, allocator,
  exception, monitor, and checker cluster passed unchanged.
- `2271-profile-inline-cache` and `304-method-tracing` remain under generic
  artifact diagnosis. No per-test workaround was introduced.

### Progress — 2026-09-07 generic runner path contracts

- The runner now honors AOSP `$DEX_LOCATION` expansion and per-mode staged JAR
  identity, preserving profile/method-trace file contracts without test-specific
  exceptions. Focused `304`, `2271`, and `082` runs pass all modes.
- Full resumable corpus replay restarted from the beginning under the corrected
  runner; real APK validation and Nterp remain open.

### Progress — 2026-09-07 early replay and StackWalk diagnosis

- Corrected full replay reached `052-verifier-fun` with the early runtime,
  verifier, exception, and reference tests passing. `004-StackWalk` is under
  separate native-library reproduction; no workaround has been added.

### Progress — 2026-09-07 full replay continuation

- Full resumable replay restarted from the corpus head with corrected path
  contracts. Tests through `004-ThreadStress` passed; `004-StackWalk` is now
  under generic ABI/stack-walk diagnosis, with no test-specific workaround.

### Progress — 2026-09-07 early corpus continuation

- Replay reached `065-mismatched-implements` with verifier, exception,
  finalizer, OOM, process-manager, and field-access tests passing. StackWalk
  remains isolated for native ABI diagnosis.

### Progress — 2026-09-07 StackWalk root cause

- Native StackWalk evidence shows the unwinder is healthy; failure is an app-OAT
  identity mismatch between dex2oat's outer temp JAR and the mode-sandbox JAR.
- Corpus execution is paused during the generic OAT staging correction to avoid
  mixed-hash results. No runtime gate or per-test exception was introduced.

### Progress — 2026-09-07 OAT staging correction

- Per-mode OAT/VDEX/app-image generation now uses the same staged JAR path that
  the runtime launches, preserving AOSP DexFile identity and profile/stack-walk
  behavior generically.
- `004-StackWalk` focused verification is in progress before corpus restart;
  no runtime or test-specific bypass was added.

### Progress — 2026-09-07 OAT staging verified

- Per-mode OAT/VDEX/app-image alignment is verified by `004-StackWalk`, `304`,
  and `2271` passing all execution lanes. No StackVisitor or ART ABI change was
  required.
- The full resumable corpus restarted from `000-nop` with the generic runner
  correction; Nterp and real APK validation remain open.

### Progress — 2026-09-07 verified replay restart

- Full replay now passes `004-StackWalk` and has advanced through
  `011-array-copy2` under the corrected per-mode OAT staging contract.
- Nterp implementation work is delegated for a real ARM64ng build slice and
  fail-closed Mach-O/CFI audits; no eligibility gate is being bypassed.

### Progress — 2026-09-07 OAT staging regression closed

- `004-StackWalk`, `1001-app-image-regions`, and `628-vdex` pass all execution
  lanes with mode-local OAT/VDEX/art/profile paths. This confirms a generic
  staging contract rather than a StackWalk-specific workaround.
- The full corpus is still running; the untracked harness file must be included
  when finalizing, while Nterp and real APK validation remain open.

### Progress — 2026-09-07 resumed corpus and Nterp implementation

- Corrected full replay is active through `018-stack-overflow`; early runtime,
  verifier, reference, and exception coverage has no new failures. Runner unit
  suites pass (18 tests).
- ARM64ng Nterp build support is being implemented as a real ABI-preserving
  slice; suppression gates remain untouched pending symbol/CFI validation.

### Progress — 2026-09-07 corpus continuation

- Replay reached `033-class-init-deadlock` without new failures, covering core
  runtime/verifier/exception/finalizer/class-init behavior after staging fix.
- Nterp build work remains isolated and gate-preserving pending assembly and
  CFI audits.

### Progress — 2026-09-07 post-staging replay

- Corrected replay reached `029-assert` with no new failures, including the
  repaired StackWalk/OAT path and early verifier/runtime tests.
- Nterp ARM64ng build work continues as an isolated ABI-preserving slice with
  fail-closed audits; real APK validation remains open.

### Progress — 2026-09-07 Nterp build slice in progress

- ARM64 runtime assembly integration now includes a staged Darwin Mach-O CFI
  lowering path and ARM64ng Nterp wiring. Build acceptance remains pending
  exact symbols, unwind/DWARF checks, and runtime ABI tests.

### Progress — 2026-09-07 Nterp integration review

- ARM64ng Mach-O assembly/CFI support is present, but Nterp suppression remains
  active in the manifest and no premature enablement was made.
- Wiring, symbol inventory, unwind/DWARF audits, and runtime matrix are pending;
  corpus execution stays paused to avoid mixed artifacts.

### Progress — 2026-09-07 ARM64 build audit

- The ARM64 runtime builder compiles and verifies 12 Mach-O objects with
  retained unwind/DWARF metadata. ARM64ng Nterp is not yet linked; suppression
  remains active pending real object wiring and execution tests.

### Progress — 2026-09-07 Nterp wiring status

- Existing ARM64 runtime objects pass build/unwind audits, but the archive still
  lacks a linked ARM64ng Nterp object. Suppression remains active.
- Corpus replay stays paused until Nterp wiring is complete and its execution
  matrix is verified.

### Progress — 2026-09-07 Nterp compile gate

- Bootstrap unit tests pass 4/4 and the existing 12 ARM64 Mach-O objects retain
  verified unwind/DWARF metadata.
- Nterp source generation is fail-closed at the five Apple CFI join errors;
  no suppression patch was removed and no Nterp runtime enablement is claimed.

### Progress — 2026-09-07 Nterp Mach-O wiring attempt

- The builder now reaches the real clang Mach-O assembly step. Remaining
  diagnostics are confined to CFI directives emitted outside an active FDE
  and external/local branch relocation lowering; Nterp remains unarchived and
  disabled pending a clean audit.

### Progress — 2026-09-07 Nterp helper-CFI diagnosis

- Relocation lowering is accepted by the assembler path; only helper CFI
  scope remains unresolved. Suppression stays active pending FDE-preserving
  assembly and runtime-matrix verification.

### Progress — 2026-09-07 Nterp relocation audit

- Mach-O relocation lowering now reaches clang and normalizes local targets.
  Remaining failure is helper CFI scope; suppression remains active and no
  archive linkage or runtime enablement is claimed.

### Progress — 2026-09-07 Nterp FDE-preserving CFI lowering

- The ARM64ng Nterp lowerer now gives the four non-linear return handlers and
  `nterp_helper` independent adjacent Mach-O FDEs. Helper CFI is no longer
  emitted outside an active FDE, and the invalid helper `.cfi_restore x22`
  expansion is suppressed with explicit frame seeds preserving the unwind
  contract.
- The real build gate passes:
  `cargo run -p art-bootstrap -- build-nterp-arm64ng` emits a Mach-O arm64
  object with all required Nterp symbols, `__eh_frame`, 23 unwind entries, and
  a clean `llvm-dwarfdump --verify`; `cargo test -p art-bootstrap` is 4/4.
  No Nterp suppression patch or runtime eligibility gate was removed.

### Progress — 2026-09-07 Nterp enabled regression lanes

- Linked Nterp passed clinit and verification-rethrow lanes, while the
  concurrent CHA-inlining run completed without a runtime failure. GC,
  concurrency, and real APK validation remain outstanding.

### Progress — 2026-09-07 Nterp GC/concurrency expansion

- Precise-GC, GC-thrash, exception, and thread lanes passed concurrently in
  interpreter/JIT/unmodified modes. Broader stress and real APK validation
  remain outstanding.

### Progress — 2026-09-07 ClassLoader mismatch found

- A real class-loader contract failure was isolated in
  `497-inlining-and-class-loader` interpreter mode. The staged DEX identity is
  rejected by the runtime; a generic AOSP-compatible fix is under diagnosis.

### Progress — 2026-09-07 Nterp compiler shard 464–476

- Clinit/GVN, vreg/regalloc, deopt environment, huge methods, inliner,
  dead-block, FP, constructor-barrier, and static-invoke inputs passed.

### Progress — 2026-09-07 Nterp compiler shard 477–492

- Bound-type/precision, clinit pruning, nested/recursive inlining, null-check,
  phi/DCE/dead-block, loop-edge, register-hint, and current-method inputs
  passed concurrently.

### Progress — 2026-09-07 Nterp compiler shard 300–411

- Dispatch, verification, tracing, Dex v37, optimizing compiler/regalloc,
  field/array, floating-point, and checker arithmetic inputs passed.

### Progress — 2026-09-07 Nterp compiler shard 412–431

- New-array/regalloc/static-field, type and invoke, exception/large-frame,
  monitor/bitwise/bounds, SSA, live-register, and propagation inputs passed
  concurrently.

### Progress — 2026-09-07 Nterp compiler shard 432–447

- Comparison/GVN, invoke-direct, exception/finally, allocation, float/shift,
  volatile, inline, and checker optimization inputs passed concurrently.

### Progress — 2026-09-07 Nterp compiler shard 448–463

- Multiple-return, BCE/type, spill/float, vreg/register, GVN, array-set,
  instruction simplification, long/FPU, dead-phi, dex inlining, and boolean
  checker inputs passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 159–178

- App-image, read-barrier, method-resolution, lock/interface, VM-stack, JNI,
  allocation, and class-init deadlock inputs passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 123–138

- Secondary-dex, class-loading, GC coverage, register allocation, daemon/JNI
  shutdown, hprof, CFI, and duplicate-class inputs passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 179–1914

- Native/default method dispatch, linking, RMW, suspend/native-suspend,
  JVMTI locals, allocation tracking, and TLS inputs passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 139–158

- Native registration, class lifecycle, allocation/reference stress, GC and
  suspend-all, loader, loadlibrary, and app-image class-table inputs passed.

### Progress — 2026-09-07 Nterp corpus shard 085–1004

- Loop/override/monitor, GC-link, reflection, serialization, VMDebug,
  app-image, and volatile-reference inputs passed concurrently on Nterp.

### Progress — 2026-09-07 Nterp corpus shard 102–121

- Concurrent GC, suspend, multidex, native bridge, dex2oat, exception, invoke,
  field, and arithmetic inputs passed on the linked Nterp runtime.

### Progress — 2026-09-07 Nterp corpus shard 071–084

- DexFile, GC/fence, verifier, polymorphic dispatch, reference/OOM, exception,
  inline, compiler-regression, and class-init tests passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 050–070

- Synchronization, verifier, exception, OOM, classloader, field, and NIO
  inputs passed concurrently on the linked Nterp runtime. Full corpus and APK
  validation remain outstanding.

### Progress — 2026-09-07 Nterp memory/deopt expansion

- Non-moving-space stress and deoptimizeable lanes passed interpreter/JIT and
  unmodified checks; app-image-regions completed its runtime build path. Full
  GC stress and real APK validation remain outstanding.

### Progress — 2026-09-07 Nterp archive wiring

- The archive path now requires and links a successful ARM64ng Nterp object;
  failed CFI/relocation audits stop publication of a partial runtime.

### Progress — 2026-09-07 Nterp linked and exercised

- The generated ARM64ng interpreter is now a verified Mach-O archive member;
  `821-many-args`, `825-unbalanced-lock`, and `830-goto-zero` pass both
  interpreter and JIT lanes. Full runtime and real-APK validation remain.

### Progress — 2026-09-07 Nterp corpus shard 000–010

- The first 20 inputs on a fresh corpus ledger passed concurrently, covering
  core opcodes, JNI, stack/reference maps, signals, and thread stress. Full
  corpus and APK validation remain outstanding.

### Progress — 2026-09-07 Nterp deopt/monitor/JNI lanes

- Deoptimization, monitor-inflation, and JIT-JNI lanes passed in all three
  execution modes. Broader compatibility and APK validation remain open.

### Progress — 2026-09-07 Nterp corpus shard 201–2033

- Thread OOME/checkpoints, inlining/loop optimizations, memory couples,
  instrumentation, contended monitors, child/shutdown mechanics, JNI
  structural failures, and zygote deopt inputs passed concurrently.

### Progress — 2026-09-07 Nterp corpus shard 011–029

- The next 20 discovered inputs passed on a fresh parallel ledger, covering
  arrays, arithmetic, strings, interfaces, access checks, and stack depth.

### Progress — 2026-09-07 Nterp corpus shard 030–049

- Finalizer/class-init, inheritance, reflection, proxy, and return-path tests
  passed concurrently on the Nterp runtime. Full corpus and APK validation
  remain outstanding.

### Progress — 2026-09-07 Per-loader DexCache patch / follow-up fault

- Added the generic `(DexFile, ClassLoader)` DexCache lookup and registration
  path in patch `0154-darwin-per-loader-dex-cache.patch`; runtime/JIT/dex2oat
  compilation and patch dry-run pass against pinned AOSP 16.
- Nterp Mach-O symbol decoration and unresolved-symbol audits now pass. The
  497 class-loader test advances beyond the previous mismatch, but its
  interpreter lane currently faults in generated code at `0x70000884`; the
  generated-code fault remains open for the next iteration.

### Progress — 2026-09-07 Nterp compressed-reference fault localized

- The new post-ClassLoader fault is in Nterp object.S static-field handling:
  `ArtField::declaring_class_` is still a compressed reference when used by
  the sput resume path. Darwin requires decoding with the compressed-reference
  base before the access/card mark; fast and slow read-barrier points are being
  patched and will be revalidated through the 497 lanes.

### Progress — 2026-09-07 Nterp card-table base correction

- AOSP CardTable uses a biased begin derived from the native heap base, so the
  Nterp write barrier must decode the compressed reference before shifting.
  The Darwin lowering now preserves/restores the high compressed-reference
  base for card-index calculation, matching compiler patch 0053. Verification
  is continuing through the 497 lanes.

### Progress — 2026-09-07 Nterp invoke receiver decode

- The card-table correction is validated; 497 now reaches virtual invocation.
  The remaining fault is an invoke.S receiver dereference of a compressed
  reference. The planned lowering decodes only dispatch receiver/class
  temporaries and preserves compressed managed argument registers, maintaining
  the AOSP calling convention.

### Progress — 2026-09-07 Nterp fill-array-data boundary

- Virtual/interface receiver decoding is validated and 497 advances further.
  `op_fill_array_data` still passes a compressed array reference into the
  native `FillArrayData` helper; the next lowering decodes non-null references
  at that boundary while preserving the null representation, then audits all
  adjacent array helper calls.

### Progress — 2026-09-07 Nterp array reference inventory

- Array helper auditing identified and lowered all direct vreg object
  dereferences and the fill-array-data native boundary. The existing
  `aput-object` Darwin-specific lowering is preserved with a decoded bounds
  temporary. Six focused Nterp tests and the ARM64/DWARF build pass; 497 is
  being rerun after relink.

### Progress — 2026-09-07 Stack-walk loader selection

- With array references corrected, 497 reaches the expected inlined stack
  walk. A terminal CHECK remains because `MethodInfo` with an unknown dex
  index selects the canonical earliest DexCache instead of the active
  ClassLoader's cache. The next fix scopes inline resolution to the outer
  method/declaring-class loader.

### Progress — 2026-09-07 Nterp card-table barrier fault

- After decoding the declaring-class reference at all four resume points, the
  static store succeeds. The remaining generated-code fault is the write
  barrier's card-table index using the decoded pointer rather than Darwin's
  logical compressed reference representation. The fix is being kept inside
  the Nterp/host lowering boundary and will be validated across all lanes.

### Progress — 2026-09-07 Loader-scoped inline method resolution

- `GetResolvedMethod` and OatDexFile resolution now select the active
  ClassLoader's DexCache, including same-dex inline sentinels, while the boot
  class path explicitly remains loader-null. The permanent patch applies
  cleanly to pinned AOSP; rebuild and 497 lane verification remain.

### Progress — 2026-09-07 ClassLoader/Nterp 497 closure

- ClassLinker registration is now loader-scoped for non-boot DEX files, so one
  native DexFile can legally back distinct application DexCaches/ClassTables.
  The boot-versus-user collision guard is intentionally retained because
  hidden-API domain metadata and compiled BSS roots still belong to the native
  DexFile rather than a loader-specific cache.
- Inline metadata resolution carries the immediate caller's loader/DexCache;
  it does not fall through to the deterministic loader-agnostic compatibility
  lookup. Boot-class-path metadata remains explicitly loader-null.
- The staged ARM64 Nterp templates now lower Darwin compressed references at
  object static-field, write-barrier, invoke-dispatch, and array/native-helper
  boundaries without changing the managed argument ABI. Nterp unit tests pass
  6/6, runtime build/link auditing passes, and AOSP test 497 passes interpreter,
  JIT, and unmodified source interpreter+optimized lanes.

### Progress — 2026-09-07 Nterp corpus shard 493–510

- A fresh four-worker ledger at
  `_build/art-upstream-corpus-nterp-493-rerun` ran the next 20 deterministic
  inputs beginning with `493-checker-inline-invoke-interface` and ending with
  `510-checker-try-catch`. All 20 passed (exit code 0), including the
  ClassLoader/inlining, checker, type-propagation, array, baseline, and
  referrer variants. No new runtime failure or generic fix was identified.

### Progress — 2026-09-07 Nterp corpus shard 511–527

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-511` ran the
  next 20 deterministic discovered inputs beginning with `511-clinit-interface`
  and ending with `527-checker-array-access-split`. All 20 passed (exit code 0),
  covering clinit, array deoptimization, shifts, DCE, checker fallthrough,
  null-array access, class loading, phi equivalence, monitor/throw regressions,
  array fields/access, and long/register allocation. No new runtime failure or
  generic fix was identified.

### Progress — 2026-09-07 Nterp corpus shard 528–531

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-528` ran the
  next 20 deterministic discovered inputs beginning with `528-long-hint` and
  ending with `531-regression-debugphi`. All 20 passed (exit code 0), covering
  long hints/splits, unresolved and loop checkers, instance-of/checkcast,
  load/store elimination, peeling/unrolling, reference typing, instanceof,
  LSE regression, and debug-phi handling. No new runtime failure or generic
  fix was identified.

### Progress — 2026-09-07 Nterp reference-boundary inventory

- Added shared nullable/non-null compressed-reference decode macros and
  fail-closed expected-boundary audits across object, array, invoke,
  control-flow, and monitor templates. Ten focused Nterp tests pass and the
  generated 256-handler Mach-O/DWARF artifact is clean; runtime smoke remains.

### Progress — 2026-09-07 Full Nterp managed-reference boundary audit

- Object type/hierarchy checks, field holders, nullable throw, and monitor
  native-object boundaries now use the shared Darwin decode macros; managed
  argument/return and heap-store compressed ABI paths are preserved. The
  fail-closed inventory and assembled 99 heap-base expansions are verified;
  `cargo test -p art-bootstrap` 13/13, ARM64/link/DWARF audits, 497, and
  003-omnibus-opcodes all pass in their respective lanes.

### Progress — 2026-09-07 542 contract audit correction

- `542-unresolved-access-check` reproduces under unchanged AOSP DEX in
  Nterp, `-Xint`, JIT/AOT, isolated JIT, and soft-fail modes. `interp-ac` is
  only the interpreter/soft-verification flags, and reflection confirms the
  parent and custom loader own distinct `PlaceHolder` classes; the deliberate
  null `loadClass` therefore persists. No generic Darwin defect or test
  bypass is justified by the evidence.

### Progress — 2026-09-07 Nterp corpus shard 532–545 (pending access-check contract)

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-532` ran the
  next 20 deterministic discovered inputs from `532-checker-nonnull-arrayset`
  through `545-tracing-and-jit`; 19 passed. `542-unresolved-access-check`
  fails in both interpreter and JIT lanes because its unchanged AOSP custom
  ClassLoader intentionally returns null during verification, producing
  `ClassLoader.loadClass returned null for p1.OtherInP1`. JIT-only isolation
  reproduces the result, identifying the missing `interp-ac`/soft-
  verification launch contract rather than a new runtime lowering defect; no
  generic runtime change or test-specific bypass was added.

### Correction — 2026-09-07 542 loader contract diagnosis

- The prior missing-`interp-ac`/soft-verification explanation is withdrawn.
  Test 542 has no custom `run.py`; pinned ART's `interp-ac` selects switch
  interpretation and soft verification, not a special classpath or shared
  ClassLoader state. Its verifier already clears failed class loads into
  unresolved reference types, and its execution-time null-loader NPE is
  unchanged upstream behavior.
- An unchanged-Dex, no-app-OAT diagnostic proves that parent-loaded
  `MyClassLoader` reads the parent `PlaceHolder.entered=false`, while
  child-loaded `InP1` has written its separate `PlaceHolder.entered=true`.
  The class objects are unequal (PathClassLoader versus MyClassLoader).
  Failure occurs on final-iteration `new-instance` after the flag store,
  not during verification. Pinned AOSP already disables the test globally
  for its cross-loader native-Dex registration; Darwin's per-loader cache
  support exposes this further state-sharing assumption.
- Fresh unchanged-source Nterp, switch, JIT/AOT, isolated JIT, softfail Nterp,
  softfail JIT, and combined `interp-ac` runtime-option runs all reproduce
  the NPE. Isolated JIT confirms
  optimized Main installation. `_build/542-access-check-diagnosis/` contains
  all lane outputs plus the separate loader-identity diagnostic. No runtime
  workaround, source/expected-output change, or skip/knownfailure entry was
  added; 542 remains a corpus FAIL, not a claimed compatibility pass.
- A separate AOT-backed diagnostic showed a differing child static-field
  value and merits follow-up of compiled-cache/field-state isolation. The
  no-AOT result independently establishes the test's mismatched flag state;
  this diagnosis does not claim to close that ancillary AOT observation.

### Progress — 2026-09-07 Nterp corpus shard 546–557

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-546` ran the
  next 20 deterministic discovered inputs from `546-regression-simplify-catch`
  through `557-checker-instruct-simplifier-ror`; all 20 passed (exit code 0).
  The shard covers catch/try-catch regressions, checker inlining and type
  propagation, clinit and invoke-super variants, implicit null checks,
  sharpening, AVX2 bit manipulation, checkcast, UnsafeGetLong, and rotate-right
  simplification. No new runtime failure or generic fix was identified.

### Progress — 2026-09-07 Nterp corpus shard 558–566

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-558` ran the
  next 20 deterministic discovered inputs from `558-switch` through
  `566-polymorphic-inlining`; all 20 passed (exit code 0). The shard covers
  packed/sparse switches, BCE/SSA, loop and shared-slowpath handling, div/rem,
  checker no-intermediate/fakestring/invoke-super/bitcount/irreducible/
  negbitwise/codegen-select cases, and polymorphic inlining. No new runtime
  failure or generic fix was identified.

### Progress — 2026-09-07 Nterp corpus shard 567–580

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-567` ran the
  next 20 deterministic discovered inputs from
  `567-checker-builder-intrinsics` through `580-crc32`; all 20 passed (exit
  code 0). The runner hash is
  `8601b0f3a5247ea8336e633b3824bb3c972c6521152c9c99c8e75c8d9d2c29b9`.
- The initial run's only two failures, `580-checker-fp16` and
  `580-checker-string-fact-intrinsics`, were javac pre-runtime failures: the
  generic compiler staging lacked signatures for pinned AOSP
  `libcore.util.FP16` and `StringFactory`, despite the sources being present
  in `_aosp/libcore-full` and the runtime boot DEX containing the classes.
  The generic compiler-only companion described below now stages the shared
  signatures; no test-source capability detection is used. Focused and
  complete-shard reruns pass; no test source, expected output, or runtime
  behavior changed.

### Progress — 2026-09-07 libcore compiler classpath staging

- Inspection of the pinned Android 16 inputs shows that both missing symbols
  are in `core-libart.jar`'s DEX (`Llibcore/util/FP16;` and
  `Ljava/lang/StringFactory;`), with source ownership in the corresponding
  `_aosp/libcore-full/luni` and `_aosp/libcore-full/libart` trees. They were
  absent from javac because the boot JARs are DEX containers, not classfile
  libraries; `core-oj-compat.jar` cannot provide symbols owned by core-libart.
- The generic framework/libcore build now materializes the shared compiler
  capability set as `_build/android16-libcore-compiler-api/core-libart-compiler.jar`.
  The upstream runner adds this companion to javac's boot/class path for all
  tests. Runtime boot images and app execution retain the pinned
  `_prebuilt/android-16/bootclasspath/core-libart.jar` DEX, so compiler-only
  declarations cannot alter runtime fidelity or app DEX contents.
- Both focused 580 tests pass in interpreter, JIT, and unmodified-source
  checker lanes; the existing upstream corpus and javac/build contract unit
  tests remain green.

### Progress — 2026-09-07 Nterp corpus shard 581–594

- A fresh four-worker ledger at `_build/art-upstream-corpus-nterp-581` ran the
  next 20 deterministic discovered inputs from `581-checker-rtp` through
  `594-invoke-super`; all 20 passed (exit code 0). The runner hash is
  `c5816ef02710725f8c1e73f1ca841664b029598b5ec1969c1da0d45aa6f6429`.
- No new runtime or compiler failure was identified, so no generic fix or
  rerun was needed.

### Progress — 2026-09-07 Nterp corpus shard 595–608

- `_build/art-upstream-corpus-nterp-595` completed the next 20 discovered
  AOSP inputs (`595-error-class` … `608-checker-unresolved-lse`) with 20/20
  passing on the Nterp runtime using four workers.
- The first `595-profile-saving` run compiled and completed dex2oat, then
  failed at runtime because the AOSP shell-assembled option
  `-Xcompiler-option --compiler-filter=verify` had been passed as one host
  argv element. The generic runner now applies the same typed shell-word
  boundary to every `runtime_option` entry (with a focused unit test), after
  resolving `$DEX_LOCATION`; no test-name allowlist or source mutation was
  added. Focused and complete-shard reruns pass interpreter, JIT, and
  unmodified-source optimized lanes.
- Final ledger results are 20/20 passed with runner hash
  `8711a87dbf1979af6b6170789c07740a9496061e93527d545c1e457cc745c5a8`.

### Progress — 2026-09-07 Nterp corpus shard 609–619

- `_build/art-upstream-corpus-nterp-609` completed the next 20 discovered
  AOSP inputs (`609-checker-inline-interface` … `619-checker-current-method`)
  with 20/20 passing on the Nterp runtime using four workers. No new runtime
  failure was identified, so no generic fix was needed.

### Progress — 2026-09-07 Nterp corpus shard 620–636

- `_build/art-upstream-corpus-nterp-620` completed the next 20 discovered
  AOSP inputs (`620-checker-bce-intrinsics` … `636-wrong-static-access`) with
  20/20 passing on the Nterp runtime using four workers. The runner hash is
  `2d168dd76b52cf79ef7c4bb504a263d92a4f07d6f8e8559725b80c58205ea08a`.
- The only initial failure, `629-vdex-speed`, was a generic per-mode staging
  mismatch: VDEX/ODEX artifacts compiled against the outer JAR were not valid
  for the mode-private JAR path, so ART interpreted `Main.main`. The runner now
  rebuilds VDEX/ODEX and app images against each mode's exact DEX location;
  focused and complete-shard reruns pass. No test source, expected output,
  allowlist, or runtime behavior was changed.
### Progress — 2026-09-08 Nterp corpus shard 620 (in progress)

- The 20-input shard has 19 passes. `629-vdex-speed` is being isolated as an
  AOT/VDEX artifact-loading issue while preserving the original AOSP test.

### Correction — 2026-09-08 Nterp corpus shard 620

- The preceding in-progress note is superseded: the generic per-mode VDEX
  staging fix was verified and the complete shard passed 20/20.

### Progress — 2026-09-08 continuation

- The next AOSP corpus shard and a focused JIT architecture-gap audit are in
  progress. Remaining eligibility, OSR, and deoptimization coverage is not yet
  complete.

### Progress — 2026-09-08 Nterp corpus shard 637 (in progress)

- The shard has 19 passing inputs. `641-iterations` currently aborts during
  class/method resolution (`Main.init()`), so its generic DEX/class-linker
  path is being isolated before any compatibility claim is made.

### Correction — 2026-09-08 Nterp corpus shard 637

- The preceding in-progress record is superseded. The corrected
  `_build/art-upstream-corpus-nterp-637-rerun` ledger passed all 20 inputs,
  including `641-iterations`.

### Correction — 2026-09-08 Nterp corpus shard 637

- The preceding in-progress note is superseded. A fresh four-worker ledger at
  `_build/art-upstream-corpus-nterp-637-rerun` ran the next 20 deterministic
  inputs from `637-checker-throw-inline` through `648-inline-caches-unresolved`;
  all 20 passed (exit code 0), with runner hash
  `2d168dd76b52cf79ef7c4bb504a263d92a4f07d6f8e8559725b80c58205ea08a`.
- `641-iterations` exposed a generic shared-native-registration bug: its
  ordinary managed `Main.init()V` collided with a name in the harness's
  libarttest native table. Registration now reifies the exact JNI method ID and
  checks ART's `ArtMethod::IsNative()` before calling `RegisterNatives`, so
  managed collisions are skipped while real native declarations remain
  registered. Focused and complete-shard reruns pass interpreter, JIT, and
  unmodified-source optimized lanes. No test source, expected output, APK,
  allowlist, or runtime bypass was changed.
### Progress — 2026-09-08 continuation

- Local `art-bootstrap` validation remains green (13/13) after the generic
  native-registration fix. The next discovered AOSP corpus range is running;
  remaining breadth and production-app evidence is still outstanding.

### Progress — 2026-09-08 continuation

- The `649-vdex-duplicate-method` shard is active with four workers and all
  completed inputs passing. A Sol-high audit is independently verifying
  unmodified-test JIT execution and remaining architecture gaps.

### Progress — 2026-09-08 continuation

- The active `649` ledger has reached 17 completed inputs with no failures;
  remaining inputs and JIT execution audit continue.

### Progress — 2026-09-08 continuation

- The active `649` ledger now has 19 passing inputs with one final case left;
  the independent JIT execution audit remains active.

### Progress — 2026-09-08 Nterp corpus shard 649

- `655-jit-clinit` reaches the expected interpreter result, but its JIT lane
  times out waiting for `Foo.$noinline$hotMethod` to become JIT compiled. The
  unresolved state is a genuine JIT pipeline issue under focused diagnosis.

### Progress — 2026-09-08 focused JIT diagnosis

- AOSP's global JIT lane contract includes `-Xjitthreshold:0`; the current
  host log remains at warmup threshold `65535`. Generic runner parity and the
  focused rerun are underway.

### Progress — 2026-09-08 focused JIT diagnosis

- AOSP parity also requires `--compiler-filter=verify` for implicit
  differential prebuilds. This generic runner correction is applied and its
  12/12 unit contract passes; `655-jit-clinit` is being rerun with both parity
  fixes.

### Progress — 2026-09-08 focused JIT diagnosis

- The execution loop was rebuilding a synthetic default action with `speed`
  after the `verify` prebuild. It now uses the precomputed invocation plan so
  the AOSP-compatible mode policy is preserved; focused rerun is active.

### Progress — 2026-09-08 corpus continuation

- Normal JIT policy is now verified independently of the focused threshold-zero
  mode: the AOSP `655-jit-clinit` test passes with default thresholds and a
  worker compiler thread, while the broader 649 shard is blocked only by the
  separate `658-fp-read-barrier` generated-code crash under investigation.

### Progress — 2026-09-08 read-barrier boundary fix

- The generated ARM64ng `sget-object` marking branch now applies the shared
  Darwin compressed-reference decode before its native field load. This keeps
  the Android compressed-reference contract intact and isolates only the
  address conversion at the host boundary; Nterp unit and Mach-O audits pass.

### Progress — 2026-09-08 read-barrier verification

- The unmodified `658-fp-read-barrier` interpreter and JIT lanes both pass with
  the generated `sget-object` boundary correction. The full 649 corpus shard
  is being rerun against the rebuilt runtime before advancing to the next shard.

### Progress — 2026-09-08 shard 649 complete

- The rebuilt ARM64ng runtime passes all 20 tests in the corrected 649 shard
  ledger, including the read-barrier stress case in both interpreter and JIT
  modes. Work now advances to the next unmodified AOSP shard without adding a
  compatibility fallback.

### Progress — 2026-09-08 focused JIT diagnosis

- The corrected focused run compiled `Foo.$noinline$hotMethod` and completed
  with `Main main(String[]) PASS`. A normal-policy run without threshold override
  is queued for final JIT evidence.

### Progress — 2026-09-08 shard 661 initial result

- The next shard's first run is 15/20. Failures are isolated to malformed-DEX
  staging, the `aget` verifier behavior, and a runner environment-snapshot type
  contract; these are being fixed at shared boundaries before rerun.

### Completion — 2026-09-08 Nterp corpus shard 649

- The rebuilt four-worker ledger `_build/art-upstream-corpus-nterp-649-final2`
  ran the 20 deterministic discovered inputs from `648-many-direct-methods`
  through `661-classloader-allocator`; all 20 passed (interpreter, JIT, and
  unmodified-source optimized lanes), with no remaining failures.
- The ledger is uniform at runner hash
  `57483a2ead8aa5022d7be6b82fde6f6570e222d402130251d9c8cf26182834ca`.
- The only runtime failure encountered, `658-fp-read-barrier`, was fixed
  generically by decoding the compressed declaring-class reference after the
  `sget-object` read-barrier mark path and before its native field load. No
  corpus input, expected output, APK, allowlist, or test-specific workaround
  changed. The next deterministic shard begins after `661-classloader-allocator`.

### Completion — 2026-09-08 Nterp corpus shard 661

- `_build/art-upstream-corpus-nterp-661/summary.json` records 20/20 passing
  deterministic inputs (`661-oat-writer-layout` through `674-HelloWorld-Dm`)
  across interpreter and optimized lanes using four workers.
- All records use runner hash
  `6a9db37a07b4afb9498a4c2ac91e03c69d5c2c5094a7afa23a0994ac2b0bf249`.
- Shared runner fixes preserve original malformed prebuilt multidex payloads
  while adding only the generic harness support entry, infer required AOT
  verification from the unchanged native contract, and stringify process
  environment assignments. No AOSP input or test-specific compatibility path
  was added; detailed artifacts remain in the ledger's per-test result paths.

### Progress — 2026-09-08 shard 674 started

- Shard `674-hiddenapi` through `688-shared-library` is running in four-worker
  parallel mode against the rebuilt runtime. Failures will be addressed only at
  shared runtime or runner boundaries.

### Progress — 2026-09-08 shard 674 initial result

- Initial result is 16/20. The remaining failures are compiler companion
  symbols for hidden class-loader APIs and boot-oat trust handling in the FSI
  runtime path; fixes are being kept at shared boundaries.

### Progress — 2026-09-08 shard 674 shared fixes verified

- Global export audit now passes for the `DexFileLoader` symbols, and focused
  FSI variants pass with logical `/system/framework/boot.art` oat identity plus
  stderr logger aggregation.
- The shard is 19/20 after rebuild. Only unmodified `674-hiddenapi` remains,
  failing its AOSP `opened_dex_files` assertion after native loading; the next
  fix targets Darwin DSO dependency/symbol ownership at the loader boundary.

### Progress — 2026-09-08 674 hidden-api ownership fix

- Focused `674-hiddenapi` now passes interpreter, JIT, and unmodified-source
  optimized execution after rebuilding the runtime. JNI bridge registration
  is now excluded per method when the unchanged test DSO exports that
  `Java_Main_*` owner, preventing split native state while preserving Android
  linker ownership semantics.

### Completion — 2026-09-08 Nterp corpus shard 674

- The resumed four-worker ledger records 20/20 passing deterministic inputs
  (`674-hiddenapi` through `688-shared-library`) in interpreter and optimized
  lanes. Runtime relink/audit and Python/Cargo validation passed.
- JNI bridge ownership is now derived from native DSO source exports, matching
  Android's declaring-library semantics and eliminating the split-state failure.
-
### Verified completion — 2026-09-08 shard 674

- The four-worker ledger `_build/art-upstream-corpus-nterp-674/summary.json`
  completed all 20 deterministic inputs from `674-hiddenapi` through
  `688-shared-library`: 20 passed, 0 failed. Runner hashes recorded by the
  ledger are `57c6c62647271891454e4ee09e2903305c4d2e56bff4abf13fffc0b76c117ae5`
  and `d8b97556810cbef8b09ba13c576685c0a01df9d0a3362f8fbe3ae1887d9aac35`.
- The runtime/runner boundary preserves AOSP source ownership: compiler
  companion APIs, `DexFileLoader` exports, logical boot-oat naming, and
  source-derived JNI registration are generic. No AOSP input, expected
  output, allowlist, or fallback was changed. Final per-test evidence is
  recorded by the ledger; focused 674 evidence is at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-674-hiddenapi-swwau7nb`.

### Progress — 2026-09-08 shard 689 started

- Shard `689-multi-catch` through `706-checker-scheduler` is running in
  four-worker parallel mode against the rebuilt runtime.

### Progress — 2026-09-08 shard 707 initial result

- The 20-input range `707-checker-invalid-profile` through
  `728-imt-conflict-zygote` initially reached 17/20. A generic sibling native
  source/include discovery correction makes focused `717-integer-value-of`
  pass; three remaining failures are being reduced to shared runner/runtime
  boundaries.

### Progress — 2026-09-08 shard 689 initial result

- Initial execution is 18/20. The two VDEX loader tests fail only because
  their native JNI implementation is an unchanged sibling AOSP source, not a
  source under the test directory. The runner will be corrected to discover
  owners from Java declarations and exported `Java_Main_*` symbols generically.

### Completion — 2026-09-08 Nterp corpus shard 689

- The resumed four-worker ledger records 20/20 passing inputs
  (`689-multi-catch` through `706-checker-scheduler`) in interpreter and
  optimized lanes. The runner now discovers sibling JNI owners from unchanged
  AOSP declarations and keeps secondary VDEX staging faithful to each
  invocation's `secondary_compilation` contract.

### Progress — 2026-09-08 JIT admission and OpenJDK header audit

- A fresh source audit found no production Darwin-only JIT admission gate,
  opcode allowlist, or compiler/inliner rejection. Active JIT and Nterp paths
  use the pinned AOSP conditions; the remaining eligibility helper is
  diagnostic-only. Historical OSR-rejection wording in this document is stale
  and is superseded by the current runtime-common source audit.
- The OpenJDK/JVM host build now consumes the canonical runtime-common shadow,
  preventing stale runtime-bootstrap headers from changing exported ABI.

### Verified completion — 2026-09-08 shard 689

- Four-worker deterministic execution in
  `_build/art-upstream-corpus-nterp-689/summary.json` completed the 20-input
  range `689-multi-catch` through `706-checker-scheduler` with 20 passed and
  0 failed (all exit code 0); the ledger runner hash is
  `235530cac3521c3f5cdafa18a8bd210c6474d2859b79e298e0531a7ec3f4340c`.
- The shared runner's source-derived JNI owner discovery fixes the two VDEX
  tests without test-name gates. Per-action `secondary_compilation=False`
  now removes secondary oat/vdex/art artifacts from the mode staging and
  runtime-file capability before each launch, matching AOSP's default_run
  contract. No AOSP input, expected output, allowlist, or fallback changed.

### Progress — 2026-09-08 shard 707 current reduction

- `717-integer-value-of` passes after generic source-parent include handling.
  The ledger is currently 17/20; `712-varhandle-invocations` still has a JIT
  sandbox boot-classpath path-resolution failure and `725-imt-conflict-object`
  faults in generated IMT conflict code.

### Progress — 2026-09-08 focused 712 recheck

- Ordinary detached launches now pass absolute boot-classpath paths into ART;
  focused `712-varhandle-invocations` reaches the runtime and its interpreter
  lane passes. The remaining failure is optimizing compilation of `Main.main`
  for VarHandle invocation bytecode, while `725` is a separate IMT generated
  code fault.

### Verified focused fix — 2026-09-08

- `725-imt-conflict-object` passes interpreter, JIT expected-output, and
  unmodified-source interpreter+optimized lanes in the latest focused run.

### Verified completion — 2026-09-08 shard 729

- Four-worker execution of the contiguous range `729-checker-polymorphic-intrinsic`
  through `809-checker-invoke-super-bss` completed 20/20 with exit code 0 in
  `_build/art-upstream-corpus/summary.json`. It exercises deoptimization,
  interface/field dispatch, app images, smali, and invoke-super behavior
  without changing AOSP inputs or adding gates.

### Progress — 2026-09-08 shard 810

- The range through `829-unresolved-enclosing` reached 17 passes out of 20.
  Only `817-hiddenapi`, `822-hiddenapi-future`, and `823-cha-inlining` remain;
  the other verifier, deoptimization, lock, loop, and invoke-super inputs pass.

### Verified completion — 2026-09-08 shard 707

- `_build/art-upstream-corpus-nterp-707/summary.json` records 20 passed and
  0 failed for the deterministic range `707-checker-invalid-profile` through
  `728-imt-conflict-zygote`, executed with four workers; all results have exit
  code 0. Final runner hash:
  `499ce23a4812133eb4f4b4e88b6c7c00bd342cf69f73cd44c430d6187f2cf358`.
- The rebuilt runtime evidence is focused 712 artifact
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-712-varhandle-invocations-ndll4kyw`
  and focused 725 artifact
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-725-imt-conflict-object-zwpcswg2`;
  each passed interpreter and optimized output comparison and the unchanged
  source contract.
- Runtime/runner changes remain generic and AOSP-compatible: physical versus
  logical boot-classpath paths are separated, an optimized application
  constructor can provide the JIT installation proof when a generated
  dispatcher is too large, and Nterp's decoded class pointer remains 64-bit
  through IMT vtable indexing. No AOSP input, expected output, allowlist, or
  fallback changed.

### Verified focused fix — 2026-09-08

- `822-hiddenapi-future` passes all interpreter/JIT and unchanged-source
  optimized lanes after the generic hidden-API encoder gained the AOSP future
  target flag.

- `817-hiddenapi` now passes all three lanes after generic native owner
  resolution linked its AOSP hidden-API helper.

### Diagnostic progress — 2026-09-08 `823-cha-inlining`

- Sampling the live hanging process shows the main thread repeatedly walking
  exception stacks through `Throwable_nativeFillInStackTrace`,
  `StackVisitor::WalkStack`, and `OatQuickMethodHeader::IsStub`. The remaining
  issue is therefore frame advancement/header detection in the exception path.

### Verified completion — 2026-09-08 shard 810

- The resumed four-worker deterministic 20-test range
  `810-checker-invoke-super-default` through `828-partial-lse` completed 20/20
  with exit code 0 in `_build/art-upstream-corpus/summary.json`.
  `823-cha-inlining` now passes all lanes after the generic stack-walker fix;
  no AOSP input or compatibility gate changed.

### Verified completion — 2026-09-08 shard 830

- A fresh two-worker run of `830-goto-zero` through `847-filled-new-aray`
  completed 21/21. Rebuilt compiler stub APIs fixed the stale 845/846 ledger
  results without changing runtime policy or AOSP inputs.

### Verified completion — 2026-09-08 shard 848

- Four-worker execution of `848-pattern-match` through
  `907-get-loaded-classes` completed 20/20 after generic D8 JVM-flag and
  JVMTI plugin lifecycle fixes. No test-specific allowlist or fallback was
  added.

### Verified completion — 2026-09-08 shard 908

- Four-worker execution of `908-gc-start-finish` through `927-timers`
  completed 20/20, covering GC and JVMTI lifecycle/object/thread paths with
  all three execution lanes passing.

### In progress — 2026-09-08 shard 928 follow-up

- Focused reruns now pass `936-search-onload` and `938-load-transform-bcp`
  after generic JVMTI search/classpath fixes; the complete 928–947 shard is
  being revalidated in `_build/art-upstream-corpus-shard928-v2`.

### Verified focused fixes — 2026-09-08 `849-records`, `900-hello-plugin`

- The runner preserves AOSP d8's `-J<arg>` JVM boundary for direct R8
  invocation; `849-records` passes interpreter/JIT/unchanged-source optimized
  lanes.
- Android JVMTI plugin aliases resolve to the staged Darwin test DSO, while
  the generic dispatcher is omitted when the fixture owns `Agent_OnLoad`.
  `900-hello-plugin` passes all three lanes with expected lifecycle output.

### Verified focused fixes — 2026-09-08 `936-search-onload`, `938-load-transform-bcp`

- Both tests pass interpreter, JIT expected-output, and unchanged-source
  optimized lanes in repeat focused runs. Detached sandbox launches now use
  absolute physical boot-classpath locations for libcore URL resolution;
  `936-search-onload` is stable after the runner/runtime state was rebuilt.
  No AOSP input, test-specific gate, allowlist, or interpreter fallback was
  changed.

### Verified completion — 2026-09-08 shard 928

- Fresh four-worker rerun of `928-jni-table` through `947-reflect-method`
  completed 20/20 across interpreter, JIT expected-output, and unchanged-source
  optimized lanes.

### Verified completion — 2026-09-08 shard 948

- Four-worker execution of `948-change-annotations` through `967-default-ame`
  completed 20/20 across all three execution lanes.

### In progress — 2026-09-08 shard 988

- The 988–999 executable tests passed; seven following entries are AOSP
  helper fixtures without a runnable test contract. Structural corpus-layout
  handling is being added so they are not misreported as runtime failures.

### In progress — 2026-09-08 shard 968 follow-up

- Initial 968–987 execution passed 14/20. Generated-interface DEX materializing
  and native method binding remain under generic runner/runtime diagnosis.

### Verified focused fixes — 2026-09-08 shard 968

- The generic runner now materializes Smali-only tests as primary DEX when no
  API-level support DEX exists; 968, 970, 971, 975, and 978 pass all three
  execution lanes. Null-ClassLoader bootstrap/agent native loads use the
  process-wide symbol scope required by JVMTI dlsym(RTLD_DEFAULT), while
  application ClassLoader loads remain local; 986-native-method-bind passes
  all three lanes. No AOSP input, allowlist, or fallback was changed.

### Verified completion — 2026-09-08 shard 988

- Corpus discovery structurally filters non-runnable AOSP helper fixtures by
  the absence of `expected-stdout.txt`; the executable 988–999 range passes
  13/13 across interpreter, JIT, and unchanged-source optimized lanes.

### Verified completion — 2026-09-08 shard 968

- Fresh four-worker rerun of `968-default-partial-compile-gen` through
  `987-agent-bind` completed 20/20 across interpreter, JIT, and unchanged
  source optimized lanes.

### In progress — 2026-09-08 full corpus audit

- A fresh four-worker run of all 1,075 structurally runnable AOSP contracts is
  active in `_build/art-upstream-corpus-final-audit`; helper fixture trees are
  excluded by layout, not by name.

### Audit progress — 2026-09-08

- The live audit has processed 187/1,075 executable contracts: 164 pass and 23
  remain under generic runtime diagnosis. No completion claim is made yet.

### Audit progress — 2026-09-08 (continued)

- The live audit advanced to 241/1,075 contracts: 217 pass and 24 fail.
  Remaining failures are being addressed as shared runtime issues across
  generated code, JNI, GC/OOM, class loading, and app-image paths.

### Verified focused fix — 2026-09-08 allocation/GC cluster

- A common compressed-reference/raw-class ABI boundary caused the OOM/GC
  shard faults inside AOSP quick allocation entrypoints. The runtime now
  normalizes Darwin compressed class arguments at that boundary, preserving
  AOSP allocator and GC behavior. Fresh interpreter/JIT/unchanged-source
  focused runs pass for 061, 064, 074, 080-oom-fragmentation, 080-oom-throw,
  096, and 1000. The change is generic and does not add test inputs,
  allowlists, or fallbacks.

### Audit progress — 2026-09-08 (live)

- The authoritative full audit reached 278/1,075 contracts (253 pass, 25
  fail). Allocation/GC boundary changes are present; post-fix full-audit
  coverage is still pending.

### Audit progress — 2026-09-08 (live update)

- The still-running pre-rebuild audit reached 310/1,075 contracts: 282 pass
  and 28 fail. GC entries in this ledger predate the allocation-boundary
  patch and require a clean post-fix rerun.

### Audit progress — 2026-09-08 (live update 2)

- The pre-rebuild audit advanced to 314/1,075 contracts: 286 pass and 28
  fail. It remains active; only the clean post-patch rerun will be used for
  final compatibility evidence.

### Audit progress — 2026-09-08 (live update 3)

- The pre-rebuild audit advanced to 333/1,075 contracts: 305 pass and 28
  fail. It is still running; final evidence will come from a clean post-fix
  rerun.

### Audit progress — 2026-09-08 (live update 4)

- The pre-rebuild audit advanced to 340/1,075 contracts: 312 pass and 28
  fail. Newly completed contracts continue to pass; clean post-patch rerun
  remains required for final evidence.

### Audit progress — 2026-09-08 (live update 5)

- The pre-rebuild audit advanced to 351/1,075 contracts: 323 pass and 28
  fail. The process remains live; no completion claim is made.

### Verified focused fixes — 2026-09-08 early JNI/stack cluster

- `004-JniTest` uses a generic Mach-O exported-symbol bridge for native
  methods lacking JNIEXPORT, and `004-StackWalk` preserves logical boot-class
  path locations through dex2oat/runtime loading. All three lanes pass for
  both tests; `004-ThreadStress` also passes on fresh rerun.

### Audit progress — 2026-09-08 (live update 6)

- The pre-rebuild audit reached 365/1,075 contracts: 337 pass and 28 fail.
  A Sol-high review is examining remaining generated-code, class-loader,
  app-image, and obsolete-const failures in parallel.

### Audit progress — 2026-09-08 (live update 8)

- The pre-rebuild audit reached 411/1,075 contracts: 382 pass and 29 fail.
  Sol-high identified a shared boot-image logical-location mismatch and a
  separate native-bridge invalid-signal loop; generic fixes are in progress.

### Audit progress — 2026-09-08 (live update 9)

- The pre-rebuild audit reached 443/1,075 contracts: 414 pass and 29 fail.
  The Sol review remains active; clean post-fix verification is pending.

### Audit progress — 2026-09-08 (live update 10)

- The pre-rebuild audit reached 507/1,075 contracts: 478 pass and 29 fail.
  Sol-high's generic boot-image and native-bridge fixes remain under review.

### Audit progress — 2026-09-08 (live update 11)

- The pre-rebuild audit reached 520/1,075 contracts: 491 pass and 29 fail.
  Sol-high implementation and focused verification of the shared fixes are
  still in progress.

### Audit progress — 2026-09-08 (live update 12)

- The pre-rebuild audit reached 551/1,075 contracts: 522 pass and 29 fail.
  Sol-high's shared boot-image/native-bridge fix is still under active
  implementation and review.

### Verified focused update — 2026-09-08

- `103-string-append` passes all three lanes after logical boot-class-path
  alignment. `115-native-bridge` is traced to Darwin ucontext PC high-bit loss;
  generic host-boundary normalization is under review.

### Audit progress — 2026-09-08 (live update 7)

- The pre-rebuild audit reached 401/1,075 contracts: 373 pass and 28 fail.
  Newly completed checker contracts continue to pass; clean post-fix coverage
  remains pending.

### Verified focused update — 2026-09-08 app-image and D8 wrapper

- `1001-app-image-regions` passes all three lanes with the current logical BCP
  contract; the earlier failure was stale.
- `1948-obsolete-const-method-handle` is a generic Bash 3.2 `set -u`
  empty-array expansion in `d8-jar-compat.sh`; the wrapper was corrected and
  three-lane verification is running.

### Verified focused update — 2026-09-08

- `1948-obsolete-const-method-handle` now passes all three lanes after the
  generic D8 wrapper fix. Sol-high is validating class-loader and multi-loader
  contracts next.

### Audit progress — 2026-09-08 (live update 16)

- The pre-rebuild audit reached 764/1,075 contracts: 734 pass and 30 fail.

### Audit progress — 2026-09-08 (live update 17)

- The pre-rebuild audit reached 786/1,075 contracts: 756 pass and 30 fail.
  Focused validation of class-loader and multi-loader fixes is in progress.

### Audit progress — 2026-09-08 (live update 18)

- The pre-rebuild audit reached 818/1,075 contracts: 788 pass and 30 fail.
  Class-loader focused reruns remain active.

### Audit progress — 2026-09-08 (live update 13)

- The pre-rebuild audit reached 639/1,075 contracts: 609 pass and 30 fail.
  New failures remain diagnostic until generic fixes are rebuilt and a clean
  rerun is completed.

### Audit progress — 2026-09-08 (live update 14)

- The pre-rebuild audit reached 675/1,075 contracts: 645 pass and 30 fail.
  No signal patch has been accepted yet; Sol-high continues implementation
  and focused validation.

### Verified focused fix — 2026-09-08 native bridge signal ABI

- `115-native-bridge` passes all three lanes after generic Darwin sigchain PC
  normalization at the host ucontext boundary. The adapter fix adds no
  test-specific gate.

### Audit progress — 2026-09-08 (live update 15)

- The pre-rebuild audit reached 722/1,075 contracts: 692 pass and 30 fail.
  Native-bridge focused validation is complete; app-image clean verification
  remains pending.

### Verified focused update — 2026-09-08 class-loader/app-image

- `142-classloader2` and `158-app-image-class-table` pass all three lanes.
  `156-register-dex-file-multi-loader` is being fixed at the generic libcore
  logical-to-physical BCP filesystem boundary after sandbox ENOENT reproduction.

### Audit progress — 2026-09-08 (live update 19)

- The pre-rebuild audit reached 932/1,075 contracts: 902 pass and 30 fail.
  `156-register-dex-file-multi-loader` remains under generic filesystem-bridge
  implementation; no test-specific mapping is being added.

### Audit progress — 2026-09-08 (live update 23)

- The running pre-rebuild audit reached 1,010 result records: 980 pass and 30
  fail. The parent remains active while final contracts drain; all listed
  failures are pre-fix/stale and will be replaced by a clean rerun.

### Focused verification update — 2026-09-08 multi-loader semantics

- `156-register-dex-file-multi-loader` now passes the BCP resource-open phase;
  its remaining `Unreachable` comes from `0154-darwin-per-loader-dex-cache.patch`
  incorrectly allowing duplicate DexFile registration across ClassLoaders.
  Restoring the AOSP semantic contract and checking regressions is in progress.

### Audit progress — 2026-09-08 (live update 22)

- The pre-rebuild audit reached 988/1,075 contracts: 958 pass and 30 fail.

### Verified focused update — 2026-09-08 multi-loader filesystem boundary

- `156-register-dex-file-multi-loader` now maps logical and physical BCP
  entries by aligned index at the libcore syscall bridge, guarded by read-only
  and exact host-capability checks. Three-lane execution is in progress.

### Audit progress — 2026-09-08 (live update 20)

- The pre-rebuild audit reached 974/1,075 contracts: 944 pass and 30 fail.

### Focused verification update — 2026-09-08

- `156-register-dex-file-multi-loader` first rerun opens tail BCP entries and
  handles unsafe DEX with the expected ZipException. The first three entries
  still fail because later runner invocations narrow capability state; the
  generic runner rewrite is in progress.

### Audit progress — 2026-09-08 (live update 21)

- The pre-rebuild audit reached 982/1,075 contracts: 952 pass and 30 fail.

### Environment verification — 2026-09-09

- On the replacement ARM64 macOS host, the Rust host test suite (10 tests),
  host `cargo check`, and D8 wrapper shell syntax check all pass. Full ART
  compatibility verification remains active.

### Clean verification — 2026-09-09

- A clean single-test ledger reproduced `156-register-dex-file-multi-loader`
  failing only at the AOSP semantic `Unreachable` after BCP resource mapping.
  The docs-referenced `0154` patch is absent as a standalone file; its
  effective source changes are being located for generic restoration.

### Continued work — 2026-09-09

- A Luna-high agent is tracing the effective per-loader DexCache changes in
  tracked sources and restoring AOSP duplicate-registration semantics for the
  clean `156` failure.

### Investigation checkpoint — 2026-09-09

- AOSP `ClassLinker::RegisterDexFile` already contains the expected
  cross-ClassLoader rejection. A comment-only candidate patch was discarded;
  no semantic change is claimed until the actual Darwin execution path is
  identified and the clean three-lane `156` contract passes.

### Runtime evidence — 2026-09-09

- The retained `156` artifacts show `VMClassLoader` logging a raw
  `unsafe-boot-dex/classes.dex` ZIP warning, then `Main.main` reaches the
  AOSP `Unreachable` branch. This proves the remaining failure is in the
  Darwin DexFile/class-loader registration path, not a test or APK rewrite;
  the exact loader/DexFile identity divergence still needs a runtime fix.

### Design correction — 2026-09-09

- A proposed location/checksum fallback for DexFile registry lookup was
  rejected as too broad: independently opened files at the same path can be
  legitimate AOSP objects. The fix must preserve pointer identity semantics
  and correct the Darwin cookie/loader handoff at its actual boundary.

### Trace checkpoint — 2026-09-09

- A conditional registry trace was rebuilt and a clean `156` run was repeated,
  but no `DexFile_defineClassNative`/registry trace appeared while `Main` was
  reached. This shifts the investigation earlier: the custom loader likely
  has no usable app `dexElements` and falls through to its parent, so the next
  check is canonical `PathClassLoader` construction/publication.

### Build portability checkpoint — 2026-09-09

- A full incremental `art-bootstrap all` reached the Skia surface smoke on the
  replacement host but failed at link time on pre-existing input-queue and
  socket-broker symbols. The SDK deprecation errors were fixed narrowly with
  `-Wno-deprecated-declarations`; the remaining link closure is still a build
  graph issue and does not justify claiming runtime completion.

### Build verification — 2026-09-09

- Added inert providers only to the standalone headless Skia smoke executable;
  production graphics targets still link the runtime-owned providers. The
  replacement host now passes `build-skia` with 120 IOSurface frames,
  `staging-copies=0`, and the expected final/sequence hashes.

### ClassLoader regression verification — 2026-09-09

- After fixing runtime archive invalidation, a clean parallel corpus run for
  `142-classloader2` through `158-app-image-class-table` passed all 16 tests,
  including interpreter, JIT, and unchanged-source lanes. The previously
  failing `156-register-dex-file-multi-loader` now passes with AOSP's real
  duplicate-registration rejection.

### ART regression verification — 2026-09-09

- The clean parallel run for `159-app-image-fields` through
  `179-nonvirtual-jni` passed all 21 AOSP tests in interpreter, JIT, and
  unchanged-source lanes. This covers app-image fields/methods/strings,
  read-barrier stress, resolution trampolines, locks, monitor ownership,
  initialization deadlocks, and JNI dispatch.

### ART regression verification — 2026-09-09 (continued)

- `180-native-default-method` through `183-rmw-stress-test` passed all 4 AOSP
  tests in interpreter, JIT, and unchanged-source lanes, covering default
  method dispatch/linking and read-modify-write stress.

### Tooling verification — 2026-09-09

- `cargo fmt --all -- --check` now passes after normalizing the manifest test
  assertion. The full bootstrap test suite still has one pre-existing nterp
  shadow-source drift assertion failure (12/13 pass); no runtime contract is
  being hidden behind that failure.

### Tooling verification — 2026-09-09 (continued)

- Updated the nterp invoke-dispatch audit fixture to include the AOSP
  `arm64ng/invoke.S` interface-vtable `add w2` fragment. The audit now matches
  the pinned source without weakening fail-closed checks; `cargo test
  -p art-bootstrap` passes all 13 tests.

### Nterp entry ABI — 2026-09-09

- Added idempotent low-32-to-`DARWIN_ART_REFERENCE_BASE` `ArtMethod*`
  normalization at both generated `ExecuteNterpWithClinitImpl` and
  `ExecuteNterpImpl` entrypoints. The generated ARM64ng object and 13-object
  runtime archive rebuild and Mach-O/DWARF audits pass; `821-many-args` passes
  interpreter, JIT, and unchanged-source lanes. Dynamic Nterp CFA semantics
  still require a separate ABI-correct unwind implementation and are not
  claimed complete.

### ClassLoader regression diagnosis — 2026-09-09

- Reproduced `497-inlining-and-class-loader` with the unmodified AOSP test and
  printed its suppressed exception temporarily (then reverted the test file).
  The failure is AOSP `ClassLinker::RegisterDexFile` rejecting the same native
  DexFile pointer when the test's second custom loader calls
  `loadClassBinaryName`; this is a real multi-loader identity/lifecycle gap,
  not a runner or JIT-only failure. A general fix is still required; no test
  input or duplicate-registration rejection was weakened.

### Replacement-host Nterp verification — 2026-09-09

- On the replacement ARM64 Mac, formatting, the 13 bootstrap tests, and the
  ARM64ng Nterp build all pass; Mach-O/DWARF verification reports no errors.
- Preserving upstream dynamic CFI verbatim is not yet viable with Apple's
  assembler at the `.org` handler layout, so the existing metadata lowering
  remains explicit and fail-closed. A real dynamic unwind implementation is
  still required before claiming full AOSP parity.

### Multi-loader identity trace — 2026-09-09

- Re-running the unmodified AOSP 497 test with temporary diagnostics confirms
  the first custom-loader define fails at the canonical duplicate-DexFile
  `InternalError`, leaving `foo` null. Diagnostics were reverted; the generic
  runtime task is to model the distinct app-image/PathClassLoader DexFile
  ownership seen by AOSP while retaining the rejection contract.

### JIT acceptance/deoptimization correction — 2026-09-09

- The complete `audit-art-jit.sh` acceptance now passes. Its deoptimization
  check accepts the AOSP-selected Nterp entrypoint as well as the bridge after
  `JitCodeCache::RemoveMethod`, matching the pinned runtime implementation.
- This is a probe-contract correction, not a runtime bypass; broad corpus and
  ClassLoader identity coverage are still required for the 100% objective.

### Replacement-host JIT audit — 2026-09-09 (continued)

- After rebuilding the changed runtime probe, `audit-art-jit.sh` exits 0 with
  the complete JIT acceptance path. The transient frame-clock timing assertion
  also passes on exact rerun and the full host test suite; no runtime fallback
or admission exception was added.

### Corpus stale-failure revalidation — 2026-09-09

- Current-runner reruns of historical failures 126, 149, 2031, 2271, and 304
  all pass their AOSP lanes; those ledger failures were stale artifacts.
- 497 remains the sole reproduced failure: duplicate DexFile registration at
  the first custom ClassLoader. The runtime must preserve AOSP rejection while
  separating the app-image and Java PathClassLoader ownership boundary.

### Dynamic Nterp unwind CFA — 2026-09-08

- ARM64ng Nterp now emits the AOSP-compatible dynamic CFA
  `*(x25 - 8) + 160` instead of the fixed `sp+224` approximation. Mach-O/DWARF
  verification, bootstrap tests, runtime graphics rebuild, and full JIT audit
  all pass.
- No interpreter dispatch or JIT admission policy changed. The generic
  DexFile/ClassLoader identity gap in case 497 is still next.

### Multi-loader boundary experiment — 2026-09-08

- AOSP 156 passes all interpreter/JIT/source lanes, preserving the canonical
  duplicate registration error. Reopening fresh Java DexFile objects in the
  PathClassLoader did not resolve 497 and was reverted; the next fix must model
  app-image and path-list DexFile ownership at the ART loading boundary.

### 497 loader-order isolation — 2026-09-08

- 497 fails identically under interpreter and JIT, whereas 156 passes all
  lanes. The issue is therefore launcher ordering around publication of the
  system PathClassLoader versus app-image/DexCache ownership, not JIT code.
  The eventual fix belongs in generic runtime loader registration and must
  retain AOSP's duplicate-DexFile rejection.

### System-loader override A/B — 2026-09-08

- An A/B build with the detached system-loader and thread override removed
  produced the same 497 failure. This rules out the late setter alone; the
  remaining discrepancy is DexFile/DexCache registration during initial
  system-loader construction and app-image startup.

### System loader startup ordering — 2026-09-08

- AOSP constructs the system class loader during `Runtime::Start`; the
  detached launcher currently republishes it later through
  `runtime_context_loader`. This ordering is the remaining 497 boundary, while
  156 confirms the rejection contract. The next implementation must move the
  app-image/DexCache split into generic runtime startup rather than altering
  test inputs or weakening duplicate registration.

### DexFile cookie decode follow-up — 2026-09-08

- Direct JNI reflection could not reliably decode the hidden DexFile cookie;
  the diagnostic was removed. Native tracing must be placed at ART's existing
  `CollectDexFilesFromJavaDexFile` path to compare the path-list DexFile with
  the app-image-side registration.

### DexFile identity instrumentation — 2026-09-08

- A disabled-by-default trace records one system PathClassLoader element and a
  distinct native `DexFile*` for `Main`, indicating an app-image/cache-side
  mapping. The remaining work is to trace the Java cookie's native pointer and
  its registration timing, while keeping AOSP duplicate rejection intact.

### Native DexFile cookie trace hook — 2026-09-08

- Added manifest patch `0156` at ART's native
  `CollectDexFilesFromJavaDexFile` path. Staging applies it successfully, and
  logging is disabled by default for normal runs.

### Cookie hook reachability — 2026-09-08

- 497 ran against the rebuilt hook-bearing runtime with zero collection-hook
  events, so the failure occurs before that OAT class-loader-context path. The
  next trace target is the native `defineClassNative` registration boundary.

### Runtime shadow source coverage — 2026-09-08

- Runtime staging now treats `class_loader_context.cc` and
  `native/dalvik_system_DexFile.cc` as patched sources, matching the compile
  list. The shadow identity moved to v6 so existing caches cannot hide this
  change. A graphics bootstrap rebuild compiled the changed closure; no
  behavioral workaround or test-specific loader fallback was introduced.

### Diagnostic patch validation — 2026-09-08

- Kept only the valid `CollectDexFilesFromJavaDexFile` trace hook and removed
  the malformed native registration experiment. A clean graphics bootstrap
  confirms the source-shadow contract; no loader behavior was changed and the
  497 ownership failure remains the next implementation target.

### Corpus regression refresh — 2026-09-08

- Current-runtime reruns pass `126-miranda-multidex`,
  `149-suspend-all-stress`, `2031-zygote-compiled-frame-deopt`,
  `2271-profile-inline-cache`, and `304-method-tracing` in interpreter, JIT,
  and unmodified lanes. The runner now recognizes branch predicates using the
  AOSP `javac_post` `$1` contract. Only 497 remains reproducibly failing.

### Current-runtime confirmation — 2026-09-08

- Repeated current-runtime execution confirms the five previously stale
  failures pass across interpreter, JIT, and unmodified paths. The remaining
  497 boundary is specifically app-image/DexCache ownership during a custom
  `DexFile.loadClassBinaryName` call; the runtime still preserves AOSP's
  duplicate-DexFile rejection semantics.

### 497 failure narrowing — 2026-09-08

- The remaining 497 failure is observed as a Java NPE (`Main.java:96`) because
  `MyClassLoader.loadClass("LoadedByMyClassLoader")` returns null. Native ART
  remains alive and test 156 continues to validate the AOSP duplicate-loader
  error contract. The next implementation comparison is app-image
  DexCache/class-table publication, not a loader-specific fallback.

### Graphics runtime rebuild confirmation — 2026-09-08

- The graphics runtime bootstrap was rebuilt successfully against the v12
  shadow identity with 256 cached ART objects. No diagnostic or loader
  behavior was added; 497 remains the only reproduced compatibility failure.

### 497 loader boundary audit — 2026-09-08

- Current evidence shows a valid canonical PathClassLoader/DexFile element and
  a null class result only when the AOSP custom loader invokes
  `loadClassBinaryName`. The 156 duplicate-loader contract still passes; the
  next change must align app-image DexCache/class-table publication rather
  than bypassing registration checks.

### 497 native registration trace — 2026-09-08

- ART tracing confirms the same native DexFile is first registered to the
  canonical PathClassLoader, then requested by the custom loader for
  `LoadedByMyClassLoader`; the existing DexCache causes AOSP's duplicate-loader
  rejection. The next fix must reproduce AOSP's app-image ownership transition
  while retaining that guard.

### 497 class-table ownership trace — 2026-09-08

- `RegisterDexFile` sees the canonical table populated but the custom loader's
  table still null when `defineClassNative` requests the same DexFile. This
  makes the ownership gap concrete: publish the custom loader table/cache at
  the AOSP lifecycle point before defining the class, without weakening the
  duplicate-loader contract.

### 497 DexCache reuse experiment — 2026-09-08

- A speculative reuse path caused a native fault after clearing ART's pending
  exception state and was removed immediately. Production again follows the
  AOSP duplicate-loader guard; the next step is the correct pre-definition
  ownership lifecycle, not ad hoc cache reuse.

### AOSP lazy ClassTable comparison — 2026-09-08

- In pinned AOSP, `RegisterClassLoader` is deliberately deferred until after
  `RegisterDexFile` checks for an existing DexCache. The custom loader's null
  table in our trace therefore matches the upstream lifecycle. The remaining
  discrepancy is specifically app-image/class-definition ownership; global
  loader pre-registration is not an acceptable fix.

### 497 DexCache owner trace — 2026-09-08

- The existing DexCache explicitly points to the canonical PathClassLoader;
  `defineClassNative` requests it from a distinct custom loader whose table is
  still null. The ownership discrepancy is therefore real and deterministic;
  the remaining implementation work is app-image publication/definition
  ordering, not pointer validity or generic loader registration.

### 497 startup-order diagnostic — 2026-09-08

- Skipping `ResolveMainDexStrings` did not change 497, and the temporary
  diagnostic was removed. The failure is not caused by that helper's eager
  string resolution; focus remains on app dex open and class-definition
  publication ordering.

### Full JIT acceptance audit — 2026-09-08

- On the replacement machine, `bash tools/audit-art-jit.sh` exited 0. All
  exercised compiled execution, GC/read-barrier, exception, JNI/native-exit,
  synchronization, VarHandle, invoke-polymorphic/custom, typed-array, and
  OSR variants reported PASS.
- A shutdown warning remains (`Current thread not detached in Runtime
  shutdown`), so thread lifecycle ownership still needs cleanup. The 497
  custom-loader discrepancy also remains unresolved; the audit is not a
  claim of complete AOSP compatibility.

### VM shutdown detach parity — 2026-09-08

- The native shutdown bridge now detaches the current ART thread before
  `DestroyJavaVM` in every process mode, instead of only in the dalvikvm
  branch. This removes the upstream Runtime-destructor warning while keeping
  app-thread quiescing and resource teardown ordering intact.
- Full JIT audit remains exit 0 after the change. The 497 custom-loader
  discrepancy is unaffected and remains the next ClassLinker investigation.

### Replacement-machine revalidation — 2026-09-08

- `cargo test -p art-bootstrap` passed 13/13, the incremental graphics link
  audit passed, and the full JIT acceptance audit exited 0 with no runtime
  detach warning.
- The unmodified AOSP 497 test still fails in both interpreter and optimized
  lanes when its custom loader receives a null class. No allowlist or fallback
  was added; ClassLinker/DexCache publication ordering remains open.

### AOSP regression recheck after shutdown fix — 2026-09-08

- Rechecked the prior failure candidates covering JNI, stack walking,
  ThreadStress, OOM, fields, GC thrash/concurrent array copy, string append,
  ParallelGC, native bridge, Miranda multidex, suspend-all, multi-loader 156,
  compiled-frame deopt, profile inline cache, and method tracing.
- All 16 tests passed interpreter/JIT/unmodified lanes. Only 497 still
  reproduces, matching the pinned AOSP `knownfailures.json` description of a
  deliberately broken loader that registers one DexFile with multiple
  loaders; no workaround was introduced.

### Fresh full-corpus recheck started — 2026-09-08

- A fresh ledger (`_build/art-upstream-corpus-recheck`) is running the entire
  pinned corpus with four parallel workers, avoiding stale result summaries.
- The first 66 completed tests are all passing; the run remains active and
  will be classified only after every selected test reaches a terminal state.

### Fresh corpus checkpoint — 2026-09-08

- The fresh four-worker ledger has completed 105 tests with no failures so far.
  Workers remain active; final compatibility classification awaits terminal
  results for the entire corpus.

### Fresh corpus checkpoint 2 — 2026-09-08

- The same live ledger has completed 148 tests with no failures. This is an
  interim observation only; the full pinned corpus remains active.

### Fresh corpus checkpoint 3 — 2026-09-08

- The fresh ledger has completed 170 tests with no failures so far, including
  CFI, daemon-shutdown, and register-natives paths. Final classification waits
  for the live corpus to finish.

### Fresh corpus checkpoint 4 — 2026-09-08

- The fresh ledger has completed 199 tests without failure, covering GC-loop,
  lock visitation, thread-group JNI, and app-image paths. The worker pool is
  still running, so the complete-corpus verdict remains pending.

### Fresh corpus checkpoint 5 — 2026-09-08

- The fresh ledger has completed 208 tests with no failures, covering RMW
  stress, allocation tracking, bytecode inspection, and suspend paths. The
  worker pool remains active and the full verdict is pending.

### Fresh corpus checkpoint 6 — 2026-09-08

- The fresh four-worker ledger has completed 220 tests without failure,
  including native suspend/resume, per-agent TLS, transform, and local
  variable-table paths. The remaining corpus is still running.

### Fresh corpus checkpoint 7 — 2026-09-08

- The fresh ledger has completed 227 tests without failure, including VM-init
  timing and native/recursive/owned monitor paths. Workers remain active and
  final classification is pending.

### Fresh corpus checkpoint 8 — 2026-09-08

- The fresh ledger has completed 234 tests without failure, including JVMTI
  frame-pop, missed-frame-pop, exception-event, and exception-catch paths. The
  worker pool remains active and final classification is pending.

### Fresh corpus checkpoint 9 — 2026-09-08

- The fresh ledger has completed 243 tests without failure, including
  monitor/JVMTI signal, current-frame, thread-end, and transform paths. The
  worker pool remains active and the final verdict is pending.

### Fresh corpus checkpoint 10 — 2026-09-08

- The fresh ledger has completed 249 tests without failure, including DDMS,
  dispose stress, raw-monitor suspend/exit/wait, and proxy method-argument
  paths. The worker pool remains active and final classification is pending.

### Fresh corpus checkpoint 11 — 2026-09-08

- The fresh ledger has completed 257 tests without failure, including obsolete
  method-handle, short-dex, monitor-enter, and pop-frame JIT paths. Workers
  remain active and the final classification is pending.

### Fresh corpus checkpoint 12 — 2026-09-08

- The fresh ledger has completed 266 tests without failure, including
  exception-ext, transform/redefine instrumentation, obsolete-JIT multithread,
  bounds-codegen, and loop-vectorizer paths. The corpus remains active.

### Fresh corpus checkpoint 13 — 2026-09-08

- The fresh ledger has completed 273 tests without failure, including add-to-
  dex ClassLoader, JVMTI local primitive/object/bad-slot, and multi-force-
  early-return paths. The corpus remains active.

### Fresh corpus checkpoint 14 — 2026-09-08

- The fresh ledger has completed 282 tests without failure, including JNI ID
  swap (indices/pointers), resize-array, and structural transformation/
  obsolescence paths. The corpus remains active.

### Fresh corpus checkpoint 15 — 2026-09-08

- Corpus triage found no ART semantic regression in the structural redefine
  cluster. Runner parity was tightened by adding the AOSP dlmalloc include
  root and ignoring unresolved generated Java symlinks during source-owner
  discovery; focused 1986/1987/2000 interpreter and JIT runs now pass.

### Handoff checkpoint 16 — 2026-09-08

- Runner fix is pushed as `df07931`; focused structural-redefine tests pass.
  The existing ledger has historical rows from the brief pre-fix syntax
  window and should be refreshed or rechecked after the worker completes.

### Handoff checkpoint 17 — 2026-09-08

- Representative rows `420-const-class` and `2286-invokevirtual-invokeexact`
  were rechecked; interpreter, JIT, and unmodified lanes pass.

### Fresh corpus checkpoint 18 — 2026-09-08

- 675 corpus rows are complete with no new failures since the runner repair.
  The only current semantic failure is the upstream-known broken
  `497-inlining-and-class-loader` fixture (AOSP bug b/34193123), not a
  platform-specific fallback or allowlist condition.

### Fresh corpus checkpoint 19 — 2026-09-08

- 773 rows are complete with no post-repair failures. Later checker and
  compiler rows continue to pass; only the documented upstream-broken 497
  class-loader fixture remains failed.

### Fresh corpus checkpoint 20 — 2026-09-08

- The worker has completed 815 rows without a new post-repair failure. The
  ledger's failing rows remain classified as historical runner syntax records
  and the upstream-known 497 fixture.

### Fresh corpus checkpoint 21 — 2026-09-08

- The worker reached 860 rows without a new failure after the runner repair;
  exception, JNI, and compiler lanes continue to pass.

### Fresh corpus checkpoint 22 — 2026-09-08

- 878 rows are complete without a new post-repair failure. The failure set is
  unchanged and remains limited to historical runner syntax records plus the
  AOSP-known broken 497 class-loader fixture.

### Fresh corpus checkpoint 23 — 2026-09-08

- 979 rows are complete. Recorded failures remain attributable to the pre-fix
  runner SyntaxError window or the upstream-known 497 fixture; no post-fix
  ART/JIT failure is present.

### Final corpus checkpoint 27 — 2026-09-08

- All 1,075 pinned AOSP corpus entries completed. The final 112 failures are
  classified as 109 historical runner SyntaxError records, the documented
  broken 497 fixture, and two genuine post-repair gaps (`936-search-onload`,
  `938-load-transform-bcp`) in JVMTI agent/boot-class transformation.

### Fresh corpus checkpoint 24 — 2026-09-08

- 994 rows are complete. The post-repair suffix is fully green; the 110
  recorded failures remain historical SyntaxError rows plus AOSP's known
  broken 497 fixture.

### Fresh corpus checkpoint 25 — 2026-09-08

- 1,001 rows are complete with no post-repair failure. Remaining failures are
  unchanged historical SyntaxError records and the upstream-known 497 fixture.

### Fresh corpus checkpoint 26 — 2026-09-08

- At 1,057 rows, `936-search-onload` and `938-load-transform-bcp` are the
  first post-repair semantic failures, both in JVMTI/boot-class transformation
  behavior. They remain open runtime work, not hidden gates.

### Final corpus audit checkpoint 28 — 2026-09-08

- The 1,075-test corpus is terminal. Runner fixes are validated by focused
  reruns; only `936-search-onload` and `938-load-transform-bcp` remain as
  genuine JVMTI boot/system search and boot transformation gaps.

### Diagnostic checkpoint 29 — 2026-09-08

- Runtime architecture remains Android-shaped: the open work is in native
  JVMTI boot-class search and transformation semantics, not in the harness.
- Unexpected host exits now retain the last 40 lines of the detached ART log,
  making phase ordering, class-linker, and signal failures directly actionable
  during focused compatibility runs.

### Diagnostic checkpoint 31 — 2026-09-08

- Current-machine reruns preserve the same Android semantic boundary: ordinary
  JVMTI transformation works, but injected boot-Dex loading and boot-class
  transformation remain incomplete.
- The runner now provides the native ART fault tail needed to fix the runtime
  class-linker path rather than masking it in the harness.

### Nterp checkpoint 32 — 2026-09-08

- Darwin ARM64 Nterp now matches ART's logical-reference ABI at both execution
  entry points, with a fail-closed ordering audit and regression test.
- Generator, Mach-O, CFI/DWARF, and focused Nterp checks are green; JVMTI
  boot-class work remains the next runtime slice.

### Diagnostic checkpoint 30 — 2026-09-08

- The host/runtime split remains intact: ordinary JVMTI transformation is
  green, but boot-class injection and boot-class transformation still diverge
  from AOSP.
- Native crash tails are now retained by the test runner, so the next fix can
  be validated against the actual ART class-linker phase and fault site.

### Nterp checkpoint 33 — 2026-09-08

- ARM64 Nterp entry normalization is verified at the generated instruction
  boundary, preserving Android's logical compressed-reference contract.
- Remaining divergence is isolated to JVMTI boot-class search and
  transformation semantics; no interpreter fallback or test gate was added.

### Runtime checkpoint 37 — 2026-09-08

- The native boot/system class-loader path remains the active implementation
  slice, with host behavior unchanged and Android lifecycle semantics intact.

### Runtime checkpoint 35 — 2026-09-08

- A clean runtime/graphics closure rebuild does not alter the focused JVMTI
  failures, confirming a native ART boot-loader semantic defect rather than a
  stale artifact.
- Investigation is narrowed to the Android-shaped `ClassPreDefine` and boot
  `ClassLoaderHelper` path; host-side fallbacks remain unchanged.

### Runtime checkpoint 34 — 2026-09-08

- A fresh incremental graphics/runtime link rebuild passes its strict closure
  audit; focused JVMTI failures persist, so the remaining work is in the ART
  boot-class callback/class-linker path itself.

### Runtime checkpoint 36 — 2026-09-08

- The compatibility layer preserves Android's boot/system class-loader
  contract; the remaining issue is a native implementation defect, not a host
  policy substitution.

### Runtime checkpoint 38 — 2026-09-08

- Android-shaped boot/system class-loader semantics remain the target. The
  unresolved behavior is still confined to the native ART implementation path.

### Runtime checkpoint 39 — 2026-09-08

- Android-shaped boot/system class-loader semantics remain unchanged while the
  native `ClassPreDefine`/DexFile identity review proceeds.

### Runtime checkpoint 41 — 2026-09-08

- The Android-shaped boot/system class-loader contract is unchanged while the
  native implementation review continues; no host substitution was made.

### Runtime checkpoint 40 — 2026-09-08

- Android-shaped boot/system class-loader semantics remain unchanged while the
  implementation review continues; completion is not claimed.

### Runtime checkpoint 42 — 2026-09-08

- Canonical boot-image generation and the ordinary JVMTI transform lane were
  revalidated on the replacement Mac. No duplicate runtime-phase patch or
  host fallback was retained; boot-image class-selection/profile semantics
  remain the next native implementation boundary for 938, with 936 tracked
  independently as a generated-code crash.

### Runtime checkpoint 43 — 2026-09-08

- The replacement-Mac boot-image rebuild and ordinary JVMTI transform lane are
  green. An empty preload-list experiment was explicitly rejected because it
  did not restore boot-class transformation, preserving the Android-shaped
  architecture and narrowing the next change to profile-derived image class
  selection plus native boot-Dex loading.

### Runtime checkpoint 44 — 2026-09-08

- The boot-image builder now uses embedded AOSP `profman` output for
  profile-driven class selection. This fixes `938-load-transform-bcp` in both
  interpreter and JIT modes without an APK rewrite or host fallback; the
  independent 936 generated-code crash remains open.

### Runtime checkpoint 45 — 2026-09-08

- 936's crash boundary is now identified as the ONLOAD `java.class.path`
  update dereferencing a null optional `Properties.defaults` chain. The
  Android-shaped JVMTI implementation is patched to update the owning
  properties object when that chain is absent. Full runtime relink remains
  blocked only by the replacement host lacking Android NDK 28.2.

### Runtime checkpoint 46 — 2026-09-08

- A fresh linked-runtime experiment disproved the `Properties.defaults` null
  hypothesis for 936; the temporary JVMTI patch was removed. The remaining
  failure is still an ART generated-code/JNI or boot-Dex entrypoint defect and
  is being kept explicit for the next instrumentation pass.

### Runtime checkpoint 47 — 2026-09-08

- Selecting the installed Android NDK 28.2 makes the complete graphics link
  audit green. 936 nevertheless fails in the newly linked runtime, confirming
  an independent generated-code/boot-Dex entrypoint defect rather than a host
  setup problem.

### Runtime checkpoint 48 — 2026-09-08

- The OpenJDK JVMTI search path now follows ART class-linker semantics for
  system classes and safely handles an absent `Properties.defaults` chain.
  Rebuild and link audit are green, while 936 continues to fault at `0x4` in
  generated code; JNI/quick-call ABI investigation remains the next task.

### Runtime checkpoint 49 — 2026-09-08

- Static `System.props` access now also follows ART initialization semantics
  (`EnsureInitialized` with a stack handle). Rebuild and link audit pass, but
  936 still faults at `0x4`; the remaining work is generated-code/JNI quick
  invocation ABI parity.

### Runtime checkpoint 50 — 2026-09-08

- Mach-O symbol analysis confirms the 936 fault occurs while reading the null
  `System.props` static field, before any JNI call. The boot profile now seeds
  `Hashtable`/`Properties` and the boot image rebuild passes, but 936 remains
  unchanged; static field initialization/state publication is the next ART
  parity boundary.

### Runtime checkpoint 51 — 2026-09-08

- Fresh post-seed disassembly continues to fault on an ART internal null pointer
  (`ldr w8, [x23,#4]`, `x23=0`) before JNI dispatch. This rules out the quick-call
  receiver as the immediate cause; the next experiment must isolate ART static
  field resolution/initialization without changing callback ordering.

### Runtime checkpoint 52 — 2026-09-08

- The null register survives boot-image changes and occurs before JNI dispatch;
  investigation is narrowed to compressed `ObjPtr` normalization at the Darwin
  C++/ART boundary.

### Runtime checkpoint 54 — 2026-09-08

- Runtime-core now applies base-relative Darwin `ObjPtr` encoding/decoding
  consistently. `938-load-transform-bcp` passes in interpreter and JIT modes;
  936 advances past the prior static-field boundary but still fails in a
  dex2oat internal reference path requiring another ABI audit.

### Runtime checkpoint 55 — 2026-09-08

- The common `ObjPtr` conversion compiles and keeps 938 green, but 936 moves to
  a later dex2oat null-page fault. This confirms partial progress and leaves an
  absolute-pointer versus base-offset representation mismatch to isolate.

### Runtime checkpoint 57 — 2026-09-08

- The remaining boundary is `defaults_field->GetObject(props_obj)` with null
  `props_obj`; the next patch must recover the object before field dereference.

### Runtime checkpoint 56 — 2026-09-08

- 938 remains green in interpreter and JIT after the common `ObjPtr` change;
  936 is narrowed to a separate dex2oat internal reference failure.

### Runtime checkpoint 53 — 2026-09-08

- The null is observed before JNI dispatch, so field-ID fallback alone cannot
  fix 936. The next change must correct compressed-reference or handle usage at
  the ART C++ boundary.

### Runtime checkpoint 58 — 2026-09-08

- Darwin minimal startup matches Android dependency order by registering
  libcore natives before root class initialization and intrinsics setup under
  ART's scoped object access. Upstream 936 and 938 pass in interpreter and
  JIT modes.

### Runtime checkpoint 59 — 2026-09-08

- The post-registration startup ordering is validated by the graphics runtime
  build and the ART JIT audit (intrinsics, ABI/unwind, GC, JNI, exceptions,
  monitors, and inlining). This is a compatibility milestone, not completion:
  real APK breadth and dynamic class-loader integration remain to be closed.

### Runtime checkpoint 60 — 2026-09-08

- GC-stress is now the active compatibility gap: a reproducible arm64e PAC
  trap occurs before managed output in the stress launcher, while ordinary
  JIT/GC and 936/938 lanes pass. The next change is being constrained to the
  shared Darwin ABI path rather than adding a test or interpreter bypass.

### Runtime checkpoint 61 — 2026-09-08

- Serial gcstress reproduction confirms the arm64e PAC trap is stress-specific
  and not a concurrent build artifact. Diagnostic launcher instrumentation was
  removed after violating captured-output handling; no runtime workaround was
  retained. Symbolization of the shared ABI boundary remains the next step.

### Runtime checkpoint 62 — 2026-09-08

- Local Darwin unwinding now uses Mach frame records with stripped return
  addresses instead of Apple's PAC-authenticating libunwind across Android
  quick/Nterp frames. GC-stress 074/837 and CFI 137 pass across all lanes;
  the provider smoke and graphics link audit remain green.

### Runtime checkpoint 63 — 2026-09-08

- AOSP marks 497 as a known broken duplicate-DexFile loader and disables it;
  its failure is therefore not evidence for weakening ClassLinker registration.
  The valid 156 multi-loader rejection remains the compatibility contract, so
  investigation continues with supported dynamic-loader tests instead.

### Runtime checkpoint 64 — 2026-09-08

- `142-classloader2` passes interpreter, JIT, and optimized lanes, validating
  supported PathClassLoader subclass and secondary-Dex resolution. Further
  InMemoryDexClassLoader, JVMTI transform, and shared-library loader coverage
  remains active.

### Runtime checkpoint 65 — 2026-09-08

- Loader parity expanded with three-lane passes for `692-vdex-secondary-loader`,
  `693-vdex-inmem-loader-evict`, and `944-transform-classloaders`; the runtime
  now exercises secondary VDEX ownership, in-memory cache eviction, and
  transform/class-loader interactions through the normal AOSP contracts.

### Runtime checkpoint 66 — 2026-09-08

- `2237-checker-inline-multidex` passes all three execution lanes, confirming
  multidex checker/inlining behavior remains on the standard ART pipeline.

### Runtime checkpoint 67 — 2026-09-08

- `1946-list-descriptors` passes interpreter, JIT, and optimized lanes, adding
  JVMTI loader-specific DEX descriptor enumeration to the compatibility matrix.

### Runtime checkpoint 68 — 2026-09-08

- `2239-varhandle-perf-vh-cas` passes all three lanes, covering JIT VarHandle
  compare-and-set and managed-memory barrier behavior without a Darwin-only
  execution path.

### Runtime checkpoint 69 — 2026-09-08

- `2247-checker-write-barrier-elimination` and `2277-methodhandle-invokeexact`
  pass all lanes, validating optimized write-barrier elimination and exact
  MethodHandle dispatch on the standard ART pipeline.

### Runtime checkpoint 70 — 2026-09-08

- `2264-throwing-systemcleaner` and `2282-single-step-before-catch` pass
  interpreter, JIT, and optimized lanes, validating exception delivery and
  debugger single-step catch-boundary handling.

### Runtime checkpoint 71 — 2026-09-08

- `570-checker-osr` and `088-monitor-verification` pass all lanes, covering
  optimized OSR transfer and monitor-verifier behavior on the standard ART
  execution pipeline.

### Runtime checkpoint 72 — 2026-09-08

- `406-fields` and `407-arrays` pass interpreter, JIT, and optimized lanes,
  validating field/array access and bounds/type checks through the shared ART
  ABI.

### Runtime checkpoint 73 — 2026-09-08

- `412-new-array` and `420-const-class` pass all lanes, validating allocation
  and class-constant resolution without a Darwin-specific execution path.

### Runtime checkpoint 74 — 2026-09-08

- SystemProperties native handles now use stable numeric tokens mapped to
  property names, matching Android's process-lifetime `prop_info` semantics.
  Framework compatibility build and `936-search-onload` pass all lanes.

### Runtime checkpoint 75 — 2026-09-08

- The full JIT acceptance audit is green across GC/read barriers, JNI/native
  exits, framework/window/lifecycle smoke, and launcher startup. BCP transform
  regression `938-load-transform-bcp` passes all three execution lanes.

### Runtime checkpoint 76 — 2026-09-08

- Fixed the framework SystemProperties handle getter JNI ABI and verified
  stable handles, post-find value updates, typed reads, and invalid-handle
  safety with native smoke and an incremental compat check. LSE
  acquire/release regression `2242-checker-lse-acquire-release-operations`
  passes all execution lanes.

### Runtime checkpoint 77 — 2026-09-08

- System-property boolean parsing now accepts Android's `y`/`n` aliases; the
  native compatibility and managed-native-load/graphics-link audits remain
  green.

### Runtime checkpoint 78 — 2026-09-08

- `2275-integral-unsigned-arithmetic` passes all execution lanes, validating
  unsigned integral operations through the shared ARM64 JIT lowering.

### Runtime checkpoint 79 — 2026-09-08

- `410-floats` and `419-long-parameter` pass interpreter, JIT, and optimized
  lanes, covering floating-point code generation and long-argument ABI paths.

### Runtime checkpoint 80 — 2026-09-08

- `2239-varhandle-perf-vh-unsafe-cas` passes all lanes, validating
  Unsafe-backed atomic CAS and managed memory ordering through the standard
  ART JIT path.

### Runtime checkpoint 81 — 2026-09-08

- `401-optimizing-compiler` and `304-method-tracing` pass interpreter, JIT,
  and optimized lanes, covering optimizer startup and instrumentation tracing
  across compiled method entry/exit.

### Runtime checkpoint 82 — 2026-09-08

- `411-optimizing-arith` and `414-static-fields` pass all lanes, validating
  arithmetic lowering and static-field lifecycle/barrier behavior on the
  shared ART pipeline.

### Runtime checkpoint 83 — 2026-09-08

- `418-const-string` and `2256-checker-vector-replacement` pass interpreter,
  JIT, and optimized lanes, covering string constants and vector lowering in
  the shared optimizer.

### Runtime checkpoint 84 — 2026-09-08

- InputChannel now follows Android InputTransport token identity: one Binder
  token is shared across each pair/dup wrapper, exposed via `nativeGetToken`,
  with VM-safe global-ref teardown. Incremental native build and graphics-link
  audit pass.

### Runtime checkpoint 85 — 2026-09-08

- `370-dex-v37` passes all execution lanes, confirming DEX v37 parsing,
  verification, and execution remain on the shared ART pipeline.

### Runtime checkpoint 86 — 2026-09-08

- `412-new-array --gcstress` passes all lanes, covering array allocation and
  reference barriers while the concurrent-copying collector is stressed.

### Runtime checkpoint 87 — 2026-09-08

- Added process-local InputChannel Parcel serialization/restoration of name,
  endpoint role, and shared Binder token identity. The implementation matches
  the current single-process Darwin transport while leaving cross-process FD
  transfer as an explicit future transport-layer task; native graph check is
  green.

### Runtime checkpoint 88 — 2026-09-08

- `543-env-long-ref` and `686-get-this` pass all lanes, validating JNI long
  references and instance receiver argument ABI through the shared ART JIT.

### Runtime checkpoint 89 — 2026-09-08

- `543-env-long-ref --gcstress` passes interpreter, JIT, and optimized lanes,
  covering JNI long-reference lifetime during concurrent-copying collection.

### Runtime checkpoint 90 — 2026-09-08

- InputChannel disposal now releases shared transport state immediately, while
  the wrapper remains available for the registered finalizer, matching Android
  lifecycle semantics and avoiding VM-shutdown global-ref leaks. Native checks
  remain green.

### Runtime checkpoint 91 — 2026-09-08

- Real managed InputChannel Parcel round-trip confirms name and Binder token
  identity across all execution lanes; `2258-checker-valid-rti` and
  `2283-checker-remove-null-check` also pass. The remaining cross-process FD
  transport mismatch is tracked separately rather than hidden by registry
  fallback.

### Runtime checkpoint 92 — 2026-09-08

- KeyCharacterMap Parcel serialization now preserves device id through a
  versioned record, removing the previous silent device-1 reset. Native graph
  check passes; managed `obtainEmptyMap` → Parcel → CREATOR identity and
  keyboard-type smoke passes interpreter, JIT, and optimized lanes.

### Runtime checkpoint 93 — 2026-09-08

- `9999-key-character-map-parcel-smoke` passes all execution lanes, validating
  device-id identity and FULL keyboard type; `2255-checker-branch-redirection`
  also passes interpreter, JIT, and optimized lanes.

### Runtime checkpoint 94 — 2026-09-08

- InputChannel Parcel now emits the Android initialized marker even for a
  disposed/uninitialized channel, keeping containing Parcel fields aligned.
  Native graph verification and managed disposed-channel-plus-sentinel smoke
  pass interpreter, JIT, and optimized lanes.

### Runtime checkpoint 95 — 2026-09-08

- `2252-rem-optimization-dividend-divisor` and `2278-nested-loops` pass all
  lanes, validating remainder optimization and nested-loop lowering on the
  shared ART JIT pipeline.

### Runtime checkpoint 96 — 2026-09-08

- `2259-checker-code-sinking-infinite-try-catch` and
  `2284-regression-test-368984521-loop-opt` pass interpreter, JIT, and
  optimized lanes, validating exception-region sinking and loop optimization.

### Runtime checkpoint 97 — 2026-09-08

- `2253-checker-devirtualize-always-throws` passes all execution lanes,
  covering devirtualized always-throwing calls and exception-edge lowering.

### Runtime checkpoint 98 — 2026-09-08

- `2248-checker-smali-remove-try-until-the-end` passes interpreter, JIT, and
  optimized lanes, covering try-region elimination and exception-table bounds.

### Runtime checkpoint 99 — 2026-09-08

- KeyCharacterMap text synthesis now follows the Android event contract:
  characters resolved by the shared key mapping produce key down/up sequences,
  shifted characters are bracketed by Shift down/up, and device/source identity
  is retained. Managed lowercase, uppercase, and unmapped-character coverage
  passes interpreter, JIT, and optimized lanes. Layout-specific fallback
  actions remain an explicit future key-layout data task.

### Runtime checkpoint 100 — 2026-09-08

- KeyCharacterMap behavior queries now share one physical-keyboard mapping:
  `getMatch` honors requested base/Shift preference before a same-key fallback,
  and `getNumber` exposes decimal-row numbers without adding legacy phone-key
  T9 metadata to a FULL keyboard. Managed null/empty and match/number coverage
  passes interpreter, JIT, and optimized lanes. Cross-process InputChannel is
  still the larger input-architecture gap because its payload queue remains
  process-local even though Binder wire FD transfer already exists.

### Runtime checkpoint 101 — 2026-09-08

- The InputChannel audit fixes the next architecture boundary: a real
  cross-process implementation must transfer endpoint descriptors and use a
  versioned payload/finish-ACK wire protocol. The existing same-process
  registry remains valid for local callers; no fake FD-only compatibility shim
  was added.

### Runtime checkpoint 102 — 2026-09-08

- Parcel FD ownership is now centralized in broker-aware helpers, preserving
  descriptor duplication and cleanup rules for future InputChannel endpoint
  import/export. Native graph and graphics-link audits pass; no process-local
  payload behavior was misrepresented as cross-process support.

### Runtime checkpoint 103 — 2026-09-08

- InputChannel parcel handling now follows the AOSP field order and uses the
  broker FD duplication helpers. The implementation is deliberately not
  marked as full cross-process support: payload and finish-ACK framing still
  require a two-process endpoint test before that boundary can be closed.

### Runtime checkpoint 104 — 2026-09-08

- InputChannel Parcel serialization now matches AOSP's token/name/FD order and
  can adopt an imported broker endpoint. Same-process behavior remains intact;
  event payload framing and finish acknowledgements remain explicitly pending
  a two-process validation.

### Runtime checkpoint 105 — 2026-09-08

- Imported InputChannel descriptors now have an explicit remote-endpoint
  owner, separate from the local pair state, with single-close cleanup. This
  keeps the AOSP Parcel boundary honest while the framed payload/ACK reader is
  still under implementation.

### Runtime checkpoint 106 — 2026-09-08

- The remote InputChannel constructor now owns the imported endpoint while
  retaining an independent local wake pair, matching Android's initialized
  marker plus token/name/FD parcel contract. Cross-process event framing is
  not claimed until a two-process smoke exercises it.

### Runtime checkpoint 107 — 2026-09-08

- Remote InputChannel endpoints now have a versioned fixed-size packet frame
  and receive-side reassembly path, while local wake-pair behavior is retained
  for same-process channels. This closes the one-way input payload slice;
  finish acknowledgements and an end-to-end two-process smoke remain open.

### Runtime checkpoint 108 — 2026-09-08

- The remote endpoint path now has bounded, validated packet framing with
  serialized writes and receive-side reassembly. This is the first real
  one-way payload slice; ACK framing and a process-separated smoke are still
  required for full InputTransport parity.

### Runtime checkpoint 110 — 2026-09-08

- Remote InputChannel now registers both the local wake endpoint and imported
  endpoint where present, with ACK-aware stream decoding and shared finish
  wait semantics. A real two-process smoke is still required before claiming
  complete InputTransport parity.

### Runtime checkpoint 111 — 2026-09-08

- The APK runner now handles an empty split-APK vector safely with `set -u`,
  removing a harness-only failure that prevented base-only acceptance runs.
  Runtime transport parity is unchanged and still requires the two-process
  smoke.

### Runtime checkpoint 109 — 2026-09-08

- Finish acknowledgements now have a dedicated validated wire frame and share
  the existing condition-variable wait path. Decoder ordering handles ACK and
  input interleaving without discarding buffered frames; process-separated
  end-to-end validation is still pending.

### Runtime checkpoint 110 — 2026-09-08

- The imported InputChannel endpoint now safely multiplexes any alternating
  sequence of validated payload and completion frames. Only ACKs for registered
  pending sequences enter the bounded completion path, preventing an external
  peer from accumulating unsolicited results. Native graph and graphics-link
  verification pass; the remaining boundary is a real two-process smoke.

### Runtime checkpoint 112 — 2026-09-08

- Window-menu acceptance now reaches real Calculator, Calendar, and Chrome APK
  interactions: popup dispatch, History/Day-Week-Month labels, Chrome menu/new
  tab, outside dismissal, and crash checks pass. Resizing the host surface does
  not yet trigger Android-style relayout of the existing popup ViewRoot; its
  stale position is recorded as the next WindowManager transaction task.

### Runtime checkpoint 113 — 2026-09-08

- WindowManager display changes now force a WMS relayout on every registered
  ViewRoot, matching Android 16's `forceWmRelayout()` path. The popup resize
  lane proves the frame transition `320,8 → 208,8`; the APK acceptance harness
  also corrects scaled Calendar physical coordinates. Calculator, Calendar,
  dismiss, and resize checks pass; the full suite still exposes Chrome's
  pre-existing `MockContext.sendBroadcast()` stub abort during startup.

### Runtime checkpoint 114 — 2026-09-08

- Detached app contexts now implement the broadcast-send contract instead of
  throwing through `MockContext`; DEX counts were refreshed and the button DEX
  build passes. Chrome advances beyond that startup point, then reveals a
  separate mutex-lock failure during compositor/child teardown and an ART
  generated-code fault, which is the next concurrency/JIT lifetime boundary.

### Runtime checkpoint 115 — 2026-09-08

- Diagnostic symbols show `BlastBufferQueueNativeDestroy` entering the native
  window transaction-callback setter after the Java Surface release has
  already destroyed its host object. The resulting `std::mutex` EINVAL is a
  native-window UAF/lifetime bug, not a generated-code semantic mismatch. The
  next patch gives the queue observer an explicit reference through destroy.

### Runtime checkpoint 116 — 2026-09-08

- Corrected the Surface lifecycle ABI: `nativeDestroy` no longer decrements
  the native-window reference, matching Android's disconnect-versus-release
  split. Fresh Chrome teardown logs show no mutex EINVAL or generated-code
  fault. A separate child-service registry miss (`unknown service child PID`)
  remains for the next multiprocess compatibility slice.

### Runtime checkpoint 117 — 2026-09-08

- Service-child release now tolerates duplicate lifecycle callbacks after a
  child has been reaped, matching Android's idempotent stop/unbind behavior.
  Host check/tests pass and a fresh Chrome startup/menu smoke returns status 0
  without the previous teardown or PID-registry errors.

### Runtime checkpoint 118 — 2026-09-08

- Two fresh Chrome lifecycle runs exercise the physical bottom-menu/new-tab
  path and complete child-process teardown without native-window UAF, mutex
  errors, generated-code faults, or service-PID registry failures. Startup and
  menu lifecycle is now stable; tab content/renderer behavior remains to be
  validated.

### Runtime checkpoint 119 — 2026-09-08

- The tab-graphics harness now executes correctly after moving an inline shell
  comment out of the environment-assignment continuation. Long-running Chrome
  tab/grid rendering reaches sustained Metal/SurfaceFlinger scanout but still
  ends in an ART generated-code fault, marking a distinct renderer/JIT lifetime
  boundary beyond the stable startup lifecycle.

### Runtime checkpoint 120 — 2026-09-08

- Rebuilt on the replacement Mac with the new ImageReader native path: Java
  ImageReader now owns an ANativeWindow-backed queue and AHardwareBuffer
  references, with callback-context lifetime protected across teardown. The
  graphics bootstrap/link audit passed and an 8-second real Chrome APK smoke
  exited cleanly (no ImageReader linkage error or fatal signal). Full tab-grid
  graphics endurance and remaining ImageReader plane/HardwareBuffer contracts
  are still open.

### Runtime checkpoint 121 — 2026-09-08

- Added the Java `HardwareBuffer` native bridge used by ImageReader, retaining
  AHardwareBuffer ownership across SurfaceImage access and registering the
  basic allocate/describe/finalizer ABI. The rebuilt graphics link audit and a
  fresh Chrome APK smoke pass on the replacement Mac; logs confirm
  IOSurface-backed buffer allocation. Parcel/GraphicBuffer conversion and
  complete CPU plane behavior remain explicit compatibility work.

### Runtime checkpoint 122 — 2026-09-08

- Registered the Android `SyncFence` native ABI with owned fd lifetime,
  finalizer, validity/fd accessors, bounded waits, signal-time queries, and
  refcount increments backed by the Darwin broker. The graphics bootstrap/link
  audit and a fresh Chrome APK smoke pass cleanly (`RC=0`), with no SyncFence
  linkage or fatal-signal error. Fence and parcel stress coverage remains.

### Runtime checkpoint 123 — 2026-09-08

- Revalidated the replacement-machine toolchain: `cargo test
  -p darwin-art-host` passes all 10 host/graphics tests and `cargo test
  -p art-bootstrap` passes all 14 Nterp/build-contract tests. This confirms
  no regression from the SyncFence bridge while the full AOSP corpus,
  long-running Chrome graphics, and parcel stress gates remain open.

### Runtime checkpoint 124 — 2026-09-08

- A fresh 70-second Chrome tab-grid run reached 11,280 SurfaceFlinger scanout
  requests and 43 presents with ImageReader/SyncFence linkage intact, but
  reproduced repeated generated-code faults in the JIT address range. This
  isolates the next investigation to generated-code lifetime/GC or
  signal-unwind behavior under sustained renderer activity; the end-to-end
  graphics acceptance gate is still failing.

### Runtime checkpoint 125 — 2026-09-08

- Replacement-machine verification passes host/bootstrap tests, incremental
  native build, graphics-link closure audit, and MAP_JIT W^X audit. Fault
  context records signal-safe Darwin JIT write depth; reproduced Chrome faults
  all had depth zero, so code-cache range lifetime/GC reclaim or signal-unwind,
  rather than a leaked write scope, remains the active blocker.

### Runtime checkpoint 126 — 2026-09-08

- Raw Darwin fault logging now runs only after ART's registered fault handlers
  decline the signal, eliminating false positives from recoverable MAP_JIT
  transitions. Build/link audits pass, but the fresh 70-second Chrome run still
  has 2,558 unhandled generated-code faults (`jit_write_depth=0`); JIT lifetime,
  signal recovery, or generated-code correctness remains unresolved.

### Runtime checkpoint 127 — 2026-09-08

- After rebuilding with raw fault logging placed after registered ART handlers,
  a live 70-second Chrome run reached 11,280 scanouts and 44 presents but still
  emitted 2,558 unhandled generated-code faults with zero JIT write depth. The
  ordering correction is committed (`3593f47`); next work targets code-cache
  retirement/lifetime and generated-code correctness.

### Runtime checkpoint 128 — 2026-09-08

- Sigchain correlation shows ART's special handler precedes the Darwin user
  trampoline that restores Android V8 MAP_JIT permissions. The 2,558 records
  are recoverable transitions logged too early; `unresolved=0` and `fatal=0`
  throughout the run. Next is pre-special W^X recovery and a fresh 70-second
  acceptance verification.

### Runtime checkpoint 129 — 2026-09-08

- Added a shared Darwin runtime-signal recovery helper and call it before ART's
  special handlers. The fresh 70-second Chrome run had zero generated-code
  fault diagnostics and sustained 11,340 scanouts/42 presents. The harness did
  not detect its real tab-switcher button, so the full acceptance gate is still
  pending even though the W^X fault logging boundary is resolved.

### Runtime checkpoint 130 — 2026-09-08

- The bionic process-state facade tests pass after pre-chain recovery. Two
  70-second Chrome runs show zero generated-code diagnostics, unresolved
  signals, or fatal aborts. Only the acceptance harness's stale tab-switcher
  coordinate assertion remains to be updated.

### Runtime checkpoint 131 — 2026-09-08

- Acceptance logs show the stale sequence misses the tab switcher after scale
  conversion: `(225,610)` becomes `(450,1220)` (miss) and `(90,320)` becomes
  `(180,640)` (a `SuggestionsTileView` hit). No JIT/graphics fault occurred;
  the harness must target the top-toolbar tab-switcher coordinate next.

### Runtime checkpoint 132 — 2026-09-08

- A clean physical-coordinate sweep confirms the 720x1280 Chrome hierarchy has
  no `TabSwitcherButtonView`: toolbar hits resolve to `UrlBarApi26` or
  `avatar_button`. This is a Chrome tablet/window configuration issue, not a
  JIT or input dispatch fault.

### Runtime checkpoint 134 — 2026-09-08

- `run-android-apk-app.sh` now honors explicit `DARWIN_ART_WINDOW_SCALE` with
  default 2. A scale-1 Chrome probe confirms 360x640 physical coordinates and
  a real `BottomBarAppMenu` hit, removing the forced-scale mismatch.

### Runtime checkpoint 133 — 2026-09-08

- Physical-click probing confirms the runtime display contract is 720x1280 at
  320 dpi (360x640 dp), but Chromium chooses a URL-bar/avatar-only toolbar and
  does not instantiate `TabSwitcherButtonView`. Track this as a Chrome
  configuration/harness mismatch; sustained JIT execution remains fault-free.

### Runtime checkpoint 135 — 2026-09-08

- Host, bootstrap, and bionic facade regression suites pass after the display
  scale fix. Scale-1 Chrome receives true 360x640 physical coordinates and hits
  a real menu view; tab-switcher end-to-end acceptance remains pending on
  Chrome's selected window configuration.

### Runtime checkpoint 136 — 2026-09-08

- `tools/audit-art-jit.sh` exits successfully on the replacement machine,
  covering ARM64 intrinsics, compiled calls/JNI, concurrent moving GC and read
  barriers, field/array/type operations, exceptions, OSR, deoptimization,
  virtual dispatch, Android lifecycle/window, and launcher smoke with normal
  JIT enabled.

### Runtime checkpoint 137 — 2026-09-08

- Unmodified AOSP Calculator and DeskClock acceptance passes: physical input
  computes `2+3=5`, the Material tab reaches Timer (`00h 00m 00s`), and both
  apps publish visible HWUI/SurfaceFlinger/Metal buffers without crashes.

### Runtime checkpoint 138 — 2026-09-08

- The unmodified `SolitaireCG` game APK passes physical drag acceptance:
  `SolitaireView` receives and consumes MotionEvents over the input channel,
  and an 8-second native-free run finishes without crash or activity error.

### Runtime checkpoint 139 — 2026-09-08

- An unchanged VLC APK probe reaches native/JNI loading but fails when its
  rendering path calls the unimplemented Android
  `Surface.nativeLockCanvas(long, Canvas, Rect)`. This is now the concrete next
  framework-native compatibility gap; no ART JIT fault was seen.

### Runtime checkpoint 140 — 2026-09-08

- Confirmed the Surface JNI registration table lacks
  `nativeLockCanvas(long, Canvas, Rect)` while lifecycle and BLAST methods are
  present. VLC reaches this AOSP software-surface contract without any JIT
  fault; the native Canvas implementation is the next compatibility task.

### Runtime checkpoint 141 — 2026-09-08

- The AOSP Surface JNI boundary is specified: lock an `ANativeWindow` buffer,
  bind it to Java Canvas through the existing HWUI `ACanvas` bridge with dirty
  clip, then detach and post using `ANativeWindow_unlockAndPost`, retaining the
  IOSurface/Metal compositor. Implementation and regression tests remain
  pending.

### Runtime checkpoint 243 — 2026-09-08

- The `626-checker-arm64-scratch-register`–`641-iterations` slice passed all
  25 tests, covering ARM64 register/veneer paths, vdex/linking, casts and
  bounds, volatile access, inlining and caches, SIMD/code sinking, arraycopy,
  irreducible loops, and iteration handling. Remaining corpus and real-app
  validation remain pending.

### Runtime checkpoint 279 — 2026-09-08

- Rebuilt HWUI/graphics on the new machine and validated the Darwin capability
  boundary: wide-gamut FP16 is reported unsupported in AOSP EglManager for the
  RGBA_8888 Metal path. Snapseed still launches normally; only the separate
  optional 101010-2 format warning remains.

### Runtime checkpoint 262 — 2026-09-08

- Baseline compatibility is 1,075/1,075 AOSP tests, with Calculator and
  DeskClock real APK input/rendering validated through HWUI/SurfaceFlinger/Metal.
- Snapseed installation and arm64 ELF resolution pass, but unchanged managed
  loading still stops before JNI registration. Next architecture task is to
  trace Runtime.nativeLoad through the app ClassLoader and JavaVMExt, then
  validate the complete native lifecycle.

### Runtime checkpoint 259 — 2026-09-08

- After rebuilding the graphics runtime, unchanged AOSP Calculator and
  DeskClock APKs pass end-to-end: physical clicks compute `2+3=5`, Timer-tab
  navigation succeeds, and HWUI/SurfaceFlinger/Metal buffer publication is
  observed without a fatal crash. Real Blue Archive validation remains
  pending.

### Runtime checkpoint 258 — 2026-09-08

- Added the hidden `android.view.InputChannel` framework stub with paired
  ParcelFileDescriptor endpoints, Binder token identity, Parcelable round-trip,
  and disposal. It is included in support DEX/compiler inputs, and the final
  smoke test now passes interpreter/JIT/optimized lanes. The corpus ledger is
  1,075/1,075 passed; real-app validation remains pending.

### Runtime checkpoint 257 — 2026-09-08

- The lock-enabled rerun confirms the shared boot-artifact race is resolved:
  both previously affected tests pass again. The authoritative ledger is
  1,074/1,075 passed; only the custom InputChannel smoke test lacks its
  compile-time framework stub. Remaining corpus and real-app validation
  remain pending.

### Runtime checkpoint 255 — 2026-09-08

- Reconciliation exposed a shared bootstrap race: parallel workers could
  replace `unsafe-boot-dex` during boot-class-path resolution. The runner now
  uses a cross-process lock around the typed bootstrap default action while
  preserving parallel execution. Syntax and diff checks pass; remaining
  corpus and real-app validation remain pending.

### Runtime checkpoint 256 — 2026-09-08

- With the stale pre-lock sweep stopped, the race-sensitive
  `149-suspend-all-stress`–`156-register-dex-file-multi-loader` range was
  rerun using the locked runner and all 8 tests passed. The ledger now has
  1,074 passed of 1,075 discovered tests; only the custom InputChannel smoke
  test remains blocked by its missing compile stub. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 250 — 2026-09-08

- The `838-override`–`860-vdex-failure` slice passed all 24 tests, covering
  override/resolution, clinit/default interfaces, exceptions, data images,
  verification and multidex, arrays/records, branch/inlining, native/clone,
  access checks, Unsafe/VarHandle intrinsics, and vdex failure handling.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 253 — 2026-09-08

- The `952-invoke-custom`–`976-conflict-no-methods` slice passed all 25 tests,
  covering invoke-custom/polymorphic and MethodHandle paths, default-interface
  resolution/init/verification, conflict and IMT behavior, interface-super,
  multidex, private interfaces, and no-method conflicts. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 254 — 2026-09-08

- A full `--resume` corpus reconciliation is active because the ledger lacks
  terminal records for some earlier tests. It is re-executing the 030–075
  range instead of assuming prior evidence; the separate `9999` smoke test is
  blocked at compilation by the missing `android.view.InputChannel` stub.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 252 — 2026-09-08

- The `926-multi-obsolescence`–`951-threaded-obsolete` slice passed all 26
  tests, covering JVMTI JNI/search, retransformation and agents, BCP/classloader
  transforms, recursive obsolete/JIT, native/throw obsolete paths, reflection,
  annotations, in-memory transforms, intrinsic redefine, and threaded obsolete
  behavior. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 251 — 2026-09-08

- The `900-hello-plugin`–`925-threadgroups` slice passed all 26 tests,
  covering JVMTI agents and transforms, tagging, allocation/free, heap/GC
  lifecycle, class/method/stack inspection, obsolete JIT, field/object
  operations, properties/failure, monitors, threads, and thread groups.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 246 — 2026-09-08

- The `669-checker-break`–`692-vdex-inmem-loader` slice passed all 31 tests,
  covering throw/NPE, hidden API, hotness/vdex, proxy JIT, field resolution,
  quickening/locks, deoptimization and clinit, SIMD/select/shifts, shared
  libraries, multi-catch/zygote deopt, and in-memory vdex loading. Remaining
  corpus and real-app validation remain pending.

### Runtime checkpoint 247 — 2026-09-08

- The `692-vdex-secondary-loader`–`718-zipfile-finalizer` slice passed all 26
  tests, covering vdex loaders/eviction, clinit/loop/throw handling, string
  and select codegen, register/branch/FP/MAC paths, scheduling and cache
  churn, VarHandle and invoke-custom behavior, JLI samples, and finalizers.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 249 — 2026-09-08

- The `800-smali`–`837-deopt` slice passed all 40 tests, covering smali,
  MethodHandle/resolution, deoptimization, class hierarchy and invoke-super,
  FP/field/interface calls, verification/rethrow, vdex multidex, hidden API,
  CHA, locks/loops/LSE, unresolved access, background verification, and large
  class counts. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 248 — 2026-09-08

- The `719-varhandle-concurrency`–`736-interface-super-Object` slice passed
  all 19 tests, covering VarHandle/thread scheduling, OSR, invoke-super/IMT,
  array and class resolution, polymorphic/CHA deoptimization, bounds and
  app-image paths, ICCE/field validation, condition merging, and interface
  cloning. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 245 — 2026-09-08

- EOF progress record: the latest 44-test `642-fp-callees`–`668-aiobe` slice
  passed across the available interpreter/JIT/optimized lanes, extending
  ARM64 codegen, barriers, JNI, deoptimization, class loading, dex/vdex,
  SIMD, verifier, and bounds coverage. Remaining corpus and real-app
  validation remain pending.

### Runtime checkpoint 280 — 2026-09-08

- Rebuilt the HWUI/graphics path on the replacement Mac and recorded the
  Darwin RGBA_8888 capability boundary. Core-app graphics acceptance and
  Snapseed launch pass; the remaining 101010-2 warning originates in the
  separate android-graphics-jni route and is not yet eliminated.

### Runtime checkpoint 281 — 2026-09-08

- Re-linked the graphics runtime after the capability patch. A fresh Snapseed
  run now launches without the prior wide-gamut/101010-2 EGL diagnostics;
  AOSP Calculator and DeskClock graphics acceptance remains PASS.

### Runtime checkpoint 282 — 2026-09-08

- The complete ART JIT acceptance runner passed on the replacement Mac
  (`rc=0`), including GC, deoptimization, field/array, VarHandle, invoke
  polymorphic/custom, native-exit, and concurrency coverage. Real-app and
  remaining AOSP corpus work remain active.

### Runtime checkpoint 283 — 2026-09-08

- The authoritative corpus summary now contains 1,075 results and all 1,075
  are `passed`; no non-passed entries remain. InputChannel is no longer a
  corpus gap. Blue Archive and broader real-app validation remain active.

### Runtime checkpoint 284 — 2026-09-08

- The normal runner launched the unchanged Blue Archive APK for 10 seconds
  with exit code 0. Unity/IL2CPP initialized and reported ARM64, 12 cores,
  8192 MB memory, and version `1.93.454564`; sustained gameplay/input and
  service coverage remain active follow-up work.

### Runtime checkpoint 285 — 2026-09-08

- A 30-second unchanged Blue Archive installed-record soak exited 0 with
  Unity/IL2CPP alive on ARM64/12 cores. It revealed that legacy launch records
  can bypass existing-install `oat/arm64` cache migration, causing a non-fatal
  anonymous-vdex directory warning. Packaging lifecycle repair is the next
  task; application startup remained successful.

### Runtime checkpoint 286 — 2026-09-08

- Installed-record launches now invoke the APK installer's dedicated
  `--ensure-oat` migration helper, preserving sealed APK/native files while
  creating the writable `oat/arm64` leaf. Source formatting and diff checks
  pass; end-to-end rerun awaits recovery from stale host processes.

### Runtime checkpoint 287 — 2026-09-08

- After system-server recovery, the installed-record Blue Archive launch was
  re-tested for 10 seconds with exit code 0. `--ensure-oat` provisioned
  `oat/arm64/base.vdex` and the prior anonymous-vdex directory warning did not
  recur; the APK and native payload stayed sealed.

### Runtime checkpoint 288 — 2026-09-08

- Replacement-computer smoke check: repository is at `810e0d2`, release binaries
  are present, and `git diff --check` passes. The graphics acceptance runner
  starts but cannot create its log because only about 139 MiB is free; no code
  failure was observed.

### Runtime checkpoint 289 — 2026-09-08

- Environment audit found the checkout intact and `cargo metadata` passing.
  Re-running graphics acceptance is currently storage-blocked: `_build` is
  26G and the DarwinART profile store is 126G while the volume has only about
  136MiB free. No cleanup was performed without explicit authorization.

### Runtime checkpoint 290 — 2026-09-08

- The authoritative `_build/art-upstream-corpus/summary.json` was re-read:
  all 1,075 entries have exit code 0 and empty errors. The corpus ledger is
  green; real-app coverage and storage recovery remain open.

### Runtime checkpoint 291 — 2026-09-08

- Storage triage identified generated `_build/android16-ps16k-r07/system.img`
  as the only top-level file larger than 1G; `_build` totals 26G, `target`
  2.5G, and the DarwinART profile store 126G. It was not deleted because it is
  a runtime artifact and no cleanup authorization was provided.

### Runtime checkpoint 293 — 2026-09-08

- User-authorized stale-profile cleanup reclaimed about 3.5GiB while preserving
  the active `default` profile and runtime images. On the replacement computer,
  `aosp-core-apps-graphics-acceptance.sh` passed (Calculator `2+3=5`, DeskClock
  timer, HWUI/SurfaceFlinger/Metal), `audit-art-jit.sh` exited 0, and the
  installed Blue Archive record ran for 10 seconds with exit 0 and Unity/IL2CPP
  initialization (ARM64, 12 cores, 8GiB) and no bootstrap/vdex failure.

### Runtime checkpoint 295 — 2026-09-08

- Root-cause fix: `run-android-apk-app.sh` now prunes only `mnt/run/app.*`
  directories older than 24 hours, reopening their sealed permissions before
  removal. This prevents killed hosts from accumulating copied system roots;
  `bash -n` and `git diff --check` pass.

### Runtime checkpoint 296 — 2026-09-08

- Corrected storage interpretation: the earlier 123G profile figure double
  counted the 61G sparsebundle and its mounted view. After stale-root cleanup,
  the sparsebundle is 61G and `mnt/run` was reduced from 58G to 463M; Git
  objects are only about 122MiB total (5.81MiB packed).

### Runtime checkpoint 297 — 2026-09-08

- After stale-root cleanup, an attempted sparsebundle compaction left the
  `default` APFS image unmountable. `diskutil verifyVolume` reports corrupted
  fsroot and extent-ref trees (exit 8), and `diskutil repairVolume` could not
  complete deferred repairs. No reformat or further destructive write was
  attempted; source/Git remain intact while profile recovery is pending.

### Runtime checkpoint 298 — 2026-09-08

- A non-destructive read-only mount attempt also failed (`mount_apfs` exit 65/66)
  after `hdiutil attach -nomount`; the corrupt image was detached again. No
  profile recreation or overwrite has been performed.

### Runtime checkpoint 299 — 2026-09-08

- Root cause evidence: the sparsebundle compaction attempt failed with
  `hdiutil: ... 메모리를 할당할 수 없음`; subsequent APFS verification found
  zeroed fsroot/extent-ref blocks. A fresh `recovery` profile was created
  without touching the corrupt image; Calculator was installed and launched
  for 5 seconds with exit 0 and Nterp acceptance PASS.

### Runtime checkpoint 300 — 2026-09-08

- Recovery profile validation: `recovery` mounted with one lease; Calculator and
  Chromium were installed from immutable APK sources. Chromium launched for 8
  seconds with exit 0, created its initial tab, spawned sandbox/privileged
  services, and used the GPU IOSurface path. Only nonfatal cache/model warnings
  appeared.

### Runtime checkpoint 302 — 2026-09-08

- VLC follow-up after rebuilding framework-compat: the API-29
  `ConnectivityManager.requestNetwork` overloads (Handler/timeout/Executor)
  are now present. VLC's 8-second recovery-profile run exited 0, retained four
  native ELF libraries, registered LibVLC JNI classes, and no longer emitted
  the prior `NoSuchMethodError`.

### Runtime checkpoint 303 — 2026-09-08

- VLC recovery run after the API fix completed with exit 0 and native/JNI
  initialization. The previous `ConnectivityManager.requestNetwork` linkage
  error is absent; remaining output is limited to app-level/nonfatal startup
  diagnostics. This is an app-compatibility increment, not full VLC playback
  validation yet.

### Runtime checkpoint 304 — 2026-09-08

- VLC surface acceptance passed on `recovery`: software Canvas lock/post path
  produced 3 locks and 3 posts with no nativeLockCanvas or fatal-signal errors.
  This validates the media app's Android window buffer bridge; an actual media
  file playback assertion remains separate.

### Runtime checkpoint 305 — 2026-09-08

- Recovery profile inventory contains no existing media fixture. The VLC
  surface/canvas acceptance remains green, but an actual decode/playback
  assertion is intentionally not claimed until a media file is supplied or
  generated through the test harness.

### Runtime checkpoint 306 — 2026-09-08

- Generated a small H.264/AAC fixture and placed it in VLC's recovery-profile
  external directory. VLC launched with exit 0, but the headless launcher did
  not emit a playback/decoder event; only startup and surface assertions remain
  proven. The fixture is retained for a future physical-file-picker/intent
  test, and no APK was modified.

### Runtime checkpoint 301 — 2026-09-08

- Recovery profile app coverage expanded without APK changes: Snapseed
  (native `libsnapseed_native.so`) installed atomically and launched for 8
  seconds with exit 0; ELF namespace retention, PathClassLoader native path,
  JNI registration, and Activity transition all succeeded.

### Runtime checkpoint 294 — 2026-09-08

- Storage accounting after cleanup: active DarwinART `default` profile is 123G,
  repository `_build` is 26G, and Cargo `target` is 2.5G. These project assets
  explain roughly 151.5G of usage; APFS reports no snapshots, so the remaining
  volume usage belongs to other host data outside this checkout.

### Runtime checkpoint 292 — 2026-09-08

- Profile storage inspection shows `default` is the only profile modified on
  2026-09-08 (about 126G, including `mnt`); `productcompare*`, `bundle*`,
  `goaltest`, `producttest`, and `managerui` are dated 2026-08-29/30. These
  stale profiles are candidate cleanup targets, but remain untouched pending
  authorization.

### Runtime checkpoint 277 — 2026-09-08

- EGL capability filtering now declares FP16 pixel formats unsupported for the
  RGBA_8888 IOSurface backend. Snapseed remains functional; another extension
  source still advertises wide gamut and is the next tracing target.

### Runtime checkpoint 276 — 2026-09-08

- Existing sealed package installations now lazily provision `oat/arm64`
  without weakening their final permissions. Snapseed relaunch shows no VDEX
  directory error and still reaches EditActivity/JNI initialization.

### Runtime checkpoint 275 — 2026-09-08

- Post-install-cache regression passes formatting and the Calculator/DeskClock
  real-APK graphics/input acceptance after adding writable `oat/arm64`.

### Runtime checkpoint 274 — 2026-09-08

- APK installation now creates a writable `oat/arm64` code-cache directory
  without weakening APK/native read-only payloads. Fresh Snapseed execution
  writes `base.vdex` successfully and launches EditActivity with exit code 0.

### Runtime checkpoint 273 — 2026-09-08

- Post-ELF-lifecycle regression: corpus ledger is 1,075/1,075 passed, and
  Calculator/DeskClock real APK graphics/input acceptance remains green.

### Runtime checkpoint 272 — 2026-09-08

- SurfaceControl capture confirms Snapseed's default managed path publishes an
  app surface and launches EditActivity with 309 JNI registrations, exit 0,
  and no new diagnostic report.

### Runtime checkpoint 271 — 2026-09-08

- Desktop capture retained an old macOS crash-report dialog after the host was
  terminated; no new diagnostic report appeared, and a fresh 8-second
  Snapseed run exited 0 with edit Activity launch. This is stale UI state.

### Runtime checkpoint 270 — 2026-09-08

- A 45-second Snapseed run reached edit activity and logged no ART fatal
  signal before timeout. A stale macOS crash-report dialog appeared in the
  desktop capture from an earlier host process; visual stability remains an
  open verification item.

### Runtime checkpoint 269 — 2026-09-08

- A 20-second default Snapseed run exits 0 after JNI registration and
  edit-activity launch. No ART fatal signal or missing-JNI error appears;
  remaining messages are optional GMS/FeatureFlags and EGL/VDEX warnings.

### Runtime checkpoint 268 — 2026-09-08

- Repeated default Snapseed launch exits 0 after managed JavaVMExt/NativeBridge
  loading and JNI registration. Remaining diagnostics are non-fatal VDEX
  placement and EGL gamut probes.

### Runtime checkpoint 267 — 2026-09-08

- Snapseed now runs through the default managed native path (exit 0), reaches
  JavaVMExt/NativeBridge, registers 309 JNI methods, and launches its edit
  activity. Remaining warnings concern writable-vdex placement and EGL wide
  gamut probing, not app bootstrap failure.

### Runtime checkpoint 266 — 2026-09-08

- Rebuilt provider closure and graphics runtime now allow Snapseed's default
  managed `System.loadLibrary` path to reach JavaVMExt/NativeBridge. JNI
  registers 309 methods and the edit activity launches without overrides.

### Runtime checkpoint 265 — 2026-09-08

- Borrowed ELF registration now honors the Android 4 KiB guest page contract
  independently of the host's 16 KiB Mach VM pages. Snapseed eager
  JavaVMExt/NativeBridge loading reaches JNI initialization successfully.

### Runtime checkpoint 263 — 2026-09-08

- Current authoritative state: AOSP corpus 1,075/1,075; Calculator and
  DeskClock real APK graphics/input acceptance pass.
- Snapseed installs and resolves arm64 ELF but still fails before JNI
  registration in managed loading. The next architecture step is tracing
  `Runtime.nativeLoad` through ClassLoader, JavaVMExt, and NativeBridge.

### Runtime checkpoint 261 — 2026-09-08

- Snapseed's unchanged APK reaches JavaVMExt/NativeBridge and successful arm64
  ELF loading through `System.loadLibrary("snapseed_native")`, but its
  obfuscated `NativeCore.verifyLibraryHasBeenLoadedProperly()` method remains
  unbound. The next fix is JNI_OnLoad/dynamic RegisterNatives or symbol-table
  binding after load; APK extraction is not the failure. Blue Archive remains
  pending.

### Runtime checkpoint 260 — 2026-09-08

- AOSP Calculator/DeskClock remain green end-to-end. Snapseed's unchanged
  arm64 APK installs and its ELF native library resolves, but bootstrap fails
  registering obfuscated `NativeCore.verifyLibraryHasBeenLoadedProperly()`.
  Native-library/JNI registration is the next compatibility gap; Blue Archive
  validation remains pending.

### Runtime checkpoint 258 — 2026-09-08

- Added the hidden `android.view.InputChannel` framework stub with paired
  ParcelFileDescriptor endpoints, Binder token identity, Parcelable round-trip,
  and disposal. It is included in support DEX/compiler inputs, and the final
  endpoint/Parcel smoke passes interpreter/JIT/optimized lanes. The corpus
  ledger is 1,075/1,075 passed; real-app validation remains pending.

### Runtime checkpoint 257 — 2026-09-08

- The lock-enabled rerun confirms the shared boot-artifact race is resolved:
  both previously affected tests pass again. The authoritative ledger is
  1,074/1,075 passed; only the custom InputChannel smoke test lacks its
  compile-time framework stub. Remaining corpus and real-app validation
  remain pending.

### Runtime checkpoint 257 — 2026-09-08

- The lock-enabled rerun confirms the shared boot-artifact race is resolved:
  both previously affected tests pass again. The authoritative ledger is
  1,074/1,075 passed; only the custom InputChannel smoke test lacks its
  compile-time framework stub. Remaining corpus and real-app validation
  remain pending.

### Runtime checkpoint 244 — 2026-09-08

- The `642-fp-callees`–`668-aiobe` slice passed all 44 tests, covering FP/read
  barriers, SIMD and arraycopy, JNI IDs, inline caches/thunks, deoptimization,
  JIT clinit/loops, branches, array layouts and stores, classloader/oat paths,
  dex/verifier behavior, JNI stubs, and bounds/AIOOBE. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 237 — 2026-09-08

- The `538-checker-embed-constants`–`561-shared-slowpaths` slice passed all 37
  tests, covering constants/deopt, bitfield rotates, try/catch/DCE, access
  checks, tracing/JIT, type merges and MAC/wide stores, clinit/new-instance,
  invoke-super/null checks, primitive propagation/sharpening, bit manipulation,
  checkcast/UnsafeGetLong, switches/BCE/irreducible loops, divrem, and shared
  slow paths. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 217 — 2026-09-08

- The `055-enum-performance`–`073-mismatched-field` slice passed all 21 tests,
  including OOM/finalizers, encodings, process and classloader behavior,
  fields/types, NIO/DexFile mapping, precise GC, reachability fences, and
  intrinsics. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 142 — 2026-09-08

- Registered and linked AOSP-shaped `Surface.nativeLockCanvas` and
  `nativeUnlockCanvasAndPost` with ANativeWindow/ACanvas dirty-buffer handling.
  VLC resolves the methods and graphics audit passes, but real lock calls still
  return `IllegalArgumentException`; ownership, buffer dequeue, or Canvas bind
  diagnostics remain before completion.

### Runtime checkpoint 143 — 2026-09-08

- Replacement-Mac validation passes for host compilation/tests and the
  graphics-link closure. The VLC fixture APK must be reinstalled before the
  physical lock/unlock regression can run; diagnostics identified the previous
  `-EINVAL` as an unsupported logical format and added AOSP-shaped RGBA_8888
  normalization before `ANativeWindow_lock`.

### Runtime checkpoint 144 — 2026-09-08

- Replacement-Mac JIT memory and ARM64 acceptance audits pass across compiled
  calls/JNI, moving GC/read barriers, fields/arrays, exceptions, OSR/deopt,
  dispatch, and mixed root locations. Remaining sentinel-page and membarrier
  messages are Darwin host-boundary warnings observed during the audit.

### Runtime checkpoint 145 — 2026-09-08

- Re-enabled AOSP implicit null checks in the Darwin JIT instead of forcing an
  explicit-check-only path. Build-time audits cover HNullCheck, field,
  interface, and virtual receiver nullable decoding; AOSP implicit-null and
  null-call regressions pass in interpreter and optimized modes.

### Runtime checkpoint 153 — 2026-09-08

- `audit-art-jit.sh` now captures output and requires the real empty-checkpoint
  contention marker. Fresh execution passes with checkpoint latency 159 µs and
  lock wait 500,441 µs (RC=0), closing the stale-object/false-green gap.

### Runtime checkpoint 147 — 2026-09-08

- `cargo test --workspace` passes for all Rust crates and doc tests after the
  implicit-null-check JIT change. The remaining Darwin synchronization
  divergence under review is pthread polling for ART empty checkpoints versus
  AOSP futex wakeups.

### Runtime checkpoint 148 — 2026-09-08

- Runtime core rebuild succeeds with the Darwin pthread monitor and
  empty-checkpoint patches. Existing synchronized/reentrancy/GC/exception and
  contention acceptance cases remain green; a dedicated bounded-latency
  checkpoint regression is still pending.

### Runtime checkpoint 149 — 2026-09-08

- Compared the AOSP futex checkpoint wake contract with the Darwin pthread
  polling adaptation. Existing contention, GC, and exception-release tests
  remain green; a dedicated blocked-lock checkpoint-response measurement is
  still required before changing this host-specific synchronization layer.

### Runtime checkpoint 152 — 2026-09-08

- The linked ART Thread contention fixture confirms AOSP checkpoint semantics:
  during a 500 ms mutex hold, `RunEmptyCheckpoint()` completes in 176 µs and
  the blocked lock acquires at 500,738 µs. Full JIT audit, graphics incremental
  link, xtask, and bootstrap tests pass.

### Runtime checkpoint 151 — 2026-09-08

- Added fail-closed source checks for Darwin pthread checkpoint polling and
  tracked the transitive JIT checkpoint header in the native build graph.
  Bootstrap/xtask tests, runtime-core rebuild, and the full ART JIT audit pass;
  final runtime invocation of the dedicated blocked-lock fixture remains to be
  confirmed.

### Runtime checkpoint 150 — 2026-09-08

- Preserved the AOSP empty-checkpoint semantics while retaining the narrow
  Darwin pthread polling adaptation. Runtime-core compilation and existing
  contention/GC/exception coverage remain green; the blocked-lock response
  measurement is still the next required synchronization test.

### Runtime checkpoint 146 — 2026-09-08

- Rebuilt the pinned ARM64 JIT after removing the Darwin explicit-check-only
  setting and reran the full ART JIT audit. AOSP 551/479/034 implicit-null and
  null-call regressions pass in interpreter and optimized modes.

### Runtime checkpoint 154 — 2026-09-08

- Removed the Darwin-only compiled-JNI transition fallback that sent every
  ordinary native call through `pJniMethodStart`/`pJniMethodEnd`. Darwin now
  uses AOSP's inline ARM64 runnable/native CAS fast paths and enters those C++
  helpers only when thread flags require the normal slow path.
- A fail-closed compiler-shadow audit rejects reintroducing the Apple bypass.
  The full JIT audit, incremental graphics link, untouched AOSP `004-JniTest`,
  and all three `137-cfi` JIT/unwind runs pass with the inline path enabled.

### Runtime checkpoint 155 — 2026-09-08

- Replacement-host validation confirms the JNI fast path and empty-checkpoint
  contract remain green after push. The compatibility objective remains open;
  the next work must address remaining AOSP semantic/ABI differences rather
  than treating the current probe suite as full completion.

### Runtime checkpoint 156 — 2026-09-08

- The replacement host rebuilt and validated the ARM64ng Nterp Mach-O
  artifact (256 handlers, symbols, CFI, and DWARF checks). Nterp remains on
  the normal AOSP admission path; this is artifact verification, not a
  completion claim for remaining runtime parity work.

### Runtime checkpoint 157 — 2026-09-08

- Added an executable Nterp admission gate to the runtime acceptance path.
  After ARM64's normal visibly-initialized publication flush, a cold app
  method must point at `ExecuteNterpImpl`, execute there, return 42, and retain
  that entrypoint; the full JIT audit now fails closed if this marker is absent.
- Removed the two obsolete, unreferenced Darwin patches that disabled Nterp
  and its catch entry. The full JIT audit and unchanged AOSP `837-deopt`
  interpreter/JIT/optimized lanes pass; this proves native-interpreter
  execution rather than only a linked 256-handler artifact.

### Runtime checkpoint 158 — 2026-09-08

- Strict post-push Nterp validation passed on the current host: startup and
  AOSP admission selected `ExecuteNterpImpl`, the cold method executed there,
  and returned `42`; full JIT and `837-deopt` regression lanes remain green.
  This is evidence for the Nterp slice only, not completion of app parity.

### Runtime checkpoint 159 — 2026-09-08

- Full Rust workspace tests (unit and doc tests) pass on the replacement host
  after Nterp enablement. The broader AOSP feature matrix and real-app gates
  remain open and are not waived by this regression run.

### Runtime checkpoint 160 — 2026-09-08

- Replaced synthetic Surface identities with managed producer pointers and
  verified Android's public software Canvas lifecycle end to end. The
  lock/bind/dirty-clip/post/release focused acceptance passes on the current
  host; full VLC and application compatibility remain future work.

### Runtime checkpoint 161 — 2026-09-08

- The default runtime audit now verifies Surface producer identity, RGBA
  format, Canvas dimensions, and dirty-clip normalization in addition to the
  complete lock/post lifecycle. The strict replacement-host run passes with
  Nterp and synchronization markers; broader application parity remains open.

### Runtime checkpoint 162 — 2026-09-08

- Repeated the default audit after the Surface contract hardening: producer,
  Canvas, dirty-clip, Nterp, and empty-checkpoint markers all pass on the
  current host. This confirms regression stability but does not close the
  remaining real-application/AOSP matrix.

### Runtime checkpoint 163 — 2026-09-08

- The default audit now includes MediaCodec output-surface producer lifetime:
  configure, release, `setOutputSurface`, and final codec release all pass on
  managed producers. The focused replacement-host run is green; full VLC and
  application media playback remain open.

### Runtime checkpoint 164 — 2026-09-08

- MediaCodec output testing was extended to decoded-frame posting and retained
  producer snapshots. The strict run reaches VP9 configure but stalls before
  completion, so the configure/fromSurface boundary is an active unresolved
  issue and the implementation is not yet accepted.

### Runtime checkpoint 165 — 2026-09-08

- MediaCodec producer publication follows an AOSP-like ownership boundary:
  retain the output producer while holding codec state, release the codec lock
  before ANativeWindow lock/post, and publish queued decoder frames only after
  releaseOutputBuffer unlocks. Combined strict audit is green (Surface,
  configure/setOutputSurface/release, Nterp, synchronization; RC=0).

### Runtime checkpoint 166 — 2026-09-08

- Fresh rebuild and strict audit against the current tree pass the Surface
  Canvas contract, MediaCodec producer configure/rebind/release lifetime,
  native Nterp, and empty-checkpoint contention (RC=0;
  `/tmp/audit-fresh-final.log`). Decoded-frame pixel posting remains a later
  compatibility gate.

### Runtime checkpoint 167 — 2026-09-08

- Fresh four-worker corpus rerun of `061-out-of-memory` through `100-reflect2`
  passes all 42 contracts across interpreter, JIT, and unchanged optimized
  lanes. This confirms the allocation/GC and core reflection fixes on the
  current runtime rather than relying on the stale full-audit ledger.

### Runtime checkpoint 168 — 2026-09-08

- The next 40 AOSP contracts (`101-fibonacci` through
  `180-native-default-method`) pass all interpreter, JIT, and unchanged
  optimized lanes on a fresh four-worker run, covering GC/JNI, class loading,
  app-image, and default-method behavior.

### Runtime checkpoint 169 — 2026-09-08

- The fresh corpus slice `181-default-methods` through
  `203-multi-checkpoint` passes all seven executable contracts across
  interpreter, JIT, and unchanged optimized lanes, covering method linking,
  RMW stress, exception/OOME behavior, and checkpoint coordination.

### Runtime checkpoint 170 — 2026-09-08

- `300-package-override` passes all three execution lanes, validating package
  override and application class-loader behavior without source changes.

### Runtime checkpoint 171 — 2026-09-08

- The fresh corpus slice spanning executable tests `301`–`500` passes all 54
  contracts in interpreter, JIT, and unchanged optimized lanes, including
  optimizing compiler control flow, allocation/register handling, exceptions,
  monitors, inlining, and deoptimization.

### Runtime checkpoint 172 — 2026-09-08

- The fresh corpus slice spanning executable tests `501`–`700` passes all 40
  contracts in interpreter, JIT, and unchanged optimized lanes, including
  checker optimization, deopt/OSR, inline-cache, volatile/read-barrier,
  class-loader, and JNI-stub behavior.

### Runtime checkpoint 173 — 2026-09-08

- Fresh four-worker runs pass all 48 executable contracts across the `701`–`736`
  and `800`–`860` ranges in interpreter, JIT, and unchanged optimized lanes.
  This validates VarHandle/concurrency, cache churn, OSR/deoptimization,
  hidden API/VDEX, resolution, and plugin/JVMTI behavior.

### Runtime checkpoint 174 — 2026-09-08

- Stale parallel corpus runners were terminated to restore deterministic test
  isolation. Direct fresh execution of `904-object-allocation` passes all
  interpreter, JIT, and unchanged optimized lanes.

### Runtime checkpoint 175 — 2026-09-08

- The fresh `1000`–`1004` corpus slice passes all five contracts across
  interpreter, JIT, and unchanged optimized lanes, covering non-moving GC,
  app-image regions, startup notification, metadata sections, and volatile
  reference-load lowering.

### Runtime checkpoint 176 — 2026-09-08

- The fresh `1336`–`1339` GC/reference slice passes all four contracts across
  interpreter, JIT, and unchanged optimized lanes, validating finalizer
  timing, coverage/no-LOS collection, and dead-reference handling.

### Runtime checkpoint 177 — 2026-09-08

- The fresh `1900`–`1919` JVMTI slice passes all eight executable contracts in
  interpreter, JIT, and unchanged optimized lanes, covering allocation
  tracking, bytecode/local-variable access, suspend/resume, and thread-start
  timing.

### Runtime checkpoint 178 — 2026-09-08

- The fresh corpus slices `2000`–`2048` (13 contracts) and `2230`–`2286`
  (14 contracts) pass all execution lanes, validating structural
  redefinition/stack scope, inlining and loop optimization, reference
  processing, checker lowering, and native registry behavior.

### Runtime checkpoint 179 — 2026-09-08

- `004-JniTest` now passes all three execution lanes in a direct fresh run,
  validating the compiled JNI transition and native exit path independently of
  the earlier stale corpus ledger.

### Runtime checkpoint 180 — 2026-09-08

- The fresh `1920`–`1960` (6 contracts) and `1961`–`1999` (8 contracts) slices
  pass all execution lanes, validating monitor/event JVMTI, frame-pop and
  breakpoint/redefinition, checker bounds/vectorization, and structural
  transformation paths.

### Runtime checkpoint 181 — 2026-09-08

- Generalized native-owner discovery to link every sibling libarttest source
  contributing a declared JNI entry point. This resolves the missing
  `GetMethodId` symbol in `2262-default-conflict-methods`; all interpreter,
  JIT, and unchanged optimized lanes now pass without a test-name exception.

### Runtime checkpoint 182 — 2026-09-08

- The post-fix `2230`–`2286` corpus rerun passes all 12 discovered contracts in
  every execution lane, including `2262-default-conflict-methods`, confirming
  that multi-member sibling JNI linking is regression-free.

### Runtime checkpoint 183 — 2026-09-08

- The fresh `2001`–`2007` structural-redefinition slice passes all seven
  contracts across interpreter, JIT, and unchanged optimized lanes, validating
  multithreaded dispatch, initialization/finalization, and pause-all behavior.

### Runtime checkpoint 184 — 2026-09-08

- The fresh slices `2008`–`2012` (5 contracts) and `2019`–`2039` (22
  contracts) pass all execution lanes, validating structural local-ref/stack
  walk/JNI-ID behavior, inlining/loops, monitor/shutdown, and native
  allocation/transform handling.

### Runtime checkpoint 185 — 2026-09-08

- The fresh `2040`–`2048` slice passes all eight execution contracts across
  interpreter, JIT, and unchanged optimized lanes, validating large native
  allocation, cleaner/reference processing, stack traces, userfaultfd,
  checker lowering, and native registry behavior.

### Runtime checkpoint 186 — 2026-09-08

- The fresh `114`–`175` execution slice passed every contract apart from one
  known-flaky `149-suspend-all-stress` attempt; an isolated rerun then passed
  all three lanes. This validates GC/class loading, native bridge, unloading,
  multi-loader registration, lock ownership, and allocation stress behavior.

### Runtime checkpoint 187 — 2026-09-08

- The fresh `1948`–`2039` slice passes all 90 contracts in interpreter, JIT,
  and unchanged optimized lanes. Structural redefinition/obsolescence, JVMTI
  transforms, inlining/loops, monitor and deoptimization, hidden API, and JNI
  file-channel behavior are all validated without reproducing stale failures.

### Runtime checkpoint 188 — 2026-09-08

- Isolated reruns of `542-unresolved-access-check`, `936-search-onload`, and
  `938-load-transform-bcp` pass all three execution lanes, validating
  access-check resolution, class-loader on-load search, and boot-classpath
  transformation without a persistent failure.

### Runtime checkpoint 189 — 2026-09-08

- The fresh `061`–`103` slice passes all 51 execution contracts across
  interpreter, JIT, and unchanged optimized lanes, covering OOM/GC, fields and
  arrays, class loading, verifier/monitor behavior, loops, reflection, and
  string concatenation.

### Runtime checkpoint 190 — 2026-09-08

- The fresh `176`–`183` slice passes all eight execution contracts across
  interpreter, JIT, and unchanged optimized lanes, covering app-image
  strings/native methods, nonvirtual JNI, default-method linking, deadlock
  handling, and read-modify-write stress.

### Runtime checkpoint 191 — 2026-09-08

- The fresh `1900`–`1947` slice passes all 46 execution contracts across
  interpreter, JIT, and unchanged optimized lanes, validating suspend/resume
  and raw monitors, frame-pop/exception events, JVMTI transforms, proxy
  frames, and breakpoint/deoptimization behavior.

### Runtime checkpoint 192 — 2026-09-08

- The fresh `2230`–`2286` slice passes all 85 execution contracts across
  interpreter, JIT, and unchanged optimized lanes, validating checker
  lowering, VarHandle/Unsafe, write-barrier elimination, exception/inlining,
  method handles, JVMTI, and sibling-JNI owner linking. The transient
  `2262-default-conflict-methods` failure is PASS in the completed ledger.

### Runtime checkpoint 193 — 2026-09-08

- The fresh `300-package-override` through `500-instanceof` slice passes 128
  of 129 contracts in every execution lane. `497-inlining-and-class-loader`
  exposes a real class-loader gap: `DexFile.loadClassBinaryName` returns null
  for `LoadedByMyClassLoader`, leading to the `getDeclaredMethod` NPE. This is
  retained as an implementation task rather than hidden by a test exception.

### Runtime checkpoint 194 — 2026-09-08

- Investigated `497-inlining-and-class-loader` with DEX identity tracing. The
  app dex is already registered to the process PathClassLoader; the test then
  passes that same `DexFile` to a child loader, and `RegisterDexFile` rejects
  the second identity, so `loadClassBinaryName` returns null. A temporary
  DexCache-reuse experiment did not pass the subsequent `DefineClass`
  registration and was reverted; the class-loader gap remains explicit.

### Runtime checkpoint 195 — 2026-09-08

- Rebuilt the graphics runtime with a temporary generic DexCache-reuse path
  and reran `497-inlining-and-class-loader`. `DefineClass` still performs a
  second registration and rejects the shared DexFile identity, so the class
  remains unresolved. The experiment was reverted; the required fix is now
  narrowed to per-(DexFile, ClassLoader) cache identity, with no test-specific
  exception added.

### Runtime checkpoint 196 — 2026-09-08

- A second generic experiment returning the existing DexCache from
  `RegisterDexFile` for a different loader still failed `497` and was
  reverted. The remaining fix is a genuine per-loader DexFile/cache clone
  with independent registration and lifetime; global registration relaxation
  and test-specific gates remain absent.

### Runtime checkpoint 197 — 2026-09-08

- A source audit confirms `ClassLinker::dex_caches_` is keyed solely by
  `DexFile*`, forcing both native `loadClassBinaryName` and `DefineClass` back
  through single-loader registration. The correct next step is a loader-aware
  DexFile clone/cache with explicit ownership and GC cleanup; relaxing global
  registration is insufficient and was not retained.

### Runtime checkpoint 198 — 2026-09-08

- A zero-copy child-loader `DexFile` clone prototype was attempted in
  `DexFile_defineClassNative`. It patched the full runtime tree but failed the
  graphics bootstrap because one reduced source variant has no
  `dalvik_system_DexFile.cc`, causing `No file to patch`; the prototype was
  removed. The next implementation must be variant-aware and preserve AOSP
  loader ownership/registration semantics.

### Runtime checkpoint 199 — 2026-09-08

- The fresh four-worker `501`–`550` AOSP slice passes all 79 discovered tests,
  including deoptimization, monitor/exception paths, array/field lowering,
  inlining, tracing, and JIT regressions. The isolated `497` multi-loader
  identity case remains the next structural implementation item.

### Runtime checkpoint 200 — 2026-09-08

- Shared patched-source concurrency was investigated. A lock prototype was
  reverted after revealing that incomplete-shadow recovery also needs generated
  headers such as `quick_entrypoints.h`; no runtime semantics were changed.
  The next loader implementation must combine atomic shadow publication with
  complete generated-header staging.

### Runtime checkpoint 201 — 2026-09-08

- The shared shadow manifest now includes upstream
  `entrypoints/quick/quick_entrypoints.h`. Clean regeneration confirms the
  generated-header dependency is materialized and the graphics bootstrap
  reuses all 256 runtime objects successfully; no runtime behavior was
  relaxed.

### Runtime checkpoint 202 — 2026-09-08

- The loader clone retry confirmed the AOSP API shape: construct
  `ArtDexFileLoader` over mapped bytes and call `Open` with its container
  argument. The experimental diff was removed after validation remained
  unsafe; clean graphics bootstrap was restored and `497` remains the gap.

### Runtime checkpoint 203 — 2026-09-08

- Canonical diff generation confirmed the mapped-byte `ArtDexFileLoader`
  instance API for the child-loader clone. It remains unmerged pending safe
  patch publication; no partial runtime semantics are retained.

### Runtime checkpoint 204 — 2026-09-08

- The corrected loader clone retry still exposed shared-shadow multi-owner
  patch publication races; it was removed without runtime changes. Clean
  graphics bootstrap passes, leaving atomic source publication and the
  loader-aware DexCache implementation as the next structural work.

### Runtime checkpoint 205 — 2026-09-08

- Shared patched-source publication now uses an atomic directory lock with a
  post-lock completeness check. A concurrent two-process clean graphics
  bootstrap test passed in both processes, making patch application
  deterministic for the upcoming loader-aware runtime change.

### Runtime checkpoint 206 — 2026-09-08

- Clean-shadow concurrency was revalidated with two simultaneous graphics
  bootstrap owners; both exited successfully with generated headers intact.
  The build publication layer is ready for the next loader-aware DexCache
  implementation, while `497` remains unresolved.

### Runtime checkpoint 207 — 2026-09-08

- The mapped-byte clone prototype compiled with serialized shadow publication,
  but `497` still produced the same NPE. The unverified global clone lifetime
  path was removed; clean graphics bootstrap passes. The remaining gap is the
  ClassLinker class-definition/cache association.

### Runtime checkpoint 208 — 2026-09-08

- Worktree audit confirms no clone patch is tracked; the latest `497` NPE is
  baseline behavior, not evidence against the clone. The next run must retain
  the canonical patch through build and identity-traced execution.

### Runtime checkpoint 209 — 2026-09-08

- The canonical clone retry still failed patch parsing on a secondary hunk and
  was removed without runtime changes. Clean graphics bootstrap remains PASS;
  the next attempt should publish a mechanically generated staged-source diff.

### Runtime checkpoint 210 — 2026-09-08

- The mechanically generated clone patch applies and links in the graphics
  runtime. Identity tracing confirms a distinct child-loader DexFile is
  registered, and `501-null-constant-dce` passes interpreter/JIT/optimized
  lanes. `497` still fails after registration; class-definition association is
  the remaining gap.

### Runtime checkpoint 211 — 2026-09-08

- Fresh-machine verification reproduces successful distinct child-loader
  DexFile registration but the same `497` null result after `DefineClass`.
  Investigation remains focused on post-registration class association;
  temporary diagnostic logging was discarded.

### Runtime checkpoint 213 — 2026-09-08

- Root cause fixed: clone registration previously succeeded but
  `DexFile_defineClassNative` still supplied the original DexFile to
  `ClassLinker::DefineClass`. Passing `defining_dex` fixes the association;
  fresh relink and all three `497` lanes now pass.

### Runtime checkpoint 214 — 2026-09-08

- The first ten deterministic AOSP corpus inputs after the class-loader fix
  all passed (`000-nop` through `004-SignalTest`) using the normal runner.
  Full corpus coverage and real application validation are still pending.

### Runtime checkpoint 215 — 2026-09-08

- The next deterministic corpus slice through `024-illegal-access` passed in
  full (25 tests), including exception, array/type, arithmetic/floating-point,
  string, interface, unsafe, stack-overflow, and thread-stress behavior. Full
  corpus and real application validation remain pending.

### Runtime checkpoint 216 — 2026-09-08

- The `025-access-controller`–`054-uncaught` corpus slice passed completely
  (30 tests), covering access control, class initialization, inheritance,
  reflection/proxy, synchronization, thread lifecycle, verifier, finalizers,
  and exception propagation. Remaining corpus and real-app validation remain
  pending.

### Runtime checkpoint 212 — 2026-09-08

- A one-shot callback trace showed `LLoadedByMyClassLoader` never reaches
  `ClassPreDefine`; original registration fails, clone registration succeeds,
  then Java receives status 122. The remaining path is an earlier
  `DefineClass` precondition/exception failure, not callback substitution.
  Temporary tracing was removed.

### Runtime checkpoint 218 — 2026-09-08

- The `074-gc-thrash`–`096-array-copy-concurrent-gc` slice passed all 24 tests,
  including verification/type behavior, polymorphic calls, phantom refs,
  OOM/finalizers, hot exceptions, inlining/compiler regressions,
  class-init/monitors, loop formation, serialization, and concurrent-GC array
  copies. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 219 — 2026-09-08

- A corpus run was interrupted by filesystem exhaustion from 1,834 generated
  temporary test directories. After deleting only those temp directories,
  `1001-app-image-regions` independently passed all three lanes. Completed
  neighbors `097-duplicate-method`, `100-reflect2`, and
  `1000-non-moving-space-stress` also passed; the remaining range is queued.

### Runtime checkpoint 222 — 2026-09-08

- Generated unsafe boot support is now published as a JAR container and all
  runner/audit references use it. `149-suspend-all-stress` passes all lanes
  after regeneration and `497` remains green. `156-register-dex-file-multi-loader`
  still reveals an AOSP semantic mismatch (clone allowed where InternalError
  is expected); the next fix belongs in ClassLinker policy, not the harness.

### Runtime checkpoint 220 — 2026-09-08

- Corpus execution resumed at `1002-notify-startup` and completed through
  `125-gc-and-classloading` with all 28 tests passing. This covers startup and
  metadata, volatile references, concurrent/parallel GC, invoke/exception and
  suspend checks, multidex/native bridge, dex2oat/no-image, compiler MT and
  inline execution, missing classes, and class loading. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 225 — 2026-09-08

- The `1917-get-stack-frame`–`1948-obsolete-const-method-handle` slice passed
  all 32 tests, covering JVMTI frames/locals, suspend and native monitors,
  monitor/exception events, signal-thread and JIT frame handling, transforms,
  proxy/DDMS/dispose paths, raw-monitor suspension, descriptors, breakpoint
  redefine/deoptimization, and obsolete method handles. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 221 — 2026-09-08

- The `126-miranda-multidex`–`145-alloc-tracking-stress` slice passed all 25
  tests, covering secondary dex, register spills, thread/daemon/JNI shutdown,
  hprof/CFI, invoke-super, GC coverage and reference safety, duplicate classes,
  native registration, DCE/field packing, unloading/classloaders, static-field
  SIGQUIT, and allocation tracking stress. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 223 — 2026-09-08

- Child-loader semantics now match AOSP: an exact parent `LookupClass` keeps
  the multiple-loader `InternalError`, and only new child classes take the
  zero-copy clone path. Fresh relink verifies both
  `156-register-dex-file-multi-loader` and `497-inlining-and-class-loader` in
  interpreter/JIT/optimized lanes. Unsafe boot DEX is exposed in a JAR
  container for correct bootclasspath handling.

### Runtime checkpoint 224 — 2026-09-08

- The `182-method-linking`–`1916-get-set-current-frame` slice passed all 19
  tests, including method/RMW stress, allocation tracking, bytecode access,
  suspend and native-resume variants, suspend-list ordering, agent TLS, JVMTI
  transforms, and local variable/object/frame inspection. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 226 — 2026-09-08

- The `1949-short-dex-file`–`1965-get-set-local-primitive-no-tables` slice
  passed all 18 tests, covering transforms/monitor suspension, pop-frame/JIT
  frames, error and retry paths, bounds/loop compiler checks, obsolete
  multithread JIT, event delivery, dex-classloader insertion, and primitive
  frame access without tables. Remaining corpus and real-app validation remain
  pending.

### Runtime checkpoint 227 — 2026-09-08

- The `1966-get-set-local-objects-no-table`–`1998-structural-shadow-field`
  slice passed all 33 tests, including local/object slots, force returns, JNI
  ID swaps, array resizing, structural/obsolete redefinition across threads,
  monitor and verification failures, retransformation, and final/virtual
  shadow method/field resolution. Remaining corpus and real-app validation
  remain pending.

### Runtime checkpoint 236 — 2026-09-08

- The `515-dce-dominator`–`537-checker-jump-over-jump` slice passed all 45
  tests, covering DCE/dominators, builder/null-array/bound loads, phis,
  array/field and monitor/throw regressions, boolean simplification,
  register allocation and SIMD/split arrays, loop/LSE/peel-unroll variants,
  reference typing, debug phi, BCE deoptimization, deopt/inlining,
  intrinsic/access checks, arraycopy, debuggable/unverified inline paths, and
  jump elimination. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 235 — 2026-09-08

- The `478-checker-inline-noreturn`–`514-shifts` slice passed all 38 tests,
  covering inlining/calls/interfaces, current-method paths, instanceof and
  checkcast, class-loader inlining, type propagation/BCE/phi, switches and
  dead instructions, baseline/verifier/referrer behavior, checker disassembly,
  pre-header/try-catch, interface clinit, array deopt, shifts, null-checks,
  register hints, loop DCE, and implicit-null-check regressions. Remaining
  corpus and real-app validation remain pending.

### Runtime checkpoint 234 — 2026-09-08

- The `450-checker-types`–`478-checker-clinit-check-pruning` slice passed all
  40 tests, covering vreg/reg allocation, type propagation/SSA/GVN, array and
  instruction simplification, long/float conversion, dead phis, dex/nested
  inlining, boolean/condition materialization, huge methods, deopt and locals,
  constructor barriers, clinit inlining, bound types, and dead/unreachable
  block pruning. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 231 — 2026-09-08

- The `2250-inline-throw-into-try`–`2275-pthread-name` slice passed all 31
  discovered tests, covering try/throw and irreducible loops, devirtualization,
  checker branch/vector/constant folding, RTI/code sinking, intrinsics,
  cleaner/reference paths, method tracing/profile caches, write-barrier and
  bitwise optimizations, empty/unsigned loops, method handles/hidden API,
  class self-implementation, nested loops, and pthread naming. Remaining
  corpus and real-app validation remain pending.

### Runtime checkpoint 228 — 2026-09-08

- The `1999-virtual-structural`–`2029-contended-monitors` slice passed all 26
  tests, covering virtual structural dispatch/redefinition, multithread
  pause-all, reflective/structural locals, exception details, concurrent stack
  walks, constant sinking, thread OOME, invoke inlining, invariant loops,
  memory-couple optimizations, backward loops, and contended monitors.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 241 — 2026-09-08

- The `577-checker-fp2int`–`599-checker-irreducible-loop` slice passed all 39
  tests, covering FP conversion, BCE and inlining, CRC32/RTP, dispatch and
  loop handling, primitive/alias analysis, profiles and app images, class
  loaders, monitor inflation, and deoptimization. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 242 — 2026-09-08

- The `600-verifier-fails`–`625-checker-licm-regressions` slice passed all 35
  tests, covering verifier/access failures, deoptimization, class/string
  handling, daemon stress, interface and bounds inlining, dex caches, CHA
  dispatch/unloading, clinit OOME, induction/BCE/loop regressions, string
  operations, and LICM. Remaining corpus and real-app validation remain
  pending.

### Runtime checkpoint 232 — 2026-09-08

- The `2276-const-method-type-gc-cleanup`–`414-static-fields` range passed all
  38 tests, covering method-handle GC/invokeexact, loop/class-init/checker
  regressions, static-field tracing, access/float conversion, verification,
  dex v37, and optimizing compiler control-flow, long/float arithmetic,
  allocators, fields/arrays, div/rem simplification, new-array, regalloc, and
  static fields. Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 229 — 2026-09-08

- The `2030-long-running-child`–`2048-bad-native-registry` slice passed all 21
  tests, including child/deopt frames, default/private methods, shutdown,
  native/JNI file channels, hidden API, large transforms/allocations,
  cleaner/reference processing, stack traces, UFFD, checker comparisons and
  string lengths, and native registry errors. Remaining corpus and real-app
  validation remain pending.

### Runtime checkpoint 230 — 2026-09-08

- The `2230-profile-save-hotness`–`2249-checker-return-try-boundary-exit-in-loop`
  slice passed all 40 discovered tests, covering profile/metrics, heap
  poisoning, suspend-check removal, JdkUnsafe, recursive inlining, the full
  VarHandle matrix, tracing/single-step, LSE operations, checker transforms,
  write-barrier elimination, and smali boundary checks. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 240 — 2026-09-08

- The `562-bce-preheader`–`576-polymorphic-inlining` slice passed all 25
  tests, covering BCE, loop/irreducible-loop analysis, bitwise and liveness
  simplification, select/intrinsic codegen, OSR, array/checkcast regressions,
  string aliasing, and polymorphic inlining. Remaining corpus and real-app
  validation remain pending.

### Runtime checkpoint 233 — 2026-09-08

- The `416-optimizing-arith-not`–`449-checker-bce-rem` slice passed all 38
  tests, covering optimizing arithmetic/constants, large frames, type/call/
  monitor/bitwise/bounds paths, SSA/register slow paths, type propagation,
  invoke/shifter operands, allocation/finally, volatile/NPE, checker
  inlining/folding/NCE/LICM, multiple returns, and BCE. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 238 — 2026-09-08

- The `498-type-propagation`–`529-long-split` slice passed all 38 tests,
  covering type propagation/BCE, instanceof and verifier paths, DCE and
  switch handling, try/catch and clinit, array deopt/access, class loading,
  monitor/throw checks, SIMD and register allocation, loop/LSE setup,
  unresolved calls, and long-register splitting. Remaining corpus and
  real-app validation remain pending.

### Runtime checkpoint 239 — 2026-09-08

- The `530-checker-instance-of-simplifier`–`537-checker-jump-over-jump` slice
  passed all 28 tests, covering loop/LSE optimization, SIMD and unrolling,
  instanceof/checkcast, debug/deoptimization, array stores/copy, intrinsic
  and constant folding, access checks, and inline/unverified control flow.
  Remaining corpus and real-app validation remain pending.

### Runtime checkpoint 245 — 2026-09-08

- EOF progress record: the latest 44-test `642-fp-callees`–`668-aiobe` slice
  passed across the available interpreter/JIT/optimized lanes, extending
  ARM64 codegen, barriers, JNI, deoptimization, class loading, dex/vdex,
  SIMD, verifier, and bounds coverage. Remaining corpus and real-app
  validation remain pending.
### Runtime checkpoint 307 — 2026-09-08

- Activity/window ownership now follows the AOSP transaction boundary for
  secondary Activities: resume publishes the ViewRoot/renderer before the
  Darwin native surface bridge observes it. This removes the VLC video
  Activity startup abort while preserving the existing persistent GPU path.
### Runtime checkpoint 308 — 2026-09-08

- Scoped-storage copy now retains Android's package-scoped path, allowing
  native VLC to open the requested URI. The subsequent MediaCodec crash is
  narrowed to an ART generated-code null target and is queued as the next JIT
  ABI investigation.
### Runtime checkpoint 309 — 2026-09-08

- VLC now reaches MediaCodecList through the real scoped-storage and native
  path. A null quick entry remains immediately after capability enumeration,
  including with JIT disabled, narrowing the next fix to the shared ART
  managed-call entry/resolution ABI.
### Runtime checkpoint 310 — 2026-09-08

- Rejected signal-handler stack dereferencing for fault diagnosis because it
  would violate the runtime's async-signal safety boundary. The null target
  remains fatal until an owner-thread-safe ART frame hook identifies it.
### Runtime checkpoint 311 — 2026-09-08

- Scoped native filesystem ownership now includes `posix_fallocate`, matching
  the Android libc contract required by VLC/MediaLibrary. Resolver closure and
  facade tests pass; the remaining crash is unchanged and remains in the ART
  managed-call entry path.
### Runtime checkpoint 312 — 2026-09-08

- ELF disassembly maps the VLC crash LR to `vlc_stream_MemoryNew`; the null
  target is a vtable slot inside libvlc, not a direct ART call PC. Native
  relocation/constructor completeness for the VLC dependency graph is now the
  active blocker, while ART signal handling remains unchanged.
### Runtime checkpoint 313 — 2026-09-08

- Native crash ownership is now concrete: `libvlc.so` executes a null vtable
  indirect call in `vlc_stream_MemoryNew`. This is a dependency relocation or
  initialization boundary after MediaCodec enumeration, not a graphics or
  Activity lifecycle failure.
### Runtime checkpoint 314 — 2026-09-08

- Native dependency comparison confirms the VLC graph's declared Android
  platform dependencies are routed through the sealed provider namespace.
  The remaining null vtable call is inside libvlc's own stream object and is
  not attributable to an omitted dependency fallback.

### Runtime checkpoint 315 — 2026-09-08

- Profile corruption was an APFS lifecycle error, not an Android runtime or
  source-control error: compaction of the mounted live sparsebundle raced
  filesystem writes and left extent-ref/fsroot metadata inconsistent. The
  unaffected `recovery` profile confirms APK contents and runtime code were
  intact. Profile maintenance must quiesce the daemon, unmount before
  compaction, and verify before remounting.

### Runtime checkpoint 316 — 2026-09-08

- The application PackageManager now models the AOSP-resident `android`
  package explicitly. Framework identity resolves to the actual locked
  `framework-res.apk`, carries system-package flags, and remains certificate
  neutral. This prevents app startup code from mistaking the detached host's
  registry boundary for a missing Android framework package.

### Runtime checkpoint 317 — 2026-09-08

- Application DEX regeneration and verification pass after exposing the
  framework package. The ARM64 intrinsic source-contract audit passes with
  all classified entries accounted for; full AOSP differential and real-app
  validation remain open.

### Runtime checkpoint 318 — 2026-09-08

- Native loader correctness improved without widening compatibility fallbacks:
  page-relative TLS tests are deterministic, and absolute ELF symbols are
  fail-closed except for the AOSP/Bionic linker marker set. The full ELF
  loader gate passes, including dependency-first constructors, TLS, IFUNC,
  RELRO, namespace isolation, lifecycle teardown, and FFI smoke coverage.

### Runtime checkpoint 319 — 2026-09-08

- The strict pinned-AOSP ART contract manifest is clean: all 419 test
  directories and 491 sources are owned and represented, with zero unsupported
  or opaque actions. This is a build/contract invariant only; end-to-end JIT,
  GC, concurrency, and real-app evidence is still required.

### Runtime checkpoint 320 — 2026-09-08

- ART bootstrap tests and the signed Darwin JIT memory contract pass. Nterp
  reference boundaries, write barriers, monitor/throw paths, and fail-closed
  source checks are covered; concurrent MAP_JIT execution and W^X rejection
  behave as required. This does not replace end-to-end AOSP/app validation.

### Runtime checkpoint 321 — 2026-09-08

- The remaining 28 upstream intrinsic ordinary-call fallbacks are explicitly
  distinguished from Darwin allowlists and are not used as an interpreter
  fallback or launch gate. Full intrinsic parity and real-app differential
  evidence remain open work.

### Runtime checkpoint 322 — 2026-09-08

- Workspace test/doctest coverage remains green across runtime, native loader,
  APK, profile, host, Binder, and Bionic components. These are integration
  regressions checks, not a substitute for the remaining full AOSP differential
  corpus and real-app validation.

### Runtime checkpoint 323 — 2026-09-08

- Refreshed the framework compatibility artifact so VLC's Android
  `ConnectivityManager.requestNetwork` API-29 overloads are present in runtime
  input. This is a framework-surface correction, not an app patch; a fresh
  unmodified-VLC execution remains required to validate native playback.

### Runtime checkpoint 324 — 2026-09-08

- Confirmed the normal launcher path resolves the refreshed framework artifact
  rather than a copied manager bundle. The original VLC APK is not currently
  available in local roots, so playback verification remains pending without
  modifying or synthesizing an APK.

### Runtime checkpoint 325 — 2026-09-08

- Inspected the generated framework DEX rather than only Java sources: all
  five `requestNetwork` overloads are present, and launcher and boot-image
  paths point to the same regenerated artifact. No APK modification was used;
  native VLC playback still needs the original APK input.

### Runtime checkpoint 326 — 2026-09-08

- Provisioned a clean, isolated `aosp-api29` profile and verified successful
  mounting with zero leases and no installed packages. Existing profiles
  remain untouched; this is the new-host baseline for subsequent APK tests.

### Runtime checkpoint 327 — 2026-09-08

- The complete ART JIT acceptance audit now passes on the replacement host
  (exit 0), including GC-sensitive field/array access, VarHandle and invoke
  polymorphic/custom paths, exception cases, and Android window/framework
  smoke coverage. Full AOSP differential and real-app validation remain open.

### Runtime checkpoint 328 — 2026-09-08

- Repaired the ART corpus runner's own fixtures to include the AOSP
  `expected-stdout.txt` contract. Its 7 focused tests now pass, and the
  production discovery boundary enumerates 1,076 pinned AOSP tests without a
  name allowlist; end-to-end execution is still separate work.

### Runtime checkpoint 329 — 2026-09-08

- Regenerated the AOSP speed boot image to match the refreshed framework DEX;
  the prior stale oat checksum failure is gone. The first 10 pinned corpus
  tests pass on a fresh ledger, covering opcode, interface, allocation, JNI,
  reference-map, sleep, and signal paths through the normal ART runner.

### Runtime checkpoint 330 — 2026-09-08

- Corrected the Darwin JIT memory admission path: a low-4GiB/adjacent mapping
  invariant from Android was rejecting valid independent MAP_JIT mappings.
  The host-specific exception now leaves the real 32-bit stack-map invariant
  checked at formation. Rebuilt runtime/link artifacts and re-ran
  `004-InterfaceTest`; optimized JIT execution passes.

### Runtime checkpoint 331 — 2026-09-08

- A fresh four-way run of the first 50 pinned AOSP tests passes 50/50 after
  the MAP_JIT placement fix. Coverage includes control flow, interfaces,
  allocation, JNI, exceptions, arrays, class-init deadlock, finalization,
  stack overflow, and thread stress; full corpus and real-app validation are
  still outstanding.

### Runtime checkpoint 332 — 2026-09-08

- The following 50 pinned AOSP inputs (`040-miranda` onward) pass 50/50 with
  four-way parallel execution. This extends evidence through reflection,
  proxy/monitor, class loading, NIO, precise/reachability GC, OOM,
  verification, hot exceptions, and inline execution; full corpus and
  real-app checks remain open.

### Runtime checkpoint 333 — 2026-09-08

- The next 50 pinned AOSP inputs (`086-null-super` onward) pass 50/50 in a
  fresh four-way run. Coverage now includes loop formation, serialization,
  concurrent/parallel GC, multidex, suspend checks, native bridge,
  compiler-regression, class-loading, and daemon-lock shutdown paths; full
  corpus and real-app checks remain outstanding.

### Runtime checkpoint 334 — 2026-09-08

- Added an ART-side `ManagedStack` query plus a weak provider fallback for
  generic-JNI unwind recovery; the graphics-link audit passes. Focused AOSP
  `137-cfi` still fails in optimized mode because Darwin native registration
  reaches the callback without an ART generic-JNI tag or quick-frame registry.
  This remains an isolated runtime-boundary blocker; no interpreter fallback or
  test-specific allowlist was added.

### Runtime checkpoint 335 — 2026-09-08

- Re-tested `137-cfi` without requiring the generic-JNI tag. `ManagedStack` is
  now observed with the expected 224-byte frame shape, but its saved return PC
  is not resolved by the JIT debug map; optimized local/remote CFI checks still
  fail. The remaining work is the native-bridge saved-PC/stack-map contract,
  with no interpreter fallback or test-specific allowlist.

### Runtime checkpoint 337 — 2026-09-09

- Audited the complete libunwindstack DexFile path. Enabling
  `DEXFILE_SUPPORT` in the standalone smoke provider exposes missing
  `ADexFile_*`, ART MemMap, and ZipArchive owners, so that partial link was
  reverted. The provider graph and graphics-link audit are green again; the
  remaining work is a real runtime owner for DexFile support, not unresolved
  static symbols.

### Runtime checkpoint 336 — 2026-09-08

- Threaded `DexFiles` through Darwin libunwindstack while keeping the existing
  backtrace call ABI source-compatible. Graphics-link audit passes. The
  standalone provider still cannot enable the full DexFile implementation
  because its smoke link lacks the libdexfile owner; AOT frame-name resolution
  therefore remains an explicit open item.

### Runtime checkpoint 338 — 2026-09-09

- Reverted the stale `DexFiles.cpp` portable-object experiment. Forcing the
  incomplete libdexfile owner graph crashed the standalone unwind smoke
  process, so the partial change was not accepted. A complete ART-owned
  DexFiles provider, including MemMap/ZipArchive owners, remains required.

### Runtime checkpoint 339 — 2026-09-09

- Restored AOSP `DexFiles.cpp` without forcing the incomplete `DexFile.cpp`
  owner, restoring the `CreateDexFiles` runtime contract. Darwin local unwind
  now branches on `tid` presence instead of unresolved Android `GetThreadId`.
  Provider smoke passes (frames=5, context=2, thread=5, remote=4) and the
  graphics-link closure audit is green (registrar=51, fake-symbols=0).
  Complete DexFile metadata ownership remains open.

### Runtime checkpoint 340 — 2026-09-09

- Separated the full AOSP `DexFile.cpp` parser into a production-only
  `libunwindstack-dex-darwin.a` owner archive. The portable smoke archive
  keeps the safe `CreateDexFiles` contract stub, while runtime/graphics links
  consume the real owner and its `DexFile::Create`, `GetFunctionName`, and
  `art_api::dex::DexFile` symbols. Fast graphics-link audit passes
  (`registrar=51`, `fake-symbols=0`); APK metadata and complete
  MemMap/ZipArchive execution still require validation.

### Runtime checkpoint 341 — 2026-09-09

- Revalidated the separated production owner: `DexFile.cpp` plus
  `dex_file_supp.cc` compile into `libunwindstack-dex-darwin.a`, and the fast
  graphics-link audit passes (`registrar=51`, `fake-symbols=0`). The portable
  smoke remains contract-only by design; direct DexFile execution needs the
  complete runtime foundation closure, so APK metadata lookup remains the next
  validation target.

### Runtime checkpoint 342 — 2026-09-09

- Confirmed the separated production owner is consumed by the real graphics
  runtime link and existing AOSP app-Dex/native-load checks pass. A direct
  `DexFile::Create` executable requires the complete production foundation
  closure, so the next validation must invoke metadata lookup through that
  runtime rather than add ad-hoc replacement symbols.

### Runtime checkpoint 344 — 2026-09-09

- Tested enabling `DEXFILE_SUPPORT` directly in the portable core/provider.
  The smoke link exposed the missing complete owners (`PaletteTrace`, fmt,
  bionic filesystem, and related ART dependencies), so that experiment was
  reverted. The production-only DexFile archive remains the correct boundary;
  future work must extend its explicit owner graph without contaminating the
  portable smoke target.

### Runtime checkpoint 343 — 2026-09-09

- Ran the real AOSP `137-cfi` test through dex2oat and the normal JIT host.
  All five native unwind assertions returned `FAIL` while stdout matched the
  expected structure. The unresolved issue is managed-frame publication and
  optimized JNI metadata lookup; APK loading and the runtime link itself are
  not the failure point.

### Runtime checkpoint 345 — 2026-09-09

- Reverted the direct full-DEX support experiment after the portable smoke
  exposed its complete owner requirements (`PaletteTrace`, fmt, bionic
  filesystem, and ART dependencies). The production-only owner boundary is
  preserved while optimized JNI metadata lookup remains open.

### Runtime checkpoint 346 — 2026-09-09

- Attempted to enable `DEXFILE_SUPPORT` in the shared AndroidUnwinder provider
  for managed metadata resolution. Portable smoke then required the complete
  libdexfile owner closure (`PaletteTrace`, fmt, bionic filesystem, and
  related ART objects), so the experiment was reverted. The next fix needs
  separate production/smoke provider variants or a runtime-lazy owner.

### Runtime checkpoint 347 — 2026-09-09

- Added separate production/smoke provider variants. Production
  AndroidUnwinder uses AOSP `DEXFILE_SUPPORT` with full `DexFiles.cpp` and
  `DexFile.cpp` ownership; smoke uses a generated null-contract stub. Smoke
  unwind and graphics-link audits pass. Real `137-cfi` still reports five
  `FAIL`s, isolating the remaining problem to boot/JIT debug-map frame naming,
  not DexFile owner linkage.
### Runtime checkpoint 348 — 2026-09-09

- Production provider validation confirms both JIT and Dex providers are
  instantiated, the exported JIT descriptor is readable, and all in-memory
  JIT ELF entries load. Real `137-cfi` PCs at `0x210dxxx` remain outside those
  JIT ranges and format as `<unknown>`. The next architectural work is
  AOT/boot-image PC-to-Dex metadata resolution.
### Runtime checkpoint 349 — 2026-09-09

- Unwinder diagnostics show AOT return PCs (`0x210d168`, `0x210d788`) are not
  present in `Maps::Find`; JIT/Dex providers themselves are live. The next
  architectural change is publishing ART guest/AOT code ranges to Darwin
  `Maps` (or an equivalent oat-map source) so Dex metadata lookup can run.
### Runtime checkpoint 350 — 2026-09-09

- Added an AOSP-aligned `ClassLinker` oat executable-range registration bridge.
  Darwin retains the oat path, file offset, and code range and publishes them
  into unwindstack `Maps` lazily. Graphics-link audit passes; a fresh real CFI
  run is still required to validate AOT frame naming.
### Runtime checkpoint 351 — 2026-09-09

- Fresh real `137-cfi` still reports five FAILs after the oat-range bridge.
  Registration timing or guest/host address identity remains unresolved; the
  next step is to trace those values directly without adding an allowlist.
### Runtime checkpoint 352 — 2026-09-09

- Forced runtime-shadow rebuild confirms ClassLinker now registers 12 oat code
  ranges. Their host addresses (`0x1007...`/`0x11...`) differ from managed
  logical return PCs (`0x210dxxx`), and `137-cfi` remains five FAILs. The next
  change must define the logical-to-host AOT PC mapping explicitly.
### Runtime checkpoint 353 — 2026-09-09

- Corrected patch application and rebuilt runtime now show 12 registered oat
  ranges, but host addresses still differ from logical `0x210dxxx` managed PCs;
  `137-cfi` remains five FAILs. The required next step is explicit per-oat
  logical code-base identity propagation, not a generic alias.
### Runtime checkpoint 354 — 2026-09-09

- Shadow rebuild confirms the ClassLinker bridge runs and registers 12 oat
  ranges, but host ranges (`0x1007...`) still differ from managed logical PCs
  (`0x210dxxx`). The required next step is preserving per-image logical oat
  begin and applying its relocation delta in the unwinder.
### Runtime checkpoint 355 — 2026-09-09

- Reviewed raw `ImageHeader` logical oat begin integration. Because the current
  image-loading call site lacks a verified patch-safe per-oat identity path,
  the speculative alias was reverted. Host/logical PC translation remains the
  next required architectural change.
### Runtime checkpoint 356 — 2026-09-09

- Boot/app oat registration is active, yet `0x210dxxx` remains outside both
  oat identities and loaded JIT ELF ranges. Evidence now points to JIT
  code-cache logical/host pointer translation, which must be traced explicitly
  before adding another map source.

### Runtime checkpoint 357 — 2026-09-09

- Implemented map-validated compressed-window PC lifting for managed return
  addresses. The CFI probe now sees host-window PCs (`0x1000210dxxx`) and a
  real anonymous mapping, while method-name/Dex attribution remains unresolved.
  The next change must connect that mapping to the JIT code-cache debugger
  identity rather than widening aliases.

### Runtime checkpoint 359 — 2026-09-09

- Candidate tracing shows the compressed-window target is a real but
  non-executable anonymous RW mapping. The guard remains conservative;
  producer-side JIT/AOT return-PC identity must be fixed before another map is
  published.

### Runtime checkpoint 358 — 2026-09-09

- Restricted compressed-window PC lifting to executable mappings. A mapped
  anonymous heap region is insufficient evidence of code identity; this keeps
  unwinding conservative until the JIT debugger publishes the exact executable
  code-cache range.
