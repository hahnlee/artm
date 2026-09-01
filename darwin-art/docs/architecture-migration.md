# Architecture migration contract

This is the implementation target for the next phase. It is deliberately
stronger than the current proof-of-concept boundaries: a passing probe is not
treated as evidence that the ownership or build boundary is complete.

## End state

```text
art-bootstrap (CLI/orchestrator only)
    │ invokes typed build/probe commands
    ▼
darwin-art-host (event loop and presentation only)
    │ owns one RuntimeSession value
    ▼
darwin-art-runtime (Rust lifecycle + typed resources)
    ├── EngineOwner: loaded image + symbol table + shutdown
    ├── NativeGraphOwner: ELF graph/provider leases
    ├── NativeArtifactOwner: graph-atomic Darwin/ELF selection
    ├── SurfaceOwner: CAMetalLayer/Metal drawable lease
    ├── FrameOwner: input/pulse scheduling
    └── rollback and reverse teardown
    │ one versioned POD boundary
    ▼
darwin-art-engine-sys (raw FFI declarations only)
    │
    └── C++/ObjC++ phase modules
        ART/JNI | ELF adapter | HWUI/Metal | platform providers
```

The Rust CLI is organized by change domain rather than one command file:
`runtime_commands.rs` is a small command facade; `runtime_art_build.rs` owns
low-level ART platform/ARM64/interpreter products; `source/` owns locked source
materialization and foundation/Skia builders; `audit/` owns CPU/graphics link
audits; `probe/` owns runtime/graphics/APK fixture commands; `native_probe/`
owns cached C++ probe builders; and `runtime_bootstrap/` separates patched
source staging, per-object compilation, and archive finalization. These
modules share command/context helpers but not ABI declarations or lifecycle
policy.

Rust owns resources, state transitions, leases, rollback, and build graph
decisions. It also owns content-based native-artifact admission: a complete
Darwin graph is selected as one unit, otherwise the original Android ELF graph
is selected as one unit. C++/ObjC++ owns only operations that require ART/HWUI/Skia/Metal
ABI or platform object access. A C++ phase may not own a second lifecycle
state machine or call a shutdown callback that is not held by the Rust owner.

## Non-negotiable invariants

1. The production graphics build is GPU-only: Android `RecordingCanvas` and
   HWUI display lists are replayed into the Ganesh Metal drawable. There is no
   runtime CPU framebuffer or IOSurface upload fallback. Headless acceptance
   may run without a surface, but it must still execute ART and tear down
   normally.
2. `darwin-art-engine-sys` is the only raw ABI declaration crate. `AbiHeader`,
   process/config PODs, and callback signatures are defined once and checked
   by Rust layout tests plus the native static assertions.
3. `RuntimeSession` contains concrete typed engine/surface/provider owners;
   the FFI edge retains only a small synchronous lifecycle hook table whose
   context points back to that session. Rust remains authoritative for phase,
   lease counts, and teardown order; native callbacks only report the
   corresponding ART transition and cannot own resources. The
   owner slots are declared surface → provider → engine and are tested to
   preserve that reverse dependency order even on implicit Rust drop.
   Provider lease accounting uses the Rust `ProviderKind` enum internally;
   only the C callback adapter converts the versioned `u32` ABI tag.
   Host installation order is engine → provider → graphics. Shutdown removes
   the provider lease before the engine lease, keeps provider callbacks alive
   while `DestroyJavaVM` runs, then clears the provider hooks before the engine
   image is unmapped.
   The native provider shim does not keep a second process-wide refcount:
   `ProviderLeaseTable` reserves each 0→1 and 1→0 transition and invokes the
   C ABI only at those boundaries. C++ therefore performs stateless
   install/uninstall operations, so Rust and C++ cannot disagree about graph
   owner counts or filesystem authority identity.
4. `runtime_link_probe.cc` is an orchestration shell. ART setup, framework
   registration, ELF acceptance, and HWUI frame phases are separate cached
   native translation units with value-only phase inputs/outputs.
5. A native edit invalidates only the phase and its dependent link product.
   A provider-only edit must not recompile ART/HWUI foundation archives.
6. Every acceptance command reports cold compile count, warm compile count,
   link time, and the invalidated phase. A structural change is incomplete if
   it only moves code while the measured warm graphics build remains broad.

## Migration milestones

### M1 — concrete Rust owners (landed)

The loaded engine, provider bridge, and optional surface now live in typed
`darwin-art-runtime::RuntimeSession<E, P, S>` slots. Every subsystem lease is
bound to the session that issued it, so a stale lease from another runtime
cannot tear down a same-generation subsystem. `darwin-art-host` uses
borrowed views of that one session rather than maintaining a parallel owner
container. Rollback is covered for run failure, surface creation failure,
window close, and `DestroyJavaVM` failure; provider callbacks are invoked
outside the lease mutex to preserve reentrancy.

### M2 — phase modules (landed; extraction continues)

Split the process probe into small, separately cached phase objects. The first
landed boundaries are `runtime_process_options`, `runtime_shutdown_probe`,
`runtime_acceptance_phases` (network), `runtime_graphics_phase` (content
validation/presentation), `runtime_graphics_input` (pointer/frame dispatch),
and `runtime_jni_acceptance_probe` (DEX/JNI ABI matrix). Each phase receives a narrow JNI/value boundary and
returns a status snapshot; the heavy Android JNI/HWUI implementation remains
in its own object. Keep the existing JNI/ELF/HWUI acceptance unchanged while
the remaining framework/activity setup is extracted.

### M3 — real native graph (landed)

Ninja is now the normal path after the first cold bootstrap for the native
probe graph, with persisted per-object commands and depfiles. The graphics-JNI,
HWUI, and ICU foundation shell builders retain per-TU object/command stamps, so
repeated registrar and object-audit builds do not invoke clang++ for unchanged
sources. GraphicsJNI, HWUI, and all 458 ICU translation units are promoted to
first-class Ninja edges.
HWUI now builds from a stable patched shadow tree: pristine AOSP sources are
verified against the lock, then the tracked animation-pulse patch is applied
outside `_aosp`. The 81+5 command stamps therefore survive warm builds without
depending on ignored checkout edits. The remaining foundation work is the
ART-side bootstrap archive; it retains one cold fallback command for cache
population while the runtime object graph is promoted after a complete archive
exists.

### M4 — acceptance and removal (landed; measured follow-up continues)

Run ART DEX, recursive ELF/JNI, libc++, TLS, APK, Button, Metal, input/ripple,
and shutdown gates from the new owners. The legacy CPU presenter is now gone;
headless runs tear down without a surface and graphics runs use only the direct
Metal/HWUI loop. Remaining work is duplicate ABI/fallback removal and a
measured phase-local invalidation audit.

### M5 — ownership and orchestration decomposition (landed; follow-up optimization remains)

The first M5 slices are now in the tree. Locked AOSP file/archive download and
generated-source materialization live in
`art-bootstrap::source_materialization` instead of the command dispatcher.
The host's surface, engine, and provider owners also use one best-effort Rust
rollback path on every early return; a surface destroy or engine shutdown
error is recorded without skipping the remaining owners. This keeps cleanup
policy in Rust while leaving native ABI operations in `darwin-art-engine`.

Host run orchestration now lives behind the private `run.rs` module, while
bootstrap/probe orchestration is split by phase and emitted as independent
native graph products. Each extraction preserves the existing acceptance
gates and reports its native invalidation boundary; broad fallback rules are
rejected once the persistent cache is promotable.

