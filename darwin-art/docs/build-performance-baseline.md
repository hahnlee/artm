# Build-performance baseline

This document defines the measurement contract for the current build graph.
It is deliberately separate from the build implementation: the measurement
tool invokes the existing commands unchanged and writes command output only to
a temporary directory.

## Measurement contract

From the repository root, run:

```sh
tools/measure-build-performance.sh --all
```

The default invocation measures only `cargo check --workspace`. Selectors are
available when a full native measurement is not appropriate:

```sh
tools/measure-build-performance.sh --cargo-check
tools/measure-build-performance.sh --graphics-bootstrap
tools/measure-build-performance.sh --audit-graphics
```

The selected commands are:

| Label | Existing command |
| --- | --- |
| `cargo-check` | `cargo check --workspace` (measured with Cargo's output-only `-v`) |
| `graphics-bootstrap` | `cargo run -p art-bootstrap -- build-runtime-graphics-bootstrap` (measured with output-only `-v`) |
| `audit-graphics` | `cargo run -p art-bootstrap -- audit-runtime-graphics-link` (measured with output-only `-v`) |

For each command the tool records exit status, `/usr/bin/time -p` wall time,
and heuristic counts of compile/build versus fresh/cache messages. The counts
are diagnostic signals, not a correctness gate: some native shell gates emit
no cache message even when their artifacts are reusable. Use
`--keep-output` when the raw command logs are needed for investigation.

The measurement is safe to repeat. It does not delete `_build`, `target`, or
any source tree, and it does not alter compiler flags, source sync, generated
artifacts, or gate behavior. Only the temporary log directory is removed on
exit (unless `--keep-output` is supplied).

## Baseline table

Record one row per command and state whether the run was warm (all existing
artifacts/cache available) or intentionally cold. Do not compare cold and
warm rows as if they were the same workload.

| Date | State | Command | Exit | Wall (s) | Compiled signal | Cached signal |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 2026-08-21 | warm/no-op | `cargo check --workspace` | 0 | 0.08 | 0 | 13 |
| 2026-08-21 | warm/no-op | `build-runtime-graphics-bootstrap` | 0 | 4.06 | 0 | 12 |
| 2026-08-21 | warm/no-op | `audit-runtime-graphics-link` | 0 | 58.88 | 0 | 11 |

The first safe no-op criterion is unchanged output plus a successful exit on a
second consecutive warm run. A useful future criterion is a measured wall-time
budget, but the repository does not yet have enough stable samples to choose
one. A cache hit must never be inferred from a short run alone; retain the
reported signals and the source/toolchain revision with each baseline.

## Known bottlenecks

The current graph is dominated by large native translation units and serial
shell gates rather than by Rust type-checking:

- `probes/runtime_link_probe.cc` combines ART bootstrap, JNI, View, RenderNode,
  input, Metal presentation, and shutdown responsibilities.
- `crates/art-bootstrap/src/main.rs` combines source materialization, patching,
  native compilation, archive/link orchestration, audit, and probes.
- ICU, HWUI, Skia/graphics JNI, and related AOSP objects are rebuilt through
  shell actions without one persistent depfile-backed native graph.
- The workspace contains a small number of production crates while many
  independent `tools/*` Cargo projects are outside that graph.
- The existing gates have useful artifact checks but do not expose a stable
  compiled-versus-reused object count.

These are observations to validate with the baseline, not permission for the
measurement script to change the graph. The intended follow-up is to introduce
an explicit Ninja/depfile native graph and persistent object cache, then move
runtime ownership and lifecycle state into Rust behind a narrow unsafe FFI
boundary. Each structural change should preserve this measurement contract.
