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