The graphics boundary is now an explicit eighth native phase. Rust owns a
`RuntimeSession<..., GraphicsSession>` slot containing only an opaque C handle;
JNI global references, RenderNode, AnimationContext, and TimeLord remain
private to the native graphics session. Creation, owner-thread validation,
dispatch/pulse, close, and destroy are separate ABI operations, so graphics
cleanup closes during ART shutdown, finalizes the bound native session, and
destroys the opaque handle before the engine image is unmapped. Headless
engines may omit these optional symbols, while the graphics link exports and
audits them when a drawable is present.

The direct presenter is a separate cached native phase:
`runtime_graphics_gpu.cc` owns `RecordingCanvas`/RenderNode replay and Ganesh
Metal drawable submission, while `runtime_graphics_probe.cc` owns orchestration
and state transitions. Its source/header are explicit graph inputs, so a
presenter edit does not rebuild activity/resource/input phases. The Rust owner
keeps the opaque surface and graphics session alive across the whole frame
loop; C++ does not own a second lifecycle state machine. The surface is
transferred into `RuntimeSession` immediately after the ART process call
publishes it, before the host chooses the GPU/headless branch. An error in
that interval therefore cannot leave a foreign surface owner outside the Rust
shutdown transaction.

The host no longer consumes the complete raw `EngineSymbols` table. Provider
acquire/release/clear callbacks are converted directly into the Rust-owned
`ProviderBridge` (`darwin-art-runtime/src/provider_bridge.rs`); its lease
state machine remains in `provider.rs`. Raw symbol resolution and
graphics/surface dispatch remain crate-private. This removes the former
duplicate `ProviderHooks` capability from the engine crate and keeps provider
callback code independent of unrelated engine ABI entries.
The bridge also owns the early-drop safety boundary: clear is idempotent,
acquisition is closed after clear, and an unresolved live lease fails closed
instead of allowing the engine image to unload with a dangling callback.

The native graph emitter is split by ownership rather than kept in one command
file: `graph::inputs` owns invalidation/input stamps, `graph::foundation` owns
HWUI/GraphicsJNI/ICU family partitioning, `graph::cache` owns persisted
command/fingerprint promotion, and `graph::representative` owns small
independently cacheable production TUs. `darwin-art-xtask/src/main.rs` remains
the graph assembly/CLI boundary instead of owning those policies.
The graph identity/cache-manifest calculation is now isolated in
`graph::manifest`; `emit.rs` only assembles Ninja rules. The native graph audit
also queries every phase object and rejects a phase that directly inherits
another phase's source, so the invalidation contract is checked structurally
rather than inferred from mtimes. A successful warm audit reports
`invalidation=direct-source`.

The runtime owner teardown is now a separate `darwin-art-runtime::shutdown`
transaction, and the provider ABI vocabulary is isolated in
`provider_kind.rs`; `session.rs` contains owner/lifecycle accessors while the
lease algorithm remains in `provider.rs`. The ELF loader follows the same
boundary: `src/parser.rs` owns ELF header/program-header/dynamic-tag policy,
while `mapping.rs` owns mmap/mprotect/munmap, and `lib.rs` retains relocation,
symbol resolution, and lifecycle.
These are production module boundaries, not compatibility wrappers, and each
one is covered by the workspace/loader gates below.

The process ABI now accepts an additive `darwin_art_lifecycle_hooks_t` table.
The production host passes a Rust-owned table backed by `RuntimeLifecycle`, so
the native entrypoint no longer decides whether the production session is
bootstrapping, running, or shutting down. The C++ process-state object retains
only ART-specific handles/snapshots plus readiness gates for legacy direct C
callers that omit the table; it no longer contains a duplicate phase enum.
The hook table is synchronous, owner-thread-only, and remains borrowed until
the matching native shutdown returns.

The staging boundary records a content identity for the patched ART shadow
tree. Unchanged upstream sources and patches are not recopied on every
invocation, preserving depfile metadata so a canonical fallback can retain
the existing object set. The small `compat/darwin_art_abi_layout.cc` phase
provides the native half of the cross-language POD layout gate; Rust offset
tests remain the other half.

Detached Activity/PhoneWindow presentation uses the shared
`darwin_art_jni_scope::ScopedLocalFrame` boundary. This is intentionally a
small C++-side safety net: framework construction has many early-return paths,
so JNI locals are released even when resource/theme setup fails before the
Rust-owned runtime shutdown guard is reached.
The framework `AssetManager`/`ApkAssets`/`Resources` construction is now a
separate `runtime_app_resources` native object; the Activity/PhoneWindow
object is emitted by a separate Ninja rule. Activity.attach, ContextThemeWrapper,
PhoneWindow, and Theme setup now form a third `runtime_app_activity` object,
while DecorView/content recording remains in `runtime_app_presentation`.
Resource edits therefore invalidate only the resource object; lifecycle/theme
edits invalidate only the activity object; display-list edits invalidate only
the presentation object. The direct-APK and graphics link gates consume all
three narrow objects in dependency order and keep one ownership path.

The ART-side bootstrap now has a shared runtime-core cache boundary. Common
upstream runtime TUs and flavor-independent compat adapters compile from one
patched shadow and one include identity under `_build/runtime-common`; only
framework, libcore, and ICU graphics adapters remain flavor-specific. Graphics
therefore reuses the runtime archive's common objects instead of compiling a
second ART core, while a cache-identity mismatch forces a complete canonical
repopulation rather than mixing header ABIs.
The cache identity and flavor archive names live in the dependency-free
`darwin-art-build-contract` crate, shared by bootstrap and xtask, so this
boundary is not duplicated as stringly-typed build policy.

The Rust owner crates follow the same phase rule. ELF TLSDESC registry,
per-thread allocation, and unload sealing live in
`crates/darwin-art-elf-loader/src/tls.rs`, while the loader `lib.rs` retains
ELF parsing/relocation policy. Runtime provider callback ABI adaptation lives
in `crates/darwin-art-runtime/src/provider_bridge.rs`; the lease state machine
is isolated in `provider.rs`. These are separate Rust modules with unchanged
public contracts, so ownership policy and ABI wiring can evolve independently.

The framework JNI boundary follows the same rule: Choreographer/
DisplayEventReceiver, PropertyValuesHolder, Perfetto, and their registration
table live in `compat/darwin_framework_animation_natives.cc`; Android resource
registration (StringBlock, XmlBlock, ApkAssets, and VirtualRefBasePtr) lives in
`compat/darwin_framework_resource_registration.cc`, while the AssetManager
theme/attribute bridge and RenderNode support table each have their own
`compat/darwin_framework_asset_manager_natives.cc` and
`compat/darwin_framework_render_node_natives.cc` phases.
MessageQueue, EventLog, Log, Trace, and SystemClock live in
`compat/darwin_framework_system_natives.cc`; the remaining framework
implementations stay in `compat/darwin_framework_natives.cc`. Each phase is an
independent persisted native object in the ART bootstrap archive, so changing
animation, resource, or system registration no longer recompiles the other
implementation phases.

The libcore Linux compatibility archive applies the same boundary:
`compat/libcore_darwin_linux.cc` owns the generated JNI method table, file
descriptor JNI operations, and Java object/error translation;
`compat/libcore_darwin_linux_system_natives.cc` owns environment, identity,
password, diagnostic-string, and `sysconf` JNI semantics; and
`compat/libcore_darwin_linux_syscalls.cc` owns Android-flag translation,
descriptor I/O, and mmap. The standalone libcore gate archives all three
objects and verifies the split smoke/managed ABI path, so edits to system
metadata or syscall policy do not recompile the generated 135-entry JNI
registrar.

