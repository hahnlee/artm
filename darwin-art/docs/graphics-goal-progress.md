# Android graphics compatibility goal log

This is the append-only handoff log for the active Darwin ART graphics goal.
Before changing code, read only this fixed header and the most recent two
`## Progress` entries (`sed -n '1,80p'` plus `tail -160`). Before ending a
work turn, append one concise entry with evidence, changed files, the exact
remaining fault, and the next command. Do not rewrite old entries; corrections
belong in a newer entry so the investigation remains auditable.

## Final objective

Run the Android-owned graphics pipeline end to end:

`ViewRootImpl / Choreographer -> HWUI RenderThread / RenderNode -> BufferQueue
-> AOSP SurfaceFlinger transaction + layer lifecycle -> IOSurface gralloc ->
Metal shared-event fences -> Darwin Metal Composer/HWC -> CAMetalLayer`.

The host must not poll `View.draw()`, replay product RenderNodes, or use
per-application graphics exceptions. Completion requires Chrome's tab hub
open/select/return path to be immediate and atomic, Calculator and DeskClock to
pass on the same common path, documentation and build gates to pass, and all
intended changes to be committed and pushed.

## Completion gates

- Product traversal, damage, animation, and buffer submission are owned by
  AOSP ViewRootImpl/Choreographer/HWUI RenderThread.
- The real AOSP SurfaceFlinger transaction handler, lifecycle manager, and
  hierarchy determine retained layers and ordering.
- Central composition uses IOSurface plus Metal shared-event acquire/release
  fences; Darwin HWC performs only GPU scanout to CAMetalLayer.
- No product host-side `View.draw()` polling, RenderNode replay, local z-sort,
  CPU presentation fallback, or app-specific hardware flag is required.
- Actual Chrome APK tab hub transitions are visually correct and latency is
  measured; actual Calculator and DeskClock APKs regress cleanly.
- Incremental native builds do not rebuild unrelated ART/ICU archives.
- Source/audits/tests are documented, committed, and pushed.

## Progress 2026-08-30 13:03 KST — central composition boundary isolated

Evidence and completed work:

- AOSP SurfaceFlinger `TransactionHandler`, `LayerLifecycleManager`, and
  `LayerHierarchyBuilder` are built and used by the Darwin service. Runtime
  probe covers hierarchy ordering and handle destruction.
- Product transactions fail closed without central SurfaceFlinger. Calculator
  actual APK resize reached Android WMS/ViewRoot relayout.
- Product `present_gpu_content` no longer replays the ViewRoot RenderNode or
  manually advances its animation context.
- Added Darwin HWC scanout in `runtime_graphics_input.cc`: it GPU-blits the
  central display IOSurface to CAMetalLayer without calling `View.draw()`.
  This restored the NSWindow, but its composed content is currently black.
- Native cache promotion now parses persisted depfiles and emits explicit
  header dependencies; repeated runtime archive generation reached
  `ninja: no work to do` instead of rebuilding 254–505 ART objects.
- Official installed Chrome APK acceptance passed once end to end (macOS CA,
  MotionEvent, physical keyboard, three Android tabs, renderer/GPU services,
  Binder SCM_RIGHTS, WebGL ANGLE-Metal, WebM/Opus/CoreAudio). One earlier run
  had a nondeterministic retired-renderer poison-address SIGSEGV and remains a
  lifecycle issue; page-side PASS is not visual tab-hub proof.

Current exact fault:

- `ViewRootImpl` submits a 720x1280 AHardwareBuffer and the central HWC scanout
  window is present, but the central SurfaceFlinger target IOSurface is black.
  Need determine whether the source AHardwareBuffer is already black or Metal
  Composer loses it.

Changes made during this investigation include
`compat/surfaceflinger/service_darwin.mm`,
`compat/darwin_android_platform.mm`, `probes/runtime_graphics_gpu.cc`,
`probes/runtime_graphics_input.cc`,
`tools/android-apk-app-runtime/fixture/DarwinServiceBridge.java`, and
`crates/darwin-art-xtask/src/graph/cache.rs`. The shared worktree contains a
larger uncommitted graphics series; inspect `git status` before commits.

Next action:

