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
3. `RuntimeSession` must contain concrete typed engine/surface/graph owners;
   type-erased cleanup callbacks are a rollback bridge, not the final
   production ownership API.
4. `runtime_link_probe.cc` is an orchestration shell. ART setup, framework
   registration, ELF acceptance, and HWUI frame phases are separate cached
   native translation units with value-only phase inputs/outputs.
5. A native edit invalidates only the phase and its dependent link product.
   A provider-only edit must not recompile ART/HWUI foundation archives.
6. Every acceptance command reports cold compile count, warm compile count,
   link time, and the invalidated phase. A structural change is incomplete if
   it only moves code while the measured warm graphics build remains broad.

## Migration milestones

### M1 — concrete Rust owners

Move the loaded engine, provider graph, and optional surface into typed
`darwin-art-runtime` owner structs. `darwin-art-host` becomes a thin loop over
borrowed handles. Verify rollback for run failure, surface creation failure,
window close, and `DestroyJavaVM` failure.

### M2 — phase modules

Split the process probe into `runtime_framework_phase`, `runtime_elf_phase`,
`runtime_graphics_phase`, and `runtime_shutdown_probe`. Each phase receives a
small `#[repr(C)]` input and returns a value snapshot; globals are removed from
the phase modules. Keep the existing JNI/ELF/HWUI acceptance unchanged.

### M3 — real native graph

Make Ninja the normal path after the first cold bootstrap. Promote foundation
archive objects (ART, ICU, HWUI, graphics JNI) to the same content-addressed
per-TU cache already used by the probe objects. Keep one cold fallback command
only for cache population.

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
| HWUI/Metal phase change | graphics phase + graphics link |
| AOSP foundation header change | affected foundation TU closure |

The current implementation has the first probe/object cache and lifecycle
scaffolding, but M1–M3 are not claimed complete until these concrete owner and
timing checks pass.