The character/primitive and ICU registrations are now isolated in
`compat/darwin_libcore_unicode_natives.cc`. The main
`compat/darwin_libcore_natives.cc` TU retains Linux/System/FileDescriptor and
OpenJDK registration orchestration; it calls the Unicode registrar before the
platform registration phases and the ICU registrar at the original final
position. The ICU adapter lock and native graph list both track the new object,
so edits to Unicode semantics no longer rebuild the libcore syscall/JNI
orchestration object.

The ART adapter now applies the same boundary to platform compatibility shims:
Palette, runtime-image, intrinsic-printing, HWASAN, and unwind stubs live in
`compat/darwin_runtime_platform_stubs.cc`. The graph-aware adapter is now
split into narrow phases: `darwin_runtime_elf_lifecycle.cc` owns image
publish/finalize and provider teardown, `darwin_runtime_elf_resolver.cc` owns
closed provider resolution, `darwin_runtime_native_loader.cc` owns trusted
discovery and NativeLoader open/close, and
`darwin_runtime_jni_registration.cc` owns RegisterNatives and trampoline
publication. The remaining `darwin_runtime_adapters.cc` is limited to
NativeBridge callbacks and JNI-on-load dispatch. These phases share only the
opaque `ElfLibrary` ABI header, so changing one policy does not recompile the
others.

The process-wide NativeBridge/NativeLoader hooks follow the same rule in
`compat/darwin_native_bridge_stubs.cc`. Only `NativeBridgeGetTrampoline2` and
the native-bridge pointer classifier remain graph-aware because they select
the per-image JNI trampoline owned by the ELF handle. All unsupported bridge
operations stay explicit closed-capability stubs.

JNI descriptor validation is now an independent `darwin_jni_shorty.cc` phase.
It owns only Android descriptor parsing and the current ART `JNIEnv` lookup;
the graph-aware proxy still owns class lookup, while
`darwin_runtime_jni_registration.cc` owns RegisterNatives and trampoline
publication. Its tiny headers are the only shared declarations between those
phases.

The proxy's environment/class lookup is now a separate
`darwin_jni_proxy_lookup.cc` phase. It is deliberately limited to resolving the
current ART environment and validating a class lookup against the graph-owned
fixture state. RegisterNatives, trampoline publication, and rollback now live
in `darwin_runtime_jni_registration.cc`, so changing lookup policy no longer
recompiles that transaction boundary.

The proxy's exception bridge is separately cacheable in
`darwin_jni_proxy_registration.cc`; it only translates `ThrowNew` into the
current ART environment. The registration transaction still shares the opaque
C++ `ElfLibrary` state; moving that graph-owned rollback state behind a
dedicated Rust session ABI is the next ownership milestone.

On the Rust side, the dynamic engine ABI is grouped into four private
capabilities (`ProcessSymbols`, `SurfaceSymbols`, `GraphicsSymbols`, and
`ProviderSymbols`) instead of one flat raw-symbol bag. `EngineSession` exposes
safe operations, while provider callbacks are owned by `ProviderBridge` in the
runtime crate; surface/graphics owners receive only their capability group.
This keeps raw function pointers inside the engine crate and makes future
`RuntimeSession` ownership moves independent of unrelated ABI groups.

Subsystem lease tokens are now owned by `RuntimeLifecycle` during production
shutdown. Host teardown asks the session to remove its newest subsystem and
receives only the checked subsystem identity; lease generations and session
identity never escape into `run.rs` or `gpu_loop.rs`. This removes a duplicate
host-side teardown state machine while preserving strict Graphics → Surface →
Engine → Provider ordering.

The process-call boundary is also now owned by the engine crate. `ProcessRequest`
keeps all five path strings, heap limits, callback contexts, and graphics
handle alive together, then materializes the raw `ProcessConfig` only inside
`EngineSession::run_request`. Host orchestration no longer constructs or stores
the raw FFI struct, so its unsafe lifetime surface is limited to one synchronous
engine call. `CallbackBindings` validates the required host/provider context
and callback pairing once at that boundary; the host no longer mutates a
partially initialized request with several independent raw-pointer calls.

The host arms `RuntimeShutdownGuard` immediately after the Rust session enters
bootstrapping, before opening the dynamic engine image. Engine/provider attach
rollback, graphics-session failure, request construction, and process-call
errors therefore all use the same owner-thread reverse shutdown path. During
normal shutdown the provider lease is removed first, `DestroyJavaVM` runs while
the engine image and provider callbacks are still valid, and only then are the
provider hooks cleared and the engine image dropped. No process-global
callback can outlive either its Rust bridge or its code image.

The guard now delegates the complete native close sequence to
`RuntimeSession::shutdown_native()` through the `NativeResource` trait. Host
code supplies only the four thin close/clear adapters; it no longer contains a
second graphics/surface/provider/engine ordering state machine. A Rust unit
gate records the exact surface → graphics → engine → provider-clear sequence,
including the stopped/empty postcondition.

Flavor-neutral probe compilation follows the same ownership boundary. The six
core probe TUs (`runtime_elf`, ABI, process state/options, shutdown, and frame)
are implemented by `native_probe/core.rs` and emitted under one
`_build/native-probes/core` dependency-fingerprinted cache. CPU runtime,
graphics runtime, and APK graph actions consume those objects instead of each
owning a copy of the compile policy. Graphics archive discovery and member
validation are likewise owned by `GraphicsRuntimeInputs`, leaving the link
routine responsible only for native compilation, linking, and symbol gates.

The headless runtime link follows the same split: `audit/runtime_link.rs`
assembles and caches native compile/link inputs, while
`audit/runtime_link_checks.rs` owns the exported-symbol and undefined-symbol
policy. Changing the acceptance census therefore does not alter the link
orchestration module or its native graph inputs.

The host composition is now one explicit `HostRuntime` alias in
`darwin-art-host/src/runtime.rs`. Surface, GPU-loop, and teardown modules no
longer repeat the four generic owner parameters, so changing the concrete
engine/provider/surface/graphics composition has one compile-time boundary.
The canonical core include search path is likewise constructed once by
`native_probe::core::core_probe_includes`; CPU, Graphics, and direct-APK
actions now fingerprint the same paths and consume the same object directory.
This is a real cache boundary, not only a type alias: a second flavor does not
recompile the six core TUs after the first flavor has populated the cache.

For foundation inner-loop work, `audit-runtime-graphics-link-incremental`
reuses successful source-pinned foundation products through per-script stamps,
then still reruns the graphics closure audit and final dylib/symbol checks.
The strict `audit-runtime-graphics-link` path remains unchanged and executes
every upstream build/managed acceptance gate; the incremental command is not a
replacement for release validation. The cache key for each foundation gate is
now limited to that gate's own lock/script and declared archive outputs; an
unrelated provider or probe archive cannot invalidate every foundation gate.
On the reference machine this reduced a post-cold incremental audit from
roughly 139 seconds to 12.5 seconds while retaining the final closure/link
checks.

Native ELF resource lifetime now crosses the same boundary through the Rust
`darwin-art-runtime` static library. `RuntimeNativeOwner` stores opaque graph,
image-registry, DSO, namespace, and provider slots with C drop callbacks and
releases them in descending order. `ElfLibrary` no longer manually unloads the
graph before provider teardown; `OpenNativeLibrary` registers each successful
resource and `CloseNativeLibrary` destroys the Rust owner. The handle returned
to ART is itself a Rust-registered owner token; C++ `ElfLibrary` is callback
context only, and stale or foreign tokens fail closed before dereference. The
callback ABI is owner-thread-only and fail-stop on a destructor error. This is
the first production path where Rust owns both the native ELF handle lifetime
and teardown rather than merely recording a C++ lease.

