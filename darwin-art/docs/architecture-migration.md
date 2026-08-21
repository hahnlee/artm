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
low-level ART platform/ARM64/interpreter products and `runtime_bootstrap.rs`
owns patched-source/runtime archive orchestration. `native_probe_commands.rs`
owns the cached filesystem/network/HWUI/graphics probe builders. These modules
share command/context helpers but not ABI declarations or lifecycle policy.

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
   operation, while lease counts and teardown order remain in Rust.
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
`darwin-art-runtime::RuntimeSession<E, P, S>` slots. `darwin-art-host` uses
borrowed views of that one session rather than maintaining a parallel owner
container. Rollback is covered for run failure, surface creation failure,
window close, and `DestroyJavaVM` failure; provider callbacks are invoked
outside the lease mutex to preserve reentrancy.

### M2 — phase modules (landed; extraction continues)

Split the process probe into small, separately cached phase objects. The first
landed boundaries are `runtime_process_options`, `runtime_shutdown_probe`,
`runtime_acceptance_phases` (network), `runtime_graphics_phase` (content
validation/presentation), and `runtime_graphics_input` (pointer/frame
dispatch). Each phase receives a narrow JNI/value boundary and
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

### M4 — acceptance and removal

Run ART DEX, recursive ELF/JNI, libc++, TLS, APK, Button, Metal, input/ripple,
and shutdown gates from the new owners. Then remove the legacy CPU presenter,
duplicate ABI declarations, broad bootstrap stamps, and type-erased ownership
from production paths.

## Measurement gates

The migration is considered successful only when all of these are recorded on
the same machine:

| Change | Expected invalidation |
|---|---|
| Rust host CLI change | Rust host/CLI crates only |
| provider facade change | provider object, closure link, provider audit |
| framework/JNI phase change | framework phase + runtime link |
| graphics presentation/JNI phase change | `runtime_graphics_phase` + runtime link |
| HWUI/Metal implementation change | `runtime_graphics_probe` + graphics link |
| AOSP foundation header change | affected foundation TU closure |

The current implementation has the probe/object cache, graphics-JNI/HWUI/ICU
foundation stamps, concrete RuntimeSession ownership, and the low-level ART
build/bootstrap split. M1–M3 are landed for the measured boundaries above;
M4 is now the acceptance sweep and removal of the remaining legacy presenter,
duplicate ABI declarations, and broad fallback edges.
