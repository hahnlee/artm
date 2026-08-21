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
| 2026-08-21 | warm/no-op | `audit-native-graph.sh` | 0 | 1.99 | 0 | 1035 |
| 2026-08-21 | warm/no-op | `audit-runtime-graphics-link-fast` | 0 | 4.62 | 0 | 11 |

The first safe no-op criterion is unchanged output plus a successful exit on a
second consecutive warm run. A useful future criterion is a measured wall-time
budget, but the repository does not yet have enough stable samples to choose
one. A cache hit must never be inferred from a short run alone; retain the
reported signals and the source/toolchain revision with each baseline.

## Post-workspace sample

After consolidating the core Rust crates and adding the ABI/runtime/xtask
packages, a warm `cargo check --workspace -v` measured **0.16 s**, exit 0,
compiled signal 0, cached signal 19. This is recorded separately from the
original baseline so future graph changes can be compared without mixing
different workspace shapes.

## Known bottlenecks

The current graph is dominated by the production dylib link and serial full
acceptance gates rather than by Rust type-checking:

- `probes/runtime_graphics_probe.cc` still owns the RenderNode/Metal bridge;
  the smaller phase/input/shutdown objects are separate cache products.
- `crates/art-bootstrap/src/main.rs` combines source materialization, patching,
  native compilation, archive/link orchestration, audit, and probes.
- The full graphics audit intentionally reruns serial upstream source/ABI
  gates; use `audit-runtime-graphics-link-fast` for the inner loop.
- The workspace still contains independent `tools/*` Cargo projects outside
  the main Rust graph, so provider-specific edits should use their local audit
  before the full workspace gate.
- The remaining broad bootstrap stamps do not affect the promoted warm graph,
  but their phase-local invalidation still needs an explicit mutation test.

These are observations to validate with the baseline, not permission for the
measurement script to change the graph. The intended follow-up is to finish
the phase-local invalidation audit and remove the remaining broad bootstrap
stamps while keeping runtime ownership and lifecycle state in Rust behind a
narrow unsafe FFI boundary. Each structural change should preserve this
measurement contract.