1. Inspect `/tmp/darwin-art-central-pixels.log` and the current system-server
   log for `ART SurfaceFlinger: source pixels` and matching transaction/layer.
2. If source is non-black, inspect Metal Composer target after completion. If
   source is black, trace HWUI ANativeWindow queue/acquire fence and persistent
   IOSurface snapshot before central submission.
3. Remove or retain pixel instrumentation according to whether it becomes a
   useful guarded diagnostic, then rebuild only the affected object/archive.

## Progress 2026-08-30 13:20 KST — AOSP root lifecycle semantics fixed

Evidence and completed work:

- HWUI-produced gralloc buffers now bypass ANGLE EGLImage bookkeeping and
  reach the central SurfaceFlinger as IOSurface-backed layers. This changed
  the failure from `layers=0` to one visible layer reaching the AOSP frontend.
- Failure-only diagnostics identified the exact omitted layer as
  `local=1 global=1 parent=0 what=0x10020804b`: the root was incorrectly sent
  with `eReparent` on every retained snapshot.
- AOSP `RequestedLayerState::merge` confirms `eReparent(null)` means detach,
  and permanently clears `canBeRoot`; it does not mean attach to display root.
- `compat/darwin_android_platform.mm` now emits `eReparent` only for controls
  explicitly reparented in the current Android transaction. The bridge also
  converts Darwin ABI parent `0` to AOSP `UNASSIGNED_LAYER_ID`.
- The central lifecycle now creates structural parent handles before a
  buffer-producing child when the parent's own buffer state is absent.
- `surfaceflinger-transaction-runtime` passes including
  `buffer-child-parent=PASS`; final graphics link/audit passes with
  `registrar=51 fake-symbols=0`.
- After the fix, a Chrome launch produced no further `AOSP hierarchy omitted`
  or central compose failure for the new process. The screenshot at
  `/tmp/darwin-art-root-reparent-fix.png` captured only the macOS lock screen,
  so visible Chrome composition is not yet accepted.

Changed in this slice:

- `compat/darwin_android_platform.mm`
- `compat/darwin_angle_egl.cc`
- `compat/surfaceflinger/transaction_bridge.cc`
- `compat/surfaceflinger/service_darwin.mm`
- `probes/surfaceflinger_transaction_handler_runtime.cc`

Current exact remaining fault:

- Re-run while the macOS user session is unlocked and prove the Chrome window
  receives the central IOSurface. If it is still black, enable
  `DARWIN_ART_DEBUG_SURFACECONTROL_PIXELS=1` for the system service and compare
  source IOSurface samples with the Metal composer target. The prior hierarchy
  omission must not recur.

Next action:

1. Restart the current `android.system` process after resolving its PID with
   `target/release/darwin-artctl ps`.
2. Run the installed official Chrome APK for at least 20 seconds, capture the
   unlocked desktop, and inspect new central log lines for the app PID.
3. Once the root buffer is visibly composed, exercise the actual tab hub and
   measure open/select/return latency before Calculator and DeskClock regressions.

## Progress 2026-08-30 13:51 KST — central target and Chrome tab hub proven

Evidence and completed work:

- Metal completion-fence diagnostics proved the source IOSurface and central
  display target contain matching non-black Chrome pixels. Direct target
  capture at `/tmp/darwin-art-tab-target.png` shows the real installed Chrome
  APK upright; `compat/surfaceflinger/metal_composer.mm` now applies Android's
  gralloc-to-Metal vertical texture-coordinate convention at the HWC boundary.
- Per-transaction target captures in
  `/tmp/darwin-art-tab-series-20260830-1343` prove transaction 25/27 contains
  the actual four-card Chrome tab hub. The tab-counter MotionEvent was consumed
  through Android's input channel in 1.6–1.8 ms.
- `ProcessAlive` now rejects Darwin zombies using `proc_pidinfo`, so a dead
  producer cannot retain a fullscreen snapshot forever.
- An empty SurfaceControl transaction no longer erases the process's retained
  snapshot. Android transactions with zero layer updates are no-ops; layer
  removal belongs to explicit handle destruction or process/Binder death.
