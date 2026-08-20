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
discovers the persisted runtime object commands and emits one
`native_cached_cpp` edge per object (214 objects in the current ART flavor),
plus a deterministic archive edge. The graphics bootstrap/link edge and the
four independently owned probe objects remain separate products. The old
bootstrap command remains the source of truth for the cold-start transition;
the per-object graph is the source of truth for subsequent incremental builds.

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
