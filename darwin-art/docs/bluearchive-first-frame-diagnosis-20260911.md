# Blue Archive first-frame diagnosis — 2026-09-11

## Result

The current unmodified Blue Archive 1.93.454564 base + arm64 split does **not**
pass title/menu/interactive-game acceptance. A normal 40-second run exits 0,
but its actual composed 1280x720 scanout is entirely black. Unity initialization,
Vulkan creation, CoreAudio output and a successful host exit are not proof of
game UI progress.

Original installed APK SHA-256 values, unchanged during this investigation:

- base.apk: `25479ffb2e0710285a6f11e5666273b97edb68f3d6ae16ba78fd388fd7cd45e8`
- split-0.apk: `2049eeaba16d4f6b29c51d0dc5551c1e3e8598274dcfce140c3e816d47602ee4`

No APK, account, login, game consent or purchase was modified. The existing
user change in `crates/art-bootstrap/src/runtime_art/foundation.rs` was untouched.

## Evidence

Artifacts are under `_build/bluearchive-acceptance-20260911/` (ignored):

- `scanout-000002.png`: actual scanout-source Metal blit readback; 1280x720,
  ImageMagick RGB mean=0, standard deviation=0.
- `calculator-000002.png`: control run through the same capture path; genuine
  Calculator UI, 720x1280, mean=0.66885, standard deviation=0.257452. Run exit 0.
- `lifecycle-000001.png`, `lifecycle-000002.png`: both black despite actual
  InputChannel DOWN/UP delivery, `consumed=1`, including a center tap after 15s.

Logs:

- `/tmp/bluearchive-astra-diagnostic-run.log`: normal 40s run, exit 0;
  exactly two scanout artifacts, Unity/Vulkan/CoreAudio initialized.
- `/tmp/bluearchive-astra-lifecycle.log`: nativeResume, nativeRecreateGfxState,
  and nativeFocusChanged(focused=1) return without exceptions; first
  nativeRender enters but does not return. Synthetic input is consumed but
  produces no visible game response. This run was explicitly terminated for
  further diagnosis, not classified as a clean exit.
- `/tmp/bluearchive-astra-debug.log`: SIGQUIT managed thread dump; UnityMain
  is Native inside UnityPlayer.nativeRender. Version-check Java work has not
  started, unlike the historical five-minute file-checking phase.
- `/tmp/bluearchive-astra-all-stall.log`: self-process Mach snapshots of the
  target and other process threads, all inspected snapshots return status 0.
- `/tmp/bluearchive-astra-signals.log` and `/tmp/bluearchive-astra-mask.log`:
  guest pthread signal delivery and mask/disposition evidence.

An initial normal 600s run was externally SIGKILLed (exit 137) after roughly
two minutes, with no preceding fatal marker. Its cause was not determined;
it is not a successful soak. External sample/LLDB attach attempts hung and
were stopped; the usable native snapshots came from inside the process.

## Native wait classification

The first nativeRender is sleeping in:

`__semwait_signal -> nanosleep -> usleep -> libil2cpp.so+0x19c964c`
`-> libil2cpp.so+0x19bf874`.

Disassembly of the original ELF shows that `+0x19c955c` repeatedly calls
sem_getvalue and usleep(3000), waiting for GC stop/start-world acknowledgments.
Its warning reference is the embedded Boehm GC string
`GC Warning: Lost some threads while stopping or starting world?!`.

The original ELF initializes suspend signal 30 and resume signal 24 at
`+0x19c8dc8` and `+0x19c8de0`. The shared provider maps these to Darwin
SIGINFO (29) and SIGXCPU (24), respectively. This mapping is consistent across
the pthread and process-state providers.

The GC Finalizer (guest pthread token 19) receives one successful suspend /
resume pair, then repeated suspend attempts. All observed pthread_kill calls
return 0. The all-thread snapshot finds it back in the ordinary guest
CondWait path (`libil2cpp.so+0x192b690`), not stuck inside sem_post or a signal
handler. Other Unity workers are in ordinary futex waits.

Mask instrumentation proves the Finalizer starts with SIGINFO **unblocked**
(host mask `0xef7ef857`) before ART attachment, after attachment, and when it
is named GC Finalizer. The registered host action is non-default/non-ignored
and has flags `0x42` (SA_SIGINFO | SA_RESTART). ART attachment does not change
this mask. Earlier engine helper threads inherit `0xfffef857`, but are not the
token to which the repeated failed-to-ack suspend attempts are addressed.

Therefore an initial bad mask, missing thread-token mapping, ESRCH, absent
handler, renderer deadlock, or network/file-check latency does not explain
the evidence. The remaining boundary is handling/restoration **after the
first nested suspend/resume signal cycle**. pthread_kill success establishes
signal submission, not execution or acknowledgment of the guest handler.

## Next concrete regression/fix boundary

Build a two-cycle guest signal test using the actual process-state trampoline:
worker waits on the provider condition variable; suspend handler posts an
ack semaphore and waits in sigsuspend for restart; repeat suspend/resume and
assert every ack plus restoration of the pre-signal mask. Record the worker's
mask immediately after the first cycle and the trampoline entry/exit masks.
Compare nested signal restoration with Android/POSIX sigsuspend/sigreturn
semantics before changing the runtime. Do not bypass GC or manufacture ack
counts to make the game advance.

Separately, current sem_post uses std::mutex and an unordered-map lookup and
thus does not satisfy POSIX async-signal-safe sem_post requirements. This is
a real review finding, but no observed thread is deadlocked there in this
run; it must not be asserted as the demonstrated cause of this black screen.

## Diagnostic-only source status

Temporary opt-in instrumentation remains uncommitted in:

- `compat/darwin_surface_bridge.mm` (`DARWIN_ART_DIAGNOSTIC_FRAME_PREFIX`);
- `compat/darwin_runtime_jni_registration.cc` (self-process nativeRender
  watchdog, `DARWIN_ART_DEBUG_UNITY_STALL`, optional `_ALL`);
- `tools/android-bionic-pthread-provider/src/provider.cc`
  (`DARWIN_ART_DEBUG_PTHREAD_SIGNALS`).

Do not classify or commit these as a compatibility fix. Capture does no GPU
readback/allocation/wait when unset. Watcher code is only reachable with the
existing Unity lifecycle diagnostic wrapper plus its explicit stall option;
its completion flag has static lifetime, thread rights are retained/released,
and each successful thread_suspend is paired with thread_resume before
symbolization/logging. Stack walking is bounded and uses checked self-memory
reads. This is diagnostic stop-the-thread sampling, not an unrestricted
production performance measurement.

Incremental graphics closure/link passes with registrar=51, fake-symbols=0,
host-icu=0, host-fmt=0, CoreText=0. `git diff --check` passes. All Blue Archive,
sampler and debugger processes started for this investigation are stopped.
