# Runtime identity and acceptance review — 2026-09-11

## Rebuild identity

After provider signal diagnostic fix `3245c8ac`, the official command
`cargo run -q -p art-bootstrap -- audit-runtime-graphics-link-incremental`
completed with exit 0. Log: `/tmp/astra-review-graphics-rebuild.log`.
Closure: registrar=51, fake-symbols=0, host-icu=0, host-fmt=0, CoreText=0.
Generated graph: `_build/native-graph/build.ninja`, inputs=472, digest
`63eeef4f07e5105ddd9207734e172089a6eebc9fc7e133940581d24556849743`.
The older `_build/native-graph.json` is not this invocation's graph identity.

`bash tools/prepare-darwin-art-host.sh target/debug/darwin-art-host development`
completed with exit 0. `xcrun vtool -show-build` confirms minos 11.0/sdk 12.0;
`codesign --verify --strict` succeeds. Final SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| `_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib` | `c767a68f1f8ecef797ba773754e7163ba78e86d3facd74f21386da0fff96c0a5` |
| `target/debug/darwin-art-host` | `eea5168a5cb4c391eb1e286e557ec6b5d11f27cc178740061adeb32cc46dfa6d` |
| `tools/android-bionic-pthread-provider/src/provider.cc` | `d7c099959357c574623cd1560b07aad414108e0d8d710339068b0e838f5582ec` |

## Focused checks

- `bash tools/android-bionic-pthread-provider/audit-signal-cycles.sh`: exit 0,
  100 real guest sigaction/semaphore/mask cycles. The resulting executable
  repeated 20 times passed all 2,000 cycles. Repeated guest mask queries hung
  before diagnostic getenv was cached; nested trampoline restoration passes
  after that narrow change. This is not proof of fixing the game's GC stall.
- `bash tools/android-bionic-pthread-provider/audit-foreign-threads.sh`: exit 0;
  ASan, UBSan and TSan each pass 64 rounds with actual signal delivery, stack
  bounds, foreign ownership, stale ESRCH and transactional reset.
- `sem_abi_stress.cc` linked with the real provider using
  `clang++ -std=c++17 -pthread -O1 -g -fsanitize=address,undefined`: exit 0.
- `bash tools/test-hwui-jni-attachment.sh`: exit 0, owned=100, borrowed=1,
  explicit-detach=1, ART-TLS-exit=0.
- `cargo test -q -p darwin-art-runtime`: 28 passed, 0 failed.
- `git diff --check`: exit 0.

Shutdown source review confirms graphics/application quiescence, HWUI common
pool join before libcore/ELF unload, then caller detach before DestroyJavaVM.
The TLS owner detaches only its owned attachment and skips an already-explicit
detach. Focused tests above are not a new full APK shutdown soak.

## Highest-priority open acceptance

Blue Archive first real game frame and meaningful input remain unverified.
The last measured nativeRender stalls in IL2CPP GC acknowledgment; all measured
game scanouts are black (including lifecycle-000004.png, mean=0/std=0).
The previous installed base/split and ELF directory is currently absent, so no
Blue Archive run can be attributed to the rebuilt hashes above. Restore the
unchanged original APK inputs, then repeat nativeRender return, scanout pixels
and actual interaction checks. Do not substitute RC=0 or Unity initialization.

The JIT ledger's top-level status now separates already-completed default-on
JIT/CFI work from remaining per-method compiled evidence and app acceptance.
The separately edited `foundation.rs` was neither modified nor reverted by
this review; these artifacts reflect the current shared worktree, not a claim
of a clean-HEAD build.
