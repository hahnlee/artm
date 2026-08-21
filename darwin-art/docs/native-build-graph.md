# Native build graph transition

`darwin-art-xtask` owns the transition to a persistent Ninja graph. It keeps
the existing bootstrap command as the cold-start fallback, then promotes a
materialized runtime archive to real per-translation-unit edges using the
fingerprints and depfiles produced by `art-bootstrap`.

Generate the graph with:

```sh
cargo run -p darwin-art-xtask -- native-graph --out _build/native-graph/build.ninja
ninja -f _build/native-graph/build.ninja -n graphics-bootstrap
```

On a clean checkout with no native fingerprints, the runtime target is a
single bootstrap edge. After that first successful build, graph generation
discovers the persisted runtime and graphics object commands and emits one
`native_cached_cpp` edge per object (214 objects per current ART flavor), plus
one deterministic archive edge per archive. The four independently owned
probe objects remain separate products. The old bootstrap command remains the
source of truth for the cold-start transition; the per-object graph is the
source of truth for subsequent incremental builds.

The input digest includes the graph generator version, repository paths, and
contents of the tracked production bootstrap/fixture/proxy closure. Each
digest gets a persistent `_build/native-cache/<digest>/` manifest and stamp.
Native objects, depfiles, fingerprints, and archives stay at stable
`_build/runtime-*` paths so direct bootstrap commands and Ninja share the same
cache. The graph declares the archive and every object as outputs; deleting an
object or the archive therefore causes Ninja to rebuild the missing product.
Compiler depfiles add transitive AOSP header edges after the first compile.
Unrelated acceptance probes do not invalidate the runtime archive, and each
filesystem/network/HWUI/graphics probe has its own narrow direct-input edge.

The remaining migration is to apply the same persisted-command promotion to
the graphics/ICU/HWUI foundation archives. That can proceed without changing
the Rust runtime or Android acceptance contract.

The graph now exposes that boundary as `graphics-foundation` (with
`foundation` as a short alias). It declares the stable HWUI static/APEX
archives and Android GraphicsJNI/registrar/force-loaded products. A complete
command-stamped object set is promoted to ordinary Ninja TU/archive edges;
otherwise the corresponding shell builder remains the safe cold/bootstrap
edge. The builders remain responsible for source locks, generated manifests,
ABI gates, and per-TU command stamps; changing a tracked foundation source or
lock invalidates only the foundation closure and not the ART runtime archive:

```sh
ninja -f _build/native-graph/build.ninja -n graphics-foundation
cargo run -p art-bootstrap -- build-graphics-foundation
```

GraphicsJNI promotion is active after its command/depfile materialization: 61
JNI objects plus the registrar are compiled in parallel and archived directly,
with the force-loaded object as a separate link edge. HWUI remains on the
shell-owned fallback until its pinned source manifest is reconciled and all 81
plus 5 APEX command stamps are available; no source-lock failure is hidden by
the graph.

The first foundation step is now landed in the two largest graphics shell
builders. `build-android16-android-graphics-jni.sh` keeps one command stamp per
registrar/JNI translation unit under `_build/android-graphics-jni/objects/`,
and `build-android16-hwui-static-foundation.sh` uses the same scheme under
`_build/hwui-static-foundation/objects/`. The stamp includes the compiler
identity-independent command line, all flags, and the source digest; an
interrupted compile cannot be reused because its object has no matching stamp.
Archives are still recreated from the cached objects, so the public archive
paths and member-count gates do not change. On the development machine the
graphics-JNI object audit was 74s cold and 11s warm (with no source changes),
while the registrar-only path was 5.0s cold and 3.4s warm. HWUI foundation
source identity remains separately locked; when that upstream manifest is
reconciled, its 81 objects will use the same cache without weakening the
source pin.

## Current measurement

On the development machine, the materialized graphics graph currently
contains 172 cached C++ translation-unit edges and one archive edge. A clean
graph execution completed the 172-object parallel compile and archive in
75.25s (`ninja -j8`); the immediately repeated execution reported `no work to
do` in 0.01s. The remaining link/audit command is intentionally measured
separately because it includes the production dylib link and symbol gates.

The first promoted foundation slice is GraphicsJNI: after the shell builder
materialized its command stamps, Ninja rebuilt 61 JNI objects plus the
registrar in parallel in 9.69s, and the next archive query reported `no work
to do` in 0.01s. The HWUI 81+5 object set still correctly falls back to its
source-locked shell edge until its manifest is valid and materialized.

The process probe is now split at the first stable boundary: environment and
fixture-mode validation lives in `probes/runtime_process_options.cc` and is a
separately cached object. The ART orchestration object therefore only changes
when the process lifecycle or JNI/HWUI implementation changes; adding a new
fixture selector recompiles the options object and relinks the probe instead of
recompiling the full ART TU.

Shutdown/finalizer ordering is similarly isolated in
`probes/runtime_shutdown_probe.cc`. It consumes a value snapshot of the
process acceptance state, performs the owner-thread ART/DSO teardown, and is a
separate cached object; the main probe only exports the ABI wrapper and builds
that snapshot.

Graphics presentation now follows the same boundary. The JNI/widget
validation and `present_content`/interactive-root orchestration live in
`probes/runtime_graphics_phase.cc`, while pointer/frame dispatch lives in
`probes/runtime_graphics_input.cc` and RenderNode/Metal replay remains in
`probes/runtime_graphics_probe.cc`. The linker consumes all three objects, so
a change to framework validation or input handling does not recompile the
HWUI/Skia implementation and a graphics implementation change does not rebuild
the orchestration phases.