- A proposed process-exit watcher was tested and removed: live inspection
  showed the visible `ChromeChildSurface` owner is a surviving
  `--service-child`, not either renderer PID that logs normal exit.
- Incremental rebuild touched one Objective-C++ object plus the graphics
  archive/link. Graphics link audit passed (`registrar=51 fake-symbols=0`) and
  `git diff --check` passed.

Changed in this slice:

- `compat/surfaceflinger/service_darwin.mm`
- `compat/surfaceflinger/metal_composer.mm`
- `probes/runtime_graphics_gpu.h`
- `probes/runtime_graphics_gpu.cc`
- `probes/runtime_graphics_input.cc`

Current exact remaining fault:

- Chrome's main process produces tab-hub ViewRoot buffers, but the fullscreen
  child surface is owned by a different live service process. Cross-process
  SurfaceControl identity/visibility and latch ordering are not yet represented
  like Binder handles in AOSP, so some runs keep the child above the new root;
  other runs expose intermediate black/root-placeholder frames before
  transaction 25. Open/select/return is therefore not yet atomic or accepted.

Next action:

1. Add guarded logging of each transaction's layer id, `what`, parent, z,
   alpha, and IOSurface around Chrome transactions 17–27.
2. Trace the main-process controls-only transactions at app-log lines 1523 and
   1545 to the child SurfaceControl handle. Replace per-PID local identity with
   the shared Binder/SurfaceControl identity and feed visibility/removal into
   the AOSP lifecycle transaction.
3. Re-run per-transaction target capture, then inject a card-selection tap and
   require no black hashes between home, tab hub, and selected-tab return.

## Progress 2026-08-30 14:33 KST — shared SurfaceControl retained state reached; Vulkan producer isolated

Evidence and completed work:

- Implemented real `nativeMergeTransaction` state transfer and extended the
  Surface parcel contract with `(owner pid, layer id)`. The Chrome privileged
  process now imports its native window as main-process layer 7 instead of
  creating an unrelated display root; runtime evidence is in
  `/tmp/darwin-art-tab-retained-v4-20260830/app.log`.
- SurfaceFlinger wire protocol v4 now carries shared owner/parent identities
  plus Android `flags/mask`. Bufferless show/hide/reparent updates are sent to
  the same latch as buffers. Central state is retained and merged by global
  SurfaceControl identity and `what` bits rather than replacing a per-PID
  snapshot. Metal composition skips retained layers carrying `eLayerHidden`.
- Added the required producer `glFlush` before the remote Metal shared-event
  wait. The v4 Chrome run had main and privileged-process submissions composed
  as the same two-layer AOSP hierarchy without a new producer-fence failure or
  hierarchy omission.
- AOSP `surfaceflinger-transaction-runtime` passes queue/flush, hierarchy,
  lifecycle and buffer-child-parent gates. Graphics link audit passes with
  `registrar=51 fake-symbols=0`; repeated runtime bootstrap reports
  `ninja: no work to do` after the initial closure rebuild.
- Actual target montage
  `/tmp/darwin-art-tab-retained-v4-20260830/montage.png` proves the real tab hub
  renders and the card tap is consumed, but selected Example Domain content is
  still black. Central pixel diagnostics prove why: Chrome's privileged Vulkan
  producer submits IOSurfaces 328/329 whose complete RGBA hash is the all-zero
  hash on every frame. Main HWUI correctly punches the SurfaceView region, so
  this is now a producer import fault, not a Metal Composer/layer-order fault.
- Added guarded Rust diagnostics around MoltenVK AHardwareBuffer dedicated
  memory import and `vkBindImageMemory{,2}` to compare the imported IOSurface
  `MTLTexture` with `vkGetMTLTextureMVK`. Unit tests for
  `android-dso-namespace` pass. These strings have not yet reached the linked
  graphics dylib because the native graph does not depend on the rebuilt Rust
  provider archive; this independently reproduces the missing incremental
  dependency edge behind slow/stale builds.

Changed in this slice:

- `compat/darwin_android_native_window.cc`
- `compat/darwin_android_platform.{h,mm}`
- `compat/darwin_angle_egl.{h,cc}`
- `compat/darwin_framework_natives.cc`
- `compat/surfaceflinger/{metal_composer.h,service_darwin.mm,transaction_bridge.h,transaction_bridge.cc}`
- `probes/surfaceflinger_transaction_handler_runtime.cc`
- `tools/android-dso-namespace/src/lib.rs`

