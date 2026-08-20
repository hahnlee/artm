# Native build graph transition

`darwin-art-xtask` is the first, non-invasive step toward a persistent Ninja
graph. It does not replace any existing build or audit script. It records the
current graphics bootstrap inputs, emits a deterministic digest, and exposes a
Ninja target that invokes the existing bootstrap command unchanged.

Generate the graph with:

```sh
cargo run -p darwin-art-xtask -- native-graph --out _build/native-graph/build.ninja
ninja -f _build/native-graph/build.ninja -n graphics-bootstrap
```

The generated graph is intentionally conservative. It only covers the
graphics bootstrap action first; graphics link/audit and the larger ICU/HWUI
object graph will be added after this action's input set and stamp behavior
are validated. The old command remains the source of truth during migration.

The input digest includes the graph generator version, repository paths, and
contents of the tracked production bootstrap/fixture/proxy closure. Each
digest gets a persistent `_build/native-cache/<digest>/` directory containing
the input manifest, a bootstrap stamp, and an `outputs/` directory. The
generated action sets `DARWIN_ART_NATIVE_OUTPUT_ROOT` to that `outputs/`
directory. This scopes the runtime graphics archive, objects, dependency files,
and fingerprints to the content digest while leaving the existing `_build`
layout unchanged for direct bootstrap commands. The graph declares both the
stamp and runtime archive as outputs, so deleting the redirected archive cannot
produce a stale successful Ninja hit. Unrelated acceptance probes do not
invalidate this graph.

A future graph action can replace the shell command with depfile-backed object
rules without changing the Rust runtime or acceptance contract.