The large graph emission routine itself is in `graph::emit`; the CLI entrypoint
is now a 279-line command/test boundary. This is intentionally a source/build
boundary only: it does not pretend that the remaining framework-native
registration TU is already Rust-owned.

The ELF loader's C boundary follows the same separation: `ffi_types.rs` owns
the public versioned PODs, opaque handles, and callback signatures, while
`ffi.rs` owns pointer validation, panic containment, resolver adaptation, and
the exported operations. This keeps ABI edits reviewable without mixing them
with loader transaction code.

## Measurement gates

The migration is considered successful only when all of these are recorded on
the same machine:

| Change | Expected invalidation |
|---|---|
| Rust host CLI change | Rust host/CLI crates only |
| provider facade change | provider object, closure link, provider audit |
| framework/JNI phase change | framework phase + runtime link |
| DEX/JNI ABI acceptance change | `runtime_jni_acceptance_probe` + runtime link |
| graphics presentation/JNI phase change | `runtime_graphics_phase` + runtime link |
| graphics session-state/ABI change | `runtime_graphics_session` + graphics link |
| HWUI/Metal implementation change | `runtime_graphics_probe` + graphics link |
| AOSP foundation header change | affected foundation TU closure |

The current implementation has the probe/object cache, independently
addressable app/runtime/graphics phases, graphics-JNI/HWUI/ICU foundation
stamps, concrete RuntimeSession ownership, and the low-level ART
build/bootstrap split. M1–M5 are landed for the measured boundaries above.
The app bootstrap and presentation objects are now real
Ninja edges with content stamps: after their first build, a second graph run
reports `no work to do`, while a source touch invalidates only the affected
object and the graphics link. The same persistent graph keeps a direct warm
`target/debug/art-bootstrap build-runtime-graphics-bootstrap` invocation around
2.4s on the reference machine (the Ninja portion is about 0.06s), with
unchanged ART/HWUI objects reused. The warm graphics link audit intentionally
rechecks the full closure and remains separate. The host-side legacy CPU/IOSurface upload presenter has been removed:
headless runs tear down without allocating a surface, and graphics runs enter
only the direct Metal/HWUI loop. Follow-up optimization may further decompose
archive/link phases. The remaining large orchestration surfaces are
intentionally explicit: `graph::emit` assembles Ninja text,
`elf-loader/src/ffi.rs` owns the versioned C ABI façade, and the
graphics/runtime audit modules own acceptance commands. They are split only
when a narrow invalidation measurement proves a build-time benefit; such work
is no longer required for the ownership/build-graph migration contract.

Framework graphics runtime setup is now isolated in
`compat/darwin_framework_graphics_runtime.cc`: ICU/graphics initialization,
resource-runtime install/uninstall, Layoutlib registrar configuration, and the
51-class graphics registrar no longer share a translation unit with the
MessageQueue/Binder/AssetManager support table.

Framework system-property storage and its JNI registration are likewise
isolated in `compat/darwin_framework_system_property_natives.cc`. Changes to
the property map now invalidate only that adapter object and the final link;
the Binder, AssetManager, and RenderNode phases are no longer rebuilt for a
property-only change.

Binder/ServiceManager bridge state and registration are isolated in
`compat/darwin_framework_binder_natives.cc` as well. The main framework
registration TU now only sequences the phase; the process-local service
bridge and its native holder lifetime have one compilation and ownership
boundary.

The framework registration TU is therefore an orchestration boundary rather
than an implementation bucket: a change to AssetManager, RenderNode, Binder,
system properties, or the remaining framework methods invalidates only its
own native object and the final archive link.

The adapter TU manifest is shared by `darwin-art-build-contract` and consumed
by both the Cargo bootstrap and `darwin-art-xtask`; a new native boundary now
has one source-list owner instead of two independently maintained arrays.

The process-global provider callback bridge is now owned by
`darwin-art-runtime::ProviderBridge`. The host no longer contains an unsafe
provider context or a second callback state machine: `EngineSession` converts
the live image's three function pointers into the Rust bridge, and
`RuntimeSession` owns the boxed context until provider quiescence and engine
shutdown have completed. This makes provider implementation movement a Rust
boundary change rather than a host-composition change; the remaining native
surface is limited to the versioned callback ABI.

The armed shutdown guard is now also defined in `darwin-art-runtime`; the host
frontend only adapts `RuntimeError` into its CLI error type. Early returns in a
new host cannot bypass or duplicate the dependency-ordered owner transaction.

`ProcessRequest` now borrows the provider and optional graphics owner for its
entire synchronous ART call. Raw context pointers are materialized only while
the wire `ProcessConfig` is built, so the host cannot drop or replace an owner
while the native graph may still invoke its callbacks.

With the graph materialized, regenerating it and querying the graphics audit is
a true no-op (`ninja -d explain -n` reports `no work to do`). On the reference
machine the pinned Ninja target itself completes in about 0.06s; the direct
`target/debug/art-bootstrap` wrapper completes in about 2.4s because it
regenerates the graph and launches Ninja. The wrapper executes a cached
`darwin-art-xtask` binary directly and invokes Cargo only when xtask sources
change, so it no longer pays a nested `cargo run` cost on every native build.
This is distinct from the production dylib link/audit time above: the former
measures invalidation scheduling, while the latter intentionally rechecks the
full ABI closure.

The measured native invalidation check is explicit: changing only the
AssetManager adapter rebuilt `1/234` graphics-bootstrap objects; the next
unchanged run rebuilt `0/234`. The archive and Android acceptance results were
unchanged. The graph digest now contains 189 production inputs rather than
the entire `compat/` implementation directory; changing an unrelated
`compat/android_base_logging.cc` left that digest unchanged. Later phase
extractions must retain this same evidence.

## 2026-09-01 scheduler-separation progress

The active runtime goal is to match Android's scheduling ownership: ART's UI
Looper remains responsible for framework work, while AppKit input collection,
display-vsync production, and Metal presentation are independent producers or
consumers. `FrameClock` now produces bounded 16.666 ms display edges on a
dedicated Rust thread and the host consumes only the newest pending edge. This
prevents a blocked owner loop from replaying stale vsync bursts, but it does
not yet move ART work or AppKit calls off the owner thread.

The next boundary is intentionally measurement-first. Setting
`DARWIN_ART_DEBUG_FRAME_TIMING=1` records aggregate count/average/maximum
microseconds for the owner-thread MessageQueue pump, graphics pulse, and full
Looper-plus-pulse hand-off. The hand-off also reports counts over 16/50/500/1000
ms, making long-tail stalls visible instead of hiding them in an average. These
counters are reset and printed at the end of each graphics run as one
`DARWIN_ART frame-timing ...` line. Use that line with
the Chrome tab acceptance latency before introducing a dispatcher: the native
surface bridge currently rejects non-main-thread AppKit operations, so a direct
ART worker split would be unsafe until those calls are marshalled through a
main-thread executor.

Remaining scheduler work is therefore explicit: (1) add a non-blocking
AppKit/main-thread command queue for event collection and presentation, (2)
keep ART/UI and Binder work on its owner thread while commands are delivered
without a main-thread wait cycle, and (3) replace synchronous acquire-fence
waits with an asynchronous latch queue. Each step must retain the real APK
acceptance gates and report the frame-timing counters before/after.