Current exact remaining fault:

- The selected-tab `ChromeChildSurface` is created, fenced and centrally
  latched, but its MoltenVK-imported AHardwareBuffer IOSurface remains entirely
  zero. Before diagnosing the Metal texture pointer result, force the graphics
  dylib to relink from the new `target/release/libandroid_dso_namespace.a` and
  fix the native graph dependency so future Rust provider changes trigger that
  relink automatically. Then compare `dedicated-image`, `image-texture`, and
  `match=` in the privileged-process log. If `match=false`, correct the
  `VK_EXT_external_memory_metal` pNext/import contract; if `match=true`, trace
  the final Vulkan queue signal and IOSurface contents at that exact fence.

Next action:

1. Use Ninja's target-scoped clean/relink for
   `_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib`,
   then verify `strings` contains `dedicated-image=`; do not clean the whole
   workspace.
2. Add `target/release/libandroid_dso_namespace.a` as an explicit native-graph
   input/dependency, rebuild, restart `android.system`, and run Chrome for 20s
   with `DARWIN_ART_DEBUG_GRAPHICS_DSO=1`.
3. Resolve the Vulkan AHB backing mismatch, repeat open/select/return target
   capture with no all-zero target interval, then continue Calculator and
   DeskClock common-path regressions.

## Progress 2026-08-30 14:54 KST — MoltenVK image backing mismatch proven and bridged

Evidence and completed work:

- Corrected the previous build-graph diagnosis: the generated Ninja graph
  already contains `android-dso-namespace` source -> bionic Rust provider
  closure -> graphics dylib edges. `ninja -d explain` identified the edited
  Rust source as dirty; a target-only rebuild regenerated two targets and the
  final graphics audit passed (`registrar=51 fake-symbols=0`). Both the provider
  archive and graphics dylib now contain the guarded Vulkan import diagnostics.
- Actual Chrome reproduction `/tmp/darwin-art-vulkan-import6-20260830/app.log`
  created `ChromeChildSurface` and proved both imported AHardwareBuffers had a
  valid dedicated VkImage but no Metal backing on that image:
  `image-texture=0x0 match=false` before and after `vkBindImageMemory`.
- Local MoltenVK source inspection showed that the dedicated external-memory
  allocation can materialize/replace image backing independently of the
  imported `MTLTexture`. The compatibility provider now calls MoltenVK's
  `vkSetMTLTextureMVK` after successful external-memory allocation and before
  bind, explicitly installing the IOSurface-backed texture on the dedicated
  VkImage. Failure paths free the Vulkan memory and release the texture.
- `cargo test -p android-dso-namespace`, `cargo fmt --all -- --check`, target
  graphics relink/audit, and `git diff --check` pass.

Changed in this slice:

- `tools/android-dso-namespace/src/lib.rs`
- `docs/graphics-goal-progress.md`

Current exact remaining fault:

- The provider fix is built, but three subsequent actual Chrome launches did
  not reach AHardwareBuffer import: the privileged GPU service created only
  `ChromeNativeWindowSurface` (or started late) and never created its child
  buffer surface before shutdown; one 34-second launch also failed to honor
  its visible-window deadline until interrupted. Therefore the new
  `vkSetMTLTextureMVK` path still needs one actual-child run proving
  `image-texture == imported MetalTexture`, non-zero IOSurface pixels, and a
  visible selected tab. This GPU-service startup/lifecycle nondeterminism is a
  separate compatibility defect, not evidence against the backing fix.

Next action:

1. Restart the exact `android.system` PID from `target/release/darwin-artctl ps`
   and run Chrome with the tab-hub/card sequence until `ChromeChildSurface`
   allocates; inspect `dedicated-image=... image-texture=... match=true`.
2. Capture the central target and require the child IOSurface full hash to be
   non-zero through open/select/return; if `match=true` but pixels remain zero,
   trace Vulkan queue signal completion rather than changing SurfaceFlinger.
