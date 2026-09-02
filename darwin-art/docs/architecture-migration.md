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