The first instrumented AOSP acceptance (Calculator `2+3=5` and DeskClock Timer)
passed after the input mailbox boundary. Calculator reported
`looper avg=193us max=2925us`, `graphics avg=1003us max=28303us`, and
`handoff avg=3886us max=28531us` with one hand-off over 16 ms. DeskClock
reported `looper avg=232us max=2806us`, `graphics avg=2267us max=330600us`, and
`handoff avg=39915us max=331517us` with three over 16 ms and one over 50 ms.
These values are a baseline for the upcoming dispatcher/fence slices, not a
claim that the ART owner thread is already independent.

The AppKit boundary is now executable. Surface lifecycle calls made from a
non-main ART owner are synchronously marshalled to the AppKit main queue, and a
new `darwin_art_appkit_pump_events` callback lets the host main actor service
NSApplication before a Window exists. `run()` now launches the ART/JavaVM/
GraphicsSession lifetime on a named `darwin-art-ui-owner` worker and keeps the
calling main thread in the AppKit pump. The worker still owns the Android
Looper and dispatches all framework work; the main actor never calls JNI.

The worker topology passed the unchanged AOSP Calculator (`2+3=5`) and
DeskClock Timer graphics acceptance after rebuilding the debug host. This is
the first real owner-thread split, but Metal GPU begin/end and fence waits are
not yet fully asynchronous; the next slice must move those presentation
commands to the RenderThread-equivalent queue and remove caller-side waits.

Acquire-fence latching is now non-blocking for the normal unsignaled case.
`ASurfaceTransaction_apply` probes fences with a zero-timeout check and moves
transactions with pending producers to a bounded (maximum eight) latch-worker
set. The original transaction is immediately reusable/deletable, while the
deferred copy retains its buffers and controls until the existing composition
and callbacks complete. If the bounded set is saturated, the implementation
falls back to synchronous application to preserve ordering. The graphics link
and AOSP Calculator/DeskClock acceptance both pass with this path enabled;
long-tail timing must be remeasured on Chrome before considering the fence
stage complete.

## 2026-09-01 scheduler-separation progress (FIFO latch follow-up)

The latch workers now use one process-lifetime FIFO queue rather than one
detached thread per transaction. This preserves SurfaceFlinger transaction
order when a producer fence is still pending: once one transaction is queued,
later transactions join the same queue even when their own fence is already
signalled. The queue is bounded to eight pending/active entries; saturation
uses the synchronous path as an explicit correctness-preserving backpressure
fallback. The public `ASurfaceTransaction` is cleared immediately after the
deferred copy takes ownership, and the worker releases retained controls only
after composition and callbacks finish.

Validation after the FIFO change: graphics bootstrap, fast graphics-link audit,
and `cargo build -p darwin-art-host` all pass. Real AOSP acceptance remains
green (`Calculator=2+3=5`, `DeskClock=Timer`, common path
`HWUI+SurfaceFlinger+Metal`). Chrome tab graphics acceptance also passes with
10 composed target states and the real tab-switcher/tab-grid views. With
`DARWIN_ART_DEBUG_FRAME_TIMING=1`, that run measured
`looper count=13104 avg=660us max=395428us`,
`graphics count=2063 avg=2235us max=2141265us`, and
`handoff count=11 avg=261289us max=2141640us` (6 over 16ms, 3 over 50ms,
1 over 500ms, 1 over 1s). The acceptance proves no functional regression, but
the long-tail handoff still prevents declaring Chrome tab performance solved;
the next slice must profile the first-frame startup stall and move the
remaining Metal acquire/presentation waits onto the RenderThread-equivalent
queue.

## 2026-09-01 scheduler-separation progress (AppKit scanout handoff)

Product HWC scanout now uses `darwin_art_surface_present_async` from the ART
owner. The bridge keeps the synchronous present ABI for smoke/CPU callers, but
the product path submits a single latest-wins AppKit command protected by a
surface mutex and request generation. A request arriving during a blit causes
one trailing main turn, so the last frame is not lost while a slow main actor
cannot accumulate an unbounded queue. Destroy marks the surface closing before
its main-queue handoff; requests after that point fail closed. Direct GPU
command-buffer completion is tracked separately from the main-actor IOSurface
blit, avoiding cross-thread reuse of the old completion slot.

The frame pulse no longer drains `MessageQueue.next()` synchronously a second
time immediately after posting vsync. Choreographer work is left for the next
owner-Looper turn, matching Android's callback-to-Handler ordering and keeping
the RenderThread-equivalent pulse independent of arbitrary UI message bursts.
With the latest dylib, Chrome tab graphics acceptance passes with 10 composed
states and real tab-switcher/tab-grid views. The measured graphics pulse is
`count=2048 avg=672us max=3992us`; this is the relevant scanout/frame path and
is no longer multi-second. The complete owner handoff still reports
`max=2165411us` because the UI Looper itself can spend about 2.16s draining a
startup burst (`looper max=2165057us`). This is now isolated as Android UI work,
not AppKit/Metal backpressure. AOSP Calculator (`2+3=5`) and DeskClock Timer
acceptance remain green after the change. The next scheduler slice should
profile that Chrome startup message burst and avoid unnecessary per-frame JNI
reflection, while preserving the current Android Looper ownership semantics.

## 2026-09-01 scheduler-separation progress (display-paced surface sync)

`ActivateCurrentHostSurfaces` no longer walks WindowManager/SurfaceView state on
every short owner-Looper poll. It now runs only when a display-vsync was
delivered, immediately before scanout, so the Android Looper can service input
and Binder messages without repeated JNI reflection. The normal framework
ownership remains unchanged: ViewRoot/View traversal still owns layout and
SurfaceFlinger still owns buffer latching.

Latest Chrome acceptance remains green (`target-states=10`, real tab-switcher
and tab-grid views). Timing with the new dylib is
`looper count=15332 avg=415us max=2168788us`,
`graphics count=2044 avg=379us max=43637us`, and
`handoff count=11 avg=257435us max=2169199us`. The average fast path improved
again; the single ~2.17s maximum is still a startup UI message burst rather
than AppKit scanout. AOSP Calculator (`2+3=5`) and DeskClock Timer continue to
pass. The next step is to identify that specific startup message batch and
move only non-UI work (Binder/service setup or native polling) off the UI
Looper, without reducing the Android message-delivery contract.

## 2026-09-01 scheduler-separation progress (native callback stall audit)

Slow-frame instrumentation on a real Chromium APK isolated the remaining
long-tail from Java `Handler.dispatchMessage` and Metal scanout. The same
guest `ALooper` callback thunk (registered with `ident=0` by Chromium's native
watcher) occupied the ART owner thread for roughly 0.4--2.16 seconds; the
public acceptance still passed with ten composed tab states. Android's
`ALooper` contract invokes an opaque callback on its registering Looper
thread, so dispatching this callback to an arbitrary helper thread would break
sequence affinity, callback lifetime/remove ordering, and `ALooper_forThread`
semantics. The callback body therefore remains on the owner thread pending
fd-level blocking-syscall attribution.

The host-only helper remains bounded at 128 responses for compatibility: a
trial reduction to 32 changed Chrome's native watcher progress and failed the
real tab-grid acceptance, so it is not safe to ship as a generic policy. The
public `ALooper_pollOnce` behavior and callback return/remove semantics remain
unchanged. The next diagnostic slice is to trace the registered fd's
non-blocking flags
and callback-internal read/IPC duration; if a broker operation loses
`O_NONBLOCK`, fixing that provider is the Android-compatible remedy. A fresh
Chrome acceptance and the Calculator/DeskClock acceptance must remain green
before moving any callback work off the UI Looper.