3. Diagnose why some Chrome GPU-service runs never create the child surface and
   why one bounded window did not exit, then continue Calculator/DeskClock
   common-path regressions.

## Progress 2026-08-30 — Chrome tab lifecycle passes; SurfaceControl flag bug fixed

Evidence and completed work:

- The thread has an active product Goal: preserve Android's
  `ViewRootImpl/Choreographer -> HWUI -> BufferQueue -> SurfaceFlinger`
  ownership and use IOSurface/Metal only below the Android compositor contract.
- Actual Chrome input at logical `(225,610)` hit
  `TabSwitcherButtonView`; selecting logical `(180,320)` hit `TabGridView`.
  Both DOWN/UP pairs completed with `handled=1`. Central SurfaceFlinger hashes
  changed page -> tab hub -> animated page return without a post-start all-zero
  target. Evidence: `/tmp/darwin-art-chrome-tab-return-20260830.log` and the
  profile-scoped central SurfaceFlinger log from that run.
- Calculator initially had a valid ViewRoot, EGL swap, IOSurface transfer, and
  ANativeWindow queue, but no presentation. Transaction diagnostics proved its
  root control was a composition root while its retained state had become
  `visible=0`.
- The generic cause was `SurfaceControlNativeSetFlags`: it interpreted every
  Android `layer_state_t` flag (including OPAQUE/SECURE/backpressure) as the
  HIDDEN bit. It now changes visibility only when mask bit `0x01`
  (`eLayerHidden`) is present and ignores unrelated flags for visibility.
- After the fix, actual unchanged `ExactCalculator-api28.apk` reports
  `visible=1`, presents the 720x1280 root, and submits one Metal composition
  layer repeatedly. Evidence:
  `/tmp/darwin-art-calculator-surface-flags-fixed-20260830.log`.
- Actual unchanged `DeskClock-api29.apk` also retains `visible=1` and repeatedly
  presents its 720x1280 root through the same transaction path. Evidence:
  `/tmp/darwin-art-deskclock-graphics-regression-20260830.log`.
- Incremental `ninja -f _build/native-graph/build.ninja graphics-bootstrap
  graphics-audit` rebuilt four targets in about 12 seconds and passed the final
  graphics link audit (`registrar=51 fake-symbols=0`). `git diff --check` passes.

Changed in this slice:

- `compat/darwin_framework_natives.cc`
- `compat/darwin_android_native_window.cc` (guarded queue diagnostics from the
  preceding diagnosis)
- `compat/darwin_angle_egl.cc` (guarded swap-transfer diagnostics from the
  preceding diagnosis)
- `docs/graphics-goal-progress.md`

Current exact remaining work:

- Calculator rendering is restored, but the synthetic arithmetic sequence in
  `/tmp/darwin-art-calculator-arithmetic-20260830.log` did not emit enough
  semantic input evidence to assert the final numeric value. Add or use a
  framework-state/readback acceptance seam and prove an unchanged APK computes
  a concrete expression through real MotionEvents.
- DeskClock needs one real tab/control interaction and resulting state/hash
  transition, not only initial-frame presentation.
- Chrome's successful open/select/return run must be converted into a stable
  acceptance gate; intermittent renderer/GPU-service startup retirement still
  needs repeated-run coverage.

Next action:

1. Re-run Calculator with `DARWIN_ART_DEBUG_INPUT_LATENCY=1`, known button hit
   coordinates, and framework text/state readback; require a concrete result.
2. Exercise a DeskClock tab/control through the same native MotionEvent path and
   require a post-input SurfaceFlinger target change.
3. Add the SurfaceControl hidden-mask regression assertion to the graphics gate,
   run the repeated Chrome gate, then remove diagnosis-only noise, format/audit,
   commit, and push.

## Progress 2026-08-30 — semantic app gates complete; Chrome readiness blocker isolated

Completed in this turn:

- Added debug-only generic Android `TextView` state readback behind
  `DARWIN_ART_DEBUG_VIEW_TEXT`; it traverses the real View tree and does not
  alter app code, event dispatch, or rendering.
