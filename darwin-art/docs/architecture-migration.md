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
decisions. C++/ObjC++ owns only operations that require ART/HWUI/Skia/Metal
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
   the only callback retained at the FFI edge is the one-shot ART shutdown
   operation, while lease counts and teardown order remain in Rust. The
   owner slots are declared surface → provider → engine and are tested to
   preserve that reverse dependency order even on implicit Rust drop.
   Provider lease accounting uses the Rust `ProviderKind` enum internally;
   only the one-shot C callback adapter converts the versioned `u32` ABI tag.
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

### M5 — ownership and orchestration decomposition (in progress)

The first M5 slices are now in the tree. Locked AOSP file/archive download and
generated-source materialization live in
`art-bootstrap::source_materialization` instead of the command dispatcher.
The host's surface, engine, and provider owners also use one best-effort Rust
rollback path on every early return; a surface destroy or engine shutdown
error is recorded without skipping the remaining owners. This keeps cleanup
policy in Rust while leaving native ABI operations in `darwin-art-engine`.

The remaining M5 work is deliberately incremental: move host run orchestration
out of the public host facade, then split the remaining bootstrap/probe
orchestration by phase. Each extraction must preserve the existing acceptance
gates and report its native invalidation boundary; moving code without
narrowing a rebuild edge is not considered progress.

The graphics boundary is now an explicit eighth native phase. Rust owns a
`RuntimeSession<..., GraphicsSession>` slot containing only an opaque C handle;
JNI global references, RenderNode, AnimationContext, and TimeLord remain
private to the native graphics session. Creation, owner-thread validation,
dispatch/pulse, close, and destroy are separate ABI operations, so graphics
cleanup occurs before the Metal surface and ART/provider teardown. Headless
engines may omit these optional symbols, while the graphics link exports and
audits them when a drawable is present.

The host no longer consumes the complete raw `EngineSymbols` table. Provider
acquire/release/clear callbacks are converted directly into the Rust-owned
`ProviderBridge` (`darwin-art-runtime/src/provider_bridge.rs`); its lease
state machine remains in `provider.rs`. Raw symbol resolution and
graphics/surface dispatch remain crate-private. This removes the former
duplicate `ProviderHooks` capability from the engine crate and keeps provider
callback code independent of unrelated engine ABI entries.

The native graph emitter is split by ownership rather than kept in one command
file: `graph::inputs` owns invalidation/input stamps, `graph::foundation` owns
HWUI/GraphicsJNI/ICU family partitioning, `graph::cache` owns persisted
command/fingerprint promotion, and `graph::representative` owns small
independently cacheable production TUs. `darwin-art-xtask/src/main.rs` remains
the graph assembly/CLI boundary instead of owning those policies.

The staging boundary records a content identity for the patched ART shadow
tree. Unchanged upstream sources and patches are not recopied on every
invocation, preserving depfile metadata so a canonical fallback can retain
the existing object set. The small `compat/darwin_art_abi_layout.cc` phase
provides the native half of the cross-language POD layout gate; Rust offset
tests remain the other half.

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
build/bootstrap split. M1–M4 are landed for the measured boundaries above;
M5 is in progress. The app bootstrap and presentation objects are now real
Ninja edges with content stamps: after their first build, a second graph run
reports `no work to do`, while a source touch invalidates only the affected
object and the graphics link. The same persistent graph keeps a direct warm
`target/debug/art-bootstrap build-runtime-graphics-bootstrap` invocation around
2.4s on the reference machine (the Ninja portion is about 0.06s), with
unchanged ART/HWUI objects reused. The warm graphics link audit intentionally
rechecks the full closure and remains separate. The host-side legacy CPU/IOSurface upload presenter has been removed:
headless runs tear down without allocating a surface, and graphics runs enter
only the direct Metal/HWUI loop. Remaining M5 work is measured archive/link
phase decomposition and removal of duplicate ABI declarations and broad
fallback edges.

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