## 2026-09-01 scheduler-separation progress (fd contract verified)

The follow-up Chrome run records every native watcher registration with
`status_flags=0x802`: the broker-preserved `O_NONBLOCK` bit is present on the
read and write endpoints. Slow-I/O instrumentation also produced no broker
`read`, `recv`, or pipe-read operation over 100 ms, while the same
`ALooper` callback still occupied the owner thread for approximately 0.4--2.15
seconds. This rules out a lost non-blocking flag or a host socket wait as the
source of the long tail; the callback body is executing a large Chromium
native task/IPC batch on its sequence-affine Looper thread.

The implementation therefore keeps Android callback affinity and does not
introduce a heuristic helper-thread dispatch. The next implementation slice
must add a cooperative batch boundary at the native watcher/MessagePump layer
(or move a proven non-UI service registration to its own Looper), then compare
frame latency and target-state acceptance. Any change must retain the current
`O_NONBLOCK`/callback-return semantics and keep Calculator, DeskClock, and
Chromium acceptance green.

## 2026-09-01 scheduler-separation progress (AppKit wait removed from owner)

The callback-stack sample identified an additional host-side stall: the ART
owner spent roughly 3.2 seconds of a 4-second sample in
`darwin_art_surface_pump_events -> RunOnMainSync -> __DISPATCH_WAIT_FOR_QUEUE`.
This was not Android UI work; it was duplicate AppKit event pumping from the
owner while the process main actor was already running
`darwin_art_appkit_pump_events()`.

The surface bridge now exposes a worker-safe atomic `close_requested` query.
All owner-loop slices use that query plus a bounded sleep, while the main
actor alone consumes NSEvents and sends them into the existing input mailbox.
The legacy `darwin_art_surface_pump_events` entry point remains available for
main-thread smoke callers, but no product owner path synchronously dispatches
to AppKit. A post-change `sample` shows no `RunOnMainSync` or
`__DISPATCH_WAIT_FOR_QUEUE` frames in `CrBrowserMain`; the owner samples are
instead split between `nanosleep` and the normal Android `ALooper_pollOnce`
path. Chrome tab graphics acceptance and AOSP Calculator/DeskClock acceptance
remain green after the change. The remaining long-tail is now confined to
the sequence-affine native Looper callback itself and requires a Chromium
task-batch boundary rather than host-thread migration.

## 2026-09-01 scheduler-separation progress (native drain fairness experiment)

An experiment that reduced the host native-drain budget from 128 callbacks to
one callback per `pump_main_looper()` turn was reverted after the Chromium
acceptance failed to select a real tab-grid card. This confirms that the
current Chromium watcher setup depends on draining multiple ready callbacks
before returning to the Java/UI queue; a generic count reduction is not an
Android-compatible fairness policy. The production budget remains 128, while
the owner-side AppKit wait stays removed. Any further latency reduction must
be implemented at a known Chromium task-runner batch boundary (or by giving a
proven non-UI registration its own sequence), not by moving opaque callbacks
or truncating `ALooper` progress heuristically.

## 2026-09-01 scheduler-separation progress (post-fix measurement)

With the owner-side AppKit wait removed and the production native-drain budget
restored to 128, the latest Chrome run reports
`looper avg=431us max=2117948us` and `graphics avg=403us max=4757us`.
The Metal/SurfaceFlinger path therefore remains within a frame-scale budget;
the approximately 2.12-second tail is still a single sequence-affine native
watcher/task batch. Chrome acceptance passes with ten composed states and real
tab-grid views, and Calculator/DeskClock remain green. Further improvement
requires identifying the Chromium task-runner boundary inside that callback;
the host scheduling split itself is now verified.

## 2026-09-01 scheduler-separation progress (Looper JNI lookup cache)

`DispatchDueMainMessages` now caches the five framework classes and their
method/field IDs in the `GraphicsState` session and releases the global class
references during shutdown. This removes repeated `FindClass`/
`GetMethodID` work from every owner turn without changing queue ordering,
sync-barrier handling, or callback affinity. After rebuilding, Chrome
acceptance passes with real tab-grid views; timing is
`looper avg=397us max=2127060us`, `graphics avg=393us max=6211us`.
Calculator and DeskClock remain green. The long tail is unchanged and is
therefore confirmed to be inside one native Chromium task batch, not JNI
reflection or Metal scanout.

## 2026-09-01 scheduler-separation progress (owner scheduling priority)

The ART owner pthread now requests macOS `QOS_CLASS_USER_INTERACTIVE`, which
matches the latency intent of Android's main/UI thread while leaving AppKit on
its independent main actor. The change does not migrate callbacks or alter
Looper sequence affinity. A rebuilt run passed Chrome tab acceptance with 11
composed target states and Calculator/DeskClock acceptance; timing was
`looper avg=307us max=2127961us`, `graphics avg=103us max=1849us`.
The multi-second tail remains a single native Chromium callback, so the next
step is still task-runner boundary identification rather than more host
priority tuning.

## 2026-09-01 scheduler-separation progress (Chromium callback target)

The debug callback trace now records registration identity, callback address,
caller address, fd status flags, and an optional vtable target. In a real
Chromium run, fd `1073742850` uses the same stripped `libchrome.so` thunk at
file offset `0x7ab7c60`; its object-vtable slot resolves to
`libchrome.so+0x7ab7fbc`. Disassembly shows that target reading the registered
fd and then dispatching a virtual native message-pump method. The slow samples
(`~0.4--2.16s`, with substantial CPU time) therefore represent a large
sequence-affine Chromium task/IPC batch, not a host socket wait.

The vtable memory probe is now opt-in through
`DARWIN_ART_DEBUG_CALLBACK_VTABLE=1` in addition to
`DARWIN_ART_DEBUG_SLOW_FRAME=1`; ordinary debug runs only log opaque callback
metadata and cannot dereference an arbitrary APK `data` pointer. The callback
continues to execute on its registering Android Looper thread. Moving it to a
generic helper would violate Android callback ordering and TLS/Looper
semantics. The next implementation boundary is consequently Chromium's
native message-pump/task-runner (or a proven non-UI process registration),
with Calculator, DeskClock, and Chrome acceptance as the regression gate.

## 2026-09-01 scheduler-separation progress (eventfd counter semantics)

The Bionic socket broker now models eventfd state explicitly instead of using
the datagram queue as the counter. Each descriptor owns a 64-bit counter and a
single readiness token: writes accumulate (with overflow and zero-write
validation), ordinary reads return and clear the accumulated value, and
`EFD_SEMAPHORE` reads decrement by one. Poll paths re-arm the token only while
the counter remains nonzero. This preserves the Android eventfd contract while
preventing one wake write per packet from artificially extending a Chromium
native watcher turn.

The broker probe now covers accumulated writes and semaphore reads and passes
under ASan/UBSan/TSan. A rebuilt graphics link audit and the AOSP
Calculator/DeskClock acceptance pass. Chromium's first run missed the real
tab-grid card (the test is known to be timing-sensitive); the immediate retry
passed with real `MotionEvent` input, GPU child surfaces, and nine composed
target states. The remaining long-tail measurement must be repeated with the
callback-vtable probe enabled to determine whether a single callback body is
still multi-second after wake coalescing.

## 2026-09-02 scheduler-separation progress (post-eventfd measurement)