- Added `tools/aosp-core-apps-graphics-acceptance.sh`. The unchanged AOSP API
  28 Calculator received real Android DOWN/UP packets on `digit_2`, `op_add`,
  `digit_3`, and `eq`, then exposed formula `2+3` and result `5`. The unchanged
  API 29 DeskClock received a real tab click and exposed Timer state
  `00h 00m 00s`. Gate result:
  `aosp-core-apps-graphics-acceptance: PASS Calculator=2+3=5 DeskClock=Timer common-path=HWUI+SurfaceFlinger+Metal`.
  Evidence is under `_build/aosp-core-apps-graphics-acceptance/`.
- Added constexpr regression assertions proving only Android's hidden bit
  changes SurfaceControl visibility; OPAQUE, SKIP_SCREENSHOT, and SECURE do
  not hide a layer.
- Added `tools/chromium-tab-graphics-acceptance.sh` with a fresh app-data root,
  actual `TabSwitcherButtonView`/`TabGridView` targets, child SurfaceControl,
  MoltenVK AHardwareBuffer identity, central target-hash, and crash checks.
- Audited central composition ownership. The retained map is payload lookup
  only; bottom-to-top order is copied from AOSP
  `LayerLifecycleManager` + `LayerHierarchyBuilder::traverseInZOrder()`. No
  product SurfaceFlinger path performs a local z-sort.
- `ninja -f _build/native-graph/build.ninja graphics-bootstrap graphics-audit`
  passes with registrar=51 and fake-symbols=0. `git diff --check` passes.

Current exact blocker:

- Chrome had one complete unchanged-APK page -> tab hub -> card return pass in
  `/tmp/darwin-art-chrome-tab-fresh2-20260830.log` and a first executable-gate
  pass with 7 central target states. Repeated fresh-profile runs now handle the
  real tab button and execute `android.view.View$PerformClick`, but do not call
  the native tab-switcher transition; two seconds later the same coordinate
  still hits `SuggestionsTileView` instead of `TabGridView`.
- Fence instrumentation ruled out the first central completion as the direct
  blocker: Chrome registers the returned present fence in its real epoll set,
  receives readiness, and removes it. The failed run nevertheless publishes
  only the initial `ChromeChildSurface` buffer. Latest focused evidence:
  `_build/chromium-tab-graphics-acceptance/run.ksKVrp/chrome.log`. Temporary
  socket/main-queue diagnostics and the unhelpful serialized-worker experiment
  were removed; no speculative workaround was retained.

Next action:

1. Compare the successful run's first post-click native callback
   `Java_J_N_VFJOO` and repeated child-buffer submissions against the failed
   run's completed `View$PerformClick`/animation callbacks, then identify the
   missing Chromium compositor readiness signal rather than changing the APK
   or synthesizing a tab UI.
2. Re-run
   `./tools/chromium-tab-graphics-acceptance.sh _build/installed-apps/org.chromium.chrome/802400004/2aaea8419d955677313f8b6dae3f0666916243ec55c3607a8711f46c9123b731/base.apk`
   until the gate passes repeatedly, then run both app gates, format, full
   graphics audit, review the complete diff, commit, and push.

## Progress 2026-08-30 17:54 KST — Goal visibility and Chrome lifecycle race

- Confirmed the thread Goal is active: Android-owned ViewRoot/Choreographer ->
  HWUI -> BufferQueue -> real AOSP SurfaceFlinger -> IOSurface/shared-event ->
  Metal HWC, with repeated Chrome tab-hub/card return plus Calculator/DeskClock
  on the same path before documentation, commit, and push.
- Compared pass `_build/chromium-tab-graphics-acceptance/run.OSiPJM` with failure
  `run.hY9n4m`. The earlier renderer-service hypothesis was too broad: both
  retain sandbox instance 0 and intentionally retire spare instance 1. The
  decisive difference is that the pass parcels the second `SurfaceView` to the
  privileged GPU process and creates `ChromeNativeWindowSurface`/
  `ChromeChildSurface` just before `surfaceDestroyed`; the failure destroys the
  `SurfaceView` about 0.1 s earlier and never writes/reads its Surface parcel.
