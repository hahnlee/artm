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