With `DARWIN_ART_DEBUG_SLOW_FRAME=1`, `DARWIN_ART_DEBUG_CALLBACK_VTABLE=1`,
and frame timing enabled after the eventfd change, the native callback still
measured `414ms`, `635ms`, and `2053ms` on fd `1073742850`; the aggregate was
`looper avg=314us max=2060817us`, `graphics avg=98us max=1020us`, with one
sample over one second. This confirms wake-token coalescing is correct but
does not shorten a single Chromium callback body. The safe next slice is
inside Chromium's task-runner/IPC implementation (or a proven separate
process registration), not in generic Looper callback migration. The debug
acceptance's tab-grid assertion remains timing-sensitive; a normal immediate
retry passed with nine composed states, and the AOSP Calculator/DeskClock
gate remains green.

## 2026-09-02 scheduler-separation progress (per-turn native fairness)

The host-only native Looper drain now tracks a registration generation and
allows each `(fd, generation)` callback at most once per host turn. Different
ready descriptors still run in the same turn; a descriptor that remains
readable is deferred to the next turn. Public `ALooper_pollOnce` behavior and
callback affinity are unchanged. This is the Android-compatible fairness
boundary that avoids the earlier unsafe global budget reduction.

After rebuilding, the broker ASan/UBSan/TSan probe and the AOSP
Calculator/DeskClock gate pass. Chromium acceptance passes with real tab-grid
input and ten composed target states. Debug timing remains dominated by the
same single callback (`401ms`, `617ms`, `2135ms` samples;
`looper avg=311us max=2142506us`, `graphics avg=104us max=988us`), so the
fairness boundary prevents repeated wake starvation but cannot preempt the
callback's internal task batch. Further latency work must target Chromium's
own task-runner/IPC boundary.

## 2026-09-02 scheduler-separation progress (process attribution)

Slow-callback diagnostics now include the host PID and Android process name.
The latest Chrome run attributes the multi-second callback to
`pid=57081 process=<main> fd=1073742850` (`401ms`, `617ms`, and `2018ms`
samples; `looper max=2018068us`). Child services were independently observed
with different PIDs and names, including
`org.chromium.chrome:privileged_process0`, and their Looper callbacks use
separate native images/registrations. This proves the existing service process
boundary is active and the browser-side callback is not child work that can be
moved across processes.

The callback remains on the browser's registering Looper as required by
Android. The next implementation slice is therefore a browser-side
MessagePump/IPC batch boundary (or a provider-level non-blocking fix), with
process attribution retained as a regression invariant.

## 2026-09-02 scheduler-separation progress (single-batch frame boundary)

`pump_frame()` no longer calls `DispatchDueMainMessages()` a second time after
the owner turn has already drained the Android MessageQueue/ALooper. The
steady host path is now explicitly `input -> pump_main_looper -> one frame
pulse`; a frame pulse publishes framework vsync and enqueues the asynchronous
Metal scanout without re-entering an opaque Chromium watcher callback in the
same edge. The test-only synthetic-key path was updated to keep the same
ordering.

This is a scheduling boundary, not a preemption mechanism: the callback still
runs on its registering ART Looper thread, as Android requires, and a single
Chromium callback body may still be multi-second. The architecture is already
split at the safe boundaries (AppKit main actor, ART owner, HWUI RenderThread,
SurfaceFlinger/latch, and latest-wins Metal scanout). The next host change is
an `OwnerReactor` with coalesced POD state (`input_ready`, latest vsync,
close/shutdown) and an ALooper wake token, replacing the remaining 2 ms sleep
poll without moving `JNIEnv`, `GraphicsSession`, or `SurfaceSession` across
threads. A cooperative batch limit inside Chromium's own MessagePump remains
a separate APK-side integration problem.

Verification for this change: `cargo fmt --all -- --check`,
`cargo test -p darwin-art-host` (7/7), graphics bootstrap (192/192 native
steps), graphics-link audit PASS, AOSP Calculator `2+3=5` and DeskClock Timer
PASS, and Chromium real tab-switcher/tab-grid acceptance PASS with 10 target
states and GPU `GLES+ANGLE+Graphite+Dawn+MoltenVK+AHB+SurfaceFlinger+Metal`.

## 2026-09-02 scheduler-separation progress (OwnerReactor wake path)

The first `OwnerReactor` slice is now wired through the existing ABI. The
surface's AppKit-owned input mailbox accepts a pointer-free owner-wake hook;
mouse/key/cancel enqueue signals the Android `ALooper_wake` token after the
mailbox mutex is released. The owner loop uses a bounded (maximum 16 ms)
native Looper wait before draining input and publishing one latest display
pulse, with a sleep fallback for older engine images. The wait is normalized
to timeout/wake/callback versus poll-error and keeps the `(fd, generation)`
fairness turn accounting intact.

No `JNIEnv`, `GraphicsSession`, or `SurfaceSession` crosses threads. The wake
hook is explicitly cleared during graphics shutdown before the AppKit surface
is destroyed, preserving the existing teardown ordering. This removes the
blind 2 ms owner sleep from the normal graphics image and lets native fd or
physical AppKit input wake the owner immediately; it does not preempt a single
opaque Chromium callback body.

Verification: graphics-link audit PASS; AOSP Calculator/DeskClock acceptance
PASS; Chromium acceptance passed on an immediate retry and with timing enabled
(`looper max=1.982 s`, `graphics max=286 us`, input-to-pulse `p50=36 ms`, one
startup sample over 1 s). One first Chromium run missed the timing-sensitive
tab-grid assertion and was rerun unchanged successfully with 10 target states.
The next slice is to coalesce FrameClock latest-vsync into the same wake state
and then measure physical-click latency separately from synthetic acceptance.

## 2026-09-02 scheduler-separation progress (coalesced FrameClock wake)

FrameClock now uses a bounded `latest + pending` POD state instead of a
single-slot channel. Each display edge replaces the previous timestamp; the
worker emits an owner `ALooper_wake` only on the `pending=false -> true`
transition. The ART owner clears that bit while consuming one latest edge, so
an owner stalled by application work neither replays stale vsyncs nor causes a
wake storm. The wake token contains only the opaque session handle and the
wake ABI function; no `JNIEnv`, `GraphicsSession` methods, or Surface state is
used from the display-clock thread.

The graphics session records its owner `ALooper` on the first owner-thread
looper pump. Cross-thread wake validates the live session under the session
mutex, copies the looper pointer, and calls only `ALooper_wake`; normal close
and finalization ordering remains owner-thread-affine. The existing bounded
16 ms wait remains the fallback when an older engine image lacks the optional
wake symbol.

Verification: `cargo fmt --all -- --check`, `cargo test -p darwin-art-host`
(7/7), graphics-link audit PASS, and AOSP Calculator (`2+3=5`) plus DeskClock
Timer acceptance PASS. Physical-click latency measurement and a cooperative
Chromium MessagePump batch boundary remain the next slices; this change only
coalesces display scheduling and does not preempt one opaque Chromium callback.

## 2026-09-02 scheduler-separation progress (owner wait cadence)

The remaining host loops that waited in fixed 2 ms slices now use the same
bounded 16 ms owner wait as the native Looper path. Native fd readiness,
AppKit input wake, and the coalesced FrameClock wake interrupt that wait;
older engine images still use the bounded sleep fallback. This removes the
busy polling cadence that remained in pointer-sequence and visible-window
loops while preserving the Android ordering `input -> Looper -> frame pulse`.