- A guarded `DARWIN_ART_DEBUG_ANATIVEWINDOW=1` run passed with 6 target states
  at `_build/chromium-tab-graphics-acceptance/run.zrUlIQ`; parcel evidence was
  owner pid/layer `64141/7` imported by GPU pid `64802`. A subsequent normal run
  also created/presented `ChromeChildSurface` but its real tab click remained
  blocked in Chromium's in-progress layout state. A temporary aggressive
  dynamic queue-drain/vsync-order experiment made that failure consistent and
  was fully reverted; no APK-specific workaround or relaxed 14 s gate remains.
- Retained files: `probes/runtime_graphics_input.cc` keeps the bounded 64-message
  owner-thread pump and native polling between messages; the acceptance gate
  remains a 10 s responsiveness check. Build and link audit pass: registrar=51,
  fake-symbols=0; `git diff --check` passes.

Exact remaining fault:

- The standalone host still pumps Android's UI MessageQueue from the display
  frame callback. That is not Android's continuous `Looper.loop()` ownership
  model and creates timing-dependent Surface delivery and compositor-layout
  readiness. Raising the per-frame drain budget only starves framework vsync;
  delaying the click only hides the defect.

Next command/work:

1. Split the Android owner/UI Looper from the host display pump: run continuous
   `MessageQueue.next()` semantics on the ART owner thread, feed display events
   through its native Looper registration, and make the host frame callback
   signal vsync only. Start by mapping owner-thread creation and callback entry:
   `rg -n "owner_thread_for_callback|pump_frame|run.*Looper|Looper.loop" probes crates compat`.
2. Then run the Chrome gate twice sequentially, followed by
   `tools/aosp-core-apps-graphics-acceptance.sh`, full graphics audit, format,
   diff review, commit, and push.

## Progress 2026-08-30 18:15 KST — Android UI scheduling and compatibility audit complete

Completed:

- Split Android main-Looper servicing from display cadence at the native/Rust
  session ABI. Product window loops now service the owner-thread MessageQueue
  every <=2 ms while publishing Choreographer vsync independently at
  16,666,667 ns. The ABI is
  `darwin_art_graphics_session_pump_main_looper`; session ownership and close
  tests pass.
- Removed the common runtime's Chrome-obfuscated click-listener replacement,
  reflective first-run/omnibox diagnostics, and command-line inspection.
  `RemoteServiceBinder` now discovers every remote AIDL descriptor through the
  standard Binder `INTERFACE_TRANSACTION` instead of hard-coding Chromium's
  child-process interface.
- Removed app-process `z_order` sorting. AOSP SurfaceFlinger hierarchy order,
  exported by `darwin_art_surfaceflinger_copy_layer_order`, is the sole layer
  ordering authority consumed by the Metal HWC.
- Actual unchanged Chrome APK passed twice after those removals:
  `_build/chromium-tab-graphics-acceptance/run.YmlSQL` and
  `_build/chromium-tab-graphics-acceptance/run.GPDNPA`, each with real
  `TabSwitcherButtonView` + `TabGridView`, handled MotionEvents, six distinct
  composed target states, ANGLE/AHardwareBuffer/SurfaceFlinger/Metal, and no
  Vulkan selection or crash.
- `tools/aosp-core-apps-graphics-acceptance.sh` passed Calculator `2+3=5` and
  DeskClock Timer through the same HWUI -> SurfaceFlinger -> Metal path.
- `ninja -f _build/native-graph/build.ninja graphics-bootstrap graphics-audit`
  passes (`registrar=51`, `fake-symbols=0`, `host-icu=0`, `host-fmt=0`,
  `CoreText=0`). Engine graphics-session tests pass 2/2, host tests pass 10/10,
  `cargo fmt --all -- --check` and `git diff --check` pass.

Completion state / next command:

- Implementation and acceptance criteria are complete. Product APKs have no
  host `View.draw()`/RenderNode replay, local z-sort, CPU presentation fallback,
  per-app hardware flag, or app-class graphics exception. The remaining
  `View.draw()` path is isolated to detached legacy probes without ViewRootImpl.
- Review the staged goal diff, commit it, and push `main` to `origin`; then
  record the resulting commit id here if another turn resumes:
  `git diff --stat && git status --short && git add -A && git commit ... && git push origin main`.
