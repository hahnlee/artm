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

## Current measurement

On the development machine, the materialized graphics graph currently
contains 172 cached C++ translation-unit edges and one archive edge. A clean
graph execution completed the 172-object parallel compile and archive in
75.25s (`ninja -j8`); the immediately repeated execution reported `no work to
do` in 0.01s. The remaining link/audit command is intentionally measured
separately because it includes the production dylib link and symbol gates.

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