Verification: Rust tests 7/7, graphics-link audit PASS, and the AOSP
Calculator/DeskClock acceptance PASS. A fresh Chrome tab-switcher/tab-grid
acceptance also passed with ten composed target states and
`GLES+ANGLE+Graphite+Dawn+MoltenVK+AHB+SurfaceFlinger+Metal`; timing was
`looper max=2.179 s`, `graphics max=289 us`, input-to-pulse `p50=37 ms`.
The remaining multi-second sample is the known single Chromium native
callback body, which cannot be preempted safely by the compatibility layer.

## 2026-09-02 scheduler-separation progress (physical ingress telemetry)

The v2 pointer and physical-key dispatch paths now report
`ingress_latency_us` when `DARWIN_ART_DEBUG_INPUT_LATENCY=1` is enabled. The
value is computed from the AppKit-produced Android uptime timestamp to the
owner-thread dispatch boundary, while the existing host-side
input-to-framework-pulse metric continues to cover the dispatch-to-vsync
handoff. Synthetic packets intentionally carry a zero timestamp and are not
included in this ingress metric, keeping physical and acceptance evidence
distinct.

The telemetry is observational only: it does not alter event ordering,
coalescing, or Android `MotionEvent`/`KeyEvent` timestamps. A repeatable
physical-click run through the Manager surface is still required to publish a
real percentile baseline; the direct APK acceptance remains synthetic by
design. Rust tests and the graphics-link audit pass after adding the logs.

## 2026-09-02 scheduler-separation progress (Chromium callback attribution)

A fresh debug acceptance again maps the long-tail work to the browser process
guest watcher (`ident=0`, main-process callback target) rather than to
SurfaceFlinger, Metal scanout, or a child service. The privileged and
sandboxed child services continue to register separate Looper callbacks and
PIDs. A run with callback-target tracing completed the real tab-switcher and
tab-grid flow with ten composed states; a preceding attempt missed the
timing-sensitive grid transition and was rerun unchanged, matching the known
startup race rather than a scheduling regression.

The Manager desktop session currently lacks macOS Accessibility permission, so
the new physical ingress telemetry has not yet been populated with a real
click percentile. Synthetic acceptance remains separate and continues to pass;
once permission is restored, `ingress_latency_us` plus the existing
input-to-pulse samples will provide the physical baseline without changing the
runtime path.

## 2026-09-02 scheduler-separation progress (edge-triggered input wake)

The AppKit surface mailbox now edge-triggers its owner wake: the first
pointer/key packet after an empty mailbox signals `ALooper_wake`, while
additional queued MOVE packets only update the bounded mailbox. The pending
bit is cleared while the shared event mutex holds an empty check, so a producer
cannot enqueue in the clear window and lose its wake. Pointer and key queues
share the bit; draining either queue to empty arms the next producer edge.

This reduces wake writes and owner wakeups during high-rate trackpad motion
without coalescing Android gesture boundaries (DOWN/UP/CANCEL remain ordered
packets). The callback remains the pointer-free `ALooper_wake` operation and
does not move AppKit, JNI, or GraphicsSession state across threads.

Verification: graphics-link audit PASS, Rust host tests 7/7, AOSP Calculator
(`2+3=5`) and DeskClock Timer acceptance PASS, and Chrome tab-switcher/tab-grid
acceptance PASS with ten composed target states and the full
`GLES+ANGLE+Graphite+Dawn+MoltenVK+AHB+SurfaceFlinger+Metal` path.

## 2026-09-02 scheduler-separation progress (bounded pointer mailbox)

High-rate AppKit pointer MOVE packets now replace only the current queue tail
when that tail is also MOVE. This bounds motion backlog and keeps the owner
looper focused on the newest coordinates; DOWN, UP, and CANCEL packets are
always retained in order, so Android gesture boundaries and click semantics do
not change. The edge-triggered wake bit remains protected by the same event
mutex, avoiding both wake storms and lost notifications.

Verification after the mailbox change: graphics-link audit PASS, `cargo fmt`
PASS, all darwin-art-host tests PASS (7/7), and Chrome's physical acceptance
flow reached the real tab-switcher and tab-grid with ten composed target states
and the complete GPU capability path. The Manager still needs macOS
Accessibility permission before physical-ingress percentile telemetry can be
collected; synthetic acceptance is intentionally kept as a separate gate.

## 2026-09-02 scheduler-separation progress (AppKit actor quantum)

The AppKit actor now pumps `NSApplication` in 2 ms quanta (below one quarter
of the 60 Hz display interval) while the ART owner waits independently on its
native Looper wake. This bounds the actor-side observation delay for a physical
NSEvent without moving JNI, ViewRoot, or GraphicsSession work onto AppKit.
Debug frame timing also emits bounded `appkit-pump` snapshots because APK
processes intentionally use `_exit` after the owner loop and cannot emit a
final outer-actor summary.

The latest Chrome run reached the real tab-switcher and tab-grid (10 composed
states, full GLES/ANGLE/Graphite/Dawn/MoltenVK/AHB/SurfaceFlinger/Metal path).
Actor snapshots were approximately 2.5--2.9 ms average with an 88 ms maximum
across the participating processes; owner metrics remained the known
Chromium-callback-limited tail (`handoff` p95 about 2.06 s). This separates
AppKit observation cost from the still-unpreemptible guest native callback.

## 2026-09-02 scheduler-separation progress (Looper callback fairness)

`ALooper_pollOnce` now returns immediately after dispatching one ready native
callback, matching Android's one-callback poll boundary instead of invoking
every ready descriptor in one call. The host-side native drain is additionally
bounded to eight callbacks per owner turn, allowing Java MessageQueue and
Choreographer work to regain scheduling opportunities during startup bursts.
Callback affinity is unchanged: callbacks still execute on the Looper thread
that registered them, so this is a fairness boundary rather than an unsafe
thread migration.

Verification: graphics-link audit, Rust host tests (7/7), AOSP Calculator and
DeskClock acceptance, and Chrome tab-switcher/tab-grid acceptance all pass.
The slow-frame run still reports the known single guest callback tail
(approximately 2.07 s); the change removes callback bursts but cannot
preempt a callback that is already executing.

## 2026-09-02 scheduler-separation progress (callback sequence attribution)

Native callback logging now includes the macOS Mach thread ID alongside the
ART owner ID. A Chrome run showed the long 0.4 s and 2.0 s guest callbacks on
the ART owner itself, while a separate watcher callback ran on another native
thread. This confirms that the tail is not caused by AppKit or Metal scanout;
it is guest work registered on the Android main Looper and therefore cannot be
moved wholesale to a worker without violating callback sequence affinity.

The safe boundary is consequently the one already implemented: AppKit input,
display clock, and HWC scanout remain independent, while owner-thread Java and
registered native callbacks retain Android ordering. General Chrome acceptance
after the attribution build passed with ten composed target states and the
complete GPU capability path; the earlier debug run's SIGTRAP was an isolated
startup/termination failure and was followed by a clean pass.

## 2026-09-02 scheduler-separation progress (owner callback boundary)

Thread-ID attribution confirms the long callback is genuinely executing on
the ART owner Looper thread, not on the AppKit actor: the owner and callback
Mach IDs match for the 0.4 s and 2.0 s guest watcher callbacks. A separate
watcher registration uses another native thread. Therefore the runtime keeps
Android sequence affinity and does not migrate callbacks by heuristic.

The implemented safe boundary is now explicit: one native callback is returned
per `ALooper_pollOnce`, with an eight-callback host-turn budget, while AppKit
input, the display clock, and Metal HWC scanout remain independent. This
prevents callback bursts from adding extra backpressure, but cannot interrupt
one guest callback already running on the Android owner. Clean Chrome
acceptance after rebuilding the native dylib passed with ten target states.
