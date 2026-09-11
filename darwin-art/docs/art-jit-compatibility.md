# ART JIT compatibility completion ledger

## Target and completion contract

Target: the pinned AOSP ART ARM64 JIT's normal application execution feature
set, hosted natively on macOS. This is not a promise to compile methods that
upstream itself declines. AOT/zygote are not JIT features; do not confuse their
upstream exclusions with Darwin-only execution restrictions.

Completion requires removal of Darwin-only method/bytecode admission limits,
validated runtime-helper and reference ABI coverage, upstream-supported OSR
and optimizations, and successful unmodified application runs. Interpreter
fallback is a legitimate ART mechanism but is not evidence that a missing JIT
feature works. A successful compile is not proof of execution or correctness.

Keep unavoidable macOS address-space/W^X differences in host-specific helpers.
Do not remove safety gates before their protected contracts have been ported.

## Coverage index (reviewed 2026-09-11)

The family rows retain the scope of their focused execution tests; broad corpus
PASS counts do not independently prove every compiled method or optimization.
The execution-policy, unwind and application rows below supersede the original
September 5 status. Dated checkpoints preserve historical results, not current
blockers. In particular, successful Unity initialization is not game UI proof.

| Family | Current evidence | Completion work |
| --- | --- | --- |
| Integer arithmetic | IJFD arithmetic, division/remainder, conversions, narrowing, comparisons, shifts and edge-value differential suites pass | Broader upstream ART test corpus |
| Control flow | Branches, packed/sparse switches, scalar loops, phi values and compiled suspend checks pass | Irreducible/exceptional-loop upstream corpus |
| References | Managed/quick/JNI/runtime-helper return and argument contracts pass under CC | Broader intrinsic and boot-method corpus |
| Fields | Resolved and unresolved instance/static, normal/volatile, primitive/reference fields pass exact exceptions, recovery and CC | Cross-thread stress beyond current VarHandle publication suite |
| Arrays | All primitive/reference allocation, length, get/set, covariance, bounds/type/negative-size and OOME recovery pass with CC | Multi-dimensional and upstream fuzz corpus |
| Objects/types | Allocation, constructors, casts, instanceof, class/string roots and clinit failure/concurrency pass | Remaining reflection/proxy/compiler corner cases |
| Invokes | Static/direct/virtual/interface/super/native/range, unresolved access/failure/recovery, polymorphic and custom/CallSite calls pass | Remaining MethodHandle combinators and upstream invoke corpus |
| Exceptions | Typed/catch-all handlers, nested finally, rethrow/replacement, compiled unwinding and pending state pass | Upstream exhaustive exception-edge corpus |
| Synchronization | Instance/static monitors, synchronized methods, recursion, inflation, contention and exceptional release pass | Larger contention and ownership stress |
| GC | Baker/ConcurrentCopying, moving roots, allocation pressure/OOME, stack maps, fields/arrays and post-GC compiled calls pass | Long-running concurrent stress and weak/reference-processing matrix |
| Deoptimization | Speculative virtual/reference and mixed-wide deopt plus OSR reference moving-GC deopt pass | Materialization and all environment reconstruction kinds |
| OSR | Explicit and automatic IJFD OSR, exceptions, live references, moving GC and deopt pass | Nested/irreducible loop and production workload coverage |
| Inlining/intrinsics | Pinned source audit classifies 36 specialized-HIR + 217 HInvoke entries; specialized-HIR, Unsafe, String, Math, CRC32, Memory, Reference, boxing and typed System.arraycopy families execute in interpreter/baseline/optimized with CC where reference-bearing; byte/int ordinary-call fallbacks are included | Pinned upstream compiler corpus and inlining heuristics |
| Root literals | Native64 root-slot addresses and compressed payloads pass class/string/MethodType-style use under CC | Exhaustive metadata/native literal inventory |
| Native unwind metadata | September 6 deterministic-CFI checkpoint records untouched AOSP 137-cfi optimized-JIT local/context/thread/remote success and integrated Darwin unwindstack providers | Preserve this regression under current build identity; do not generalize its coverage to arbitrary native binaries |
| Execution policy | Production JIT is default-on since September 6; Darwin bytecode/method-shape admission gates are removed. September 11 focused audit covers compiled GC, VarHandle, invokes, OSR/deopt and shutdown | Extend method-level execution evidence beyond the existing focused matrix; broader corpus interpreter/jit summaries are not an independent optimized lane |
| Original apps | September 11 Calculator 2+3=5, DeskClock Timer and Chromium external HTTPS content have real execution/rendering evidence. Thread-local sigchain fix eb90d025 lets unchanged Blue Archive render its actual Notice UI; a synthetic Cancel tap dismisses it and transitions to the game's resetting-data screen, verified in before/after scanouts | Preserve acceptance on each rebuilt identity. Physical-button-click automation, Confirm/download, login and gameplay remain unverified; synthetic input is not physical input |

## Validation rules

- For each family, compare interpreter and compiled results, including failures.
- Prove the intended method actually acquired and executed compiled code.
- Include null, signed/unsigned boundaries, high field offsets, register spills,
  GC during calls, and concurrency where applicable.
- Record build/runtime identity; stale binaries do not count as verification.
- Add upstream ART tests where the local runner supports them; list gaps rather
  than replacing them with a claim of universal compatibility.
- 100% completion means no outstanding family above, not a percentage inferred
  from the number of smoke tests. Record performance separately from correctness.

## Next implementation boundary

Blue Archive's native GC acknowledgment/first-frame blocker is fixed by keeping
sigchain masks thread-local (`eb90d025`): Darwin's process-wide sigprocmask was
overwriting other threads' masks. The independent fault probe now preserves the
uninvolved main thread's mask while a real condition-variable worker completes
100 GC cycles and 100 targeted kernel SIGBUS deliveries. The rebuilt runtime
returns from nativeRender and renders actual game UI.

Meaningful **synthetic** input is now demonstrated: Cancel at Android (514,504)
receives DOWN/UP consumed=1 with an 81,174us hold; Notice disappears and the game
shows "Resetting the game data...". The run exits 0 with 1,230 normal nativeRender
returns. Evidence: `/tmp/bluearchive-cancel-input.log`, scanouts
`_build/bluearchive-acceptance-20260911/cancel-input-000006.png` (before) and
`cancel-input-000008.png` (after), both visually reviewed. The read-only verifier
`tools/verify-bluearchive-cancel-acceptance.sh` checks event ordering, pre-click
Notice OCR and a nonblank post-click frame without Notice. Physical click
automation remains blocked by the UI tool's inability to select the unbundled
host. Confirm was not pressed; download, login and gameplay are not claimed.

Continue method-level compiled execution/OSR/deopt/GC accounting separately from
the broader corpus's interpreter/jit summaries. Production JIT enablement and
the September 6 deterministic AOSP CFI provider port are completed historical
boundaries, not work that needs to be restarted.

- Checkpoint 1090 (2026-09-11): read-only verifier
  `verify-bluearchive-cancel-acceptance.sh` was run against the unchanged APK
  Cancel capture and passed. It validated ordered DOWN/UP, pre-click Notice OCR,
  nonblank post-click frame without Notice, nativeRender success, and recorded
  before/after hashes. The result is synthetic InputChannel acceptance only;
  physical click, Confirm/download, login and gameplay remain open.

- Checkpoint 1091 (2026-09-11): upstream corpus resume (parallel=4, first 20)
  found 19 resumed PASS and one real failure, `004-SignalTest`. Its interpreter
  expected-output passes, but the optimized lane installs compiled `Main.main`,
  hits two generated-code faults, then receives unexpected signal 6 and exits 1.
  This is now an explicit JIT/signal boundary blocker; no workaround or test
  exclusion has been added.

- Checkpoint 1092 (2026-09-11): upstream corpus ledger resume identity was
  hardened. Each result now records a content digest covering the host binary,
  graphics runtime, official native graph inputs, all boot `.art/.oat/.vdex`
  artifacts and bootclasspath JARs. Legacy records without this digest are
  re-executed instead of resumed; a limit-1 migration run passed.

Read this index and the latest architecture-migration entry when resuming.
Append dated evidence as work advances; keep this status table current.

## Active execution boundary — 2026-09-11

September 10's Chromium native-thread blocker has an implemented ownership
boundary: HWUI uses one C++ TLS attachment owner, borrows already-attached JNI
threads, and joins async workers before libcore/ELF unload and DestroyJavaVM.
The focused ownership test covers 100 owned workers, borrowed ownership and
explicit detach; September 11 Calculator/Chromium lifecycle runs report no ART
TLS-exit warning. Keep this regression and build identity checked. A global ART
ThreadExitCallback auto-detach remains an invalid replacement for ownership.

Corpus의 1,069 PASS/7 timeout 기록은 유효한 회귀 증거지만 interpreter와
jit 두 lane의 요약일 뿐 독립적인 optimized lane이나 개별 메서드의 compiled
실행을 증명하지 않는다. 이후 기능별 compiled execution/OSR/deopt/GC 증거
ledger를 별도로 채우고, 동일 runtime identity에서 변경 없는 Chromium,
calculator/clock/calendar, Blue Archive acceptance를 수행한다.

### Managed return ABI implementation in progress — 2026-09-04

Sol reviewed the complete boundary set. Compiler-only native decode/encode is
removed in source while runtime bridge adapters are being implemented as one
atomic change. Do not link/run a mixed compiler/runtime pair. Native JValue,
deoptimization contexts and runtime-helper results retain native pointers;
managed results and instrumentation saved reference GPRs use compressed32.
The post-HIR and bytecode gates remain until the rest of the backend is ready.
Update: runtime0046/0047/0048/0049 integrated; full acceptance now passes at
/tmp/art-managed-return-acceptance.log, including native JNI reference returns
and interpreter/compiled/interpreter JNI cycles. OSR execution, moving-GC and
actual instrumentation listener execution remain unverified by this suite.

### Root-literal next-step source audit — 2026-09-04

Confirmed in pinned `jit_patches_arm64.{h,cc}`: string/class/method-type
patch maps and literals hold uint32_t root-slot addresses, and PatchJitRootUse
narrows the native roots_data address. These are addresses of GcRoot slots,
not compressed references; promote them to native64 along with CodeGenerator
wrapper return types and the consuming Ldr X register. Keep loaded GcRoot
payloads compressed32. Code-generator consumers are VisitLoadClass,
VisitLoadString and VisitLoadMethodType.

Do not globally promote every Uint32 literal: boot-image heap objects should
be encoded as compressed values, whereas native metadata must remain native.
VisitLoadClass/VisitLoadString currently narrow object pointers before passing
them to DeduplicateBootImageAddressLiteral; those need explicit compression.
Audit LoadBootImageAddress's other callers separately before changing its API.
This entry records confirmed source defects, not completed fixes or tests.

### Exit-hook acceptance (implemented; tested 2026-09-04)

Implementation: probes/runtime_jit_exit_hook_acceptance.h. After correcting
RemoveMethod STW usage and code-retirement order (0051), full acceptance at
/tmp/art-retirement-acceptance.log passes compiled/native exit hooks with GC.
Stable compiled entry, empty optional frame, callback/collection counts and
caller results are checked. Raw quick-return upper bits are not independently
observed; compressed writeback is additionally source-reviewed in0049.

Original reviewed recipe follows:

Sol confirmed a testable compiled path without changing runtime debuggability
or the admission gate: stop JIT workers with ScopedJitSuspend, remove/recompile
the identity fixture with SetDebuggableCompilerOption(true), restore the
compiler option, and assert ContainsPc plus CodeInfo::IsDebuggable. Install a
non-trace MethodExited listener and EnableMethodTracing(needs_interpreter=false)
under ScopedThreadSuspension, instrumentation ScopedGCCriticalSection and
ScopedSuspendAll. Confirm the entrypoint stays at the same JIT address.

The handle-valued MethodExited overload filters identity/nativeReferenceIdentity,
requires an empty OptionalFrame (not an interpreter shadow frame), and runs GC
for nonnull returns while keeping expected/results in handles. Check both final
caller result and callback counters. Cleanup under the same STW scope.
DisableMethodTracing can invalidate all JIT code: put this test LAST and do not
require entrypoint retention after cleanup. CMS validates bridge/writeback but
not moving-root correctness. This is a test recipe, not passing evidence.

### Resume checkpoint — 2026-09-05

Unsafe HInvoke execution is no longer pending: 34 direct-Dex cases pass all
three execution phases with Concurrent Copying GC. Darwin's duplicate JIT
admission gates are removed. The newly exposed background-JIT crash was the
ARM64 JNI Baker mark-bit path dereferencing a compressed static declaring class;
0095 now decodes at that native boundary and preserves 64-bit Darwin jobject
handles until JNI decode. Ten consecutive full audits pass in
`/tmp/art-jit-jni-baker-stable-{1..10}.log`. Thread CPU accounting and native
Darwin ARM64 fatal register dumps are also implemented. Resume with executable
String HInvoke coverage, then arraycopy, math, CRC, Memory, Reference and boxing.

### String, arraycopy and Math HInvoke closure — 2026-09-05

- Patch 0096 separates compressed managed references from decoded native
  dereference addresses in String compare/equality/index/factory/getChars and
  generic System.arraycopy. Class/component/super metadata remains a managed
  reference until the exact field-access boundary; Baker loads and card marking
  continue to receive compressed references.
- All 25 pinned String-family entries now execute. Public wrappers cover
  compare/equality/indexOf and every StringBuilder/StringBuffer append/toString
  shape; a trusted boot DEX in java.lang covers getCharsNoCheck and the three
  package-private StringFactory methods. All phases pass with CC and exact null/
  bounds behavior in `/tmp/art-jit-arraycopy3.log`.
- Generic/reference and char System.arraycopy pass separate and overlapping
  copies, bounds/type failures and CC in interpreter, baseline and optimized
  code. The same clean log proves the optimized callsites executed.
- All 43 pinned Math entries now have executable evidence. The prior 14
  abs/min/max/signum specialized entries combine with a 29-entry HInvoke raw-bit
  matrix covering FMA, transcendental/logarithmic operations, rounding,
  multiplyHigh and copySign. multiplyHigh uses a no-desugaring trusted boot
  caller so D8 cannot replace the intrinsic with a compatibility backport.
- The fixture compiler now emits source/target 8 against Android 16's
  `core-for-system-modules.jar` and `android.jar`, rather than imposing the Java
  SE 8 API with `--release 8`. Modern Android Math APIs are visible and
  `MethodHandle.invokeExact(I)I` remains signature-polymorphic. The complete
  audit passes in `/tmp/art-jit-math3.log`. Resume with CRC through actual boot
  CRC32 callers, then Memory, Reference, boxing and arraycopy fallbacks.

### CRC32, Memory, Reference, boxing and fallback closure — 2026-09-05

- Corrected Android 16 CRC32's `@CriticalNative` ABI and decoded byte-array
  payload addresses only at the ARM64 native dereference boundary. Single-byte,
  array and direct-ByteBuffer vectors, positions and exceptions pass all tiers.
- Added actual native-address execution for all eight Memory peek/poke byte,
  short, int and long intrinsics, including deliberately unaligned addresses.
  Trusted boot fixtures now follow ART's initialized/visibly-initialized
  lifecycle and explicit-tier runs suspend background JIT workers.
- `Reference.refersTo()` exposed a real SIGSEGV at compressed heap address
  `0x1004001c`. Patch 0098 preserves compressed managed comparisons and Baker
  forwarding values while decoding only referent-field and lock-word native
  addresses. `Reference.get`, `refersTo` and `reachabilityFence` pass null,
  identity and moving-CC cases in interpreter, baseline and optimized code.
- Byte, Short, Character and Integer `valueOf` cover cached boot-image identity,
  cache-external allocation, unboxed values and moving GC in all tiers. AOSP's
  ordinary-call byte/int System.arraycopy fallbacks cover separate/overlapping
  copies and bounds failures alongside char/reference.
- Clean evidence is `/tmp/art-jit-fallback1.log`; DEX contracts are 52 classes/
  2,611 methods and 108 classes/3,024 methods. The source inventory remains 36
  specialized-HIR and 217 HInvoke entries.
- The locked ART `test/` archive is checksum-pinned and materialized: 1,138
  top-level test directories and 1,846 Java files. Next implement the generic
  unmodified `Main.main` runner and mechanically classify/run that corpus.

### Generic upstream runner and first 151 pure-Java tests — 2026-09-05

- The runner executes unmodified pinned AOSP `Main.main(String[])` sources in
  interpreter and explicitly installed optimized tiers, compares stdout/stderr
  byte-for-byte, initializes classes through AOSP ClassLinker lifecycle, and
  waits for spawned non-daemon threads. `DEX_LOCATION` packaging and pre-clinit
  output capture close tests 084, 086 and 087 without source edits.
- Exact pinned libcore native registration closes NativeBN, StrictMath/fdlibm
  and ICU-dependent tests through the real Android Java APIs. Locale test 092
  passes with the complete 10-method `libcore.icu.ICU` table.
- Test 109 exposed AOSP SmallPatternMatcher's low-4GB quick-ABI assumption;
  patch 0100 preserves compressed managed values while decoding C++ field
  accesses and encoding object returns. Test 153 then exposed the corresponding
  missing decode in Baker `art_quick_aput_obj`'s marking path. The latter is
  fixed in patch 0055 and passes concurrent WeakReference get/clear, allocation,
  moving CC, baseline compilation and OSR.
- AOSP Android 16 R8 is now source-control pinned rather than selected from the
  host SDK. Matching the upstream `javac -g -Xlint:-options -implicit:none`
  contract preserves NPE-producing array opcodes; test 201 consequently checks
  all built-in detail messages in both tiers. Hidden `jdk.internal.misc`
  callsites compile as boot references and resolve against the runtime boot
  class, allowing 2236 to exercise the actual Android Unsafe implementation.
- 151/426 structurally pure-Java tests currently pass exact output in both
  modes. The next item is `2284-regression-test-368984521-loop-opt`.
  `external-native` (37), native-source (24), bytecode (126), multi-source (19),
  custom-script (344), no-src-main (99), and missing-expected (63) remain
  separate, visible queues. The 100% goal is not complete; those queues,
  production apps and Mach-O unwind still remain.

### Pure-Java and multi-source corpus closure — 2026-09-05

- Direct JNI `Main.main(String[])` launch and exact AOSP argv semantics replace
  the earlier reflection wrapper. All 423 currently classified pure-Java tests
  pass byte-exact stdout/stderr in interpreter and optimized JIT modes.
- Patch 0101 routes ART method-trace output through the process-private `/data`
  filesystem capability. `545-tracing-and-jit` passes in both modes. The same
  run validates real trace start/stop alongside JIT activity.
- Libcore file creation now resolves guest private paths before OpenJDK
  open/fstat/close, and the Darwin Linux provider ABI implements `ftruncate`.
  `530-regression-lse` passes its RandomAccessFile, FileChannel.map and mapped
  ByteBuffer path in both modes.
- AOSP multi-source packaging is reproduced rather than flattened: replacement
  sources, multidex/aotex/bcpex entries and separately loaded `-ex.jar` classes
  retain distinct DexFile/class-loader identities. All 22/22 classified
  multi-source tests pass both modes, including `068-classloader` after making
  the canonical app PathClassLoader the process system and context loader.
- Current executable exact-output total is 445 tests across those two queues.
  Remaining classified queues are bytecode-source 126, custom-script 344,
  external-native 37, native 24, no-src-main 99 and missing-expected 63. The
  goal remains active; next start bytecode-source build semantics, then native
  closures, production apps and Mach-O unwind validation.

Resume at bytecode-source tooling: 118/126 cases contain Smali and 8/126 use
Jasmin (one mixed). The repository has no pinned assembler yet. Add locked AOSP
assembler inputs and preserve primary/multidex merge boundaries before counting
any of these tests; Java rewrites are not acceptable evidence.

### Bytecode-source 126/126 and native JNI boundary checkpoint — 2026-09-05

- Android 16's pinned R8, Google Smali and AOSP Jasmin now build original Java,
  Smali and Jasmin inputs with AOSP's API-26 merge order. The full classified
  bytecode-source category passes 126/126 in interpreter and requested
  optimized modes with byte-exact output; evidence is
  `/tmp/art-bytecode-full-20260905.XbaPvv/results.tsv`.
- The six bytecode tests that load `libarttest` execute real JNI_OnLoad plus
  pinned ART common helpers. `088`, `2245`, `543`, `563`, `575` and `686` all
  pass both modes. Test 543's host-native helper uses the canonical
  base-relative compressed-reference decode instead of Android's low-4GB
  absolute-pointer cast.
- A generic source-native module builder now compiles unmodified per-test C/C++
  into Mach-O and preserves mixed Java/native stdout order on one process FD.
  `2036-jni-filechannel`, `647-jni-get-field-id`, and `2275-pthread-name` pass
  both modes. These runs added real `/dev/null` handling and a logical pthread
  name layer for Darwin's missing remote-thread rename operation.
- `169-threadgroup-jni` exposes the next runtime-owned gap: JNI FindClass on a
  newly attached native thread must inherit the canonical application system
  ClassLoader. Updating only `ClassLoader$SystemClassLoader.loader` and the
  current Java thread is insufficient because ART attach consults
  `Runtime::system_class_loader_`. Fix that common owner next, then resume the
  remaining native category. The 100% goal remains active; no unsupported
  opcode or APK-specific allowlist has been introduced.

### Debuggable JIT and native-corpus checkpoint — 2026-09-05

- The JIT now consumes Android's per-application debuggable policy at VM
  creation. `android:debuggable=true` reaches ART as the canonical
  `--debuggable` compiler option, making `RuntimeDebugState` and JIT `CodeInfo`
  agree before any application method is compiled. Binary-manifest audits
  cover both default `debuggable=0` and aapt2 debug-mode `debuggable=1` APKs.
- `685-deoptimizeable` passes its original interpreter and debuggable optimized
  runs byte-for-byte. The previous `runInternal` rejection was correct AOSP
  behavior for non-debuggable JIT code, which is an upstream known-failure
  variant, not a Darwin unwind bypass to remove.
- `2275-pthread-name` passes both modes after matching Bionic process-stream
  ordering on redirected stdout. External-native tests are now 37/37.
- Source-native first pass: 17/24 pass both modes after the stream fix. The
  applicable remaining work is `004-SignalTest`, `454-get-vreg`,
  `461-get-reference-vreg`, `466-get-live-vreg`,
  `497-inlining-and-class-loader`, and `993-breakpoints-non-debuggable`.
  `664-aget-verifier` is an unconditional Android 16 known failure and must be
  reported separately rather than counted as a Darwin-supported configuration.
  Resume with a general Darwin ARM64 ucontext source boundary and independent
  DexFile/ClassLoader ownership; then close base-relative vreg consumers and
  the real JVMTI plugin path.
- The 100% goal remains active; no APK-specific JIT allowlist or weakened deopt
  safety check was introduced.
- Follow-up: `004-SignalTest` passes exact output in interpreter and optimized
  modes through a Darwin ARM64 ucontext/sigaction source-ABI header; applicable
  source-native progress is now 18/24.
- Pinned AOSP also marks `497-inlining-and-class-loader` unconditionally
  disabled: its loader illegally re-registers the same DexFile. Together with
  `664`, it is excluded from supported AOSP configurations, making the current
  source-native result 18/22 with `454/461/466` and `993` remaining.

### Embedded JVMTI and Android launcher closure checkpoint — 2026-09-06

- Source-native closure is now 22/22 applicable Android 16 configurations.
  Original `454`, `461`, and `466` use a narrow base-relative
  CompressedReference source-ABI adapter; `466` also receives AOSP
  `--compile-art-test` so its no-inline root contract is honored. Original
  `993-breakpoints-non-debuggable` passes through the real embedded OpenJDK
  JVMTI plugin. Upstream-unconditional failures 497 and 664 remain explicitly
  excluded rather than weakened.
- The embedded plugin is all 29 pinned Android 16 OpenJDK JVMTI sources, loaded
  through ART's normal Plugin/AgentPath lifecycle. Darwin's minimal start now
  emits kStart/kInit around daemon creation and starts the Signal Catcher like
  a non-zygote app process. Original 901, 903–911, and 913–932 JVMTI tests
  reached so far pass byte-exact interpreter and optimized-JIT execution;
  912 remains queued and 933 is the current active blocker.
- AOSP's complete ART-independent `libtiagent-base-defaults` helper set is now
  linked into every per-test agent. The dispatcher reproduces common
  redefine/retransform/transform OnLoad routing instead of adding runtime or
  APK allowlists. A generation-counted Darwin pthread-barrier source ABI closes
  931 without changing its upstream source.
- Libcore `File.getCanonicalFile()` now reaches the Rust Bionic filesystem
  facade; guest `/proc/self` canonicalizes to `/proc/<pid>`, closing original
  913. The pinned `jdk_internal_misc_VM.cpp` registrar supplies
  getNanoTimeAdjustment, closing 924. JVMTI GetTime uses modern Darwin
  CLOCK_MONOTONIC, matching Android System.nanoTime and closing 927.
- The launcher now supplies both RuntimeArgumentMap::ClassPath and
  java.class.path at VM creation, with the APK support DEX included where
  applicable. It uses that one system PathClassLoader as the canonical app
  loader rather than constructing a duplicate parent/child pair. This closes
  JVMTI AddToSystemClassLoaderSearch in original 929.
- The host blocks Android runtime-control signals before spawning threads, so
  SIGQUIT reaches ART's sigwait-based Signal Catcher. AOSP's run-test-specific
  DumpNativeStackOnSigQuit=false option is also reproduced. Current 933 reaches
  SignalCatcher but SIGSEGVs inside the remaining managed/full DumpForSigQuit
  path before RuntimeCallbacks::SigQuit. Resume by instrumenting and fixing
  that common Darwin dump owner; do not bypass the callback or edit the test.
- The 100% goal remains active. After 933, continue the remaining custom-script
  JVMTI queue, then production APK/Blue Archive unrestricted JIT and Mach-O
  unwind/deoptimization validation.

### JVMTI class/transform continuation checkpoint — 2026-09-06

- The preceding checkpoint's 933 blocker is closed. Darwin's SIGQUIT dump
  crashed because `SignalCatcher::DumpCmdLine()` streamed a null process
  command line; the pinned ART patch now prints `<unset>`. The following abort
  came from the host crash-stack palette adapter returning NOT_SUPPORTED; it
  now writes the complete payload to stderr and reports the real write result.
  Original `933-misc-events` passes byte-exact in interpreter and optimized
  modes without bypassing RuntimeCallbacks::SigQuit.
- Original `912-classes` now passes byte-exact in both modes. AOSP's
  `runtime_state.cc` remains linked once in the runtime image; the two JNI
  entrypoints forwarded by `classes_art.cc` are explicit test ABI exports.
  The per-test agent no longer recompiles the entire ART-dependent
  `libarttest` common implementation, avoiding duplicate ownership and hidden
  C++ runtime dependencies.
- Original `934` through `945` all pass byte-exact in interpreter and
  optimized modes. `936-search-onload` uses its pinned AOSP OnLoad owner so
  AddToBootstrapClassLoaderSearch and AddToSystemClassLoaderSearch execute
  during normal agent loading; no test APK or expected output was modified.
- The next sequential JVMTI corpus item is `946-obsolete-throw`. The 100% goal
  remains active; passing this interval is evidence for class transformation,
  retransformation, obsolete frames, native methods, and loader search, not a
  declaration of full ART or production-APK completion.

- Continuation: original `946-obsolete-throw` also passes byte-exact in both
  modes. Resume at `947-reflect-method`.

### Invoke-custom and MethodHandle checkpoint — 2026-09-06

- Original `947` through `959` now pass byte-exact in interpreter and
  optimized-JIT modes. This interval covers reflected methods, annotation and
  in-memory transforms, threaded obsolete frames, invoke-custom,
  invoke-polymorphic verification/accessors, MethodHandle transforms, and
  EmulatedStackFrame dispatch.
- `952-invoke-custom` now reproduces its pinned AOSP build contract: the
  Android 16 prebuilts/misc ASM 9.6 artifact is revision- and SHA-locked, the
  upstream annotation transformer is compiled, and javac output is rewritten
  to real invokedynamic bytecode before non-desugaring D8. No generated test
  DEX or source is patched to fit Darwin.
- `956-methodhandles` exposed a platform registration gap rather than a JIT
  codegen error. The pinned libcore `java_lang_invoke_MethodHandle.cpp` and
  `java_lang_invoke_VarHandle.cpp` owners are now in the Darwin system-native
  archive and registered with libjavacore. Reflective signature-polymorphic
  calls therefore throw Android's UnsupportedOperationException instead of a
  missing-JNI UnsatisfiedLinkError; ordinary polymorphic execution remains on
  ART's interpreter/JIT paths.
- `954` applies the exact upstream run.py normalization for temporary DexFile
  locations and verifier PCs. `958` uses signature-only hidden-API compiler
  inputs for Transformers/EmulatedStackFrame; these stubs never enter the test
  DEX, whose runtime definitions remain the Android boot classes.
- System-native build/audit and the full graphics/runtime link audit pass with
  seven pinned system-native archive members. Resume the sequential corpus at
  `960-default-smali`. The 100% goal remains active; production APK/Blue
  Archive, GC/concurrency, and Mach-O unwind/deoptimization closure are still
  required.

### Generated DEX, hidden API, GC, and JVMTI continuation — 2026-09-06

- Original/generated `960` through `979` pass byte-exact in interpreter and
  optimized-JIT modes. The runner now reproduces AOSP generated-source,
  `javac_post`, ASM, smali, D8 API-level, and jar assembly contracts. Pinned
  Dexter Slicer builds the upstream transform agents instead of substituting
  test bytecode.
- Original `981` through `1000` pass both modes, except `980`. Test 980's
  Object retransform and allocation callbacks work, but the post-main VM
  shutdown allocation stream differs because this detached launcher performs
  explicit daemon shutdown; expected output is not filtered. Resume this as a
  lifecycle-owner issue, not a retransform/JIT failure.
- `999-redefine-hiddenapi` now passes using a general DEX hidden-api section
  encoder. It implements the AOSP `hiddenapi_class_data` member order and map
  entry for DEX 035–041, including DEX 041's appended container header. No
  source, CSV, or expected-output patch is used.
- `1000`, `1336`, `1337`, `1338`, and `1339` pass both modes. Coverage includes
  non-moving allocation exhaustion, CC GC/collector transitions, moving-GC
  suppression, no-LOS allocation, finalizer watchdog exit code 2, and
  dead-reference/reachability-sensitive optimization. The launcher now carries
  AOSP's FinalizerTimeoutMs and expected process exit contract.
- `1900` through `1903` pass both modes. Darwin JVMTI allocation accounting now
  uses macOS `malloc_size()` through a checksum-pinned source patch instead of
  AOSP's Apple `malloc_usable_size=0` placeholder; concurrent allocation,
  multi-environment accounting, bytecode retrieval, suspend, and self-suspend
  are exercised by the unmodified tests.
- `1001-app-image-regions` remains outside the current JIT runner closure: its
  run contract first creates a profile-guided dex2oat app image. The current
  failure occurs while linking its private ImageHeader test ABI, before any
  managed/JIT result. Add the AOT/app-image artifact pipeline rather than
  weakening the expected `App image loaded true` result.
- Resume the sequential JVMTI corpus at `1904-double-suspend`. The 100% goal is
  still active; after the 19xx/20xx corpus, return to 980 lifecycle equality,
  1001 app-image generation, unrestricted production APK/Blue Archive JIT,
  and Mach-O unwind/deoptimization validation.
- Build-graph debt observed in this interval: a changed runtime probe header
  can update its content stamp while the first top-level invocation reports no
  Ninja work; explicitly naming an object/final dylib then rebuilds it. Repair
  this dependency edge before treating incremental build latency as solved.

### Trace, profile, generated DEX, and method-handle continuation — 2026-09-06

- Unmodified AOSP `2232` through `2286` now pass byte-exact in interpreter and
  optimized ARM64 JIT modes, including all 17 `2239-varhandle-perf-*` variants.
  The only numeric gaps retained from earlier work are `980` (post-main daemon
  lifecycle equality) and `1001` (profile-guided dex2oat app-image pipeline).
- Metrics tests now use Android's Runtime start/init phases and launcher log-tag
  contract. Trace tests use Android's dual-clock default on Darwin, preserve
  persistent virtual-FD seek behavior, compile with the upstream debuggable
  contract, and expose only a two-function ART-owned trace-file bridge.
- `2271-profile-inline-cache` closes the full profile path: the launcher accepts
  serialized AOSP runtime options, ProfileSaver runs with the upstream 32 MiB
  initial cache and 3000 inline-cache threshold, and Darwin libartbase resolves
  Android private paths before open and lock/inode validation. Runtime-state JNI
  exports now follow the shared AOSP libarttest contract instead of being added
  one failure at a time.
- The test builder now honors AOSP named API levels, Java source/target levels,
  output-argument `javac_post.sh`, generator-owned DEX/JAR output, old cyclic
  DEX without D8 rewriting, multidex launcher support, Smali `-J` options, and
  upstream JNI loader-output filtering. Generated boot flag classes remain the
  real `core-libart.jar` definitions; compiler stubs are signature-only.
- Validation passes: Python syntax, all touched shell syntax, Cargo formatting,
  `git diff --check`, ART patches 0038/0108/0110/0111/0112 apply checks, and the
  full graphics/runtime link audit (`registrar=51`, no fake/host ICU/fmt edges).
- Resume by auditing non-numeric/shared test fixtures after `2286`, then return
  to `980`, `1001`, unrestricted production APK/Blue Archive JIT, and Mach-O
  unwind/deoptimization closure. The 100% compatibility goal remains active.
- Incremental graph debt is confirmed: after changing the `os_linux.cc` source
  path, the bootstrap archive considered 254 objects cached while the final link
  named a missing/stale object. Explicit internal bootstrap rebuilt exactly one
  TU. Fix source-path/fingerprint invalidation rather than relying on manual
  artifact moves.

### App-image and default-JIT closure — 2026-09-06

- Unmodified pinned AOSP `1001-app-image-regions` now passes byte-exact in both
  interpreter and optimized ARM64 JIT modes. The runner executes the real
  profile text -> embedded profman -> embedded dex2oat -> `.art/.odex/.vdex`
  -> app-image relocation/load chain with the exact 11-component boot image.
- Fixed the embedded-tool process ABI: Rust-owned C strings remain alive and
  the `argv` pointer vector is now NULL-terminated as required by ART
  `InitLogging()`. This removed the intermittent `_platform_strlen` dex2oat
  startup crash.
- Darwin image relocation now keeps image-file source addresses logical while
  converting destinations and pointer-valued ImageHeader getters to the 1 TiB
  host compressed-reference arena. This removed the doubled-base `0x200...`
  app-image fault and preserves Android's 32-bit on-disk image format.
- Apple Silicon cannot execute a fixed mapping backed by unsigned Android ELF.
  ART's narrow mmap boundary materializes only fixed executable OAT segments
  into anonymous RW pages and seals them RX; non-executable segments remain
  file-backed. Graphics/runtime link closure remains clean.
- Unmodified AOSP `980-redefine-object` now also passes both modes. The full
  JIT acceptance suite passes with no `DARWIN_ART_JIT=1`: production runtime
  default is now Android-like JIT-on, while the differential runner explicitly
  requests interpret-only mode with `DARWIN_ART_JIT=0`.
- Runtime native cache identity is part of every object fingerprint and was
  bumped for the new `oat/image.h` shadow. A cold migration rebuilt 254/254
  runtime objects once; the immediate repeat reported `compiled=0 cached=254`.
  Unchanged archives are no longer rewritten, avoiding needless downstream
  dylib relinks. Remaining 100% work includes full corpus accounting,
  production APK/Blue Archive JIT soak, and Mach-O unwind/deoptimization
  closure; the goal remains active.

### Production APK and native-provider continuation — 2026-09-06

- The unchanged Blue Archive base and arm64 split APKs (SHA-256
  `25479ffb2e0710285a6f11e5666273b97edb68f3d6ae16ba78fd388fd7cd45e8`
  and `2049eeaba16d4f6b29c51d0dc5551c1e3e8598274dcfce140c3e816d47602ee4`)
  run with the production default JIT through Conscrypt/network startup,
  native `libgrap-core.so` loading, and a 1280x720 GPU SurfaceView to the
  rendered Nexon login screen. The latest run contains no native-loader error,
  UnsatisfiedLinkError, SIGABRT, or Bionic abort.
- Detached guest native workers attach to the owning JavaVM as daemon threads
  only while constructing a new ELF dependency graph and loader context, then
  detach through RAII. Resident-library lookup remains ART-free and JNI_OnLoad
  keeps the ordinary JavaVMExt/NativeBridge boundary.
- The libc leaf facade supplies Bionic-compatible `strlcat` and ASCII
  `isprint`, closing the complete non-zlib import gap seen in Blue Archive's
  `libgrap-core.so`. `BinderInternal.handleGc()` reaches the compatibility
  transport flush boundary, matching AOSP ownership without fabricating a
  kernel Binder command buffer.
- The pthread provider's standalone graph now links the real Android errno TLS
  owner instead of relying on the composed runtime to hide that dependency.
  Its Android-arm64 ELF suite plus ASan/UBSan stresses pass recursive reads,
  concurrent readers/writers, lazy initialization, lifecycle failures,
  foreign-thread identity, mutexes, conditions, and TLS.
- The no-override JIT acceptance matrix and full graphics/runtime link closure
  pass again (`registrar=51`, no fake symbols or host ICU/fmt edges). Mach-O
  CFI is restored for memcmp16, ordinary JNI assembly except its critical
  dlsym stub, and ExecuteSwitchImplAsm except its dlsym stub. Quick-entrypoint
  nonlinear CFI and alternate Baker entries still need a real Mach-O lowering;
  the 100% goal remains active.

### Complete quick-entrypoint Mach-O CFI lowering — 2026-09-06

- The ARM64 bootstrap no longer removes CFI from any AOSP quick entrypoint.
  It expands the upstream assembler macros, then lowers ELF's nonlinear
  remember/restore programs into adjacent Mach-O FDEs with explicit entry
  state for allocation/Baker slow paths, invoke and OSR paths, checked object
  stores, suspend/deoptimization, proxy exceptions, IMT conflict dispatch,
  generic JNI, method hooks, and Baker introspection.
- Baker's fixed-offset public introspection ABI is preserved with local labels
  plus exported aliases. `nm` reports all three public symbols, including the
  arrays and GC-roots entries. The generated object has 317 compact-unwind
  entries and 259 DWARF FDEs; `llvm-dwarfdump --verify` and direct `--eh-frame`
  parsing report no errors.
- The builder now requires at least 300 unwind entries and named coverage for
  throw, deoptimization, invoke-custom, generic JNI, Baker introspection, and
  method-exit hooks. It also asserts exact 46 slow-path and 19 alternate-path
  split inventories, so upstream assembly drift fails closed.
- `otool -t` output remains byte-for-byte identical to the prior no-CFI quick
  object. Full graphics/runtime link closure passes (`registrar=51`, no fake
  symbols or host ICU/fmt edges), followed by the complete default-on JIT
  acceptance matrix covering GC/read barriers, JNI, exceptions, monitors,
  fields/arrays, MethodHandle/invoke-custom, VarHandle ordering, deopt, OSR,
  and hooks.
- The pinned AOSP 137-cfi runner executes all three upstream invocations and
  exposes the real remaining platform gap instead of manufacturing a pass:
  upstream `cfi.cc` implements local and remote unwind only under `__linux__`,
  while Darwin's `AndroidLocalUnwinder` is still a false-return stub and has no
  Mach task remote backend. The two JNI/native dlsym assembly stubs also remain
  isolated without CFI. These are the next unwind-closure tasks; the overall
  100% compatibility goal remains active.

### Production soak signal-disposition correction — 2026-09-06

- A final-CFI run of the unchanged Blue Archive APK traversed Unity/IL2CPP,
  Conscrypt, FMOD/CoreAudio, Firebase, SQLCipher, patcher, and the Vulkan guest
  graph. One run later entered Unity's SIGABRT crash reporter after loading
  `libgrap-core.so`; its reporter faulted at `libunity.so+0x6f5e44` on a null
  operand. This is a real remaining workload failure and is not counted as a
  successful soak.
- That secondary fault exposed an independent bionic signal bug: when ART's
  sigchain still owned the host signal but the guest disposition was
  `SIG_DFL`, the process-state trampoline returned to the same faulting PC and
  emitted the fault indefinitely. It now restores the host default disposition
  and re-raises, matching Android termination semantics; `SIG_IGN` remains a
  return.
- The process-state Android-ELF/concurrency/signal audit and the full 36-provider
  graphics/runtime link audit pass after the correction. A second 35-second
  original-APK run exited normally with a 166 KiB log and zero unresolved
  signal repeats. The first run's 636 MiB temporary spam log was removed after
  retaining a 9,496-byte fault context at
  `/tmp/blue-full-quick-cfi-fault-context.log` (SHA-256
  `600893b423143606cb683355d369fa5f3618fc3e5c5f1efc300507c21cae1791`).

### Complete JNI/native dlsym Mach-O CFI — 2026-09-06

- The last two ARM64 assembly exceptions no longer use no-CFI objects.
  `art_jni_dlsym_lookup_stub` is represented by two adjacent Mach-O FDEs;
  `art_jni_dlsym_lookup_critical_stub` uses seven FDEs for its generic-jump,
  dynamically sized native frame, lookup merge, normal return, and exception
  paths.
- AOSP's Apple preprocessing intentionally erases `CFI_EXPRESSION_BREG` and
  `CFI_DEF_CFA_BREG_PLUS_UCONST`. The Darwin file-format lowerer now restores
  the exact upstream DWARF expression bytecode. Saved x19-x30 locations in the
  dynamically based critical frames remain expressed relative to x29 rather
  than being replaced with false fixed offsets.
- The builder fails closed on FDE, state-stack, dynamic-frame, and register
  expression inventory drift. It compiles a private no-CFI comparison object
  only to prove that the emitted `__text` bytes are identical; that comparison
  object is not archived. The runtime archive now contains CFI-bearing objects
  for JNI, native, quick, and memcmp assembly, and both dlsym objects pass
  `llvm-dwarfdump --verify`.
- Full graphics/runtime closure passes (`registrar=51`, fake symbols/host
  ICU/host fmt all zero), followed by the unrestricted default-on JIT suite.
  The pinned full libunwindstack source tree is now source-locked and
  reproducibly materialized in preparation for replacing the remaining
  Darwin `AndroidLocalUnwinder` false-return implementation and adding a Mach
  task remote backend. The overall 100% goal remains active.

### Pinned AOSP unwindstack core boundary — 2026-09-06

- Source synchronization now materializes the complete pinned
  `platform/system/unwinding/libunwindstack` subtree rather than adopting only
  its public headers. The lock verifies the upstream `Android.bp`, and a
  guarded migration replaces only the former generated header snapshot.
- A dedicated incremental build compiles 21 unchanged upstream translation
  units into `libunwindstack-core-darwin.a`: DWARF CFA/opcode/section handling,
  ELF interfaces, JIT debug descriptors, symbols, MapInfo, architecture
  registers, XZ memory, and the AOSP Unwinder engine. A repeat build reports
  `compiled=0 cached=21`.
- This makes the remaining port boundary explicit rather than retaining a
  bespoke fake unwinder: AndroidUnwinder, Maps, Memory, Regs, ThreadEntry,
  ThreadUnwinder, and Rust-name demangling are the platform-facing units still
  to bind to Mach tasks, dyld images, Darwin ucontext, and the Rust provider.
  The core archive is not linked into production until those providers and
  their local/remote acceptance tests are complete. The 100% goal remains
  active.

### Deterministic AOSP CFI and production x18 closure — 2026-09-06

- Pinned AOSP `137-cfi` now passes its original Java call graph and all three
  optimized-JIT CFI invocations twice consecutively. Darwin's provider reads
  local/ucontext/other-thread/remote Mach state, target task memory and dyld
  maps while the unchanged AOSP `Unwinder` and `JitDebug` own managed/DWARF
  interpretation. Generic-JNI handoff records the active SaveRefsAndArgs base
  in an allocation-free task record; target Mach-O `LC_SYMTAB` lookup removes
  parent-ASLR-dependent remote symbolization.
- Android's Rust demangling boundary is no longer a weak null stub. A Rust
  static library backed by pinned `rustc-demangle 0.1.28` supplies the exact C
  ABI and libc allocation ownership expected by AOSP. Provider smoke covers
  process memory, maps, registers, local/context/thread/remote frames and Rust
  v0 names. The complete default-on JIT matrix and graphics/runtime link audit
  pass after integration.
- A real unchanged Blue Archive run then faulted in `libunity.so+0xc76794`:
  Unity loaded a valid array address into Android-general register x18, but the
  fault context contained x18=0. This was not MediaCodec state corruption. A
  Cargo relink had replaced the host after APK installation and restored a
  current SDK 26.5 Mach-O declaration, while the installed-record launcher
  only verified its signature and skipped the required task-wide Android x18
  ABI declaration.
- Every development launch now serializes host preparation, verifies SDK 12
  plus JIT entitlements, and atomically repairs/re-signs a stale Cargo output.
  Packaged runtimes fail closed instead of mutating shipped code. The x18 audit
  races two launch preparations and proves the sentinel survives 100,000
  scheduling points. The unchanged Blue Archive APK subsequently passed the
  former fault point and remained live for more than three minutes with zero
  `DARWIN signal`, SIGSEGV or SIGABRT markers (`/tmp/bluearchive-x18-fixed.log`).
- The 100% goal remains active. This closes the newly exposed production ABI
  regression, not every non-numeric ART harness configuration nor arbitrary
  native-library behavior. Continue with complete corpus accounting and longer
  interactive Blue Archive/Chrome/calculator default-JIT soaks.

### Incremental unwind dependency closure — 2026-09-06

- A clean Ninja rebuild exposed that the standalone JNI acceptance edge still
  carried a copied include list predating pinned unwindstack. It could reuse an
  old object, but failed as soon as the object was invalidated because
  `unwindstack/AndroidUnwinder.h` was outside that private list.
- Standalone JNI and GPU probe commands now consume the same canonical
  `core_probe_includes` graph as CPU, Metal and APK link consumers. The repaired
  incremental graph rebuilt the JNI object and completed the production
  graphics link audit with registrar=51 and zero fake, host-ICU or host-fmt
  edges. This is build reproducibility evidence; the overall 100% goal remains
  active.

### Corpus accounting and prebuilt DEX closure — 2026-09-06

- The corpus audit previously labeled all 63 non-numeric directories as
  `missing-expected`. They are AOSP shared fixtures (`common`, `ti-agent`,
  `jvmti-common`, dexpreopt/verifier inputs), not runnable tests. The audit now
  separates `shared-fixture` and `harness-only`, and classifies script,
  handwritten bytecode, native and alternate-source layouts before deciding a
  test lacks `src/Main.java`. There are no numeric missing-expected tests in
  the pinned tree.
- The generic runner now accepts pinned prebuilt DEX and multidex JAR inputs.
  It preserves every original DEX byte instead of passing malformed fixtures
  through D8, retains original `classes2.dex` ordering, and appends launcher
  support only as the final multidex entry.
- Eight unchanged prebuilt cases pass exact stdout/stderr in interpreter and
  optimized JIT modes: `097`, `649`, `663-odd-dex-size` plus size2/3/4, `801`,
  and `836`. This covers duplicate methods, odd DEX sizes, invalid void
  check-cast, a 32,768-class table and prebuilt multidex. Consolidated logs are
  under `/tmp/art-prebuilt-final`.
- `845-fast-verify` produced matching managed output, but its run.py contract
  is specifically a `.dm`/dex2oat `--compile-individually` regression and the
  JIT runner does not reproduce that AOT step. It is therefore not counted as
  JIT evidence. The 100% goal remains active while custom-script semantics are
  separated into JIT-relevant, AOT-only and harness-only queues.

### Post-prebuilt full JIT regression — 2026-09-06

- `tools/audit-art-jit.sh` passes after the prebuilt-input changes. The run
  re-executes unrestricted optimized compilation and the GC/read-barrier,
  primitive/reference, field/array, call/JNI, exception/monitor, VarHandle,
  MethodHandle, invoke-custom, OSR/deoptimization and exit-hook matrices.
  Evidence: `/tmp/art-jit-after-prebuilt.log`.
- This proves the runner expansion did not weaken the existing default-on JIT
  contract. It does not close the still-unclassified custom-script queue, so
  the goal remains active.

### Pinned run.py invocation semantics and ThreadStress — 2026-09-06

- The generic runner no longer unions options from multiple `default_run()`
  calls into one synthetic process. It preserves each literal call in source
  order with its own Android runtime options, shell-split test arguments and
  expected exit code, then concatenates the per-process output as AOSP does.
- Original `566-polymorphic-inlining`, `570-checker-osr-locals`,
  `652-deopt-intrinsic`, and `676-proxy-jit-at-first-use` pass together in
  interpreter and optimized-JIT modes. The runner now supplies their pinned
  `-Xjitthreshold` and `-Xjitinitialsize` settings rather than a Darwin test
  allowlist. The original 566 C++ helper consumes the runtime's AOSP C++ ABI;
  570 consumes the shared `test/common/runtime_state.cc` JNI contract.
- `004-ThreadStress` exposed two genuine compatibility gaps. Its unchanged
  native helper now links against exported ART `String::ToModifiedUtf8()` and
  `Throwable::Dump()`, while `android.system.Os.kill()` is implemented through
  the central Bionic process-state signal translator instead of the generated
  ENOTSUP wrapper. The Bionic provider header now declares its definitions as
  C ABI for C++ consumers.
- `004-ThreadStress` now passes both pinned invocations (default workload and
  `--locks-only -o 100`) in interpreter and optimized-JIT modes, including the
  original nondeterministic-number normalization. Evidence:
  `/tmp/art-004-exact-final.log` and its retained artifact directory.
- Uncaught `Main.main()` failures are emitted once in dalvikvm's
  `Exception in thread "main"` form and retain the pinned process exit code.
  `038-inner-null` passes exact stdout/stderr and exit code in both execution
  modes; three-invocation `137-cfi` also passes after the runner refactor.
- The 100% goal remains active. AST-literal invocation extraction does not yet
  execute dynamic Python control flow, compiler/AOT-only phases, or every
  custom post-processing command. Those run.py semantics must be classified
  and moved to explicit harness owners rather than silently merged or skipped.

### Android uncaught-handler process semantics — 2026-09-06

- Main-thread failure presentation and exit status are now independent state.
  An application-specific handler or a ThreadGroup delegating to an installed
  process default handler owns the Throwable output, but the launcher retains
  the failure and terminates with the original nonzero run-test status.
- With no installed default handler, the harness writes dalvikvm's single
  `Exception in thread "main"` record. It no longer calls JNI
  `ExceptionDescribe()` after Java has already written the trace.
- Unchanged `030-bad-finalizer`, `034-call-null`, `038-inner-null`,
  `054-uncaught`, and `059-finalizer-throw` now pass exact output and exit
  contracts in interpreter and optimized-JIT modes. Evidence is under
  `/tmp/art-next-*`, `/tmp/art-invocation-final-*`, and
  `/tmp/art-next-fixed-*`.
- The Bionic process-state facade's concurrent teardown, signal trampoline,
  Android ELF import and 8x1000 thread stress audit passes after exposing its
  C ABI to libcore (`/tmp/art-bionic-process-state-audit.log`). A post-handler
  rerun of `038-inner-null` also passes in both modes.
- The 100% goal remains active; the next custom-script slice must continue from
  the first unmodeled run.py control-flow, output-filter, profile/image, JVMTI,
  or runtime-option contract rather than treating these five cases as corpus
  completion.

### NativeBridge lifecycle and custom-script expansion — 2026-09-06

- Unchanged pinned tests `064-field-access`, `080-oom-throw`,
  `091-override-package-private-method`, `099-vmdebug`, `1001-app-image-regions`,
  `1002-notify-startup`, `1003-metadata-section-strings`,
  `1004-checker-volatile-ref-load`, and `141-class-unload` pass in interpreter
  and optimized-JIT modes. The two app-image cases exercise real profman and
  dex2oat artifacts rather than a synthetic JIT substitute.
- `115-native-bridge` now executes its original `run.py`, Java and native test
  sources through the pinned Android 16 libnativebridge state machine. Minimal
  runtime startup publishes the system class loader, performs late well-known
  class initialization, preinitializes/initializes the bridge, registers
  Darwin-translated signal handlers and retains NativeLoader's native-first
  bridge fallback.
- The test's real version-3 bridge rewrites the deliberately missing
  `libarttest.so` to `libarttest2.so`, enumerates native methods and shorties,
  executes JNI trampolines and attached-thread lookup, and validates bridge
  errors plus nested SIGSEGV/SIGILL handling. Interpreter and optimized output
  match the pinned expected files; retained evidence is under
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-115-native-bridge-63xbihf4`.
- Signal compatibility is scoped to that Android bridge DSO. Unchanged
  `004-SignalTest` and `150-loadlibrary` pass again in interpreter and optimized
  modes, preventing the NativeBridge work from silently changing ordinary
  Mach-O JNI signal or loader behavior.
- The 100% goal remains active. Passing this lifecycle closes one custom-script
  family; dynamic Python branches, profile/image/compiler phases, JVMTI and the
  remaining applicable run-test corpus still require exact ownership and
  execution rather than test-name skips.

### Multidex post-processing and real HPROF output — 2026-09-06

- The generated-source runner now preserves AOSP's `javac_post.sh` contract
  instead of assuming every post-processor consumes `transformer.jar`.
  `126-miranda-multidex` moves the original interface class across the
  `classes`/`classes2` boundary and passes both verify-on and verify-off runs;
  `127-checker-secondarydex` also passes interpreter and optimized execution.
- `dalvik.system.VMDebug.dumpHprofData()` no longer terminates at a successful
  no-op. The runtime links pinned AOSP `runtime/hprof/hprof.cc`, resolves its
  guest-private output path at the native filesystem boundary and produces a
  real binary heap dump. The Android SDK's host `hprof-conv` then accepts and
  converts that dump through the run-test tool layout.
- Unchanged `130-hprof` passes exact expected output in interpreter and
  optimized-JIT modes, including basic dump, allocation tracking, class-loader
  unloading and concurrent GC/dump work. Retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-130-hprof-a9hr70id`.
- NativeBridge `115` and multidex `126` pass again after the HPROF and launcher
  changes. The 100% goal remains active; the next unmodeled runtime/AOT/JVMTI
  contract must be implemented rather than inferred from these milestones.

### Runtime streams, private test ABI, and tests 139–169 — 2026-09-06

- Main execution now gives native `std::cerr` the same captured application
  stderr as Java `System.err`, while android-base/logd records remain in the
  host diagnostic stream. This restores Android's process-stream semantics
  without copying expected text or keying behavior on a test name.
  Unchanged `143-string-value` passes exact interpreter and optimized output;
  retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-143-string-value-cge8sq7f`.
- Compiler-only hidden-platform signatures now include the pinned runtime's
  `VMDebug.setAllocTrackerStackDepth()` and complete `EmptyArray` field shape.
  They never enter application DEX. Runtime execution still resolves the
  Android boot classes. This closes unchanged `145-alloc-tracking-stress` and
  `157-void-class` in both modes.
- Native run-test helpers that consume private `Heap`, `ThreadList`, or
  `Monitor` C++ ABI are owned inside the runtime module. The runner derives
  their declaring class from unchanged JNI exports and registers only methods
  actually declared by that class; it does not export ART internals through an
  application DSO. `148-multithread-gc-annotations`,
  `149-suspend-all-stress`, and `167-visit-locks` pass interpreter/JIT, and
  `1337-gc-coverage` passes again as a regression.
- The runner now interprets upstream output-only `sed '/error/!d'` and
  `tail -n 1` contracts by command meaning. It also honors target-side
  `default_build(use_jasmin=False)`, preserving deliberately malformed Smali
  instead of sending invalid classfiles through D8. `166-bad-interface-super`
  therefore reaches and passes ART's verifier unchanged.
- Exact original-source interpreter/JIT passes in this slice are
  `139-register-natives`, `143`, `144`, `145`, `146`, `148`, `149`, and every
  test from `151` through `169`. App-image tests `158`, `159`, `163`, and `164`
  use real profman/dex2oat output; `160` exercises large-field/array/Unsafe
  read barriers; `162` preserves primary/secondary Jasmin resolution.
  Representative retained evidence ends at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-169-threadgroup-jni-79g01ss2`.
- Full graphics/runtime linkage audit remains complete with 51 registrars,
  zero fake symbols and zero host ICU/fmt leakage. The 100% goal remains
  active: the next applicable unverified run-test range, dynamic run.py/AOT
  phases, JVMTI matrix, full differential regressions, and real APK/Blue
  Archive acceptance are not yet complete.

### App-image JNI/method addresses and tests 170–183 — 2026-09-06

- Unchanged pinned tests `170-interface-init` through
  `177-visibly-initialized-deadlock` pass in interpreter and optimized-JIT
  modes. `172-app-image-twice` keeps `DescribeSpace()` private to the runtime:
  the unchanged `debug_print_class.cc` implementation is runtime-owned and
  its declared JNI method is registered from the shared run-test bridge.
- `178-app-image-native-method` exposed two real ARM64 address boundaries.
  Synchronized JNI stubs now decode their compressed `this`/declaring-class
  reference before calling the standard `JniLockObject`/`JniUnlockObject`
  entrypoints. App/boot-image relative method loads likewise restore the
  Darwin image base before dereferencing `ArtMethod` entrypoints. The original
  test now passes regular JNI, FastNative, CriticalNative, synchronized JNI,
  large mixed signatures and repeated app-image dispatch in both modes.
  Retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-178-app-image-native-method-kcnvgk1x`.
- The final runtime audit now owns an explicit `build-jit-compiler` dependency,
  preventing a stale compiler archive from hiding a changed codegen patch.
  After initial staging, the image-method fix rebuilt one of 106 compiler TUs
  and reused 105 cached objects. The final full linkage audit passes with 51
  registrars, zero fake symbols and zero host ICU/fmt leakage.
- The runner now preserves generic `build.py` D8-container and DEX-magic
  rewrite contracts. It also keeps the API-26 support DEX outside a pre-26
  primary DEX, avoiding contamination of older default-method fixtures.
  Unchanged `179-nonvirtual-jni`, `180-native-default-method`,
  `181-default-methods`, `182-method-linking`, and the multithreaded
  `183-rmw-stress-test` all pass exact output in interpreter and optimized JIT.
- The 100% goal remains active. The next applicable pinned range begins at the
  `1900-*` JVMTI/monitor family; complete corpus coverage, differential
  regression and real APK/Blue Archive acceptance are still required.

### Android 16 JVMTI execution coverage through 1951 — 2026-09-06

- Unmodified applicable AOSP tests `1900`–`1951` now pass exact expected
  output in both interpreter and optimized JIT modes: 50 tests total, with
  absent test numbers skipped rather than synthesized.
- This range exercises real openjdkjvmti allocation callbacks, bytecode and
  local-variable access, per-agent TLS, class transformation/redefinition,
  thread suspend/resume (including native and self cases), owned/contended/raw
  monitors, frame-pop and stack-frame operations, exception events, proxy
  frames, DDMS extensions, agent disposal stress, breakpoints/deoptimization,
  obsolete method handles, malformed short DEX handling, and monitor-enter
  no-suspend behavior.
- No test source, APK/DEX input, expected output, method allowlist, interpreter
  substitution, or new compatibility shim was needed for this expansion.
  Representative retained artifacts are
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1900-track-alloc-zzdvlhm6`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1935-get-set-current-frame-jit-u3hdh6tw`,
  and
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1951-monitor-enter-no-suspend-6kohz8t4`.
- The goal remains active: resume at `1953-pop-frame`, continue the complete
  pinned corpus, then run differential regression and unchanged real APK/Blue
  Archive acceptance before claiming 100% support.

### Structural/JIT coverage through 2040 and libdir argv contract — 2026-09-06

- Another 84 applicable, unchanged AOSP tests now pass exact output in both
  interpreter and optimized JIT modes, from `1953-pop-frame` through
  `2040-huge-native-alloc`. Together with the prior turn, the continuous
  verified range contains 134 applicable tests from `1900` through `2040`.
- `2031-zygote-compiled-frame-deopt` found a runner contract defect rather
  than an ART defect. Its pinned `run.py` requests
  `add_libdir_argument=True`; the adapter had staged that second argument in
  the environment and then overwritten it while assembling ordinary argv.
  `RunInvocation` now parses this standard `default_run` keyword and appends
  the actual native library directory in argument order. The test-specific
  preseed was removed.
- With that fix, unchanged `2031` passes after simulated zygote specialization,
  deferred JVMTI attach and compiled-frame deoptimization. Retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2031-zygote-compiled-frame-deopt-o0gkpufv`.
- The expanded range also covers JIT frame-pop/forced return, concurrent
  instrumentation and obsolete frames, structural class/field/virtual-method
  redefinition across active multithreaded stacks, reflection/JNI ID stability,
  in-memory/file DEX class loaders, bounds and loop codegen, virtual/static
  inlining, memory-load/store loop transforms, shutdown, FileChannel JNI,
  hidden-API JVMTI extensions and huge native allocation accounting.
- Python syntax and the exact-output runs pass. No input DEX, Java/native test
  source, expected output, bytecode/method gate, or interpreter substitution
  was introduced. Resume the active 100% goal at `2041-bad-cleaner`.

### Reference/Unsafe/VarHandle coverage through 2239 — 2026-09-06

- Thirty-six additional unchanged AOSP tests pass exact output in interpreter
  and optimized JIT modes: all nine applicable `2041`–`2048` tests, ten
  `2230`–`2238` tests, and all seventeen `2239-varhandle-perf-*` variants.
- Coverage includes Cleaner failure handling, reference processing and pause
  coordination, concurrent stack traces, unavailable UFFD behavior, native
  registry failures, profile hotness, heap poisoning checks, metrics threads,
  suspend-check optimization, JDK Unsafe, multidex/polymorphic inlining, and
  VarHandle compare/exchange, CAS, weak CAS, get-and-update, plain/acquire/
  byte-array-view access, reflection, and Unsafe access paths.
- No new compatibility code or input/expected-output edits were required.
  Retained endpoints include
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2048-bad-native-registry-f4gv4zya`
  and
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2239-varhandle-perf-vh-unsafe-put-b1jhkwkb`.
- The active 100% goal resumes at `2240-tracing-non-invokable-method`; full
  corpus and real APK/Blue Archive acceptance remain mandatory.

### Tracing, CFG and barrier coverage through 2279 — 2026-09-06

- Forty-nine additional applicable AOSP tests from `2240` through `2279`
  pass exact output in interpreter and optimized JIT modes with original
  sources and inputs.
- The range covers non-invokable/JIT method tracing, single-step delivery,
  acquire/release LSE, try/catch inlining and boundary transforms, write-barrier
  elimination and kind selection, irreducible/nested loops, vector replacement,
  branch/GVN/constant folding, ClassValue and Cleaner failure behavior,
  default-conflict/Miranda methods, cached MethodType cleanup under GC,
  invokeExact, profile inline caches, pthread names and hidden-API use.
- No runtime or adapter exception was needed. Representative retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2246-trace-v2-58mr72fi`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2272-checker-codegen-honor-write-barrier-kind-j3vrsxty`,
  and
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2279-second-inner-loop-references-first-mouq86yk`.
- The 100% goal remains active; resume at `2280-const-method-handle-validation`.

### Final numeric range and run-contract accounting — 2026-09-06

- All nine applicable tests in the final pinned numeric range `2280`–`2286`
  pass exact output in interpreter and optimized JIT modes. This covers
  constant MethodHandle validation, class-unload invocation, pre-catch stepping,
  null-check and loop optimization, VarHandle/MethodHandle static initialization,
  invokeExact dispatch and tracing AOT code.
- A current corpus audit reports 1,138 directories: 1,074 numeric executable
  tests plus one numeric harness-only entry and 63 shared fixtures. The tree has
  no test directory after `2286`.
- AST accounting over all 348 numeric `run.py` files found zero mismatches
  between actual `default_run()` call count and parsed `RunInvocation` count.
  Completion is still not claimed: invocation keywords such as per-run profile,
  app-image, vdex and compiler options remain partly global/special ownership
  rather than fully represented in each parsed invocation.
- Next close those invocation-level build/AOT contracts, then rerun the full
  differential suite and unchanged production APK/Blue Archive acceptance.

### Per-invocation compiler policy and async deoptimization — 2026-09-06

- `RunInvocation` now owns literal `profile`, `jit`, `zygote`, Android log-tag,
  `Xcompiler_option`, and compiler-only option state. Profile app images are
  rebuilt only when that exact invocation's compiler contract changes, rather
  than selecting the first regex match or recompiling an identical contract.
- Unchanged `178-app-image-native-method` proves its two distinct app-image
  builds use `speed-profile --large-method-max=2000` and `verify` respectively.
  Unchanged two-invocation `552-checker-sharpening` likewise passes both modes
  with real `--generate-build-id` and `--no-generate-build-id` dex2oat runs.
- The detached runtime launcher no longer overwrites compiler options already
  parsed from Android's `-Xcompiler-option` pairs. It extends that vector with
  launcher-owned `--compile-art-test` and package debuggability, matching the
  app_process/run-test boundary.
- This exposed and closed a previously hidden JIT failure in unchanged
  `597-deopt-busy-loop`: its explicit `jit=True, --debuggable` contract now
  produces debuggable optimized code, and Simple, Float, and SIMD busy loops
  all survive asynchronous full-frame deoptimization with exact output in both
  harness configurations. Evidence is retained at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-597-deopt-busy-loop-mi1acwop`.
- Unchanged `689-zygote-jit-deopt` now compiles against a signature-only pinned
  Android 16 `ZygoteHooks` surface, executes the boot class, receives the real
  `-Xzygote` runtime option, and passes interpreter/JIT exact output. The full
  incremental graphics/runtime link audit remains green (`registrar=51`).
- No test/APK source or expected output was changed. The 100% goal remains
  active: vdex/prebuild/app-image variants that are still outside the parsed
  execution contract, full differential regressions, and unchanged production
  APK/Blue Archive soak remain completion gates.
- Cross-policy regressions also pass unchanged: `457-regs` consumes its real
  `--baseline` JIT compiler option, `993-breakpoints-non-debuggable` preserves
  the release/debugger boundary, and `2246-trace-v2` preserves method tracing
  with debuggable code. This guards the generic option merge against being a
  test-597 special case.

### VDEX/AOT execution and unrestricted compiler admission — 2026-09-06

- `RunInvocation` now owns `vdex` and `vdex_filter`. The adapter performs the
  same two-stage Android contract: create verifier dependencies, retain only
  the VDEX when no follow-up filter is present, or recompile with
  `--input-vdex` and the requested filter.
- JIT enablement no longer overwrites ART's independent `Interpret` option.
  This restores AOSP's distinction between `-Xusejit:false` (AOT or Nterp)
  and `-Xint` (switch interpreter). When JIT is enabled, the differential
  launcher also preserves an already selected speed-AOT entrypoint instead of
  replacing it with harness-forced JIT code.
- Removed the remaining Darwin `LoadClass`/`LoadString` graph allowlist from
  `OptimizingCompiler`. The ordinary AOSP admission path now compiles those
  methods for both JIT and dex2oat. Exercising it exposed and fixed the real
  ARM64 boundary: implicit null checks now decode a nullable base-relative
  compressed reference before the faulting load.
- All seven pinned VDEX cases `628`, `629`, `634`, `674`, `820`, `842`, and
  `860` pass unchanged with exact interpreter/JIT output. `629` proves the
  speed OAT method is selected; `860` proves malformed bytecode still reaches
  the expected `VerifyError` through compiled AOT callers. Retained evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-629-vdex-speed-wplkxuif`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-860-vdex-failure-a5q90kdx`,
  and
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-628-vdex-uqc9jx09`.
- Unchanged `552`, `597`, `689`, and `2286` regressions plus the complete
  graphics/runtime link audit pass after allowlist removal. The 100% goal is
  still active: finish the remaining per-invocation prebuild/app-image/DM
  semantics, then rerun the complete corpus and production APK/Blue Archive
  acceptance gates.

### Full prebuild, app-image, secondary-dex and DM contracts — 2026-09-06

- The upstream adapter now treats AOSP's default `prebuild=True` and
  `app_image=True` as real invocation state instead of creating application
  artifacts only as a side effect of `profile=True`. Each contract rebuilds
  or removes its own `.art`, `.odex`, and `.vdex` files before launch.
- Primary dex2oat now follows the invocation's profile/compiler/verifier
  policy. Secondary `-ex.jar` inputs are independently compiled with AOSP's
  default or explicitly supplied class-loader context, and
  `secondary_app_image=False` / `secondary_compilation=False` affect the
  actual generated artifacts rather than being ignored.
- `dex2oat_dm=True` creates `primary.vdex`, packages the oat-directory DM, and
  recompiles with `--dm-file`; `runtime_dm=True` packages the sibling runtime
  DM. Verifier policy, JVMTI attachment, and sync are also per invocation.
- Exact-output evidence passes for `001`, `616`, `734`, `674-HelloWorld-Dm`,
  `141`, `833`, `126`, `178`, and `628`; the 255-object incremental runtime
  audit passes. The 100% goal remains active: dynamic no-image/relocation,
  timeout/log-diff semantics, full-corpus rerun, and unchanged production APK
  and Blue Archive acceptance remain. `855` and `597-app-images-same-classloader`
  exposed missing hidden SDK compile signatures that must be repaired without
  editing pinned tests.
- Those two hidden compile-surface gaps are now closed with signature-only
  Android 16 boot overrides; they are not packaged into app DEX. Unchanged
  `855-native` passes its no-prebuild tracing path, and unchanged
  `597-app-images-same-classloader` passes with both primary and secondary
  profile-guided app images plus its explicit same-loader CLC.

### Runtime-generated app images and real Darwin GC-stress — 2026-09-06

- Removed the placeholder `RuntimeImage` methods, compiled AOSP
  `runtime_image.cc`, and confined Darwin's required logical-reference
  translations to image serialization/deserialization boundaries. Runtime
  images retain AOSP's StartupCompletedTask, heap-task daemon, stop-the-world
  image generation, temporary-file and atomic-rename lifecycle.
- The unchanged `845-data-image` and `846-multidex-data-image` tests now create
  an image in their private run-test sandbox and load it in the following
  process in both interpreter and JIT modes. Unchanged `178`, `597`, `616`, and
  `1003` app-image regressions also pass.
- Added an AOSP-shaped `--gcstress` execution variant. Explicit Android
  `-Xms`/`-Xmx` values now remain owned by ParsedOptions rather than being
  overwritten by detached-host defaults.
- Darwin `BacktraceCollector` now consumes the existing Mach/Mach-O
  unwindstack provider directly. This avoids retrying Linux's ELF unwinder and
  reparsing every Mach VM region for every allocation.
- Restored the missing `Runnable` to `Native` transition in the split Darwin
  startup at the exact point used by `Runtime::Start()`. Without it,
  SignalCatcher could wait in a native condition while still advertised as a
  runnable mutator, deadlocking the first GC-stress checkpoint.
- Unchanged `001-HelloWorld`, `072-precise-gc`, `426-monitor`, and
  `008-exceptions` pass exact output under GC-stress in interpreter and ARM64
  JIT modes. `001` reported 174 unique allocation backtraces across 13,152
  checked allocations in its interpreter process. The full runtime/graphics
  link audit and Mach unwindstack smoke pass with one affected ART object
  rebuilt and 255 cached.
- Removed the redundant test-name branch for `2271-profile-inline-cache`;
  its runtime options now come solely from the parsed unchanged `run.py`, and
  the test still passes in both modes. The 100% goal remains active: complete
  corpus-wide variant execution plus unchanged production APK and Blue Archive
  restart/soak acceptance are still required.

### Mach-O runtime-stub identity and honest custom-contract accounting — 2026-09-07

- Unchanged `004-JniTest` exposed a shared boot compiled-JNI stack-walk bug.
  The frame was the normal 176-byte JNI stub frame; Darwin's
  `OatQuickMethodHeader::IsStub()` returned `nullopt`, so ART skipped the
  current-entry header and misclassified it as a 224-byte GenericJNI frame.
- `0150-darwin-oat-quick-method-header-image-identity.patch` restores the AOSP
  classification contract using `dladdr()` Mach-O image-base identity. Only
  PCs in the same loaded image as `Runtime::Current` are runtime stubs;
  anonymous JIT/OAT mappings remain non-stubs. No stack-walker fallback or
  method/test exception was added.
- Temporary stack/GenericJNI diagnostics were removed. The incremental runtime
  rebuild compiled one object and reused 255; the full link audit passed.
  Unchanged `004-JniTest` now passes interpreter and JIT exact output, including
  the NoSuchMethodError and reflected `<clinit>` paths. Unchanged `137-cfi`,
  `178-app-image-native-method`, and the seven adjacent native/stack/thread
  tests pass. Evidence: `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-JniTest-l6psziuc`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-137-cfi-i4shr6x0`, and
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-178-app-image-native-method-l0jlx7a8`.
- A fail-closed audit found the next systemic completion blocker: the current
  runner statically guesses fragments of 348 `run.py`, 124 `build.py`, and 17
  `javac_post.sh` contracts. It can count calls while losing branch, mutable
  args/env, ordered post-processing, external-library and separate artifact
  semantics. Phase A is now building non-executing AST-to-typed-IR frontends;
  unsupported syntax must fail instead of falling back or passing a different
  program. The 100% goal remains active until all 419 custom-script directories,
  the full differential corpus, and unchanged production apps are verified.

### JVMTI agent module ownership review — 2026-09-07

- Unchanged `909-attach-agent` now reaches ART's live `VMDebug.attachAgent`
  path and successfully opens the staged `libtiagent.so`, but the DSO returns
  `-1` before invoking the pinned test callback. The retained evidence is
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-909-attach-agent-x5z1apuz`.
- ART correctly splits `libtiagent.so=909-attach-agent` into the library name
  and the option string `909-attach-agent`. Darwin's reduced
  `DARWIN_ART_TI_AGENT_MINIMAL` entrypoint incorrectly requires a comma and
  therefore rejects this valid empty-tail option. Pinned AOSP
  `ti-agent/common_load.cc` accepts the same string and dispatches it to
  `Test909AttachAgent::OnAttach`.
- The underlying mismatch is module ownership: AOSP puts `attach.cc` and
  `common_load.cc` in the shared `libtiagent`, and puts only
  `disallow_debugging.cc` in `libarttest`. The current directory scan places
  both sources in both per-test DSOs. The generic correction is to build and
  cache the pinned `libtiagent`/`libarttest` module source closures from their
  Android.bp ownership and retain ART's normal startup, lazy-plugin, live
  attach, and non-debuggable rejection lifecycle. No test-name branch or
  alternate callback is appropriate.

### Differential-mode writable sandbox isolation — 2026-09-07

- Unchanged `817-hiddenapi` initially passed interpreter output but failed in
  JIT because both variants shared `$DEX_LOCATION` and AOSP's
  `Main.createNativeLibCopy()` attempted to create the same
  `libhiddenapitest.so`. The generic runner now gives each interpreter/JIT
  process family its own writable DEX/cwd and typed-action root while sharing
  immutable jars and native fixtures.
- The unchanged test now passes exact output in both modes: `interpreter
  expected-output PASS` and `jit expected-output PASS`. Evidence is retained
  at `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-6l6q8mtr`.

### Typed build/post integration — 2026-09-07

- `run-art-upstream-test.py` now consumes typed `build.py` ActionPlans for
  source generation, compiler/D8/Jasmin/Smali settings, ordered default-build
  calls, and file transforms; build-script text/regex inference was removed.
- Typed `javac_post.sh` contracts now execute through the confined ActionPlan
  filesystem and argv boundary. No post script is sourced or launched through
  a shell, and `$1` replay across primary, secondary, and extra class roots is
  derived from the IR.

### Differential sandbox verification — 2026-09-07

- Refined the generic mode isolation to replicate only the process-private
  seed, late build-created `res/` trees, and DEX/JAR basenames required under
  `$DEX_LOCATION`; shared immutable jars and native fixtures stay outside the
  mode roots.
- A fresh unchanged `817-hiddenapi --keep` run passes exact interpreter and JIT
  output, and both sandboxes independently retain `libhiddenapitest.so`.

### Typed capability audit — 2026-09-07

- Removed remaining runner special cases whose behavior is observable in typed
  contracts: target SDK now comes from `default_run` runtime options, native
  bridge setup is detected from the declared NativeBridge option and source,
  deferred zygote JVMTI ownership from the native callback contract, and CFI
  mode from its typed test arguments.
- Remaining source-owner branches are private native ABI/module boundaries;
  they remain fail-closed and are not inferred from mutable script text.

### Dedicated OpenJDK named-JNI owner — 2026-09-07

- Replaced the minimal-start self-image load with a sibling absolute
  `libopenjdk-named-jni-owner.dylib` loaded by JavaVMExt using the AOSP
  `java.lang.Object`/null-loader boot call site and native thread state.
- Its RTLD_LOCAL export manifest is derived from the AOSP FileInputStream,
  UnixFileSystem, UnixNativeDispatcher tables plus Darwin's UnixCopyFile
  entrypoint: exactly 64 `Java_*` symbols, no JNI_OnLoad, and strict
  source/archive/owner missing-or-surplus checks. Aggregate framework/test
  Java_* symbols are not in the boot library table.

### Validation checkpoint — 2026-09-07

- The owner rebuild and exact `nm -gU` audit pass (`64` Java exports, zero
  JNI_OnLoad, zero non-Java exports). Graphics-fast and full runtime-link
  audits reach link stages but remain blocked by existing unresolved
  `darwin_art_bionic_readv`, `darwin_art_bionic_writev`, `open`, and the
  broader runtime-link undefined set; no NIO/provider edits were made here.

### CPU link ownership closure — 2026-09-07

- Reclassified the prior 59 undefineds by owner: AOSP OpenJDK registrars,
  fdlibm/BoringSSL/ICU, ART test sources, androidfw/resource JNI,
  SurfaceFlinger/framework foundations, Android graphics JNI, and the real
  GPU probe are now linked from their production archives/objects. The CPU
  audit reports `undefined=0`; no dummy provider or fallback was introduced.

### Typed build/staging capability audit — 2026-09-07

- Hidden compiler declarations are selected from source-declared type
  capabilities; target SDK, metrics, swappable JNI IDs, NativeBridge, CFI, and
  deferred-zygote behavior likewise come from typed invocation/source facts.
- Remaining test-name checks are limited to native ABI ownership or pinned
  launcher metadata that has no equivalent upstream typed capability.

### Runner capability audit — 2026-09-07

- Removed remaining build/staging branches keyed by test names: debuggable
  compiler policy, JVMTI attach, trace-v2, startup/shutdown owners, and
  redefinition-agent dispatch now use typed invocation or source capabilities.
- Focused contract tests pass; unchanged ART execution reaches the existing
  runtime-link/sandbox blockers without changing corpus inputs.

### CPU link ownership closure — 2026-09-07

- Classified the original 59 undefineds and closed the AOSP OpenJDK,
  ART-test, androidfw/resource-JNI, SurfaceFlinger/framework, graphics-JNI,
  and production GPU owners. CPU `audit-runtime-link` now passes with
  `undefined=0`; no dummy provider or fallback was introduced.

### Revalidation checkpoint — 2026-09-07

- Final-tree `817-hiddenapi --keep` was re-run serially; interpreter/JIT
  execution did not start because the sibling named-JNI owner failed its
  production `dlopen` boundary on `_JVM_GetLastErrorString`. The owner now
  carries the real FileDescriptor and OpenJDK VM support module members; no
  test input or test-name exception was used. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-loy0bxtx`.
- `909-attach-agent` and `2286-method-tracing-aot-code` were not started after
  this blocker surfaced; no result is claimed for either gate.

### OpenJDK JVM shared-owner correction — 2026-09-07

- AOSP's module graph makes `libopenjdkjvm` a shared dependency of
  `libopenjdk`; copying `OpenjdkJvm.cc.o` into the local named-JNI dylib would
  create a second VM-support owner and import additional ART-private state.
  Darwin instead keeps the pinned module in the aggregate process runtime and
  derives its full 55-entry `JVM_*`/`jio_*` export surface from the archive.
- The named-JNI build now retains `_JVM_GetLastErrorString` as an external
  relocation and fails if any `JVM_*`/`jio_*` definition is linked into that
  dylib. The graphics audit also checks every pinned libopenjdkjvm export and
  the genuine libnativehelper `jniRegisterNativeMethods` dependency at the
  runtime boundary; no shim, fallback, or test-name rule was added.
- The focused graphics link audit passes, including a subprocess proof that
  `JVM_GetLastErrorString` resolves from the global runtime before the local
  owner exposes its named JNI surface. Unchanged `817-hiddenapi --keep`
  now loads the owner successfully through the production global-runtime/local-
  owner path, then reaches a later independent startup invariant:
  `LocalReferenceTable::AssertEmpty()` reports capacity 3. Interpreter and JIT
  execution therefore remain unclaimed. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-rczqsrs1`.

### JNI initialization-frame and external-module closure — 2026-09-07

- Darwin's composed boot-native registrars now run inside a nested scoped JNI
  local frame that is popped before `FinishMinimalForDarwinProbe()`. RAII
  covers success and every early failure; ART's `AssertLocalsEmpty()` remains
  intact and now passes, preserving the Android `JNI_OnLoad` local-reference
  lifetime instead of weakening a runtime invariant.
- Typed `_external` test libraries now link their AOSP-declared `libarttest`
  provider through an explicit Mach-O dependency and fixture rpath. Thus
  817's external JNI DSO reaches the real `libarttest_api.cc` owner without
  process-global promotion, unresolved flat lookup, or a test-specific shim.
- Unchanged `817-hiddenapi --keep` passes exact interpreter and optimized JIT
  output, and adjacent unchanged `_external` consumer
  `656-annotation-lookup-generic-jni` passes both modes. Evidence:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-817-hiddenapi-m3nez_vi`.

### Progress — 2026-09-07 generic corpus ledger

- Added a no-allowlist runner that discovers all 1,138 pinned test
  directories, invokes the unchanged per-test runner, supports deterministic
  index sharding, bounded parallelism (default 1), fail-fast/continue, and
  per-test stdout, stderr, artifact, exit, and status records.
- JSON/TSV ledgers are atomically rewritten in sorted order and resume only
  when both the test input tree hash and runner hash match.

### Revalidation — 2026-09-07

- In the final shared tree, serial `909-attach-agent --keep` passed unchanged
  interpreter, JIT, and source interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-909-attach-agent-v32mh1vw`.
- Serial `2286-method-tracing-aot-code --keep` then passed unchanged
  interpreter, JIT, and source interpreter+optimized checks without build
  contention. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-2286-method-tracing-aot-code-xoj20wza`.

### Revalidation — 2026-09-07 no-op corpus input

- `000-nop` now follows its typed `ctx.echo` run contract without requiring a
  fabricated DEX or VM launch; unchanged interpreter and JIT expected-output
  checks both pass.
- The full-corpus ledger resumed the prior `000-nop` failure after the runner
  hash changed and recorded `passed` with its retained artifact path.

### Revalidation — 2026-09-07 004-JniTest JNI ABI boundary

- The first corpus artifact stopped in interpreter execution at the generated
  JNI entry after `registerNativesJniTest()`: the ARM64 stub dereferenced a
  low-32-bit logical `ArtMethod` as a host pointer. This was a stale compiler
  owner object, not a test-specific input or runtime fallback issue.
- Root cause was a missing producer edge in the headless path: `all` and the
  direct `audit-runtime-link` command linked the runtime dylib without first
  rebuilding `libart-compiler-darwin.a`. The native graph also omitted the
  0145 (and neighboring late JNI/codegen) patches from the JIT edge inputs.
  The headless audit now builds the compiler owner before linking, and the
  graph fingerprints every patch applied by the JIT staging owner.
- Rebuilt the patched AOSP JNI compiler-owner objects and relinked the runtime
  closure. Unchanged `004-JniTest --keep` now passes exact interpreter and JIT
  expected output, plus source interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-JniTest-uv29ee0y`.

### Runner lifecycle boundary — 2026-09-07

- Per-mode and corpus subprocesses now start a dedicated process group. On a
  timeout the generic runner sends group SIGTERM, waits for graceful shutdown,
  then sends group SIGKILL if needed and unconditionally reaps the direct
  child. This prevents a timed-out `darwin-art-host` descendant from leaking
  into the next corpus mode or test; no test-name exception or fallback was
  added.
- A Python unit test covers a descendant that ignores SIGTERM and verifies the
  process group is gone after the kill/reap boundary. No ART corpus execution
  is part of this validation.

### Diagnosis — 2026-09-07 004-SignalTest shutdown boundary

- The retained failure reaches every expected signal line and `Signal test OK`
  in both modes, then times out in `DestroyJavaVM`; `Shutdown thread` remains
  in ART's daemon-stop `Monitor::Wait`. The fault log contains only the
  intentional generated-code SIGSEGV.
- LLDB confirmed the native test's SIGSEGV install and query calls enter the
  generic sigchain ABI with signal 11. LLDB's Mach exception interception
  prevents observing the subsequent handler/restore call without changing
  signal semantics. No `TaskProcessor::Stop` or `RunAllTasks` entry was
  observed in the hanging shutdown path.

### Typed run contract boundary — 2026-09-07 004-ThreadStress

- The retained failure was the generic AOSP `default_run` shell boundary:
  `test_args=["--locks-only -o 100"]` must become three argv words, not one
  literal argument. The runner now applies POSIX `shlex` word splitting to
  the assembled typed suffix, with malformed quoting rejected.
- Ordered typed post-actions now materialize the aggregate stdout/stderr
  before execution and carry edits back into that aggregate. This preserves
  AOSP's final nondeterministic-number normalization across both invocations.
- Unchanged `004-ThreadStress --keep` passes exact expected output and exit
  status in interpreter and optimized JIT modes. Focused typed-run unit tests
  pass. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-004-ThreadStress-pgbfs0dd`.

### Typed invocation scalar state — 2026-09-07 116-nodex2oat

- `invocation_from_action()` now carries scalar fields from the captured
  `args_snapshot` into the typed `RunInvocation`; previously `prebuild=False`
  was silently replaced by its dataclass default `True`, causing an app oat
  to be generated and changing `hasOatFile()`.
- The regression is covered by the typed-run unit suite. Unchanged
  `116-nodex2oat --keep` passes exact interpreter and optimized-JIT output and
  exit contracts with no test-name exception or fallback. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-116-nodex2oat-wncamus_`.

### Typed javac-post argument ownership — 2026-09-07 126-miranda-multidex

- `_post_requires_argument()` now recognizes typed `$1` equality conditions
  (`argument_equals`) as well as path-valued references. The unchanged
  `javac_post.sh` therefore receives its primary/classes2 directory argument
  and moves `MirandaInterface.class` at the correct multidex phase.
- Focused typed-run tests pass. Unchanged `126-miranda-multidex --keep`
  passes exact interpreter and optimized-JIT output/exit contracts. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-126-miranda-multidex-os51ubbk`.

### Corpus suffix selection — 2026-09-07

- The generic corpus runner now supports deterministic inclusive
  `--start-at TEST`/`--stop-after TEST` boundaries, applied before optional
  sharding and limiting. This lets blocker discovery resume from a named
  suffix while preserving all out-of-range ledger records.
- JSON summaries retain every result's `runner_hash` and now include the
  sorted set of hashes, making mixed-runner validation explicit. Focused
  Python unit tests cover boundary validation, ledger preservation, and
  mixed runner hashes; no ART corpus execution was run for this change.

### Typed literal file-operation lowering — 2026-09-07 149-suspend-all-stress

- Generic shell evaluation now materializes nonnegative integer literal
  counts such as `tail -n 1` before execution. The executor rejects boolean
  values as counts, preserving an actual integer contract.
- Focused contract/action tests pass. Unchanged `149-suspend-all-stress
  --keep` passes exact interpreter, optimized JIT, and source
  interpreter+optimized checks. Artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-149-suspend-all-stress-vhrifxp3`.

### JVMTI exception-event verification — 2026-09-07 1927-exception-event

- The retained first attempt stopped in app dex2oat with `SIGILL`, before the
  JVMTI agent or Java test entered the VM. The exact unchanged command reran
  successfully under LLDB and through the typed runner, so no JVMTI event
  contract was implicated and no test-specific workaround was added.
- Unchanged `1927-exception-event --keep` passes dex2oat, interpreter,
  optimized JIT, and source interpreter+optimized expected-output checks.
  Focused adjacent `1928-exception-event-exception --keep` passes the same
  three checks. Artifacts:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1927-exception-event-wd03r81e`,
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-1928-exception-event-exception-fdeimqux`.

### Progress — 2026-09-07 2031 zygote/OAT namespace and corpus continuation

- The unchanged `2031-zygote-compiled-frame-deopt` test now passes interpreter,
  optimized JIT, and source interpreter+optimized checks. The generic runner
  keeps the logical `/system/framework/boot.art` identity and physical boot
  image FD namespace for deferred-zygote invocations; it does not weaken
  `only_use_system_oat_files_` or whitelist the writable build tree.
- After the runner fix, corpus continuation passed 2032 through 2036 under the
  new runner hash. The full corpus still requires a final uniform-hash run;
  the ledger retains the earlier 2031 failure record for provenance.
- Focused runner/contract suites remain green (16 unit tests plus ContractIR
  self-tests). Goal remains active pending the remaining corpus and app audit.

### Progress — 2026-09-07 2038–2039 continuation

- Unchanged `2038-hiddenapi-jvmti-ext` and `2039-load-transform-larger` passed
  under the current runner hash; corpus execution remains live at the next
  suffix. No test-specific fallback or APK mutation was introduced.

### Progress — 2026-09-07 2040–2047 continuation

- Unchanged 2040 huge-native-alloc, 2041 bad-cleaner watchdog, 2042–2044, and
  2045–2047 have passed under the active runner. The bad-cleaner result
  exercised the expected ART watchdog abort contract, not a timeout workaround.
- Corpus discovery remains active; final uniform-hash replay and real-app
  validation are still required.

### Verification — 2026-09-07 2041 watchdog timing

- Independent diagnosis confirms 2041's roughly 65-second-per-mode runtime is
  the unchanged AOSP contract: the Cleaner intentionally sleeps forever,
  ReferenceQueueDaemon tolerates five 10-second timeouts, then emits SIGQUIT
  and exits with status 2. Interpreter and JIT expected-output checks pass;
  this is not a runner hang or timeout workaround.
- The corpus has passed through 2047 and is continuing at 2048; uniform-hash
  replay and real-app validation remain outstanding.

### Progress — 2026-09-07 2048–2239 continuation

- Unchanged 2048 bad-native-registry and the subsequent 2230–2231,
  2233–2239 GC, checker, inlining, and VarHandle tests passed. Two metrics
  tests (2232 and 2233 metrics-background-thread) expose a shared log-stream
  contract issue: Android log lines are in host.log rather than the AOSP
  stderr file; a generic fix is in progress.

### Progress — 2026-09-07 2239 continuation

- Additional unchanged VarHandle performance variants through
  `2239-varhandle-perf-vh-get` continue to pass. The two metrics failures are
  reproducibly limited to host-log routing; no ART execution failure is shown.

### Progress — 2026-09-07 2239 VarHandle continuation

- Further unchanged `2239-varhandle-perf` variants (`get-a`, `get-bav`, and
  preceding CAE/CAS/GAA/GAB/GAS/GET cases) continue to pass. Corpus execution
  remains live; metrics stderr routing is still the only retained failure.
-
### Progress — 2026-09-07 metrics contract and current-hash replay

- Generic runner now incrementally merges Darwin host.log into the AOSP
  stderr contract for metrics tests, avoids duplicate ordered output, and
  translates basic-sed capture groups correctly. Unit/contract suites pass
  (18 tests plus art-run-contract).
- Exact `2232-write-metrics-to-log` and `2233-metrics-background-thread`
  interpreter, JIT, and unchanged-source runs all pass. A current-runner-hash
  corpus replay from 2232 is active; full uniform-hash replay and real APK
  validation remain open.

### Progress — 2026-09-07 current-hash corpus and gate audit

- Current-hash replay advanced through `2269-checker-constant-folding-intrinsics`
  with no new failures; the three ledger failures are stale records from older
  runner hashes and will be resolved by the final `--resume` replay.
- Static audit found no active Darwin JIT admission allowlist. The remaining
  Darwin-only runtime execution gate is Apple Nterp disablement (switch
  interpreter fallback); re-enabling Nterp is the next high-value runtime task
  and requires ABI/catch-handler validation, not a gate bypass.

### Progress — 2026-09-07 replay discoveries

- Current-hash replay advanced through `2286` and resumed lexicographic corpus
  entries in the 300-series. New failures are `2271-profile-inline-cache` and
  `304-method-tracing`; both require artifact-level diagnosis before any fix.
  The corpus remains live and completion is not claimed.

### Progress — 2026-09-07 expanded replay

- Current-hash replay continued through `453-not-byte`; the 305, 370,
  401–453 optimizing/compiler, exception, monitor, and allocator tests passed.
- `2271-profile-inline-cache` and `304-method-tracing` remain the only new
  failures under diagnosis. Full replay and real application validation remain
  open.

### Progress — 2026-09-07 generic runner path contracts

- Fixed two generic AOSP runner contracts: expand `$DEX_LOCATION` in typed
  actions (method-trace output paths), and launch each mode from its staged JAR
  so `VMRuntime.registerAppInfo()` and the loaded DexFile share the same path.
- `304-method-tracing`, `2271-profile-inline-cache`, and `082-inline-execute`
  now pass interpreter/JIT/unchanged-source. A clean full `--resume` corpus
  replay is running under the corrected runner.

### Progress — 2026-09-07 early replay and StackWalk diagnosis

- Corrected full replay has reached `052-verifier-fun`; tests from `004-UnsafeTest`
  through `052` pass in the current run. `004-StackWalk` is being reproduced
  separately with its native test library to distinguish stack-walk ABI failure
  from runner setup.

### Progress — 2026-09-07 full replay continuation

- Clean resumable replay restarted at the corpus head under the corrected
  runner. Early tests through `004-ThreadStress` passed; `004-StackWalk` is a
  new failure under focused interpreter/JIT/source diagnosis. The corpus stays
  active and no completion claim is made.

### Progress — 2026-09-07 early corpus continuation

- Corrected full replay advanced through `065-mismatched-implements`; the
  surrounding verifier, exception, finalizer, OOM, process-manager, and field
  access tests pass. `004-StackWalk` remains isolated for ABI diagnosis.

### Progress — 2026-09-07 StackWalk root cause

- Native instrumentation found valid frames; the assertion failed because the
  app OAT from dex2oat stayed at the outer temporary path while execution used
  the per-mode staged JAR.
- Corpus execution was paused to prevent mixed results while a generic OAT
  staging fix is implemented. No runtime or test-specific bypass was added.

### Progress — 2026-09-07 OAT staging correction

- Generic runner now rebuilds the app OAT/VDEX/app-image against each mode's
  staged JAR and keeps the exact DexFile location used at launch. This targets
  the `004-StackWalk` compiled-vs-shadow frame mismatch without changing ART.
- Focused StackWalk verification is running; only after it passes will the
  full corpus replay be restarted.

### Progress — 2026-09-07 OAT staging verified

- `004-StackWalk`, `304-method-tracing`, and `2271-profile-inline-cache` now
  pass interpreter, JIT, and unchanged-source checks with per-mode OAT/VDEX/art
  staging. The issue was generic DexFile/OAT path identity, not StackVisitor.
- Full resumable corpus replay has restarted from `000-nop` under this fixed
  runner; completion and real APK validation remain open.

### Progress — 2026-09-07 verified replay restart

- After the OAT staging fix, full replay records `004-StackWalk` as passed and
  continues through `011-array-copy2` without new failures. The focused
  `1001-app-image-regions` and `628-vdex` checks also remain green.
- A Luna implementation task is now preparing the first real ARM64 Nterp build
  slice with Mach-O/CFI audits; upstream Nterp eligibility gates remain intact.

### Progress — 2026-09-07 OAT staging regression closed

- Full replay now records `004-StackWalk` as passed with the corrected generic
  staging path. Additional `1001-app-image-regions` and `628-vdex` focused
  checks pass interpreter/JIT/unchanged-source, validating profile and image
  contracts beyond one test.
- The full resumable corpus remains active; the untracked runner changes must be
  included in the eventual source change, and Nterp/real APK validation remain.

### Progress — 2026-09-07 resumed corpus and Nterp implementation

- Corrected full replay is active again and has reached `018-stack-overflow`
  without new failures after the StackWalk/OAT fix. Python runner suites remain
  green (18 tests); contract self-checks pass.
- A Luna implementation task is preparing the first real ARM64ng Nterp build
  slice. No Nterp suppression gate has been removed yet.

### Progress — 2026-09-07 corpus continuation

- Corrected full replay has advanced through `033-class-init-deadlock` with no
  new failures after per-mode OAT staging. Core runtime, verifier, exception,
  finalizer, and class-init paths remain green.
- Nterp implementation work remains in progress and is isolated from the
  replay; no gate has been removed.

### Progress — 2026-09-07 post-staging replay

- Full replay remains green through `029-assert` after the OAT staging fix;
  StackWalk, profile, VDEX, and early runtime/verifier coverage are stable.
- ARM64ng Nterp build-slice implementation is active in parallel; no gate has
  been removed and no completion claim is made.

### Progress — 2026-09-07 Nterp build slice in progress

- `runtime_art/arm64.rs` is being extended with real Darwin Mach-O CFI lowering
  and ARM64ng Nterp assembly integration. Acceptance still requires exact
  symbol inventory, unwind/DWARF audits, and the Nterp execution matrix.

### Progress — 2026-09-07 Nterp integration review

- Darwin ARM64ng Mach-O assembly and CFI lowering are present in the working
  tree, while suppression patches remain active in the manifest.
- Build wiring, fail-closed symbol/unwind audits, and the runtime matrix are
  still pending; corpus execution remains paused for this ABI-sensitive work.

### Progress — 2026-09-07 ARM64 build audit

- `cargo check -p art-bootstrap` and `cargo run -p art-bootstrap --
  build-runtime-arm64` pass; 12 Mach-O ARM64 objects retain verified CFI/DWARF
  metadata. The archive does not yet contain an mterp/Nterp object, so Nterp is
  not enabled and the interpreter requirement remains open.

### Progress — 2026-09-07 Nterp wiring status

- The current ARM64 builder changes compile and audit the existing 12-object
  runtime archive, but no ARM64ng Nterp object is linked yet. Nterp remains
  disabled until the generated source is actually wired and exercised.
- Corpus replay is intentionally paused while this build-path change is
  finalized; no completion claim is made.

### Progress — 2026-09-07 Nterp compile gate

- `cargo test -p art-bootstrap` passes (4/4) and ARM64 ordinary runtime
  objects pass their existing Mach-O CFI/DWARF audits.
- ARM64ng Nterp generation/audit emits all 256 handlers, but Apple assembly
  still rejects five non-linear CFI sites. The generated interpreter is not
  archived or enabled; execution and full-corpus validation remain open.

### Progress — 2026-09-07 Nterp Mach-O wiring attempt

- Nterp builder now emits a lowered assembly artifact and attempts a real
  Mach-O object build. Symbol/relocation lowering exposed additional inactive
  FDE CFI and local-branch issues; the build remains fail-closed and the
  archive is not updated until those diagnostics are resolved.

### Progress — 2026-09-07 Nterp relocation audit

- Instruction-table ADR, runtime-instance page relocations, and the pending
  exception local branch are now lowered for Mach-O. Assembly still fails on
  helper CFI emitted outside an active FDE; archive validation remains closed.

### Progress — 2026-09-07 Nterp archive wiring

- Runtime build ordering now requires a successful ARM64ng Nterp object before
  creating `libart-arm64-darwin.a`, preventing a silently Nterp-less archive.

### Progress — 2026-09-07 Nterp linked and exercised

- ARM64ng Nterp compiles to Mach-O with verified `__eh_frame`/DWARF and is
  linked as object 13 in the ART archive. Darwin suppression patches are no
  longer selected.
- `821-many-args`, `825-unbalanced-lock`, and `830-goto-zero` pass interpreter,
  JIT, and unmodified-source lanes. Full matrix and Blue Archive validation
  remain open.

### Runtime checkpoint 371 — 2026-09-09

- Ran unmodified AOSP `004-ReferenceMap`. Interpreter expected output, JIT
  expected output, and interpreter-versus-optimized output all pass. This
  adds direct evidence for JIT stack maps and managed reference locations;
  JNI CFI address publication, GC stress, concurrency, and real-app criteria
  remain open.

### Progress — 2026-09-07 Nterp helper-CFI diagnosis

- Reproducible generation reaches clang after relocation lowering. The
  remaining failure is helper macro CFI scope; Nterp archive linkage and
  runtime execution remain withheld.

### Progress — 2026-09-07 Nterp FDE-preserving CFI lowering

- `runtime_art/nterp.rs` now splits the four return handlers and the late
  `nterp_helper` region into adjacent Mach-O FDEs. This prevents `.org`-induced
  backwards CFI advances and keeps helper CFA metadata inside an active FDE.
- Invalid state-stack transitions and the helper-expanded `.cfi_restore x22`
  are lowered while explicit CFA/callee-save seeds are retained at each new
  FDE boundary. Existing Nterp suppression patches remain unchanged.
- `cargo run -p art-bootstrap -- build-nterp-arm64ng` passes and produces
  `_build/nterp-arm64ng/mterp_arm64ng_darwin.o` (`Mach-O 64-bit object arm64`).
  `nm -nm` finds all six required entry/table symbols, `otool -l` contains
  `__eh_frame`, `llvm-objdump --unwind-info` reports 23 entries, and
  `llvm-dwarfdump --verify` reports no errors. `cargo test -p art-bootstrap`
  passes 4/4. Runtime Nterp eligibility and suppression gates remain active.

### Progress — 2026-09-07 Nterp enabled regression lanes

- With the current linked Nterp archive, `818-clinit-nterp` and
  `824-verification-rethrow` completed interpreter/JIT/unmodified lanes; the
  concurrent `823-cha-inlining` run produced no runtime failure.

### Progress — 2026-09-07 Nterp GC/concurrency expansion

- Parallel lanes `072-precise-gc`, `074-gc-thrash`, `421-exceptions`, and
  `051-thread` all passed interpreter/JIT/unmodified checks with Nterp linked.

### Progress — 2026-09-07 Nterp deopt/monitor/JNI lanes

- `837-deopt`, `596-monitor-inflation`, and `667-jit-jni-stub` passed all
  interpreter/JIT/unmodified lanes with linked Nterp. Full stress corpus and
  real APK validation remain open.

### Progress — 2026-09-07 Nterp corpus shard 000–010

- A fresh ledger shard of the first 20 discovered corpus inputs passed in
  parallel (`000-nop` through `010-instance`, including JNI, StackWalk,
  ReferenceMap, SignalTest, and ThreadStress). No new failures were recorded.

### Progress — 2026-09-07 Nterp memory/deopt expansion

- `1000-non-moving-space-stress`, `602-deoptimizeable`, and
  `1001-app-image-regions` completed their compile/runtime lanes without a
  reported crash; 1000 and 602 reached explicit PASS for interpreter/JIT and
  unmodified execution. Full stress coverage remains open.

### Progress — 2026-09-07 Nterp corpus shard 011–029

- The next 20 discovered inputs (`011-array-copy` through `029-assert`,
  including `018-stack-overflow`) passed on a fresh parallel ledger, covering
  arrays, arithmetic, strings, interfaces, access checks, and stack depth.

### Progress — 2026-09-07 Nterp corpus shard 030–049

- The next 20 inputs passed concurrently, covering finalizers, class-init
  deadlock, calls, inheritance, threads, narrowing, reflection, returns, and
  proxy dispatch. No new failures were recorded.

### Progress — 2026-09-07 Nterp corpus shard 050–070

- Twenty more inputs passed concurrently, covering synchronization, verifier
  behavior, wait/park, uncaught exceptions, OOM, encodings, field access,
  class loading, NIO buffers, and process management.

### Progress — 2026-09-07 Nterp corpus shard 071–084

- Twenty inputs passed concurrently, including DexFile variants, precise GC,
  reachability fences, verification errors, polymorphic virtual calls,
  phantom references, OOM/finalizer paths, hot exceptions, inline execution,
  compiler regressions, and class initialization.

### Progress — 2026-09-07 Nterp corpus shard 085–1004

- Twenty inputs passed concurrently, covering null-super, GC-after-link,
  monitor verification, loop formation, package-private overrides, locale,
  serialization/patterns, switch extremes, concurrent array copy, reflection,
  VMDebug, Fibonacci, app-image metadata, and volatile reference loads.

### Progress — 2026-09-07 Nterp corpus shard 102–121

- Twenty inputs passed concurrently, covering concurrent/parallel GC, growth
  limits, invoke and exception variants, suspend checks, multidex, native
  bridge, no-image dex2oat, hash/modifier/NPE, and math operations.

### Progress — 2026-09-07 Nterp corpus shard 123–138

- Twenty inputs passed concurrently, covering multithread compiler regressions,
  missing classes, secondary dex, GC/class loading and coverage, register
  spill/promotion, daemon/JNI shutdown, hprof, CFI, and duplicate classes.

### Progress — 2026-09-07 Nterp corpus shard 179–1914

- Twenty inputs passed concurrently, covering nonvirtual/default methods,
  method linking, RMW stress, allocation tracking, bytecode inspection,
  suspend/double-suspend/native suspend, JVMTI local-variable access, and
  per-agent TLS.

### Progress — 2026-09-07 Nterp corpus shard 201–2033

- Twenty inputs passed concurrently, covering built-in exception messages,
  thread OOME/checkpoints, invoke inlining, invariant and backward loops,
  memory-load/store couples, stack-walk instrumentation, contended monitors,
  long-running children, default-method overrides, structural JNI failures,
  shutdown mechanics, and zygote compiled-frame deopt.

### Progress — 2026-09-07 Nterp corpus shard 139–158

- Twenty inputs passed concurrently, covering register natives, DCE/field
  packing, class unloading/loading, allocation tracking, interfaces,
  multithread GC annotations, suspend-all, loadlibrary/open-file limits,
  reference stress, GC loops, multi-loader dex registration, and app-image
  class tables.

### Progress — 2026-09-07 Nterp corpus shard 159–178

- Twenty inputs passed concurrently, covering app-image fields/methods/strings,
  read-barrier stress, method resolution/linking, lock-owner proxies,
  interfaces, VM stack annotations, threadgroup JNI, allocation, and class-init
  deadlocks.

### Progress — 2026-09-07 Nterp compiler shard 300–411

- Twenty inputs passed concurrently, covering dispatch/verification, tracing,
  Dex v37, optimizing compiler control flow/long/allocator, fields/arrays,
  materialized conditions, floating point, and checker arithmetic.

### Progress — 2026-09-07 Nterp compiler shard 412–431

- Twenty inputs passed concurrently, covering new-array, regalloc, static
  fields, arithmetic/strings/long/class constants, exceptions and large
  frames, instanceof/checkcast/type conversion, invoke-interface/super,
  monitor/bitwise/bounds, SSA construction, live-register slow paths, and type
  propagation.

### Progress — 2026-09-07 Nterp compiler shard 432–447

- Twenty inputs passed concurrently, covering optimizing comparisons/GVN,
  shifter operands, invoke-direct, try/finally, new-instance, floating rem,
  shifts, inline/volatile/swap-double/NPE, STMP, and checker inliner,
  constant-folding, NCE, and LICM paths.

### Progress — 2026-09-07 Nterp compiler shard 448–463

- Twenty inputs passed concurrently, covering multiple returns, BCE/type
  checkers, float spill/regression, byte/vreg/reference-register handling,
  GVN, baseline array set, instruction simplification, long-to-FPU, dead-phi,
  dex-file inlining, and boolean simplification.

### Progress — 2026-09-07 Nterp compiler shard 464–476

- Twenty inputs passed concurrently, covering inline-sharpen calls, clinit GVN,
  live-vreg/regalloc pairs, condition materialization, huge methods, deopt
  environments, uninitialized locals, unreachable branches, inliner constants,
  dead-block removal, FP operations, constructor fences/barriers, and clinit
  static-invoke inlining.

### Progress — 2026-09-07 Nterp compiler shard 477–492

- Twenty inputs passed concurrently, covering bound types, long/float
  precision, clinit-check pruning, nested/recursive/call inlining, implicit
  null checks, phi conditions, dead-block/DCE loop/switch, loop back edges,
  register hints, null-check requirements, and current-method handling.

### Progress — 2026-09-07 ClassLoader mismatch found

- `497-inlining-and-class-loader` exposed a real interpreter-lane failure:
  the runtime rejects the staged interpreter DEX because its application
  ClassLoader identity does not match the incoming DEX path (status 122).
  Sol-high diagnosis is active; no test-specific bypass is being added.

### Progress — 2026-09-07 Per-loader DexCache patch / follow-up fault

- Added the generic `(DexFile, ClassLoader)` DexCache lookup and registration
  path in patch `0154-darwin-per-loader-dex-cache.patch`; the runtime, JIT,
  and dex2oat compilation stages pass and the patch applies cleanly to the
  pinned AOSP source.
- Nterp Darwin symbol decoration/link-closure checks now pass, including the
  graphics dylib fast audit. Re-running `497-inlining-and-class-loader` no
  longer reports the ClassLoader mismatch, but the interpreter lane currently
  reaches generated code and exits with a SIGSEGV at `0x70000884`; this is the
  next runtime fault to diagnose before claiming the test fixed.

### Progress — 2026-09-07 Nterp compressed-reference fault localized

- The follow-up SIGSEGV is localized to `.Lop_sput_object_resume_after_read_barrier`:
  Nterp reads a compressed `ArtField::declaring_class_` reference and performs
  a static-field/card-mark store without adding Darwin's compressed-reference
  base. Sol is implementing the corresponding object.S decode at fast and
  slow read-barrier resume points; the ClassLoader semantics patch remains
  intact.

### Progress — 2026-09-07 Nterp card-table base correction

- Confirmed the AOSP card-table contract: Darwin's biased card table requires
  the full native heap address before the `>> kCardShift` operation. The
  Nterp barrier macro was dropping the compressed-reference high base; the
  lowering now restores that base before computing the card index, matching
  the existing compiler lowering precedent. Unit/build/497 verification is
  in progress.

### Progress — 2026-09-07 Nterp invoke receiver decode

- Static store and card barrier now pass in 497. The next fault is the
  virtual-dispatch receiver path: invoke.S dereferences a compressed receiver
  without restoring Darwin's native reference base. Sol is adding a scoped
  receiver/class-temporary decode for virtual/interface dispatch while leaving
  managed argument W registers compressed as required by the AOSP ABI.

### Progress — 2026-09-07 Nterp fill-array-data boundary

- Virtual and interface dispatch now pass 497. The next failure is the
  `FillArrayData` quick entrypoint receiving a compressed array reference
  directly from Nterp. Sol is adding a null-preserving decode at the
  array.S-to-runtime helper boundary and auditing neighboring managed-object
  helper calls for the same contract violation.

### Progress — 2026-09-07 Nterp array reference inventory

- The array lowering now covers direct aget dereferences, primitive aput,
  array-length, and fill-array-data native-object boundaries. `aput-object`
  retains its existing dedicated Darwin lowering and uses a decoded temporary
  only for bounds checks. Focused Nterp tests (6/6), ARM64 build, and DWARF
  audit pass; 497 relink/retest is next.

### Progress — 2026-09-07 Stack-walk loader selection

- Array lowering removed the compressed-reference faults and 497 reaches its
  intended inlined `$noinline$bar` stack walk. The remaining CHECK occurs when
  an inline `MethodInfo` has `dex_file_index == UINT32_MAX` and stack walking
  falls back to the earliest, loader-agnostic DexCache. Sol is threading the
  outer method/declaring-class loader into this resolution path so the
  per-loader cache is selected consistently.

### Progress — 2026-09-07 Nterp card-table barrier fault

- The four-point compressed-reference decode now lets 497 complete the static
  field store itself; the next fault is in the generated write barrier, where
  the decoded declaring-class pointer is incorrectly reused for card-table
  indexing. ClassLoader mismatch is still absent. Sol is separating the
  logical compressed reference used by the barrier from the decoded pointer
  used for the field access, then will rerun all 497 lanes.

### Progress — 2026-09-07 Loader-scoped inline method resolution

- The permanent compatibility patch now scopes `GetResolvedMethod` and
  `FindDexCache` (including OatDexFile lookup) to the active ClassLoader; the
  same-dex sentinel uses the immediate inlined method's DexCache rather than
  the outermost canonical cache. Patch dry-run and whitespace checks pass;
  runtime rebuild and 497 verification are pending.

### Progress — 2026-09-07 ClassLoader/Nterp 497 closure

- `0154-darwin-per-loader-dex-cache.patch` now models application registrations
  by `(DexFile, ClassLoader)`: the same native DEX may own independent DexCaches
  and ClassTables in distinct user loaders. The boot-versus-user collision
  guard remains because hidden-API domain state and compiled BSS roots are
  native-Dex-wide, not loader-local.
- Inline stack resolution no longer uses the loader-agnostic earliest cache.
  Indexed app records use the immediate caller's loader, same-DEX sentinel
  records use that caller's DexCache, and boot records remain explicitly
  loader-null. This preserves loader identity across nested inline chains.
- Darwin Nterp staging now decodes compressed references at static-field,
  card-table, virtual/interface dispatch, direct array-access, and
  fill-array-data native boundaries while keeping managed argument registers
  compressed. Focused Nterp tests pass 6/6, the ARM64 runtime/link audit passes,
  and `497-inlining-and-class-loader` passes interpreter expected output, JIT
  expected output, and the unmodified source interpreter+optimized check.

### Progress — 2026-09-07 Nterp corpus shard 493–510

- The fresh `_build/art-upstream-corpus-nterp-493-rerun` ledger ran 20 inputs
  from `493-checker-inline-invoke-interface` through `510-checker-try-catch`
  with `--parallel 4`; every result passed with exit code 0. This covers the
  checker/inlining and ClassLoader lanes plus type, array, baseline, and
  referrer cases. No failure remained to diagnose, and no generic runtime
  change was needed.

### Progress — 2026-09-07 Nterp corpus shard 511–527

- The fresh `_build/art-upstream-corpus-nterp-511` ledger ran 20 inputs from
  `511-clinit-interface` through `527-checker-array-access-split` with
  `--parallel 4`; every result passed with exit code 0. This covers clinit,
  array deoptimization/access, shifts, DCE, checker fallthrough and
  regressions, class loading, phi equivalence, array fields, and register
  allocation. No failure remained to diagnose, and no generic runtime change
  was needed.

### Progress — 2026-09-07 Nterp corpus shard 528–531

- The fresh `_build/art-upstream-corpus-nterp-528` ledger ran 20 inputs from
  `528-long-hint` through `531-regression-debugphi` with `--parallel 4`; every
  result passed with exit code 0. This covers long hints/splits, unresolved and
  loop checkers, instance-of/checkcast, load/store elimination,
  peeling/unrolling, reference typing, instanceof, LSE regression, and
  debug-phi handling. No failure remained to diagnose, and no generic runtime
  change was needed.

### Progress — 2026-09-07 Nterp reference-boundary inventory

- Generalized Darwin lowering with nullable/non-null heap-reference decode
  macros and fail-closed per-template boundary counts across object, array,
  invoke, throw, and monitor paths. Focused Nterp tests pass 10/10 and the
  256-handler Mach-O/DWARF build is clean; generated-assembly and runtime
  smoke validation remain next.

### Progress — 2026-09-07 Full Nterp managed-reference boundary audit

- Extended the common lowering to object type checks/hierarchy/component
  dereferences, iget/iput holders, nullable throw, and monitor enter/exit;
  existing read-barrier mark and aput-object lowerings remain single-source.
  Fail-closed staged counts are main 1/0, array 3/1, invoke 4/0, object 20/0,
  control 0/1, other 0/2, with 99 expanded heap-base operations in the
  assembled object. `cargo test -p art-bootstrap` is 13/13, build/link/DWARF
  audits pass, and both 497 and 003-omnibus-opcodes pass all lanes.

### Progress — 2026-09-07 542 contract audit correction

- Deep comparison shows `542-unresolved-access-check` fails identically in
  unchanged AOSP DEX under Nterp, `-Xint`, JIT/AOT, isolated JIT, and both
  soft-fail modes. `interp-ac` only supplies `--switch-interpreter
  --verify-soft-fail`; there is no special classpath contract. The test's
  custom loader intentionally has a distinct `PlaceHolder` class from its
  parent loader, so `entered` remains false and `loadClass` returns null.
  No Darwin runtime divergence or safe generic fix is evidenced; no bypass was
  added.

### Progress — 2026-09-07 Nterp corpus shard 532–545 (pending access-check contract)

- The fresh `_build/art-upstream-corpus-nterp-532` ledger ran the next 20
  deterministic inputs from `532-checker-nonnull-arrayset` through
  `545-tracing-and-jit` with `--parallel 4`; 19 passed. The sole failure,
  `542-unresolved-access-check`, reproduces in both interpreter and JIT lanes
  with the unchanged AOSP custom loader's intentional `ClassLoader.loadClass`
  null return (`ClassLoader.loadClass returned null for p1.OtherInP1`). A
  JIT-only isolation run confirms this is the test's `interp-ac`/soft-
  verification launch contract, not a new Darwin reference-boundary fault;
  no test-specific runtime bypass or generic runtime change was made.

### Correction — 2026-09-07 542 custom-loader state mismatch

- The preceding `interp-ac`/soft-verification attribution was incorrect.
  Pinned ART (`c6ccc2e9cd6c1d64a9edb613c84ddc24b533e7b9`) has no `run.py`
  for 542. `testrunner.py` maps `interp-ac` to `--switch-interpreter
  --verify-soft-fail`; `default_run.py` selects `-Xint`,
  `-Xverify:softfail`, and the verify compiler filter, not a different
  ClassLoader or classpath-sharing policy.
- `MyClassLoader` itself belongs to the parent PathClassLoader, so its null
  gate reads the parent's `p1.PlaceHolder.entered`. Its child-first DEX
  loading defines `InP1` and another `PlaceHolder` in `MyClassLoader`; the
  final iteration writes that child's flag. A separate no-AOT reflection
  diagnostic observes distinct class objects, parent flag `false`, child
  flag `true`, and a subsequent loader request still returning null. The
  unmodified DEX places `sput-boolean` before `new-instance`; the exception
  stack is at `InP1.java:27`, called from the final iteration at
  `Main.java:90`, not a verifier failure during warm-up.
- AOSP `RegTypeCache::From` clears failed class-resolution exceptions and
  creates unresolved reference types. `ClassLinker::FindClass` intentionally
  throws NPE when a custom loader returns null at execution. Pinned
  `knownfailures.json` already disables 542 (and 497) in all variants because
  its loader re-registers one native DexFile across different loaders;
  unmodified upstream rejects that registration earlier. Darwin's existing
  per-loader DexCaches let this test proceed to its independent flag-state
  defect. Sharing flag/static state across loaders or ignoring a null return
  would violate the isolated loader contract, not repair verification.
- Fresh unchanged-source runs fail in Nterp, switch interpreter, JIT/AOT,
  isolated JIT without app OAT artifacts, softfail Nterp, softfail JIT, and
  combined `interp-ac` runtime options (`-Xint -Xverify:softfail` with the
  verify compiler filter);
  isolated JIT logs `optimized application method installed`. Evidence is
  in `_build/542-access-check-diagnosis/` and
  `_build/542-diagnosis-runner.log`; `identity.stdout` is diagnostic evidence,
  not an acceptance result. No test source, expected output, runtime bypass,
  or knownfailure/skip rule was changed; the corpus result remains FAIL.
- An ancillary AOT-backed identity run observed a different child-flag value
  (`false` instead of the no-AOT `true`). That compiled-cache/static-field
  observation is not explained by this diagnosis and remains a separate
  follow-up; it cannot explain away the proven no-AOT loader-state mismatch.

### Progress — 2026-09-07 Nterp corpus shard 546–557

- The fresh `_build/art-upstream-corpus-nterp-546` ledger ran the next 20
  deterministic discovered inputs from `546-regression-simplify-catch`
  through `557-checker-instruct-simplifier-ror` with `--parallel 4`; all 20
  passed with exit code 0. This covers catch simplification and try/catch
  critic edges, checker inlining/type propagation/accumulation, clinit and
  invoke-super cases, implicit null checks, sharpening, AVX2 bit manipulation,
  checkcast, UnsafeGetLong, and rotate-right instruction simplification. No
  failure remained to diagnose, so no generic runtime change was needed.

### Progress — 2026-09-07 Nterp corpus shard 558–566

- The fresh `_build/art-upstream-corpus-nterp-558` ledger ran the next 20
  deterministic discovered inputs from `558-switch` through
  `566-polymorphic-inlining` with `--parallel 4`; all 20 passed with exit code
  0. The runner hash was `b38149585b1995de3f144aaed650dc08764fee7dabedb6e366fbaa6b42799792`.
  This covers packed and sparse switch control flow, BCE/SSA and loop
  handling, shared slow paths, div/rem, checker no-intermediate/fakestring/
  invoke-super/bitcount/loop/irreducible/negbitwise/codegen-select cases, and
  polymorphic inlining. No failure remained to diagnose, so no generic runtime
  change was needed.

### Progress — 2026-09-07 Nterp corpus shard 567–580

- The fresh `_build/art-upstream-corpus-nterp-567` ledger ran the next 20
  deterministic discovered inputs from `567-checker-builder-intrinsics`
  through `580-crc32` with `--parallel 4`; all 20 passed with exit code 0.
  The runner hash is
  `8601b0f3a5247ea8336e633b3824bb3c972c6521152c9c99c8e75c8d9d2c29b9`.
- The initial run exposed two generic javac bootclasspath coverage gaps,
  `580-checker-fp16` and `580-checker-string-fact-intrinsics`, before any ART
  execution. The pinned AOSP sources are present at
  `_aosp/libcore-full/luni/src/main/java/libcore/util/FP16.java` and
  `_aosp/libcore-full/libart/src/main/java/java/lang/StringFactory.java`,
  while the staged boot DEX already contains those runtime classes. The
  compiler-only companion described below now stages the shared signatures;
  no test-source capability detection is used. Focused reruns and the
  complete 20-input rerun then passed; no test source, expected output, or
  runtime behavior was changed.

### Progress — 2026-09-07 libcore compiler classpath staging

- The two missing APIs are core-libart classes, not core-oj classes: the
  pinned `core-libart.jar` DEX contains `Llibcore/util/FP16;` and
  `Ljava/lang/StringFactory;`. Their source of truth is the matching pinned
  checkout at `_aosp/libcore-full/luni/src/main/java/libcore/util/FP16.java`
  and `_aosp/libcore-full/libart/src/main/java/java/lang/StringFactory.java`.
- `core-oj-compat.jar` remains a DEX/security-properties compatibility input;
  a DEX-only JAR cannot satisfy javac symbol lookup. The generic framework
  build now emits `_build/android16-libcore-compiler-api/core-libart-compiler.jar`
  from the shared compiler-capability source set. The companion is used only
  for javac's boot/class path, while ART continues to execute the pinned
  core-libart DEX. No test-name or test-source special case is involved.
- Focused reruns of `580-checker-fp16` and
  `580-checker-string-fact-intrinsics` pass interpreter, JIT, and the
  unmodified-source checker lanes. The compiler companion is not included in
  app DEX output or the runtime boot class path.

### Progress — 2026-09-07 Nterp corpus shard 581–594

- The fresh `_build/art-upstream-corpus-nterp-581` ledger ran the next 20
  deterministic discovered inputs from `581-checker-rtp` through
  `594-invoke-super` with `--parallel 4`; all 20 passed with exit code 0.
  The runner hash is
  `c5816ef02710725f8c1e73f1ca841664b029598b5ec1969c1da0d45aa6f6429`.
- No failure remained to diagnose, so no generic runtime or compiler change
  and no rerun were needed.

### Progress — 2026-09-07 Nterp corpus shard 595–608

- The fresh `_build/art-upstream-corpus-nterp-595` ledger now reports all 20
  deterministic inputs from `595-error-class` through `608-checker-unresolved-lse`
  passed with exit code 0 under four-way parallel execution.
- The first `595-profile-saving` run compiled and completed dex2oat, then
  failed at runtime because the AOSP shell-assembled option
  `-Xcompiler-option --compiler-filter=verify` had been passed as one host
  argv element. The generic runner now applies the same typed shell-word
  boundary to every `runtime_option` entry (with a focused unit test), after
  resolving `$DEX_LOCATION`; no test-name allowlist or source mutation was
  added. Focused and complete-shard reruns pass interpreter, JIT, and
  unmodified-source optimized lanes.
- Final ledger results are 20/20 passed with runner hash
  `8711a87dbf1979af6b6170789c07740a9496061e93527d545c1e457cc745c5a8`.

### Progress — 2026-09-07 Nterp corpus shard 609–619

- The fresh `_build/art-upstream-corpus-nterp-609` ledger ran the next 20
  deterministic discovered inputs from `609-checker-inline-interface`
  through `619-checker-current-method` with `--parallel 4`; all 20 passed
  with exit code 0. The runner hash is
  `8711a87dbf1979af6b6170789c07740a9496061e93527d545c1e457cc745c5a8`.
  No failure remained to diagnose, so no generic runtime change was needed.

### Progress — 2026-09-07 Nterp corpus shard 620–636

- The fresh `_build/art-upstream-corpus-nterp-620` ledger ran the next 20
  deterministic discovered inputs from `620-checker-bce-intrinsics` through
  `636-wrong-static-access` with `--parallel 4`; all 20 passed with exit code
  0. The runner hash is
  `2d168dd76b52cf79ef7c4bb504a263d92a4f07d6f8e8559725b80c58205ea08a`.
- The initial `629-vdex-speed` failure was generic per-mode VDEX staging:
  VDEX/ODEX artifacts were built against the outer JAR, while each differential
  mode loads a private JAR path, so ART correctly rejected the artifacts and
  observed an interpreted `Main.main`. The runner now rebuilds VDEX/ODEX and
  app images beside each mode-local JAR using its exact DEX location; focused
  and complete-shard reruns pass. No test source, expected output, allowlist,
  or runtime behavior was changed.
### Progress — 2026-09-08 Nterp corpus shard 620 (in progress)

- The shard ledger has 19 passing inputs; `629-vdex-speed` remains under
  focused diagnosis because `isAotCompiled(Main.main)` is false after VDEX
  speed recompilation. No APK or test-specific bypass was added.

### Correction — 2026-09-08 Nterp corpus shard 620

- The preceding in-progress note is superseded: after generic per-mode VDEX
  staging was applied, the complete `620–636` ledger passed 20/20, including
  `629-vdex-speed`.

### Progress — 2026-09-07 active Nterp admission audit

- The legacy `_build/runtime-bootstrap/patched-source` directory still
  contains the retired Apple Nterp-disable patches, but it is not an input to
  the current build. The active shared shadow is
  `_build/runtime-common/patched-source`; it follows AOSP admission, returns
  the ARM64ng entrypoints and catch landing pad, and its compiled
  `IsNterpSupported()` returns true on the pinned Darwin ARM64 configuration.
- Runtime staging now fails closed if either retired patch reappears in the
  active shadow. The manifest also asserts that patches 0017 and 0019 are not
  part of the production patch set, and the command help no longer describes
  the linked ARM64ng implementation as runtime-disabled.
- `build-runtime-bootstrap` passed after the audit (`25` compiled, `230`
  cached runtime objects). Unmodified upstream `667-jit-jni-stub`, whose
  `prebuild=False` interpreter lane has no application OAT artifacts and runs
  with JIT disabled, passed both interpreter and optimized modes. This closes
  the stale-shadow ambiguity; it does not identify a new JIT codegen gap or
  expand the completion claim.

### Progress — 2026-09-08 continuation

- The next deterministic corpus shard is being prepared from the discovered
  AOSP ordering. A Sol high-level audit is concurrently inventorying remaining
  JIT eligibility and OSR/deoptimization gaps; no completion claim is made.

### Progress — 2026-09-08 Nterp corpus shard 637 (in progress)

- The 20-input ledger from `637-checker-throw-inline` onward has one failing
  case, `641-iterations`. Its host log shows a method-resolution failure for
  `Main.init()` followed by SIGABRT; the failure is under generic class-linker/
  DEX-path diagnosis, with no test mutation or allowlist added.

### Correction — 2026-09-08 Nterp corpus shard 637

- The preceding in-progress record is superseded. After generic per-mode
  staging/class-linker correction, `_build/art-upstream-corpus-nterp-637-rerun`
  reports 20/20 passing, including `641-iterations`.

### Correction — 2026-09-08 Nterp corpus shard 637

- The preceding in-progress note is superseded. A fresh four-worker ledger at
  `_build/art-upstream-corpus-nterp-637-rerun` ran the next 20 deterministic
  inputs from `637-checker-throw-inline` through `648-inline-caches-unresolved`;
  all 20 passed (exit code 0), with runner hash
  `2d168dd76b52cf79ef7c4bb504a263d92a4f07d6f8e8559725b80c58205ea08a`.
- `641-iterations` exposed a generic shared-native-registration bug: its
  ordinary managed `Main.init()V` collided with a name in the harness's
  libarttest native table. Registration now reifies the exact JNI method ID and
  checks ART's `ArtMethod::IsNative()` before calling `RegisterNatives`, so
  managed collisions are skipped while real native declarations remain
  registered. Focused and complete-shard reruns pass interpreter, JIT, and
  unmodified-source optimized lanes. No test source, expected output, APK,
  allowlist, or runtime bypass was changed.
### Progress — 2026-09-08 continuation

- Local `art-bootstrap` validation remains green (13/13) after the generic
  native-registration fix. The next discovered AOSP corpus range is running;
  the full completion audit remains open.

### Progress — 2026-09-08 continuation

- The next deterministic shard beginning at `649-vdex-duplicate-method` is
  running with four workers (7 completed inputs so far, all passing). A second
  Sol-high audit is checking real JIT execution evidence in an unmodified AOSP
  test; completion remains unclaimed.

### Progress — 2026-09-08 continuation

- The same `649` ledger has advanced to 17 completed inputs, all passing. The
  remaining cases and the independent unmodified-test JIT audit are still live.

### Progress — 2026-09-08 continuation

- The `649` ledger has advanced to 19 completed inputs, all passing; one final
  input remains. The Sol-high unmodified-test JIT audit is still running.

### Progress — 2026-09-08 Nterp corpus shard 649

- The final ledger input, `655-jit-clinit`, passes its interpreter lane but the
  JIT lane times out after 120 seconds while `Foo.$noinline$hotMethod` waits
  for a JIT entrypoint. This is being treated as a real JIT compilation/install
  gap, not waived as a harness failure.

### Progress — 2026-09-08 focused JIT diagnosis

- The JIT host log shows the AOSP default warmup threshold (`65535`) while
  `testrunner.py` globally supplies `-Xjitthreshold:0` for JIT lanes. Generic
  runner parity and a focused rerun are in progress; no test-specific
  threshold is being added.

### Progress — 2026-09-08 focused JIT diagnosis

- Threshold parity alone was insufficient: `Foo.<clinit>` compiled but
  `Foo.$noinline$hotMethod` remained in speed AOT and the test loop hung. The
  generic runner now also mirrors AOSP `default_run.py` by using
  `--compiler-filter=verify` for implicit interpreter/JIT differential
  prebuilds (unit contract 12/12); focused `655-jit-clinit` rerun is active.

### Progress — 2026-09-08 focused JIT diagnosis

- Logs revealed execution was re-materializing a synthetic default action and
  rebuilding the mode-private sandbox with `speed` after the `verify` prebuild.
  Execution now consumes the precomputed invocation plan, preserving AOSP's
  generic `verify` policy; the focused rerun is active.

### Progress — 2026-09-08 focused JIT diagnosis

- The corrected focused run now compiles `Foo.$noinline$hotMethod` and reports
  `Main main(String[]) PASS`. A follow-up normal-policy run is queued to ensure
  this does not depend on the `jit-on-first-use` threshold override.

### Progress — 2026-09-08 normal-policy JIT execution

- Correction: AOSP `testrunner.py` adds `-Xjitthreshold:0` only to its distinct
  `jit-on-first-use` configuration; the normal `art-jit` configuration uses
  `--jit` and ART's default `65535` warmup/optimization thresholds. The
  temporary implicit threshold override was removed.
- The generic no-`run.py` path now carries AOSP `default_run.py`'s
  `--compiler-filter=verify` prebuild into the actual invocation instead of
  rebuilding a mode-private `speed` artifact immediately before launch. This
  removes an AOT mask that prevented hot application methods from becoming
  JIT candidates; no test-name branch or managed/native source change was
  added.
- Unmodified `655-jit-clinit` passes interpreter and normal JIT modes with
  byte-exact output. The JIT log records both thresholds at `65535`, then a
  background compiler thread installs baseline code for
  `Foo.$noinline$hotMethod` in the code cache; the test's original JNI
  `hasJitCompiledEntrypoint` poll observes that installed entrypoint and exits.
  Evidence is
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-655-jit-clinit-3edp35gq/jit.host.log`.
- Active staged `runtime/jit/jit.cc`, compiler `jit_compiler.cc`, and inliner
  contain no `DarwinJitCanCompile`/`darwin_jit_eligibility` admission call;
  production APK launch does not export `DARWIN_ART_JIT`, and the runtime
  entry defaults `UseJitCompilation` to true. This focused result proves the
  normal app JIT path is live, not exhaustive app/corpus validation.

### Progress — 2026-09-08 corpus continuation

- Sol verification confirms the normal `--jit` lane uses AOSP default warmup and
  optimize thresholds (`65535`) and still installs baseline code for
  `655-jit-clinit`; no threshold override or Darwin admission gate is required.
- The 649 corpus shard remains open because unmodified `658-fp-read-barrier`
  crashes in generated interpreter code during its floating-point read-barrier
  stress. This is now the next generic runtime fix target; no test-specific
  workaround is accepted.

### Progress — 2026-09-08 read-barrier boundary fix

- Sol mapped the fault to `nterp_op_sget_object`'s marking fast path: the
  read-barrier helper returned a compressed reference and the path dereferenced
  it before the Darwin heap-base decode. The generator now decodes `x0` before
  that load; the fail-closed object-boundary inventory is updated to 21 entries.
- `cargo test -p art-bootstrap runtime_art::nterp` passes 10/10 and the rebuilt
  ARM64ng Nterp object passes its Mach-O/256-handler audit. The unmodified 658
  runtime rerun is pending.

### Progress — 2026-09-08 read-barrier verification

- Focused unmodified `658-fp-read-barrier` now passes both interpreter and JIT
  lanes after the `sget-object` marking-path decode fix. Evidence artifact:
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-658-fp-read-barrier-ipjzr8ss`.
- The complete 649 shard rerun is in progress; no test-specific source change
  or fallback was introduced.

### Progress — 2026-09-08 shard 649 complete

- Rebuilt runtime validation completed for all 20 tests in
  `_build/art-upstream-corpus-nterp-649-final2/summary.json`: 20/20 passed.
  This includes unmodified `658-fp-read-barrier` in interpreter and JIT lanes
  after the generic `sget-object` compressed-reference boundary fix.
- The next deterministic shard is `661-oat-writer-layout` through
  `674-HelloWorld-Dm` (20 tests).

### Progress — 2026-09-08 shard 661 initial result

- The next 20-test shard completed its first pass at 15/20. Remaining failures
  are grouped as generic issues: three odd-dex-size cases stage no jar for the
  custom malformed-DEX flow, `664-aget-verifier` has an interpreter output
  mismatch, and `670-bitstring-type-check` rejects a non-string environment
  snapshot in the runner contract. Luna is reworking shared boundaries.

### Progress — 2026-09-08 shard 661 complete

- After the generic prebuilt-container and typed-environment corrections, the
  ledger `_build/art-upstream-corpus-nterp-661/summary.json` is 20/20 passed.
  All three malformed odd-Dex cases, `664-aget-verifier`, and
  `670-bitstring-type-check` pass interpreter, normal JIT, and optimized
  unmodified-source lanes.

### Completion — 2026-09-08 Nterp corpus shard 649

- The rebuilt four-worker ledger `_build/art-upstream-corpus-nterp-649-final2`
  ran the 20 deterministic discovered inputs from `648-many-direct-methods`
  through `661-classloader-allocator`; all 20 passed (interpreter, JIT, and
  unmodified-source optimized lanes), with no remaining failures.
- The ledger is uniform at runner hash
  `57483a2ead8aa5022d7be6b82fde6f6570e222d402130251d9c8cf26182834ca`.
- The only runtime failure encountered, `658-fp-read-barrier`, was fixed
  generically by decoding the compressed declaring-class reference after the
  `sget-object` read-barrier mark path and before its native field load. No
  corpus input, expected output, APK, allowlist, or test-specific workaround
  changed. The next deterministic shard begins after `661-classloader-allocator`.

### Completion — 2026-09-08 Nterp corpus shard 661

- The rebuilt four-worker ledger `_build/art-upstream-corpus-nterp-661/summary.json`
  ran the 20 deterministic inputs from `661-oat-writer-layout` through
  `674-HelloWorld-Dm`; all 20 passed in interpreter and optimized lanes.
- The ledger is uniform at runner hash
  `6a9db37a07b4afb9498a4c2ac91e03c69d5c2c5094a7afa23a0994ac2b0bf249`.
- Generic runner corrections were limited to preserving checked-in prebuilt
  multidex ZIP entries while appending the harness support DEX, selecting the
  normal speed prebuild when an unchanged native verifier checks an OAT quick
  entrypoint, and coercing `ctx.env` scalar assignments to strings. No AOSP
  test, expected output, allowlist, or fallback changed. Representative
  evidence is `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-663-odd-dex-size2-tod_7wkl`;
  per-test evidence paths are recorded in the ledger.

### Progress — 2026-09-08 shard 674 started

- The next deterministic 20-test shard (`674-hiddenapi` through
  `688-shared-library`) is running with four workers against the rebuilt
  runtime using unmodified AOSP inputs.

### Progress — 2026-09-08 shard 674 initial result

- Initial ledger result is 16/20. Four failures are grouped into shared
  boundaries: compiler companion symbols for hidden class-loader APIs in
  `674-hiddenapi`/`688-shared-library`, and boot-oat trust handling for the
  `677-fsi`/`677-fsi2` runtime path. The AOSP sources remain unchanged.

### Progress — 2026-09-08 shard 674 shared fixes verified

- Rebuilt runtime exports now expose the `DexFileLoader::Open` and
  `kInvalidFile` symbols globally; the 677 FSI trust/logging fixes and the
  compiler companion stubs pass their focused tests.
- The refreshed shard ledger is currently 19/20. `674-hiddenapi` reaches the
  native hidden-api test but still fails at its AOSP `opened_dex_files` domain
  assertion; this is an unresolved shared native-loader/DSO symbol-ownership
  issue, not an input or allowlist change.

### Progress — 2026-09-08 674 hidden-api ownership fix

- Rebuilt runtime and focused rerun now pass `674-hiddenapi` in interpreter,
  JIT, and unmodified-source optimized lanes. The generic fix derives JNI
  method ownership from native source exports and suppresses bridge
  pre-registration for methods owned by the test DSO, keeping coupled native
  state in one AOSP library owner. No test-name gate or input modification was
  added. Artifact: `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-674-hiddenapi-slzzma6i`.

### Completion — 2026-09-08 Nterp corpus shard 674

- The resumed four-worker ledger `_build/art-upstream-corpus-nterp-674/summary.json`
  now records 20/20 passing inputs from `674-hiddenapi` through
  `688-shared-library` across interpreter and optimized lanes.
- The final runtime relink/audit passed, and the generic DSO-owned JNI method
  exclusion fixed the hidden-api state split without changing AOSP tests,
  expected output, APKs, or allowlists.
-
### Verified completion — 2026-09-08 shard 674

- The four-worker ledger `_build/art-upstream-corpus-nterp-674/summary.json`
  completed all 20 deterministic inputs from `674-hiddenapi` through
  `688-shared-library`: 20 passed, 0 failed. Runner hashes recorded by the
  ledger are `57c6c62647271891454e4ee09e2903305c4d2e56bff4abf13fffc0b76c117ae5`
  and `d8b97556810cbef8b09ba13c576685c0a01df9d0a3362f8fbe3ae1887d9aac35`.
- Verified shared-boundary fixes are the compiler API companion stubs,
  globally exported `DexFileLoader` symbols, logical boot-oat identity, and
  source-derived per-method JNI ownership. No AOSP input, expected output,
  allowlist, or fallback was changed. Focused 674 evidence is at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-674-hiddenapi-swwau7nb`;
  all final per-test artifacts are recorded in the ledger.

### Progress — 2026-09-08 shard 689 started

- The next four-worker deterministic shard (`689-multi-catch` through
  `706-checker-scheduler`) is running against the rebuilt runtime with
  unmodified AOSP inputs.

### Progress — 2026-09-08 shard 707 initial result

- The next 20-input shard (`707-checker-invalid-profile` through
  `728-imt-conflict-zygote`) initially reached 17/20. `717-integer-value-of`
  exposed a generic sibling-source/include discovery gap and now passes when
  rerun. Remaining 712/716/725 failures are under investigation at shared
  path, native-source, and invocation boundaries.

### Progress — 2026-09-08 shard 689 initial result

- The shard reached 18/20. `692-vdex-secondary-loader` and
  `693-vdex-inmem-loader-evict` declare JNI methods whose unchanged C++ owner
  is shared from the sibling `692-vdex-inmem-loader` test directory; the
  runner currently discovers native sources only beneath the active test.
  The fix is being generalized to resolve JNI owners from AOSP source exports.

### Completion — 2026-09-08 Nterp corpus shard 689

- The resumed four-worker ledger `_build/art-upstream-corpus-nterp-689/summary.json`
  records 20/20 passing inputs from `689-multi-catch` through
  `706-checker-scheduler` across interpreter and optimized lanes.
- Generic fixes added sibling AOSP JNI-owner discovery from Java declarations
  and per-invocation filtering of secondary VDEX artifacts when
  `secondary_compilation=false`. No AOSP input, expected output, allowlist, or
  fallback changed. Final runner hash: `235530cac3521c3f5cdafa18a8bd210c6474d2859b79e298e0531a7ec3f4340c`.

### Inventory — 2026-09-07 Darwin admission/fallback gates

- Audited the active shared shadow (`_build/runtime-common/patched-source`) and
  JIT compiler shadow. `runtime/jit/jit.cc` has only the pinned AOSP checks
  (`IsPreCompiled`, deoptimization/instrumentation/OSR state,
  `IsCompilable`) before `TryPatternMatch`/optimizing compilation; it contains
  no `DarwinJitCanCompile` call or bytecode/method-shape allowlist. The active
  inliner and `JitCompiler::CompileMethod` likewise contain no Darwin admission
  gate. This matches normal AOSP app behavior; the Apple branches found in the
  compiler are ABI/compressed-reference/address lowering only.
- Active Nterp uses the standard AOSP `IsNterpSupported` and
  `CanRuntimeUseNterp` conditions (ISA/read barrier, debuggable,
  instrumentation, interpret-only, AOT, async-exception, and JIT-first-use
  state). No Apple-only Nterp suppression or catch-entry fallback remains;
  `RuntimeBootstrapStaging::audit_nterp_admission` and the manifest contract
  reject the retired `0017`, `0019`, and `0039` patches.
- The remaining `compat/darwin_jit_eligibility.h` helper is diagnostic-only
  (used by acceptance probes); production JIT does not call it. Historical
  `docs/architecture-migration.md` text claiming OSR is rejected by that
  helper is stale and should not be used as the runtime contract. The old
  `_build/runtime-bootstrap/patched-source` tree and generated `.rej` files are
  stale build artifacts; staging rebuilds the active runtime-common shadow and
  hashes both sources and patch contents. The OpenJDK/JVM host build was also
  corrected to consume the runtime-common headers, avoiding stale declarations.
  No runtime admission code change was safe or necessary in this inventory
  pass.
- Reviewed Apple-specific unsupported paths: Mark-Compact's
  `userfaultfd`/`mremap` path correctly selects its generic fallback on Darwin,
  and Nterp's non-`int` `filled-new-array` `InternalError` is pinned AOSP
  behavior, not a JIT admission gate. Manifest contract test passes.

### Verified completion — 2026-09-08 shard 689

- The resumed four-worker ledger `_build/art-upstream-corpus-nterp-689/summary.json`
  completed all 20 deterministic inputs from `689-multi-catch` through
  `706-checker-scheduler`: 20 passed, 0 failed. Every result has exit code 0
  and the ledger records runner hash
  `235530cac3521c3f5cdafa18a8bd210c6474d2859b79e298e0531a7ec3f4340c`.
- The generic runner resolves JNI owners from Java native declarations and
  matching `Java_Main_*` exports across the pinned AOSP test tree. It also
  enforces each typed action's `secondary_compilation` boundary by pruning
  secondary oat/vdex/art outputs and refreshing the runtime-file capability
  before launch. Focused 692/693 and the full shard passed. No AOSP input,
  expected output, allowlist, or fallback was changed.

### Progress — 2026-09-08 shard 707 current reduction

- Focused `717-integer-value-of` now passes all interpreter/JIT/optimized lanes
  after generic source-parent include propagation and bridge-owner filtering.
- The resumed ledger is currently 17/20. Remaining `712-varhandle-invocations`
  has a JIT sandbox boot-classpath path-resolution failure, while
  `725-imt-conflict-object` reaches generated code and faults in the IMT
  conflict path; both remain shared runtime/runner work.

### Progress — 2026-09-08 focused 712 recheck

- Ordinary detached launches now pass an absolute `DARWIN_ART_BOOT_CLASSPATH`;
  focused `712-varhandle-invocations` reaches ART and its interpreter lane
  passes unchanged. The remaining failure is optimizing compilation of
  `Main.main` for VarHandle invocation bytecode. `725` remains an independent
  generated-code IMT conflict fault.

### Verified completion — 2026-09-08 shard 707

- The four-worker ledger `_build/art-upstream-corpus-nterp-707/summary.json`
  completed all 20 deterministic inputs from `707-checker-invalid-profile`
  through `728-imt-conflict-zygote`: 20 passed, 0 failed, every exit code 0.
  The final ledger runner hash is
  `499ce23a4812133eb4f4b4e88b6c7c00bd342cf69f73cd44c430d6187f2cf358`.
- Focused evidence includes `712-varhandle-invocations` at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-712-varhandle-invocations-ndll4kyw`
  and `725-imt-conflict-object` at
  `/var/folders/t4/qqflgy6n3rgd5dcnv2r4x0nh0000gn/T/darwin-art-725-imt-conflict-object-zwpcswg2`;
  both report interpreter and JIT expected-output PASS plus unmodified source
  interpreter+optimized PASS.
- Generic fixes preserve unchanged AOSP inputs: detached launches use absolute
  physical boot-class paths with root-relative logical locations; optimized
  installation accepts a compilable application constructor when an oversized
  dispatcher cannot be optimized; and Darwin Nterp keeps decoded class
  addresses in 64-bit registers for IMT conflict vtable indexing. No allowlist
  or fallback was added.

### Verified focused fix — 2026-09-08

- `725-imt-conflict-object` now passes interpreter, JIT expected-output, and
  unmodified-source interpreter+optimized lanes. The prior generated IMT
  conflict fault is no longer reproducible in artifact
  `darwin-art-725-imt-conflict-object-liw26qsa`.

### Verified completion — 2026-09-08 shard 729

- Four-worker execution of the contiguous 20-test range from
  `729-checker-polymorphic-intrinsic` through `809-checker-invoke-super-bss`
  completed 20/20 with exit code 0 in `_build/art-upstream-corpus/summary.json`.
- The range covers polymorphic intrinsics, deoptimization, bounds checks, app
  images, interface/field dispatch, smali, and invoke-super paths. No AOSP
  input or compatibility gate changed.

### Progress — 2026-09-08 shard 810

- The next 20-test range through `829-unresolved-enclosing` has 17 completed
  passes. Remaining failures are `817-hiddenapi`, `822-hiddenapi-future`, and
  `823-cha-inlining`; all other input lanes passed. These are now isolated for
  generic hidden-API/CHA fixes.

### Verified focused fix — 2026-09-08

- `822-hiddenapi-future` passes interpreter, JIT expected-output, and
  unchanged-source interpreter+optimized lanes. The generic hidden-API DEX
  encoder now represents AOSP's `max-target-future` flag.

- `817-hiddenapi` now passes interpreter, JIT expected-output, and
  unchanged-source interpreter+optimized lanes after generic native owner
  resolution included its AOSP hidden-API helper.

### Diagnostic progress — 2026-09-08 `823-cha-inlining`

- A live macOS `sample` of the hanging interpreter process places the main
  thread in `Throwable_nativeFillInStackTrace` → `CreateInternalStackTrace` →
  `StackVisitor::WalkStack`/`OatQuickMethodHeader::IsStub`, with repeated
  `QuickDeliverException`. This narrows the timeout to exception stack-walk
  frame advancement/header detection during CHA default-method throws, not
  Java loop throughput or the test harness.

### Verified completion — 2026-09-08 shard 810

- Resumed four-worker execution of the deterministic 20-test range
  `810-checker-invoke-super-default` through `828-partial-lse` completed 20/20
  tests with exit code 0 in `_build/art-upstream-corpus/summary.json`.
- This includes the hidden-API encoder/owner fixes and the stack-walker fix
  exercised by `823-cha-inlining`; no AOSP input, allowlist, or fallback was
  changed.

### Verified completion — 2026-09-08 shard 830

- Fresh two-worker execution covering `830-goto-zero` through
  `847-filled-new-aray` completed 21/21 tests. The previously stale
  `845-data-image` and `846-multidex-data-image` ledger entries were rerun in
  a new ledger after rebuilding the compiler-only `OptimizationInfo` API;
  both pass interpreter, JIT expected-output, and unchanged-source lanes.
- No APK/test input, allowlist, or fallback was changed.

### Verified focused fixes — 2026-09-08 `849-records`, `900-hello-plugin`

- `849-records` passes interpreter, JIT, and unchanged-source optimized lanes;
  the runner now routes AOSP d8 `-J<arg>` flags to Java when invoking R8
  directly.
- `900-hello-plugin` passes all three lanes. Android `libartagent(.d).so`
  aliases map to the staged test DSO, and the generic JVMTI dispatcher is
  omitted when the fixture owns `Agent_OnLoad`, preserving plugin/agent
  lifecycle output without changing AOSP inputs or adding an allowlist.

### Verified completion — 2026-09-08 shard 848

- Fresh four-worker execution of `848-pattern-match` through
  `907-get-loaded-classes` completed 20/20 tests. This includes records,
  plugin/JVMTI, GC, heap, and class-enumeration paths; every test passed
  interpreter, JIT expected-output, and unchanged-source optimized lanes.
- No AOSP input, test-name gate, or interpreter fallback was introduced.

### Verified completion — 2026-09-08 shard 908

- Fresh four-worker execution of `908-gc-start-finish` through `927-timers`
  completed 20/20. GC, JVMTI object/field/thread/monitor, obsolete-method,
  and heap paths passed all interpreter, JIT expected-output, and
  unchanged-source optimized lanes.
- No AOSP input, allowlist, or fallback was changed.

### In progress — 2026-09-08 shard 928 follow-up

- Initial 928–947 run reached 18/20; generic fixes are now verified in
  focused runs: `936-search-onload` and `938-load-transform-bcp` each pass all
  three lanes. A fresh full-shard rerun is active in
  `_build/art-upstream-corpus-shard928-v2`.

### Verified focused fixes — 2026-09-08 `936-search-onload`, `938-load-transform-bcp`

- Both tests pass interpreter, JIT expected-output, and unchanged-source
  optimized lanes in repeat focused runs. `938-load-transform-bcp` was fixed
  generically by using absolute physical boot-classpath locations for detached
  sandbox launches, allowing libcore's class-path URL handler to resolve jars
  from its private working directory. The earlier `936-search-onload` native
  fault did not reproduce after the same runner/runtime state was stabilized;
  no test-specific patch, allowlist, or fallback was added.

### Verified completion — 2026-09-08 shard 928

- Fresh four-worker rerun of `928-jni-table` through `947-reflect-method`
  completed 20/20 across interpreter, JIT expected-output, and unchanged-source
  optimized lanes.

### In progress — 2026-09-08 full corpus audit

- Structural discovery now identifies 1,075 runnable AOSP test contracts
  (directories with `expected-stdout.txt`) and excludes only shared fixtures.
  A fresh current-runner audit is executing all 1,075 inputs with four
  workers in `_build/art-upstream-corpus-final-audit`.

### Verified completion — 2026-09-08 shard 948

- Four-worker execution of `948-change-annotations` through `967-default-ame`
  completed 20/20. Redefinition, invoke-custom/polymorphic, method handles,
  and default-interface paths passed all three lanes.

### In progress — 2026-09-08 shard 988

- Executable tests `988-method-trace` through `999-redefine-hiddenapi` passed
  13/13 before the corpus reached seven AOSP helper fixture directories that
  intentionally have no `run.py`/expected output. The corpus runner is being
  generalized to recognize executable test layout structurally rather than
  treating those fixtures as failed runtime tests.

### In progress — 2026-09-08 shard 968 follow-up

- Initial 968–987 execution passed 14/20. Generated-interface DEX materializing
  and native method binding remain under generic runner/runtime diagnosis.

### Verified focused fixes — 2026-09-08 shard 968

- 968-default-partial-compile-gen, 970-iface-super-resolution-gen,
  971-iface-super, 975-iface-private, and 978-virtual-interface now select
  Smali-only inputs as the primary DEX when the API-level support DEX is
  intentionally absent; all three interpreter/JIT/unchanged-source lanes pass.
  986-native-method-bind now exposes null-ClassLoader bootstrap/agent DSOs in
  the process-wide namespace, matching JVMTI's dlsym(RTLD_DEFAULT) contract
  while retaining local scope for application ClassLoader loads; all three
  lanes pass. No AOSP input, allowlist, or fallback was changed.

### Verified completion — 2026-09-08 shard 988

- Structural corpus discovery now excludes only AOSP sibling fixture
  directories lacking `expected-stdout.txt`; no names are hard-coded. The
  executable `988-method-trace` through `999-redefine-hiddenapi` range (13
  inputs, including duplicate `993` variants) passes every lane.

### Audit progress — 2026-09-08

- The live full audit has processed 187/1,075 executable contracts: 164 pass
  and 23 currently fail. Failures are being triaged by generic JNI/GC,
  class-loader, app-image, and native-bridge contracts.

### Verified completion — 2026-09-08 shard 968

- Fresh four-worker rerun of `968-default-partial-compile-gen` through
  `987-agent-bind` completed 20/20 across all three execution lanes.

### Audit progress — 2026-09-08 (continued)

- The live audit advanced to 241/1,075 contracts: 217 pass and 24 fail.
  Remaining failures are being triaged as shared generated-code/JNI, GC/OOM,
  class-loader, app-image, and native-bridge runtime issues.

### Verified focused fix — 2026-09-08 allocation/GC cluster

- The OOM/GC failures shared one ABI fault: Darwin's compressed heap reference
  was reaching AOSP quick allocation entrypoints as a raw `mirror::Class*`,
  so `artAllocObjectFromCode*`/`artAllocArrayFromCodeResolved*` faulted before
  the Java test body. Added the generic runtime-boundary normalization in
  `0155-darwin-allocation-entrypoint-class-reference-boundary.patch` and
  bumped the runtime shadow identity so the patch is included in rebuilt
  runtime artifacts. Fresh three-lane focused runs pass for 061, 064, 074,
  080-oom-fragmentation, 080-oom-throw, 096, and 1000. No AOSP input,
  allowlist, or fallback was changed.

### Audit progress — 2026-09-08 (live)

- The authoritative full audit reached 278/1,075 contracts (253 pass, 25
  fail). Allocation/GC boundary changes are present; post-fix full-audit
  coverage is still pending.

### Audit progress — 2026-09-08 (live update)

- The still-running pre-rebuild audit reached 310/1,075 contracts: 282 pass
  and 28 fail. Its GC entries predate the allocation-boundary patch, so these
  results will be replaced by a clean post-fix rerun.

### Audit progress — 2026-09-08 (live update 2)

- The pre-rebuild audit advanced to 314/1,075 contracts: 286 pass and 28
  fail. It remains active; its results are diagnostic only until the clean
  post-patch rerun is complete.

### Audit progress — 2026-09-08 (live update 3)

- The pre-rebuild audit advanced to 333/1,075 contracts: 305 pass and 28
  fail. It is still running; final evidence will come from a clean rerun
  after all generic fixes are rebuilt.

### Audit progress — 2026-09-08 (live update 4)

- The pre-rebuild audit advanced to 340/1,075 contracts: 312 pass and 28
  fail. Newly completed contracts continue to pass; clean post-patch rerun
  remains required for final evidence.

### Audit progress — 2026-09-08 (live update 5)

- The pre-rebuild audit advanced to 351/1,075 contracts: 323 pass and 28
  fail. The process remains live; no completion claim is made.

### Verified focused fixes — 2026-09-08 early JNI/stack cluster

- `004-JniTest` now exports non-JNIEXPORT native symbols through a generic
  Mach-O export bridge; all three lanes pass.
- `004-StackWalk` now preserves the boot-image logical BCP locations through
  dex2oat and runtime loading, avoiding spurious OAT invalidation; all three
  lanes pass. `004-ThreadStress` also passes on fresh rerun.

### Audit progress — 2026-09-08 (live update 6)

- The pre-rebuild audit reached 365/1,075 contracts: 337 pass and 28 fail.
  A Sol-high review is examining remaining generated-code, class-loader,
  app-image, and obsolete-const failures in parallel.

### Audit progress — 2026-09-08 (live update 8)

- The pre-rebuild audit reached 411/1,075 contracts: 382 pass and 29 fail.
  Sol-high identified boot-image logical-location mismatch as a shared cause
  for app-image/generated-code failures and invalid native-bridge signal
  looping as a separate host-boundary defect; fixes are in progress.

### Audit progress — 2026-09-08 (live update 9)

- The pre-rebuild audit reached 443/1,075 contracts: 414 pass and 29 fail.
  The Sol review remains active; no clean post-fix result is claimed yet.

### Audit progress — 2026-09-08 (live update 10)

- The pre-rebuild audit reached 507/1,075 contracts: 478 pass and 29 fail.
  Sol-high's generic boot-image and native-bridge fixes remain under review.

### Audit progress — 2026-09-08 (live update 11)

- The pre-rebuild audit reached 520/1,075 contracts: 491 pass and 29 fail.
  Sol-high implementation and focused verification of the shared fixes are
  still in progress.

### Audit progress — 2026-09-08 (live update 12)

- The pre-rebuild audit reached 551/1,075 contracts: 522 pass and 29 fail.
  Sol-high's shared boot-image/native-bridge fix is still under active
  implementation and review.

### Verified focused update — 2026-09-08

- `103-string-append` now passes all three lanes on the current tree after
  logical boot-class-path alignment; the full-audit failure is stale.
- `115-native-bridge` root cause is Darwin ucontext PC high-bit loss across the
  AOSP signal callback boundary; a generic host ABI normalization is in review.

### Audit progress — 2026-09-08 (live update 7)

- The pre-rebuild audit reached 401/1,075 contracts: 373 pass and 28 fail.
  Newly completed checker contracts continue to pass; clean post-fix coverage
  remains pending.

### Audit progress — 2026-09-08 (live update 13)

- The pre-rebuild audit reached 639/1,075 contracts: 609 pass and 30 fail.
  New failures remain diagnostic until generic fixes are rebuilt and a clean
  rerun is completed.

### Audit progress — 2026-09-08 (live update 14)

- The pre-rebuild audit reached 675/1,075 contracts: 645 pass and 30 fail.
  No signal patch has been accepted yet; Sol-high continues implementation
  and focused validation.

### Verified focused fix — 2026-09-08 native bridge signal ABI

- `115-native-bridge` passes interpreter, JIT, and unchanged-source optimized
  lanes after Darwin sigchain interrupted/resumed PC normalization at the host
  ucontext boundary. No test-specific address or gate was added.

### Audit progress — 2026-09-08 (live update 15)

- The pre-rebuild audit reached 722/1,075 contracts: 692 pass and 30 fail.
  The native-bridge fix is validated in focused runs; app-image clean
  verification is still pending.

### Verified focused update — 2026-09-08 app-image and D8 wrapper

- `1001-app-image-regions` passes all three lanes with the current logical
  boot-class-path contract; the earlier failure was stale.
- `1948-obsolete-const-method-handle` is a generic Bash 3.2 `set -u`
  empty-array expansion in `d8-jar-compat.sh`; the wrapper was corrected and
  three-lane verification is running.

### Verified focused update — 2026-09-08

- `1948-obsolete-const-method-handle` now passes all three lanes after the
  generic D8 wrapper fix. Sol-high is validating class-loader and multi-loader
  contracts next.

### Audit progress — 2026-09-08 (live update 16)

- The pre-rebuild audit reached 764/1,075 contracts: 734 pass and 30 fail.

### Audit progress — 2026-09-08 (live update 17)

- The pre-rebuild audit reached 786/1,075 contracts: 756 pass and 30 fail.
  Focused validation of class-loader and multi-loader fixes is in progress.

### Audit progress — 2026-09-08 (live update 18)

- The pre-rebuild audit reached 818/1,075 contracts: 788 pass and 30 fail.
  Class-loader focused reruns remain active.

### Verified focused update — 2026-09-08 class-loader/app-image

- `142-classloader2` and `158-app-image-class-table` pass interpreter, JIT,
  and unchanged-source optimized lanes. `156-register-dex-file-multi-loader`
  is being fixed at the generic libcore logical-to-physical BCP filesystem
  boundary after sandbox ENOENT reproduction.

### Audit progress — 2026-09-08 (live update 19)

- The pre-rebuild audit reached 932/1,075 contracts: 902 pass and 30 fail.
  `156-register-dex-file-multi-loader` remains under generic filesystem-bridge
  implementation; no test-specific mapping is being added.

### Verified focused update — 2026-09-08 multi-loader filesystem boundary

- `156-register-dex-file-multi-loader` now maps logical and physical BCP
  entries by aligned index at the libcore syscall bridge, guarded by read-only
  and exact host-capability checks. Three-lane execution is in progress.

### Audit progress — 2026-09-08 (live update 20)

- The pre-rebuild audit reached 974/1,075 contracts: 944 pass and 30 fail.

### Focused verification update — 2026-09-08

- `156-register-dex-file-multi-loader` first rerun confirms the filesystem
  bridge opens tail BCP entries and reports expected ZipException for unsafe
  DEX. The first three entries still fail because runner capability state is
  narrowed on later invocations; that generic runner rewrite is in progress.

### Audit progress — 2026-09-08 (live update 21)

- The pre-rebuild audit reached 982/1,075 contracts: 952 pass and 30 fail.

### Audit progress — 2026-09-08 (live update 23)

- The running pre-rebuild audit reached 1,010 result records: 980 pass and 30
  fail. The parent remains active while final contracts drain; all listed
  failures are pre-fix/stale and will be replaced by a clean rerun.

### Focused verification update — 2026-09-08 multi-loader semantics

- `156-register-dex-file-multi-loader` now passes the logical-to-physical BCP
  resource-open phase. The remaining `Unreachable` is a semantic regression
  in `0154-darwin-per-loader-dex-cache.patch`, which incorrectly permits
  duplicate DexFile registration across ClassLoaders; Sol-high is restoring
  the AOSP contract and checking class-loader regressions.

### Audit progress — 2026-09-08 (live update 22)

- The pre-rebuild audit reached 988/1,075 contracts: 958 pass and 30 fail.

### Environment verification — 2026-09-09

- On the replacement ARM64 macOS host, `cargo test -p darwin-art-host` passed
  all 10 unit/integration tests, `cargo check -p darwin-art-host` passed, and
  `bash -n tools/d8-jar-compat.sh` passed. The full ART goal remains active.

### Clean verification — 2026-09-09

- On the replacement host, a clean single-test ledger reproduced
  `156-register-dex-file-multi-loader` failing at the AOSP semantic
  `Unreachable` after BCP resource mapping. The referenced `0154` patch is not
  present as a standalone file in the current tree; its effective source
  changes are being located and restored generically.

### Continued work — 2026-09-09

- A Luna-high agent is tracing the effective per-loader DexCache changes in
  tracked sources and restoring AOSP duplicate-registration semantics for the
  clean `156` failure.

### Investigation checkpoint — 2026-09-09

- AOSP `ClassLinker::RegisterDexFile` already contains the expected
  cross-ClassLoader rejection. A comment-only candidate patch was discarded;
  no semantic change is claimed until the actual Darwin execution path is
  identified and the clean three-lane `156` contract passes.

### Runtime evidence — 2026-09-09

- The retained `156` artifacts show `VMClassLoader` logging a raw
  `unsafe-boot-dex/classes.dex` ZIP warning, then `Main.main` reaches the
  AOSP `Unreachable` branch. This proves the remaining failure is in the
  Darwin DexFile/class-loader registration path, not a test or APK rewrite;
  the exact loader/DexFile identity divergence still needs a runtime fix.

### Design correction — 2026-09-09

- A proposed location/checksum fallback for DexFile registry lookup was
  rejected as too broad: independently opened files at the same path can be
  legitimate AOSP objects. The fix must preserve pointer identity semantics
  and correct the Darwin cookie/loader handoff at its actual boundary.

### Trace checkpoint — 2026-09-09

- A conditional registry trace was rebuilt and a clean `156` run was repeated,
  but no `DexFile_defineClassNative`/registry trace appeared while `Main` was
  reached. This shifts the investigation earlier: the custom loader likely
  has no usable app `dexElements` and falls through to its parent, so the next
  check is canonical `PathClassLoader` construction/publication.

### Build portability checkpoint — 2026-09-09

- A full incremental `art-bootstrap all` reached the Skia surface smoke on the
  replacement host but failed at link time on pre-existing input-queue and
  socket-broker symbols. The SDK deprecation errors were fixed narrowly with
  `-Wno-deprecated-declarations`; the remaining link closure is still a build
  graph issue and does not justify claiming runtime completion.

### Build verification — 2026-09-09

- Added inert providers only to the standalone headless Skia smoke executable;
  production graphics targets still link the runtime-owned providers. The
  replacement host now passes `build-skia` with 120 IOSurface frames,
  `staging-copies=0`, and the expected final/sequence hashes.

### ClassLoader regression verification — 2026-09-09

- After fixing runtime archive invalidation, a clean parallel corpus run for
  `142-classloader2` through `158-app-image-class-table` passed all 16 tests,
  including interpreter, JIT, and unchanged-source lanes. The previously
  failing `156-register-dex-file-multi-loader` now passes with AOSP's real
  duplicate-registration rejection.

### ART regression verification — 2026-09-09

- The clean parallel run for `159-app-image-fields` through
  `179-nonvirtual-jni` passed all 21 AOSP tests in interpreter, JIT, and
  unchanged-source lanes. This covers app-image fields/methods/strings,
  read-barrier stress, resolution trampolines, locks, monitor ownership,
  initialization deadlocks, and JNI dispatch.

### ART regression verification — 2026-09-09 (continued)

- `180-native-default-method` through `183-rmw-stress-test` passed all 4 AOSP
  tests in interpreter, JIT, and unchanged-source lanes, covering default
  method dispatch/linking and read-modify-write stress.

### Tooling verification — 2026-09-09

- `cargo fmt --all -- --check` now passes after normalizing the manifest test
  assertion. The full bootstrap test suite still has one pre-existing nterp
  shadow-source drift assertion failure (12/13 pass); no runtime contract is
  being hidden behind that failure.

### Tooling verification — 2026-09-09 (continued)

- Updated the nterp invoke-dispatch audit fixture to include the AOSP
  `arm64ng/invoke.S` interface-vtable `add w2` fragment. The audit now matches
  the pinned source without weakening fail-closed checks; `cargo test
  -p art-bootstrap` passes all 13 tests.

### Nterp entry ABI — 2026-09-09

- Added idempotent low-32-to-`DARWIN_ART_REFERENCE_BASE` `ArtMethod*`
  normalization at both generated `ExecuteNterpWithClinitImpl` and
  `ExecuteNterpImpl` entrypoints. The generated ARM64ng object and 13-object
  runtime archive rebuild and Mach-O/DWARF audits pass; `821-many-args` passes
  interpreter, JIT, and unchanged-source lanes. Dynamic Nterp CFA semantics
  still require a separate ABI-correct unwind implementation and are not
  claimed complete.

### ClassLoader regression diagnosis — 2026-09-09

- Reproduced `497-inlining-and-class-loader` with the unmodified AOSP test and
  printed its suppressed exception temporarily (then reverted the test file).
  The failure is AOSP `ClassLinker::RegisterDexFile` rejecting the same native
  DexFile pointer when the test's second custom loader calls
  `loadClassBinaryName`; this is a real multi-loader identity/lifecycle gap,
  not a runner or JIT-only failure. A general fix is still required; no test
  input or duplicate-registration rejection was weakened.

### Replacement-host Nterp verification — 2026-09-09

- On the replacement ARM64 Mac, `cargo fmt --all -- --check`, all 13
  `art-bootstrap` unit tests, and `build-nterp-arm64ng` pass. The generated
  object is Mach-O arm64 with clean DWARF audits and 256 handlers.
- An experiment to retain upstream dynamic CFI was rejected because Apple's
  assembler fails on macro-expanded `cfi_adjust_cfa_offset`/register rules at
  the non-linear handler table. The previously verified metadata-lowering
  boundary remains in place; dynamic Nterp unwind semantics are still an open
  compatibility task.

### Multi-loader identity trace — 2026-09-09

- A fresh unchanged-input reproduction (diagnostic prints removed immediately)
  shows the first `MyClassLoader` call in `497-inlining-and-class-loader`
  receives `InternalError: Attempt to register dex file ... with multiple
  class loaders`; `foo` is therefore null. The process PathClassLoader has one
  app `DexFile` element, so the test DEX is already registered for that loader
  when `Main` is resolved. The remaining fix is app-image versus PathClassLoader
  DexFile identity/lifecycle parity, not weakening AOSP's duplicate rejection.

### JIT acceptance/deoptimization correction — 2026-09-09

- `audit-art-jit.sh` passes on the replacement host after aligning the
  identity-removal assertion with AOSP `Instrumentation::ReinitializeMethodsCode`:
  a verified method may return to Nterp, not only the quick interpreter bridge.
- The run exercised compiled arithmetic, JNI reference returns, concurrent GC,
  exceptions, fields/arrays, inlining, virtual/interface calls, VarHandles,
  and native exit hooks. No JIT admission gate or interpreter-only fallback was
added; the full corpus and multi-loader identity task remain open.

### Corpus stale-failure revalidation — 2026-09-09

- A fresh current-runner shard rechecked historical failures 126, 149, 2031,
  2271, and 304; all pass their configured AOSP three-lane contracts.
- The only remaining reproducible corpus failure is
  `497-inlining-and-class-loader`, narrowed to the first custom loader's
  canonical duplicate-DexFile `InternalError`. It remains open pending
  app-image/PathClassLoader identity parity.

### Replacement-host JIT audit — 2026-09-09 (continued)

- Rebuilt the changed graphics runtime and reran `audit-art-jit.sh` with the
  production library. Exit status is 0; all acceptance phases complete without
  the previous false status-121 deoptimization failure. A single frame-clock
  unit test was timing-sensitive on its first run and passed on exact rerun and
  the subsequent full host suite.

### Dynamic Nterp unwind CFA — 2026-09-08

- Reified the AOSP ARM64ng Nterp CFA as `DW_CFA_def_cfa_expression`:
  `*(x25 - 8) + 160`. Darwin lowering retains one dynamic expression per
  real Nterp FDE while filtering unsupported state-changing directives.
- `build-nterp-arm64ng`, all 13 bootstrap tests, runtime graphics rebuild, and
  the complete `audit-art-jit.sh` acceptance pass. ClassLoader case 497 remains
  the unrelated open compatibility task.

### Multi-loader boundary experiment — 2026-09-08

- Re-ran unchanged AOSP `156-register-dex-file-multi-loader`: interpreter,
  JIT, and source lanes pass, confirming duplicate-DexFile rejection remains
  AOSP-compatible. A generic attempt to reopen PathClassLoader DexFiles did
  not fix `497-inlining-and-class-loader` and was removed; the remaining gap
  is the app-image-versus-path-list identity boundary, not a broad rejection
  bypass.

### 497 loader-order isolation — 2026-09-08

- The failed 497 reproduction is identical in interpreter and JIT lanes, while
  156 still passes all lanes. This isolates the remaining defect to detached
  launcher ordering: `ClassLoader.SystemClassLoader` is published before the
  app-image/DexCache ownership split that AOSP establishes during application
  startup. The next implementation must move that split into the runtime
  loader-registration path; no test-specific behavior or rejection bypass is
  acceptable.

### DexFile cookie decode follow-up — 2026-09-08

- A native-side diagnostic confirmed `Main` resolves through a distinct
  `DexFile*`, but JNI reflection of the hidden cookie did not yield a reliable
  native pointer (the field representation is an internal ART contract). The
  diagnostic was removed. The next probe must reuse ART's own
  `CollectDexFilesFromJavaDexFile` path rather than duplicating cookie decoding.

### DexFile identity instrumentation — 2026-09-08

- Environment-gated tracing shows one Java `PathClassLoader` dex element while
  `Main` resolves through a distinct native `DexFile*` (the app-image/cache
  side). This confirms the missing comparison is the cookie's native DexFile
  mapping and registration timing, not Java loader object identity alone.
- The trace is diagnostic-only and disabled by default. The next runtime fix
  must preserve this distinct ownership without relaxing `RegisterDexFile`.

### System-loader override A/B — 2026-09-08

- Temporarily removing the detached `SetSystemClassLoaderForAppProcess` and
  thread override did not change 497: the test still failed before any JIT
  distinction, while 156 remained passing. The late publication is therefore
  not sufficient to explain the identity gap; the next trace must capture
  DexFile/DexCache registration during the initial system-loader creation.

### System loader startup ordering — 2026-09-08

- AOSP creates its system class loader from `Runtime::Start` before the
  application process phase. Darwin's detached path currently republishes and
  overrides that loader from `runtime_context_loader`; 497 fails at the
  resulting DexCache identity boundary while 156 still validates duplicate
  rejection. No speculative loader replacement was retained. The next code
  change must preserve AOSP startup ordering and separate app-image ownership
  generically.

### Native DexFile cookie trace hook — 2026-09-08

- Added manifest patch `0156` at ART's own
  `CollectDexFilesFromJavaDexFile` path. Runtime-core staging applies it;
  logging is disabled unless `DARWIN_ART_TRACE_DEX_IDENTITY=1` and does not
  alter registration.

### Cookie hook reachability — 2026-09-08

- The hook-bearing graphics runtime rebuilt successfully, but 497 emitted no
  `CollectDexFilesFromJavaDexFile` trace lines before failing. Its class
  definition path is earlier than this OAT context helper; the next diagnostic
  target is native `defineClassNative` registration.

### Runtime shadow source coverage — 2026-09-08

- The patched shadow manifest now includes both `class_loader_context.cc` and
  `native/dalvik_system_DexFile.cc`, which are compiled by the runtime job but
  had previously been omitted from the copy set. Shadow identity was bumped to
  v6 to invalidate the stale generated tree; graphics bootstrap rebuilt two
  affected objects successfully. The cookie hook is present in the staged
  source. This fixes build-graph coverage only; 497 remains an unresolved
  AOSP app-image/DexCache ownership failure and is not claimed fixed.

### Diagnostic patch validation — 2026-09-08

- The cookie collection hook remains the only enabled diagnostic patch. A
  malformed experimental `defineClassNative` patch was removed rather than
  weakening the staging contract. The graphics bootstrap now rebuilds cleanly
  with 256 cached objects and the staged cookie hook; 497 still fails before
  the collection hook and remains open.

### Corpus regression refresh — 2026-09-08

- Re-ran the five non-497 entries previously recorded as failed: `126`, `149`,
  `2031`, `2271`, and `304`. Each now passes interpreter, JIT, and unmodified
  source/optimized lanes. The 126 result also validates the typed `javac_post`
  argument detection for AOSP's multidex branch. The only remaining reproduced
  runtime failure is `497-inlining-and-class-loader`.

### Current-runtime confirmation — 2026-09-08

- A fresh serial run against the rebuilt graphics runtime independently
  confirms `126`, `149`, `2031`, `2271`, and `304` all pass interpreter, JIT,
  and unmodified lanes. This removes stale-corpus results as evidence for
  those five cases. `497` still fails before its custom loader returns a class;
  no behavior-changing workaround has been added.

### 497 failure narrowing — 2026-09-08

- The current 497 failure is a Java `NullPointerException` at `Main.java:96`:
  the custom loader returns `null` for `LoadedByMyClassLoader` and the next
  `getDeclaredMethod` call dereferences it. This is not a native crash. The
  adjacent AOSP `156-register-dex-file-multi-loader` test still passes, so
  duplicate-DexFile rejection is intact; investigation now targets the
  app-image DexCache/class-table association that should allow 497's class
  definition.

### Graphics runtime rebuild confirmation — 2026-09-08

- Rebuilt the graphics bootstrap after the loader-shadow changes; all 256
  runtime objects were reused from the validated v12 shadow and the build
  completed without introducing a behavior workaround. The 497 Java NPE
  boundary is unchanged and remains the active implementation target.

### 497 loader boundary audit — 2026-09-08

- The failure remains confined to the AOSP custom-loader path: the app's
  canonical `PathClassLoader` exposes one non-null DexFile element, but the
  custom `loadClassBinaryName` call returns no class and its caller observes a
  null result. The adjacent multi-loader registration test passes, so this is
  not permission to relax duplicate registration; app-image class-table
  publication must be compared with AOSP before changing runtime behavior.

### 497 native registration trace — 2026-09-08

- Environment-gated ART tracing captured canonical registration of dex
  `0xc9b0edc00` to loader `0x10002001ad0`, followed by
  `defineClassNative(LLoadedByMyClassLoader;)` for the same dex under custom
  loader `0x1000209f768`. An existing DexCache triggers the standard AOSP
  duplicate-loader rejection. The remaining fix is the app-image/class-table
  ownership transition, not cookie decoding or pointer transport.

### 497 class-table ownership trace — 2026-09-08

- The same run records canonical loader table `0x90f188140` and custom loader
  table `0x0` at `RegisterDexFile`. The existing DexCache therefore cannot be
  considered the custom loader's cache and AOSP rejects registration. This
  precisely identifies the missing lifecycle step: a custom loader's table and
  app-image DexCache association must be published before class definition,
  while test 156's duplicate rejection remains unchanged.

### 497 DexCache reuse experiment — 2026-09-08

- A speculative reuse path was rejected and reverted after it violated ART's
  pending-exception contract and produced a native fault. The production path
  is restored to AOSP's duplicate-loader guard; no unsafe behavior change was
  retained.

### AOSP lazy ClassTable comparison — 2026-09-08

- Pinned AOSP confirms `RegisterClassLoader` is intentionally lazy: a new
  loader's table is created by `GetOrCreateAllocatorForClassLoader` only after
  the duplicate-DexFile check. The observed custom-loader `new_table=null` is
  therefore not itself a Darwin defect. The unresolved 497 behavior must come
  from app-image metadata/publication or the class-definition ownership path;
  pre-registering every loader would diverge from AOSP and is rejected.

### 497 DexCache owner trace — 2026-09-08

- The owner trace adds the missing identity: the existing DexCache's loader is
  the canonical PathClassLoader (`0x10002001ad0`), while the requesting custom
  loader is `0x1000209f818` with no table yet. This rules out a null/invalid
  owner and leaves app-image publication/definition ordering as the remaining
  AOSP compatibility gap.

### 497 startup-order diagnostic — 2026-09-08

- Temporarily skipped the harness's `ResolveMainDexStrings` phase and reran
  497; the same custom-loader null result remained. The experiment was
  removed. Canonical DexCache creation in that helper is therefore not the
  cause; the remaining ordering gap is during app dex open/class-definition
  publication itself.

### Full JIT acceptance audit — 2026-09-08

- `bash tools/audit-art-jit.sh` exited 0 on the replacement machine. The
  audit reported PASS across compiled arithmetic, typed fields/arrays,
  exceptions, moving and concurrent GC, read barriers, JNI/native exits,
  concurrency, VarHandle (including wide/narrow/FP/ByteBuffer),
  invoke-polymorphic/custom, and OSR integer/wide/reference/exception paths.
- The audit still emits `Current thread not detached in Runtime shutdown`;
  this lifecycle cleanup remains open. This result is broad acceptance
  evidence, not completion of the full AOSP compatibility goal; the 497
  custom-class-loader failure is still tracked above.

### VM shutdown detach parity — 2026-09-08

- `darwin_art_shutdown_process` now detaches the owner thread before
  `DestroyJavaVM` for both dalvikvm and standalone host processes, matching
  AndroidRuntime's VM contract. The full JIT audit still exits 0 and no longer
  emits `Current thread not detached in Runtime shutdown`.
- This is lifecycle cleanup only. The 497 custom-loader class-definition
  failure reproduces unchanged, so the overall compatibility goal remains
  incomplete.

### Replacement-machine revalidation — 2026-09-08

- After the detach change, `cargo test -p art-bootstrap` passed all 13 tests,
  the graphics link audit passed, and `audit-art-jit.sh` exited 0 without the
  prior shutdown warning.
- The focused unmodified AOSP `497-inlining-and-class-loader` run still exits
  1 because `MyClassLoader.loadClass("LoadedByMyClassLoader")` returns null.
  This is unchanged in interpreter and optimized lanes and remains an
  authentic ClassLinker/DexCache compatibility gap, not a test bypass.

### AOSP regression recheck after shutdown fix — 2026-09-08

- Re-ran the previously stale failure set on the replacement machine:
  `004-JniTest`, `004-StackWalk`, `004-ThreadStress`, `061-out-of-memory`,
  `064-field-access`, `074-gc-thrash`, `096-array-copy-concurrent-gc`,
  `103-string-append`, `114-ParallelGC`, `115-native-bridge`,
  `126-miranda-multidex`, `149-suspend-all-stress`,
  `156-register-dex-file-multi-loader`, `2031-zygote-compiled-frame-deopt`,
  `2271-profile-inline-cache`, and `304-method-tracing`.
- Every test passed the interpreter, JIT, and unmodified-source lanes. The
  only reproduced failure remains 497, whose own pinned AOSP manifest marks
  it broken until its multi-loader test is rewritten.

### Fresh full-corpus recheck started — 2026-09-08

- Started a new, non-stale corpus ledger with four parallel workers:
  `_build/art-upstream-corpus-recheck`.
- While this turn ends, 66 tests have completed and all are `passed`; worker
  processes remain live for the rest of the pinned corpus. No result is being
  treated as final until the ledger reaches terminal completion.

### Fresh corpus checkpoint — 2026-09-08

- The same four-worker run has reached 105 completed tests; every terminal
  result so far is `passed`. The corpus process remains live, so this remains
  an interim checkpoint rather than a completion claim.

### Fresh corpus checkpoint 2 — 2026-09-08

- The live four-worker recheck has reached 148 completed tests with zero
  failures. The remaining corpus is still running and must reach terminal
  completion before its summary can guide implementation priorities.

### Fresh corpus checkpoint 3 — 2026-09-08

- The live four-worker ledger has reached 170 completed tests with zero
  failures, including CFI, daemon-shutdown, and register-natives cases. The
  remaining corpus is still active and has not yet been classified.

### Fresh corpus checkpoint 4 — 2026-09-08

- The live ledger has reached 199 completed tests with zero failures,
  including GC-loop, lock visitation, thread-group JNI, and app-image cases.
  Workers remain active and no final corpus classification is claimed yet.

### Fresh corpus checkpoint 5 — 2026-09-08

- The live ledger has reached 208 completed tests with zero failures, including
  RMW stress, allocation tracking, bytecode inspection, and suspend paths.
  The full corpus remains active; this is not a completion claim.

### Fresh corpus checkpoint 6 — 2026-09-08

- The live four-worker ledger has reached 220 completed tests with zero
  failures, including native suspend/resume, per-agent TLS, transform, and
  local-variable-table cases. The remaining corpus is still active.

### Fresh corpus checkpoint 7 — 2026-09-08

- The live ledger has reached 227 completed tests with zero failures,
  including VM-init timing and native/recursive/owned monitor paths. Workers
  remain active; the complete corpus verdict is still pending.

### Fresh corpus checkpoint 8 — 2026-09-08

- The live ledger has reached 234 completed tests with zero failures,
  including JVMTI frame-pop, missed-frame-pop, exception-event, and
  exception-catch paths. The full corpus remains active.

### Fresh corpus checkpoint 9 — 2026-09-08

- The live ledger has reached 243 completed tests with zero failures,
  including monitor/JVMTI signal, current-frame, thread-end, and transform
  paths. The remaining corpus is still active.

### Fresh corpus checkpoint 10 — 2026-09-08

- The live ledger has reached 249 completed tests with zero failures,
  including DDMS, dispose stress, raw-monitor suspend/exit/wait, and proxy
  method-argument paths. The worker pool remains active.

### Fresh corpus checkpoint 11 — 2026-09-08

- The live ledger has reached 257 completed tests with zero failures,
  including obsolete method-handle, short-dex, monitor-enter, and pop-frame
  JIT paths. The worker pool remains active; final classification is pending.

### Fresh corpus checkpoint 12 — 2026-09-08

- The live ledger has reached 266 completed tests with zero failures,
  including exception-ext, transform/redefine instrumentation, obsolete-JIT
  multithread, bounds-codegen, and loop-vectorizer paths. The corpus remains
  active.

### Fresh corpus checkpoint 13 — 2026-09-08

- The live ledger has reached 273 completed tests with zero failures,
  including add-to-dex ClassLoader, JVMTI local primitive/object/bad-slot, and
  multi-force-early-return paths. The corpus remains active.

### Fresh corpus checkpoint 14 — 2026-09-08

- The live ledger has reached 282 completed tests with zero failures,
  including JNI ID swap (indices/pointers), resize-array, and structural
  transformation/obsolescence paths. The corpus remains active.

### Fresh corpus checkpoint 15 — 2026-09-08

- Two structural-redefine failures were isolated to the test runner: native
  fixtures could not include AOSP's external `dlmalloc.h`, and a dangling
  generated Java symlink was read during native-owner discovery. The runner
  now exposes `_aosp/external/dlmalloc` and skips dangling Java links. Focused
  reruns of 1986, 1987, and 2000 pass interpreter, JIT, and unmodified lanes.

### Handoff checkpoint 16 — 2026-09-08

- Commit `df07931` is pushed; Python syntax validation passes. The background
  corpus started before the final edit and contains a transient syntax-error
  window in historical rows; newly scheduled tests use the fixed runner.

### Handoff checkpoint 17 — 2026-09-08

- Representative historical failures `420-const-class` and
  `2286-invokevirtual-invokeexact` pass all three lanes under the fixed
  runner, confirming the ledger failures were not ART/JIT semantics.

### Fresh corpus checkpoint 18 — 2026-09-08

- The corpus has passed 675 cases with no new failures after the runner fix.
  The remaining semantic row is AOSP's own disabled `497-inlining-and-class-
  loader` test; its `knownfailures.json` entry documents a deliberately broken
  loader that re-registers an already-registered DexFile under another loader.
  This is kept visible rather than hidden behind a Darwin gate.

### Fresh corpus checkpoint 19 — 2026-09-08

- The worker has reached 773 completed rows; no new failure has appeared after
  the runner repair. The single current semantic failure remains the upstream
  disabled 497 class-loader fixture, while later checker/compiler rows pass.

### Fresh corpus checkpoint 20 — 2026-09-08

- The live worker has reached 815 rows with no post-repair failures. New
  checker, compiler, and JNI rows continue to pass; remaining ledger failures
  are the historical syntax window and the upstream-known 497 fixture.

### Fresh corpus checkpoint 21 — 2026-09-08

- The live worker reached 860 rows with no new post-repair failures. Exception,
  JNI, and compiler coverage continues through the remaining corpus.

### Fresh corpus checkpoint 22 — 2026-09-08

- The live worker reached 878 rows with no new post-repair failures. The
  remaining recorded failures are unchanged: the pre-fix runner syntax window
  and the upstream-known broken 497 class-loader fixture.

### Fresh corpus checkpoint 23 — 2026-09-08

- The worker reached 979 rows. All recorded failures match the pre-fix runner
  SyntaxError window or the documented upstream 497 fixture; no post-fix
  ART/JIT failure has appeared.

### Final corpus checkpoint 27 — 2026-09-08

- The pinned corpus completed all 1,075 discovered tests. Final ledger
  classification: 109 historical rows captured while the runner had a
  transient SyntaxError, one upstream-known broken 497 fixture, and two real
  post-repair runtime gaps: `936-search-onload` and `938-load-transform-bcp`.
  The latter exercise JVMTI agent loading and boot-class transformation and
  remain open; no fallback or allowlist was added.

### Final corpus audit checkpoint 28 — 2026-09-08

- The complete 1,075-test run is terminal. Focused reruns confirm the runner
  repair (`1986`, `1987`, `2000`, `420`, `2286`), while `936` and `938` still
  fail in JVMTI boot/system class-path and boot-class transformation semantics.
  These are explicit remaining implementation work for full AOSP parity.

### Fresh corpus checkpoint 24 — 2026-09-08

- The live worker reached 994 rows. The post-repair suffix remains entirely
  green; 110 recorded failures are unchanged historical SyntaxError rows plus
  AOSP's known broken 497 fixture.

### Fresh corpus checkpoint 25 — 2026-09-08

- The worker reached 1,001 rows. The post-repair suffix remains green; the
  ledger still contains only the historical SyntaxError window and upstream
  known 497 fixture as failures.

### Fresh corpus checkpoint 26 — 2026-09-08

- The worker reached 1,057 rows. `936-search-onload` and
  `938-load-transform-bcp` exposed real remaining JVMTI/boot-class
  transformation gaps; they are tracked as open runtime work.

### Diagnostic checkpoint 29 — 2026-09-08

- Focused reruns reproduce the two remaining semantic gaps: `936-search-onload`
  terminates in ART generated code while loading the injected boot/system DEX,
  and `938-load-transform-bcp` loads its JVMTI agent but does not apply the
  supplied `java.util.OptionalLong` boot-class transformation.
- The upstream runner now emits the bounded native-host log tail whenever an
  invocation exits unexpectedly, preserving ART signal and JVMTI diagnostics
  for the next runtime fix without changing test semantics or adding a
  fallback.

### Runtime checkpoint 35 — 2026-09-08

- Rebuilt the strict runtime/graphics closure and reran the focused JVMTI
  cases. The failures persist independently of stale artifacts: `936` faults
  while resolving the injected boot/system DEX and `938` misses the boot-class
  load hook, whereas ordinary `934` transformation remains green.
- The next implementation boundary is the upstream `ClassPreDefine` to
  `ClassLoaderHelper::AddToClassLoader` boot-loader path; no harness bypass was
  added.

### Diagnostic checkpoint 31 — 2026-09-08

- Re-running on the current machine confirms the prior corpus state: `936` still
  faults in generated code during injected boot/system-Dex loading, while
  `938` still reports the untransformed `OptionalLong`; `934` remains green in
  both interpreter and JIT lanes.
- The failure-log change is validated: 936 now exposes the native signal tail
  instead of only a process exit code. No fallback or test-specific bypass was
  introduced.

### Diagnostic checkpoint 30 — 2026-09-08

- The improved runner reproduced `936-search-onload` as an ART generated-code
  SIGSEGV and now preserves the native signal/register report.
- `934-load-transform` passes in both interpreter and JIT modes, while
  `938-load-transform-bcp` still misses only the boot-class transformation;
  this narrows the implementation target to the boot-class JVMTI callback and
  injected-Dex class-linker boundary rather than generic retransformation.

### Nterp checkpoint 32 — 2026-09-08

- ARM64 Nterp entry normalization is now enforced structurally: both entry
  points lift logical low-32-bit `ArtMethod*` values before their first field
  dereference, while full native pointers remain unchanged.
- The source audit verifies per-entry ordering and rejects late-normalization
  regressions. Nterp unit checks pass 11/11, along with runtime-arm64
  Mach-O/CFI/DWARF audits.

### Nterp checkpoint 34 — 2026-09-08

- Rebuilt the graphics/runtime link closure from current sources; all native
  closure audits remain green and do not change the JVMTI result.
- Focused reruns still reproduce `936` generated-code SIGSEGV and `938`
  missing boot-class transformation, confirming these are runtime semantics
  rather than stale build artifacts.

### Nterp checkpoint 33 — 2026-09-08

- Revalidated ARM64 Nterp normalization on the current machine: per-entry
  ordering checks and generated Mach-O/CFI/DWARF audits pass.
- The Nterp logical-reference entry slice is closed; JVMTI boot-class loading
  and transformation remain open without a fallback.

### Runtime checkpoint 37 — 2026-09-08

- The active class-loader implementation review is still focused on the
  Android-compatible boot-Dex path; no policy workaround was added this turn.

### Runtime checkpoint 36 — 2026-09-08

- Boot-class JVMTI investigation remains active in the class-loader
  implementation lane. No fallback, allowlist, or APK rewrite was introduced.

### Runtime checkpoint 38 — 2026-09-08

- The native boot-class loader review remains active; no additional policy
  gates or harness substitutions were made while awaiting the focused runtime
  implementation result.

### Runtime checkpoint 39 — 2026-09-08

- The boot-class JVMTI issue remains under active native implementation review;
  no semantic workaround or test-specific bypass has been merged.

### Runtime checkpoint 41 — 2026-09-08

- The boot-class JVMTI implementation review continues; current source still
  has no native patch for 936/938, so their failures remain explicit and
  unmasked.

### Runtime checkpoint 40 — 2026-09-08

- The native boot-class JVMTI review is still active; shared worktree inspection
  shows no runtime patch landed yet, so 936/938 remain open and unmasked.

### Runtime checkpoint 42 — 2026-09-08

- On the replacement Mac, the canonical Android 16 boot-image build and
  `934-load-transform` both pass (interpreter and JIT lanes). An experiment
  with an explicit empty `--preloaded-classes` list was reverted after
  `938-load-transform-bcp` still failed; the artifact was rebuilt with the
  canonical command. The remaining 938 gap is therefore a real boot-image
  profile/class-selection issue, not a phase-order workaround, while 936
  remains a separate generated-code fault.

### Runtime checkpoint 43 — 2026-09-08

- Replacement-Mac validation rebuilt the canonical 11-component boot image and
  reconfirmed `934-load-transform` in both interpreter and JIT modes. Passing
  an explicit empty preload list changes the build artifact but does not make
  `938-load-transform-bcp` observe `OptionalLong`; it was not retained. This
  rules out a superficial preload-file switch and keeps the required fix in
  Android's profile-guided image-class selection and boot-Dex loading path.

### Runtime checkpoint 44 — 2026-09-08

- Boot-image generation now invokes the embedded AOSP `profman` to create a
  boot-format profile and passes it to dex2oat with ART/class-loader bootstrap
  seeds. The rebuilt image makes `938-load-transform-bcp` pass in interpreter
  and ARM64 JIT lanes; `936-search-onload` remains a separate generated-code
  fault.

### Runtime checkpoint 45 — 2026-09-08

- 936 reproduction narrowed the null dereference to the ONLOAD system-search
  property update path: the detached runtime can expose `Properties.defaults ==
  null`, while the upstream helper unconditionally invokes methods on it. A
  persistent OpenJDK JVMTI patch now targets the owning `Properties` object in
  that valid state. Source patching succeeds; final graphics relink is pending
  on this host's missing Android NDK 28.2 prerequisite.

### Runtime checkpoint 46 — 2026-09-08

- The proposed null-`Properties.defaults` change was linked into a fresh
  graphics runtime and `936-search-onload` still faulted at the same generated
  code address. The hypothesis is rejected and the patch was removed; 936's
  native/JIT boundary remains unresolved rather than masked by a speculative
  workaround.

### Runtime checkpoint 47 — 2026-09-08

- With the replacement Mac's installed NDK 28.2 explicitly selected, the full
  graphics/runtime link audit passes. A fresh linked-runtime rerun still
  reproduces 936's null-page generated-code fault, ruling out a missing
  toolchain or stale graphics artifact.

### Runtime checkpoint 48 — 2026-09-08

- Persisted the Android-shaped JVMTI search fix: `System` and `Properties` are
  resolved with `FindSystemClass`, and a null `Properties.defaults` receiver
  falls back to the owning `Properties` object. OpenJDK JVMTI rebuild and the
  complete graphics/runtime link audit pass. `936-search-onload` still fails
  with `SIGSEGV/SEGV_ACCERR` at `0x4`, so the remaining defect is in the
  generated-code/JNI call boundary, not class lookup or host toolchain setup.

### Runtime checkpoint 49 — 2026-09-08

- Added explicit `System` class initialization before reading the static
  `System.props` field, using a stack handle as required by ART's
  `EnsureInitialized` contract. The JVMTI archive and full graphics link audit
  remain green; 936 still reproduces `SIGSEGV/SEGV_ACCERR` at `0x4`, so this
  ordering fix is insufficient and the quick-call ABI remains open.

### Runtime checkpoint 50 — 2026-09-08

- Symbol-level crash analysis places the fault before JNI invocation, in
  `ArtField::GetObject(System.props)`: the static field value is null in the
  detached boot image. Added `java.util.Hashtable` and
  `java.util.Properties` to the profile-driven boot-image seed and rebuilt the
  image successfully. The 936 repro still faults at `0x4`, so boot-image
  seeding alone does not yet establish the static initialization contract.

### Runtime checkpoint 51 — 2026-09-08

- The post-seed crash disassembly still shows `ldr w8, [x23,#4]` with `x23=0`
  before any JNI call, confirming a null ART internal field/class pointer rather
  than a JNI receiver fault. The persistent seed and initialization changes
  remain; a JNI field-ID replacement was not retained because the patch needs
  to preserve AOSP callback ordering and is the next isolated experiment.

### Runtime checkpoint 52 — 2026-09-08

- Repeated disassembly continues to show a null ART internal pointer before JNI,
  even after boot seeding and initialization attempts. The next focused task is
  compressed `ObjPtr` normalization at the Darwin C++/ART boundary.

### Runtime checkpoint 54 — 2026-09-08

- Added the shared Darwin `ObjPtr` base-relative encode/decode boundary to the
  runtime-core build (including both `obj_ptr.h` and `obj_ptr-inl.h`). The
  independent `938-load-transform-bcp` interpreter and JIT lanes still pass.
  `936-search-onload` now reaches a different dex2oat internal reference fault,
  so the fix is productive but the full generated-code path remains open.

### Runtime checkpoint 55 — 2026-09-08

- The shared `ObjPtr` boundary patch builds cleanly and preserves the 938
  interpreter/JIT regression test. 936 now faults with a null-page access in a
  later dex2oat reference path, indicating the base-relative conversion is
  active but an additional absolute-vs-offset representation remains.

### Runtime checkpoint 57 — 2026-09-08

- Symbolization confirms `defaults_field->GetObject(props_obj)` receives a null
  `props_obj`; any fallback after that dereference is ineffective. Recovery of
  `System.props` must occur before ART field access.

### Runtime checkpoint 56 — 2026-09-08

- Re-ran `938-load-transform-bcp` after the shared `ObjPtr` update; interpreter
  and JIT lanes both pass. The remaining 936 failure is a separate dex2oat
  internal reference path, not a regression in the fixed boot-classpath path.

### Runtime checkpoint 53 — 2026-09-08

- A JNI field-ID fallback is not sufficient because the null occurs during ART
  `System.props` resolution before JNI dispatch. The next implementation must
  use an ART-safe handle or correct compressed-reference representation.

### Runtime checkpoint 58 — 2026-09-08

- Darwin startup now defers `RunEarlyRootClinits` and `InitializeIntrinsics`
  until after libcore native registration under ART's scoped object access.
  AOSP 936 and 938 pass interpreter, JIT, and unmodified optimized lanes.

### Runtime checkpoint 59 — 2026-09-08

- Rebuilt the graphics runtime after the startup-order fix and re-ran the
  end-to-end ART JIT audit. Intrinsic source contracts, JIT eligibility and
  unwind, GC/JNI/exception/monitor stress, large field access, and inline/GC
  acceptance all pass. The full APK-compatibility goal remains open pending
  broader real-app and dynamic class-loader coverage.

### Runtime checkpoint 60 — 2026-09-08

- Focus shifted to GC-stress coverage after the normal JIT audit. A Sol review
  is reproducing an arm64e PAC trap seen in the gcstress launcher path; normal
  JIT/GC acceptance and AOSP 936/938 remain green. No fallback or test-specific
  suppression has been introduced while the shared ABI boundary is isolated.

### Runtime checkpoint 61 — 2026-09-08

- A serial `074-gc-thrash --gcstress` reproduction confirms the stress-only
  arm64e PAC failure is independent of concurrent builds; normal 074 remains
  green. A temporary LLDB launcher probe was discarded because it interfered
  with the runner's captured-output contract. The runtime source remains
  unchanged while the fault is symbolized at the shared ABI boundary.

### Runtime checkpoint 62 — 2026-09-08

- Fixed the GC-stress arm64e PAC trap by replacing local `_Unwind_Backtrace`
  over Android quick/Nterp frames with the same Darwin Mach frame-record walk
  and stripped return addresses used by the remote path. `074-gc-thrash` and
  `837-deopt` now pass interpreter, JIT, and unmodified optimized lanes;
  `137-cfi` and the unwind-provider smoke also pass.

### Runtime checkpoint 63 — 2026-09-08

- Audited the remaining 497 class-loader failure against pinned AOSP
  `knownfailures.json`: it is an unconditional upstream disable for the
  broken duplicate-DexFile loader contract, and relaxing `RegisterDexFile`
  would regress the valid 156 multi-loader rejection test. No ClassLinker
  weakening was added; valid dynamic-loader tests are the next coverage set.

### Runtime checkpoint 64 — 2026-09-08

- Valid `142-classloader2` passed interpreter, JIT, and unmodified optimized
  lanes on the production graphics runtime, covering a PathClassLoader
  subclass with secondary DEX/class resolution. Supported loader breadth
  remains under active verification.

### Runtime checkpoint 65 — 2026-09-08

- Extended dynamic-loader coverage: `692-vdex-secondary-loader`,
  `693-vdex-inmem-loader-evict`, and `944-transform-classloaders` each pass
  interpreter, JIT, and unmodified optimized lanes. Together with 142, 688,
  and 949, this covers secondary-Dex VDEX ownership, in-memory verification
  cache eviction, shared-library graphs, JVMTI transforms, and loader
  subclasses without changing the original APK or weakening ClassLinker.

### Runtime checkpoint 66 — 2026-09-08

- `2237-checker-inline-multidex` passes interpreter, JIT, and unmodified
  optimized lanes, extending verification to multidex checker/inlining
  behavior without a Darwin-specific code path.

### Runtime checkpoint 67 — 2026-09-08

- `1946-list-descriptors` passes all three lanes, validating JVMTI's
  loader-specific DEX descriptor enumeration on the normal runtime path.

### Runtime checkpoint 68 — 2026-09-08

- `2239-varhandle-perf-vh-cas` passes interpreter, JIT, and unmodified
  optimized lanes, exercising VarHandle compare-and-set lowering and its
  managed-memory barriers through the standard ART ABI.

### Runtime checkpoint 69 — 2026-09-08

- `2247-checker-write-barrier-elimination` and `2277-methodhandle-invokeexact`
  both pass interpreter, JIT, and unmodified optimized lanes. This covers
  write-barrier elimination and exact MethodHandle dispatch in optimized code.

### Runtime checkpoint 70 — 2026-09-08

- `2264-throwing-systemcleaner` and `2282-single-step-before-catch` pass all
  three lanes, covering exception delivery through cleaner callbacks and
  debugger single-step state immediately before a catch handler.

### Runtime checkpoint 71 — 2026-09-08

- `570-checker-osr` and `088-monitor-verification` pass interpreter, JIT, and
  unmodified optimized lanes. This exercises optimized OSR transfer plus
  monitor-verifier rejection/handling for malformed synchronization paths.

### Runtime checkpoint 72 — 2026-09-08

- `406-fields` and `407-arrays` pass all three lanes, covering primitive and
  reference field access plus array loads/stores and their bounds/type checks.

### Runtime checkpoint 73 — 2026-09-08

- `412-new-array` and `420-const-class` pass interpreter, JIT, and unmodified
  optimized lanes, covering array allocation and class-constant resolution.

### Runtime checkpoint 74 — 2026-09-08

- Stabilized the framework `SystemProperties.native_find` handle contract with
  process-lifetime numeric tokens and typed handle reads. Framework compat
  builds and `936-search-onload` pass; the latter passes interpreter, JIT, and
  unmodified optimized lanes.

### Runtime checkpoint 75 — 2026-09-08

- Full `audit-art-jit.sh` passes, including ARM64 JIT acceptance, GC/read
  barriers, JNI/native exits, framework/window/lifecycle smoke, and launcher
  checks. `938-load-transform-bcp` also passes interpreter, JIT, and
  unmodified optimized lanes after the property-handle change.

### Runtime checkpoint 76 — 2026-09-08

- Corrected the `SystemProperties` handle getter JNI prototypes and validated
  stable identity, set-after-find updates, typed reads, and forged-handle
  rejection with focused native smoke. `cargo xtask check` for the native
  source passes; `2242-checker-lse-acquire-release-operations` also passes all
  interpreter, JIT, and unmodified optimized lanes.

### Runtime checkpoint 77 — 2026-09-08

- Matched Android boolean system-property parsing for `y`/`n` aliases and
  reran the native compatibility check; the complete managed-native-load and
  graphics-link audit remains green.

### Runtime checkpoint 78 — 2026-09-08

- `2275-integral-unsigned-arithmetic` passes interpreter, JIT, and unmodified
  optimized lanes, covering unsigned integral comparisons/division and the
  corresponding ARM64 lowering paths.

### Runtime checkpoint 79 — 2026-09-08

- `410-floats` and `419-long-parameter` pass all three lanes, validating
  floating-point lowering and wide-argument register/stack ABI marshalling.

### Runtime checkpoint 80 — 2026-09-08

- `2239-varhandle-perf-vh-unsafe-cas` passes interpreter, JIT, and unmodified
  optimized lanes, covering Unsafe-backed atomic compare-and-set and its
  managed memory ordering/barrier path.

### Runtime checkpoint 81 — 2026-09-08

- `401-optimizing-compiler` and `304-method-tracing` pass all three lanes,
  validating optimizing-compiler execution and instrumentation method-entry/
  exit tracing on JIT code.

### Runtime checkpoint 82 — 2026-09-08

- `411-optimizing-arith` and `414-static-fields` pass interpreter, JIT, and
  unmodified optimized lanes, covering optimizer arithmetic lowering and
  static-field initialization/read/write barriers.

### Runtime checkpoint 83 — 2026-09-08

- `418-const-string` and `2256-checker-vector-replacement` pass all three
  lanes, validating string constant resolution and vector replacement in the
  optimizing compiler.

### Runtime checkpoint 84 — 2026-09-08

- Implemented Android InputTransport connection-token identity: each channel
  pair owns one process-visible `android.os.Binder` token shared by client,
  server, and dup wrappers; `nativeGetToken` returns a local reference and
  teardown releases the global reference safely. Native compat incremental
  build and graphics-link audit pass.

### Runtime checkpoint 85 — 2026-09-08

- `370-dex-v37` passes interpreter, JIT, and unmodified optimized lanes,
  validating the current DEX v37 reader/verifier and execution path.

### Runtime checkpoint 86 — 2026-09-08

- `412-new-array --gcstress` passes interpreter, JIT, and unmodified optimized
  lanes, validating array allocation and reference barriers under concurrent
  copying GC stress.

### Runtime checkpoint 87 — 2026-09-08

- Implemented process-local `InputChannel` Parcel round-trip for the Darwin
  transport: name, endpoint role, and shared Binder token are serialized and
  restored, with registry lookup preserving pair/dup identity. Native graph
  incremental check passes.

### Runtime checkpoint 88 — 2026-09-08

- `543-env-long-ref` and `686-get-this` pass interpreter, JIT, and unmodified
  optimized lanes, covering JNI long-reference lifetime and instance receiver
  ABI handling.

### Runtime checkpoint 89 — 2026-09-08

- `543-env-long-ref --gcstress` passes all three lanes, validating JNI long
  reference lifetime across concurrent-copying GC and compiled/native exits.

### Runtime checkpoint 90 — 2026-09-08

- Aligned `InputChannel.dispose()` with Android's native lifecycle by releasing
  the shared transport state immediately while retaining only the finalizer
  wrapper. This prevents Binder global references from outliving JavaVM
  shutdown; native graph and managed-load/graphics-link checks pass.

### Runtime checkpoint 91 — 2026-09-08

- Managed InputChannel Parcel smoke (`Parcel.obtain` → write/read → CREATOR)
  verifies restored name and Binder token identity across interpreter, JIT,
  and optimized lanes. `2258-checker-valid-rti` and
  `2283-checker-remove-null-check` also pass all lanes. Cross-process FD export
  remains an explicit transport-layer gap.

### Runtime checkpoint 92 — 2026-09-08

- KeyCharacterMap Parcel support now preserves a Darwin map's device id with
  a versioned magic record instead of silently resetting to device 1. Native
  graph check passes; managed `obtainEmptyMap` → Parcel → CREATOR identity and
  keyboard-type smoke passes interpreter, JIT, and optimized lanes.

### Runtime checkpoint 93 — 2026-09-08

- `9999-key-character-map-parcel-smoke` verifies device-id identity and FULL
  keyboard type across all lanes. `2255-checker-branch-redirection` also
  passes interpreter, JIT, and unmodified optimized lanes.

### Runtime checkpoint 94 — 2026-09-08

- InputChannel Parcel writes an Android-compatible initialized marker for
  disposed/uninitialized channels, preserving composite Parcel cursor alignment
  instead of silently emitting no bytes. Native graph check and managed
  disposed-channel-plus-sentinel smoke pass interpreter, JIT, and optimized
  lanes.

### Runtime checkpoint 95 — 2026-09-08

- `2252-rem-optimization-dividend-divisor` and `2278-nested-loops` pass
  interpreter, JIT, and unmodified optimized lanes, covering remainder
  optimization and nested-loop control-flow lowering.

### Runtime checkpoint 96 — 2026-09-08

- `2259-checker-code-sinking-infinite-try-catch` and
  `2284-regression-test-368984521-loop-opt` pass all three lanes, covering
  exception-region code sinking and loop optimization regression handling.

### Runtime checkpoint 97 — 2026-09-08

- `2253-checker-devirtualize-always-throws` passes interpreter, JIT, and
  unmodified optimized lanes, validating devirtualization of always-throwing
  calls and their exception edges.

### Runtime checkpoint 98 — 2026-09-08

- `2248-checker-smali-remove-try-until-the-end` passes all three lanes,
  validating try-region elimination and exception-table boundary lowering.

### Runtime checkpoint 99 — 2026-09-08

- Implemented `KeyCharacterMap.nativeGetEvents` using the runtime's generic
  key-character mapping: printable characters become Android `KeyEvent`
  down/up sequences, with explicit Shift down/up events for shifted glyphs,
  monotonic event times, preserved device id, and keyboard source. A managed
  smoke validates lowercase and uppercase event ordering plus unmapped-character
  failure across interpreter, JIT, and unmodified optimized lanes. Fallback
  actions remain false until layout behavior records are available rather than
  inventing test-specific substitutions.

### Runtime checkpoint 100 — 2026-09-08

- Implemented `KeyCharacterMap.nativeGetMatch` and `nativeGetNumber` for the
  Darwin FULL physical-keyboard map. Match selection prefers the requested
  meta-state behavior and then another base/Shift behavior generated by the
  same key; decimal-row keys expose their unmodified dial-pad character while
  physical alphabetic keys do not invent legacy ALPHA-layout T9 numbers. A
  managed smoke covers preferred/fallback/no/empty matches, Java's public
  null-array `IllegalArgumentException`, and digit/letter/unknown number
  results across interpreter, JIT, and unmodified optimized lanes.

### Runtime checkpoint 101 — 2026-09-08

- Audited the remaining `InputChannel` boundary against the AOSP transport
  contract. The current process-local Parcel/token behavior is intentionally
  retained for same-process callers, while cross-process delivery is tracked
  as an open vertical slice: endpoint FDs, framed motion/key payloads, and
  finish acknowledgements must move together rather than adding a superficial
  FD field that still leaves the payload deque process-local.

### Runtime checkpoint 102 — 2026-09-08

- Extracted broker-backed Parcel file-descriptor duplication/read helpers into
  one shared path and revalidated the native graph plus graphics-link audit.
  This prepares InputChannel endpoint transfer without changing existing
  same-process semantics; endpoint adoption and framed payload delivery remain
  the next implementation slice.

### Runtime checkpoint 103 — 2026-09-08

- Revalidated the broker-backed descriptor path after wiring the AOSP parcel
  order at the native boundary: token, UTF-16 name, and one endpoint FD are
  now consumed in that order. Native graph and graphics-link audits pass;
  remote payload framing remains explicitly unimplemented until endpoint
  ownership is exercised by a two-process smoke.

### Runtime checkpoint 104 — 2026-09-08

- `InputChannel` now consumes and emits the AOSP parcel tuple (connection
  token, UTF-16 name, unique endpoint FD), adopting an imported broker
  descriptor with single-owner cleanup. Native graph and graphics-link checks
  pass. A two-process payload/finish-ACK smoke is still required before the
  transport gap can be closed.

### Runtime checkpoint 105 — 2026-09-08

- Revalidated imported endpoint ownership after separating the broker FD from
  the local same-process wake pair. The native graph and graphics-link audit
  pass; the endpoint is retained for the pending framed payload reader rather
  than being incorrectly treated as a local queue.

### Runtime checkpoint 106 — 2026-09-08

- InputChannel now preserves the Android initialized marker and adopts a
  registry-miss endpoint with the transferred Binder token plus a separate
  local wake pair. Native graph, graphics-link, format, and diff checks pass;
  payload/finish-ACK framing remains the explicit next gap.

### Runtime checkpoint 107 — 2026-09-08

- Added a versioned fixed-size InputChannel frame for imported endpoints:
  producer writes pointer/key packets to the broker FD, and the registered
  remote endpoint callback reassembles and queues complete frames before
  normal ViewRoot dispatch. Native graph, graphics-link, and format checks
  pass; finish-ACK framing remains to be added.

### Runtime checkpoint 108 — 2026-09-08

- Remote InputChannel framing is now guarded by magic/version/size and packet
  validation, serialized under a per-channel writer lock, and reassembled
  before dispatch. Imported endpoints remain distinct from local wake pairs;
  finish-ACK framing and two-process verification remain open.

### Runtime checkpoint 109 — 2026-09-08

- Added a dedicated finish-ACK frame carrying sequence and handled state;
  remote `finishInputEvent` emits it and the callback merges it into the
  bounded wait queue. Interleaved ACK/input frames are preserved by unified
  decoder ordering. Native graph and graphics-link checks pass; two-process
  end-to-end validation remains open.

### Runtime checkpoint 110 — 2026-09-08

- Hardened the shared full-duplex InputChannel stream: the callback now drains
  arbitrary alternating input and finish-ACK frames until no decoder advances,
  and ignores unsolicited ACK sequences instead of growing the completion
  queue. Incremental native compilation, graphics-link audit, Rust formatting,
  and diff checks pass; a process-separated end-to-end smoke remains open.

### Runtime checkpoint 110 — 2026-09-08

- Added ACK-aware remote stream decoding with separate local/remote Looper
  registrations. Input and finish-ACK frames can now interleave without being
  dropped, and remote ACKs enter the same bounded finish wait queue. Native
  graph check passes; process-separated end-to-end smoke remains pending.

### Runtime checkpoint 111 — 2026-09-08

- Fixed the APK launch acceptance harness for empty split lists under Bash
  nounset, allowing base-only AOSP APK runs to reach the runtime instead of
  aborting before launch. `bash -n` and diff checks pass; the two-process
  InputChannel smoke remains the next validation boundary.

### Runtime checkpoint 112 — 2026-09-08

- Re-ran the Android window-menu acceptance flow after fixing its outside-click
  sequence. Calculator menu/History, outside dismiss, Calendar popup labels,
  and Chrome menu/new-tab paths all pass without crashes. The resize lane still
  exposes a real gap: the popup ViewRoot remains at its pre-resize position
  (`at=320,8`) instead of being relaid out to `at=208,8`; keep this as a runtime
  WindowManager relayout task rather than weakening the assertion.

### Runtime checkpoint 113 — 2026-09-08

- Wired display resize into each live `ViewRootImpl` through the Android 16
  `forceWmRelayout()` contract. A live popup now recomputes its frame from
  `at=320,8` to `at=208,8` before its anchor update. The acceptance harness now
  keeps that popup alive during resize and uses physical Calendar coordinates.
  Calculator, Calendar, outside-dismiss, and resize lanes pass. Chrome reaches
  startup but still aborts in the existing `MockContext.sendBroadcast()` stub,
  so the full suite remains red until that framework broadcast path is added.

### Runtime checkpoint 114 — 2026-09-08

- Implemented the app-context `sendBroadcast`/`sendBroadcastAsUser` contract
  so detached applications no longer inherit `MockContext`'s `Stub!` failure;
  the DEX contract was updated and `build-button-dex` passes. Chrome now gets
  past startup broadcast setup, but its compositor teardown exposes a separate
  `std::system_error` mutex-lock failure followed by an ART generated-code
  fault; concurrency/JIT lifetime diagnosis remains open.

### Runtime checkpoint 115 — 2026-09-08

- Crash diagnostics identify the next boundary precisely: Chrome's
  `BlastBufferQueueNativeDestroy` calls
  `darwin_art_android_ANativeWindow_set_transaction_callback` after the Java
  Surface release path has dropped the last native-window reference. The host
  mutex is therefore accessed after free (`std::mutex::lock(EINVAL)`), not
  because of a JIT instruction defect. A queue-owned native-window lifetime
  reference is being added and will be validated through compositor teardown.

### Runtime checkpoint 116 — 2026-09-08

- Confirmed the UAF was caused by mapping both `Surface.nativeDestroy` and
  `nativeRelease` to the same refcount decrement. `nativeDestroy` is now a
  producer disconnect no-op while `nativeRelease` owns the Java reference
  release. Chrome compositor teardown no longer emits the mutex EINVAL or ART
  generated-code fault; the remaining `unknown service child PID` is isolated
  to child-service registration.

### Runtime checkpoint 117 — 2026-09-08

- Service-child release is now idempotent: duplicate stop/unbind callbacks for
  an already-reaped PID no longer abort the host. Host check/tests pass, and a
  fresh Chrome startup/menu smoke exits with status 0 without the prior mutex,
  generated-code, broadcast-stub, or unknown-child-PID failures.

### Runtime checkpoint 118 — 2026-09-08

- Chrome process-lifecycle acceptance now completes two real startup/menu
  iterations with physical `new_tab_menu_id` clicks. Both runs show clean
  compositor teardown and service-child reaping: no mutex UAF, generated-code
  fault, broadcast stub, or unknown-PID error. The next boundary is deeper tab
  content/rendering behavior rather than process startup.

### Runtime checkpoint 119 — 2026-09-08

- Began long-running Chrome tab-graphics acceptance with the real tab-switcher
  and tab-grid physical click sequence. The harness had an inline shell
  comment that accidentally terminated its `env` assignment; this was fixed.
  The run now reaches sustained SurfaceFlinger scanout and exposes a separate
  long-lived ART generated-code fault after compositor activity, so tab
  rendering remains open despite short lifecycle smoke passing.

### Runtime checkpoint 120 — 2026-09-08

- On the replacement Mac, rebuilt the native graphics bootstrap after wiring
  Android `ImageReader` JNI to the Darwin ANativeWindow/AHardwareBuffer queue.
  The build and graphics-link audit passed. A fresh 8-second Chrome APK smoke
  exited with status 0 and no `ImageReader.nativeClassInit` linkage error,
  fatal signal, or generated-code fault. Long-running tab graphics acceptance
  remains the next boundary; image-plane and hardware-buffer API coverage is
  still tracked as an AOSP compatibility gap.

### Runtime checkpoint 121 — 2026-09-08

- Completed the Java `HardwareBuffer` bridge for ImageReader surfaces: native
  AHardwareBuffer handles now cross the SurfaceImage boundary with a retained
  native lifetime, and the basic allocation/describe/finalizer registrations
  are present. Rebuilt graphics bootstrap/link audit and a fresh Chrome APK
  smoke both pass (`RC=0`); the log shows IOSurface-backed AHardwareBuffer
  allocation. Parcel/GraphicBuffer conversion and full CPU plane semantics
  remain open for broader AOSP ImageReader compatibility.

### Runtime checkpoint 122 — 2026-09-08

- Added the Android `SyncFence` JNI contract used by compositor/ImageReader
  paths: owned fence handles, finalization, validity/fd queries, bounded waits,
  signal-time reporting, and reference increments now route through the Darwin
  broker/sync primitive. Native graphics bootstrap/link audit and a fresh
  Chrome APK smoke pass with `RC=0`; no SyncFence linkage or fatal-signal error
  appeared. Full fence/parcel stress remains open.

### Runtime checkpoint 123 — 2026-09-08

- Revalidated the replacement-machine toolchain after the SyncFence bridge:
  `cargo test -p darwin-art-host` passes all 10 host/graphics tests and
  `cargo test -p art-bootstrap` passes all 14 Nterp/build-contract tests.
  These are regression evidence only; they do not close the remaining
  long-running Chrome, parcel, or full AOSP corpus gates.

### Runtime checkpoint 124 — 2026-09-08

- Re-ran the real 70-second Chrome tab-grid graphics acceptance with the
  ImageReader and SyncFence bridges. The renderer reached 11,280
  SurfaceFlinger scanout requests and 43 presents, and no ImageReader/SyncFence
  linkage error appeared. The run still reproduced a burst of ART
  generated-code faults in the JIT address range (for example
  `pc=0x50e774c9c` and `pc=0x3043d1f38`), so the remaining boundary is now
  narrowed to generated-code lifetime/GC or signal-unwind handling under
  sustained renderer activity. The full acceptance gate remains failing.

### Runtime checkpoint 125 — 2026-09-08

- Replacement-machine verification passes host/bootstrap tests, incremental
  native build, graphics-link closure audit, and MAP_JIT W^X audit. Fault
  context now records signal-safe Darwin JIT write depth; every reproduced
  Chrome generated-code fault had depth zero, ruling out a leaked write scope.
  Generated-code range lifetime/GC reclaim or signal-unwind remains the active
  blocker for the sustained acceptance gate.

### Runtime checkpoint 126 — 2026-09-08

- Moved the Darwin raw fault dump after ART's other fault handlers. The prior
  placement mislabeled recoverable page-level W^X transitions as generated-code
  faults; the rebuilt runtime and graphics-link audit pass. A fresh 70-second
  Chrome run still reports 2,558 unhandled generated-code faults with
  `jit_write_depth=0`, confirming a real JIT/runtime fault remains rather than
  a write-scope diagnostic artifact.

### Runtime checkpoint 127 — 2026-09-08

- Rebuilt the patched runtime after moving raw Darwin fault logging behind
  `HandleFaultByOtherHandlers` and completed another live 70-second Chrome
  acceptance run. SurfaceFlinger reached 11,280 scanout requests and 44
  presents; 2,558 generated-code faults remained unhandled, all with
  `jit_write_depth=0`. The diagnostic ordering fix is pushed as `3593f47`;
  code-cache retirement/lifetime and generated-code correctness remain open.

### Runtime checkpoint 128 — 2026-09-08

- Sigchain correlation shows ART's special handler runs before the Darwin user
  trampoline that restores Android V8 MAP_JIT permissions. The 2,558 records
  are therefore recoverable W^X transitions logged too early; the run reports
  `unresolved=0` and `fatal=0`. The next fix is narrowly scoped pre-special W^X
  recovery, followed by a fresh 70-second acceptance run.

### Runtime checkpoint 129 — 2026-09-08

- Implemented reusable Darwin runtime-signal recovery in the bionic process
  facade and invoke it before ART's special sigchain handlers. The rebuilt
  70-second Chrome run produced zero `ART original generated-code fault` lines
  while reaching 11,340 scanout requests and 42 presents. The wrapper still
  failed its real-button discovery assertion, so end-to-end acceptance remains
  open despite the JIT W^X fault boundary being fixed.

### Runtime checkpoint 130 — 2026-09-08

- Revalidated the bionic process-state facade after the signal-ordering change;
  its test targets pass. Two independent 70-second Chrome runs report zero
  generated-code diagnostics, zero unresolved signals, and no fatal abort. The
  remaining harness failure is limited to stale coordinate-based tab-switcher
  discovery.

### Runtime checkpoint 131 — 2026-09-08

- The failed acceptance log confirms the stale sequence misses the tab switcher
  after scale conversion: `(225,610)` becomes `(450,1220)` and misses, while
  `(90,320)` becomes `(180,640)` and hits a `SuggestionsTileView`. No JIT or
  graphics fault occurred; the next run will target the top-toolbar button.

### Runtime checkpoint 132 — 2026-09-08

- A clean physical-coordinate sweep on the active 720x1280 Chrome window found
  only `avatar_button` in the top toolbar; x=440/500/560 hit `UrlBarApi26` and
  x=640 hit the avatar. `TabSwitcherButtonView` is not instantiated, pointing
  to a tablet/window configuration mismatch rather than input delivery.

### Runtime checkpoint 134 — 2026-09-08

- Fixed `run-android-apk-app.sh` to preserve an explicit
  `DARWIN_ART_WINDOW_SCALE` instead of forcing Retina scale 2. A scale-1
  physical Chrome probe now uses 360x640 coordinates and hits the real
  `BottomBarAppMenu`; JIT and input dispatch remain clean.

### Runtime checkpoint 133 — 2026-09-08

- A clean physical-click probe confirmed the runtime publishes a 720x1280
  display at 320 dpi (360x640 dp), while Chromium selects a toolbar containing
  only URL-bar and avatar controls; `TabSwitcherButtonView` is absent. This is
  now tracked as a Chrome configuration/harness issue separate from ART JIT;
  the pre-special MAP_JIT recovery remains fault-free in sustained runs.

### Runtime checkpoint 135 — 2026-09-08

- Re-ran the Rust regression suites after the scale fix: host (8 unit plus 2
  graphics tests), bootstrap (14), and bionic process-state all pass. Scale-1
  Chrome confirms corrected 360x640 physical input; tab-switcher acceptance
  still awaits a configuration that instantiates that view.

### Runtime checkpoint 136 — 2026-09-08

- The complete `tools/audit-art-jit.sh` gate exited successfully on the new
  machine. It covers ARM64 intrinsics, compiled arithmetic/JNI exits,
  concurrent moving GC/read barriers, fields/arrays/type checks, exceptions,
  OSR/deoptimization, virtual dispatch, Android lifecycle/window smoke, and
  launcher execution with normal JIT enabled.

### Runtime checkpoint 137 — 2026-09-08

- Ran the unchanged AOSP core-app graphics acceptance. ExactCalculator
  received physical clicks and produced `2+3=5`; DeskClock switched its real
  Material tab to Timer and displayed `00h 00m 00s`. Both published visible
  HWUI/SurfaceFlinger/Metal buffer transactions with no fatal signal.

### Runtime checkpoint 138 — 2026-09-08

- Ran the unchanged `SolitaireCG` game APK through physical-input acceptance.
  `net.sourceforge.solitaire_cg.SolitaireView` was created, drag MotionEvents
  were consumed through the input channel, and the 8-second native-free run
  completed without fatal signal or activity exception.

### Runtime checkpoint 139 — 2026-09-08

- Probed the unchanged VLC Android APK with native/JNI diagnostics. VLC loads
  its native graph and reaches framework rendering, but the process currently
  fails on the missing Android `Surface.nativeLockCanvas(long, Canvas, Rect)`
  contract in the software draw path. This is a concrete framework-native gap;
  no JIT fault was observed. AOSP Surface JNI parity is the next implementation
  target.

### Runtime checkpoint 140 — 2026-09-08

- Isolated VLC's failure to the framework `Surface` native registration: the
  existing table covers lifecycle/BLAST methods but lacks
  `nativeLockCanvas(long, Canvas, Rect)`. Implementing this AOSP contract is
  required for VLC's software-surface path; no JIT fault is involved.

### Runtime checkpoint 141 — 2026-09-08

- Confirmed the AOSP implementation boundary for VLC's missing
  `Surface.nativeLockCanvas`: acquire an `ANativeWindow` buffer, bind it to the
  Java Canvas through the existing HWUI `ACanvas` bridge with dirty-clip
  semantics, then detach and post via `ANativeWindow_unlockAndPost` so the
  IOSurface/Metal compositor remains the presentation path.

### Runtime checkpoint 142 — 2026-09-08

- Added and linked `Surface.nativeLockCanvas`/`nativeUnlockCanvasAndPost` using
  the ANativeWindow lock, HWUI ACanvas buffer binding, dirty clipping, and
  unlock/post path. Graphics-link audit passes and VLC now resolves the native
  symbols, but the first real lock still returns `IllegalArgumentException`;
  the failing subcondition (surface ownership, buffer lock, or Canvas binding)
  is being isolated before claiming VLC support.

### Runtime checkpoint 143 — 2026-09-08

- Revalidated on the replacement Mac: `cargo check -p darwin-art-host`, all
  host tests (8 unit + 2 graphics acceptance), and the incremental graphics
  link audit pass. The VLC APK is not present on this machine, so a physical
  `nativeLockCanvas` run is pending after reinstalling the fixture. Diagnostics
  isolated the prior failure to `ANativeWindow_lock` returning `-EINVAL` for
  an unsupported logical surface format; the native path now normalizes that
  format to RGBA_8888 before locking.

### Runtime checkpoint 144 — 2026-09-08

- Re-ran the JIT memory and ART ARM64 acceptance audits on the replacement Mac.
  Signed `MAP_JIT` nested/concurrent W^X checks, intrinsic inventory, compiled
  arithmetic/JNI, moving-GC/read-barrier calls, fields/arrays, exceptions,
  OSR/deopt, virtual dispatch, and mixed register/stack roots all pass. The
  audit still reports the expected Darwin sentinel-page and Linux membarrier
  warnings; these are host-boundary diagnostics, not interpreter fallback.

### Runtime checkpoint 145 — 2026-09-08

- Removed the Darwin-only forced `implicit_null_checks_=false` compiler setting.
  The JIT now inherits ART's runtime setting, while ARM64 HNullCheck, field,
  interface-invoke, and virtual-call consumers all use nullable compressed-
  reference decoding. The staging audit rejects reintroduction of the forced
  disable or omission of any consumer. AOSP implicit-null and null-call
  interpreter/optimized regressions pass, and the full ART JIT audit remains
  green.

### Runtime checkpoint 146 — 2026-09-08

- Rebuilt the pinned ARM64 JIT with implicit checks enabled (106 objects, one
  changed translation unit) and reran the complete ART JIT audit. The upstream
  551/479/034 null-check regressions pass in interpreter and optimized modes;
  no Darwin explicit-only gate or interpreter fallback was introduced.

### Runtime checkpoint 147 — 2026-09-08

- Full Rust workspace regression (`cargo test --workspace`) passes across ART
  bootstrap, ELF/JNI, runtime ownership, filesystem, host graphics, and
  provider crates. The next bounded review is the Darwin pthread
  empty-checkpoint wait path, which still differs from AOSP futex wake semantics.

### Runtime checkpoint 148 — 2026-09-08

- Rebuilt the Darwin runtime core with the pthread monitor/empty-checkpoint
  patches applied (monitor bootstrap archive produced successfully). The ART
  audit's synchronized static/instance monitor, reentrancy, GC, exception
  release, and contention cases remain passing. A dedicated bounded-latency
  checkpoint test is still the next synchronization task.

### Runtime checkpoint 149 — 2026-09-08

- Re-inspected the AOSP `Mutex`/`ReaderWriterMutex` checkpoint contract and the
  Darwin pthread adaptation. Existing monitor contention plus GC/exception
  release coverage remains green; no semantic change is committed until a
  dedicated checkpoint-response measurement can exercise the blocked-lock path.

### Runtime checkpoint 152 — 2026-09-08

- The dedicated ART Thread contention fixture now executes through the linked
  acceptance runtime: with a 500 ms mutex hold, `RunEmptyCheckpoint()` completes
  in 176 µs and the waiting lock acquires after 500,738 µs. This demonstrates
  checkpoint servicing while blocked, not merely a free-lock fast path.
  `audit-art-jit.sh`, graphics incremental link, xtask 18/18, and bootstrap
  14/14 all pass.

### Runtime checkpoint 151 — 2026-09-08

- Added fail-closed build validation for the Darwin pthread empty-checkpoint
  adaptation and wired the transitive JIT checkpoint header into the native
  graph, preventing stale acceptance objects. `cargo test -p art-bootstrap`,
  `cargo test -p darwin-art-xtask`, runtime-core rebuild, and the full ART JIT
  audit pass. The dedicated fixture is linked; its runtime emission still needs
  a final invocation-path check before claiming latency coverage.

### Runtime checkpoint 150 — 2026-09-08

- Kept the AOSP empty-checkpoint contract under review without weakening it or
  adding an interpreter fallback. The Darwin runtime core rebuild remains
  green, and the existing contention/GC/exception tests pass. A dedicated
  blocked-lock response measurement is still required before changing the
  pthread polling implementation.

### Runtime checkpoint 153 — 2026-09-08

- Hardened `audit-art-jit.sh` to capture runtime output and fail if the
  empty-checkpoint contention fixture is not actually executed. Fresh audit
  execution reports `checkpoint_us=159` and `lock_us=500441` with RC=0,
  preventing stale or partial probes from appearing green.

### Runtime checkpoint 154 — 2026-09-08

- Retired `0144-darwin-compiled-jni-frame-contract.patch`, which forced all
  ordinary compiled JNI transitions through the C++ start/end helpers on
  Darwin. Generated stubs now retain AOSP's inline ARM64 CAS transition and
  branch to `pJniMethodStart`/`pJniMethodEnd` only on the existing slow labels
  when suspend/checkpoint state requires it.
- The JIT staging audit requires both inline transitions and both slow labels
  without an Apple-only bypass. `build-jit-compiler` rebuilt one of 106
  objects; graphics incremental link and `audit-art-jit.sh` pass. Untouched
  AOSP `004-JniTest` passes interpreter/JIT/unmodified optimized lanes, and
  `137-cfi` passes its three JIT CFI runs, preserving JNI ABI and unwind
  behavior while removing the hot-path fallback.

### Runtime checkpoint 155 — 2026-09-08

- Revalidated the pushed JNI transition restoration on the replacement Darwin
  host. `audit-art-jit.sh` completed with the required empty-checkpoint marker;
  AOSP `004-JniTest` interpreter/JIT/optimized lanes and `137-cfi` JIT CFI
  runs remain green. The ordinary compiled-JNI path now uses AOSP's inline
  ARM64 transition protocol, while suspend/checkpoint slow labels remain
  available. Remaining compatibility work is still tracked as unfinished;
  Nterp and other AOSP semantic/ABI gaps require separate implementation and
  acceptance coverage.

### Runtime checkpoint 156 — 2026-09-08

- Rebuilt the production ARM64ng Nterp object on the replacement host. The
  generated Mach-O object passed DWARF validation and the fail-closed
  256-handler/symbol/CFI audit (`build-nterp-arm64ng`); no legacy Apple Nterp
  suppression was introduced. The broader AOSP compatibility objective
  remains open.

### Runtime checkpoint 157 — 2026-09-08

- Runtime acceptance now completes ARM64's ordinary visibly-initialized
  publication boundary, requires the cold `Hello.answer()` entrypoint to equal
  AOSP `ExecuteNterpImpl`, invokes it, verifies result 42, and requires the
  Nterp entry to remain installed. `audit-art-jit.sh` rejects runs missing this
  executable marker, closing the prior link-only false-green gap.
- Deleted the unused legacy Darwin Nterp-disable and catch-entry-disable patch
  files. Incremental graphics/runtime linkage, the full JIT acceptance suite,
  and unchanged AOSP `837-deopt` interpreter/JIT/unmodified optimized lanes
  pass. This does not close other ART/JVMTI/application compatibility work.

### Runtime checkpoint 158 — 2026-09-08

- Re-ran the strict Nterp acceptance after push on the current host. Runtime
  startup, AOSP Nterp admission, pre/post `ExecuteNterpImpl` entrypoint
  identity, and the returned value `42` all pass (`/tmp/nterp-run4.log`);
  the full JIT audit and focused `837-deopt` lanes remain green. The overall
  normal-app compatibility objective remains open.

### Runtime checkpoint 159 — 2026-09-08

- Re-ran `cargo test --workspace` on the replacement host after the native
  Nterp work; all workspace unit and doc tests completed successfully. This
  is regression evidence only and does not replace the remaining AOSP corpus
  and real-application compatibility gates.

### Runtime checkpoint 160 — 2026-09-08

- Closed the framework Surface software-Canvas producer gap. Detached Java
  `Surface` instances now receive a distinct managed ANativeWindow producer
  pointer (not a synthetic token), and the focused lockCanvas → Canvas bind →
  dirty-clip → unlockCanvasAndPost → release acceptance passes (`RC=0`,
  `/tmp/audit-surface-lock.log`). The broader VLC/full-application matrix
  remains open.

### Runtime checkpoint 161 — 2026-09-08

- Strengthened Surface software-Canvas acceptance to require a managed
  producer, normalized `RGBA_8888` format, producer/Canvas dimensions, and
  clamped dirty rect `0,0,72,48`; the standard `audit-art-jit.sh` now runs
  this gate by default. Fresh run passes Surface, Nterp, and empty-checkpoint
  markers (`/tmp/audit-surface-strict.log`, RC=0).

### Runtime checkpoint 163 — 2026-09-08

- Added a MediaCodec output-surface lifetime acceptance to the standard audit.
  It exercises Java `configure` with a real managed producer, releases the
  first Surface, switches via `setOutputSurface`, and verifies reference
  retirement only when the codec is released. VP9 setup and producer
  replacement pass together with Surface/Nterp/synchronization markers
  (`/tmp/audit-mediacodec-surface.log`, RC=0).

### Runtime checkpoint 164 — 2026-09-08

- Extended MediaCodec acceptance toward decoded-frame posting and retained
  producer snapshots for asynchronous callbacks. The strict run currently
  reaches Java VP9 `configure` but stalls before its completion marker,
  exposing an unresolved configure/fromSurface boundary. The implementation
  remains uncommitted until that hang is fixed.

### Runtime checkpoint 162 — 2026-09-08

- Fresh standard audit on the current host passes the required Surface
  producer/Canvas contract (`360x640`, dirty `0,0,72,48`, RGBA_8888), native
  Nterp execution (`result=42`), and empty-checkpoint contention
  (`checkpoint_us=160`, `lock_us=500207`), RC=0. Remaining real-app and full
  AOSP compatibility gates are still open.

### Runtime checkpoint 165 — 2026-09-08

- MediaCodec output publication now snapshots and retains the native producer
  under the codec mutex, then performs potentially blocking ANativeWindow
  lock/post outside that mutex. Decoder callbacks only queue frames, while
  releaseOutputBuffer(render=true) publishes after unlocking. Combined strict
  audit passes Surface Canvas, MediaCodec configure/rebind/release producer
  lifetime, native Nterp, and empty-checkpoint contention (RC=0;
  `/tmp/audit-fixed.log`). Decoded-frame pixel posting remains a separate
  follow-up gate.

### Runtime checkpoint 166 — 2026-09-08

- Rebuilt the graphics closure from the current source and reran a fresh
  strict audit. Surface Canvas, MediaCodec configure/setOutputSurface/release
  producer lifetime, native Nterp, and empty-checkpoint contention all pass
  with RC=0 (`/tmp/audit-fresh-final.log`). Decoded-frame pixel posting is not
  part of this accepted baseline yet.

### Runtime checkpoint 167 — 2026-09-08

- Re-ran the AOSP corpus range `061-out-of-memory` through `100-reflect2`
  against the current runtime with four workers. All 42 discovered contracts
  pass interpreter, JIT expected-output, and unchanged-source optimized lanes;
  this refreshes the stale full-audit failures for allocation/GC, fields,
  monitors, concurrency, and reflection.

### Runtime checkpoint 168 — 2026-09-08

- Fresh four-worker corpus rerun of `101-fibonacci` through
  `180-native-default-method` passes all 40 contracts in interpreter, JIT,
  and unchanged-source optimized lanes, covering concurrent GC, JNI/native
  bridges, class loading, app-image metadata, and default-method dispatch.

### Runtime checkpoint 169 — 2026-09-08

- Fresh four-worker rerun of the seven executable contracts from
  `181-default-methods` through `203-multi-checkpoint` passes interpreter, JIT,
  and unchanged-source optimized lanes. Coverage includes method linking,
  read-modify-write stress, exception detail messages, thread OOME, and
  checkpoint coordination.

### Runtime checkpoint 170 — 2026-09-08

- `300-package-override` passes interpreter, JIT expected-output, and
  unchanged-source optimized lanes on the current runtime, extending
  application package/class-loader override coverage.

### Runtime checkpoint 171 — 2026-09-08

- Fresh four-worker corpus execution over the executable contracts in the
  `301`–`500` range completed 54/54 passes across interpreter, JIT, and
  unchanged-source optimized lanes. Coverage includes optimizing compiler
  control flow/arithmetic, fields/arrays, exceptions/monitors, register
  allocation, inlining, and deoptimization.

### Runtime checkpoint 172 — 2026-09-08

- Fresh four-worker corpus execution over the executable contracts in the
  `501`–`700` range completed 40/40 passes across interpreter, JIT, and
  unchanged-source optimized lanes. Coverage includes checker optimizations,
  deoptimization/OSR, inline caches, volatile/read barriers, class loading,
  and JNI stubs.

### Runtime checkpoint 174 — 2026-09-08

- Removed stale parallel corpus runners that were contaminating later ledgers.
  A direct fresh run of `904-object-allocation` now passes interpreter, JIT,
  and unchanged-source optimized lanes, confirming the runner can produce
  authoritative results after cleanup.

### Runtime checkpoint 175 — 2026-09-08

- Fresh four-worker execution of `1000-non-moving-space-stress` through
  `1004-checker-volatile-ref-load` passes all five contracts in interpreter,
  JIT, and unchanged-source optimized lanes. This verifies non-moving GC,
  app-image regions, startup notification, metadata strings, and volatile
  reference loads.

### Runtime checkpoint 176 — 2026-09-08

- Fresh four-worker execution of `1336-short-finalizer-timeout` through
  `1339-dead-reference-safe` passes all four contracts in interpreter, JIT,
  and unchanged-source optimized lanes, covering finalizer timing, GC
  coverage/no-LOS behavior, and dead-reference safety.

### Runtime checkpoint 177 — 2026-09-08

- Fresh four-worker execution of the eight discovered contracts in
  `1900`–`1919` passes interpreter, JIT, and unchanged-source optimized lanes,
  covering JVMTI allocation tracking, bytecode/local-variable access,
  suspend/resume, and thread-start timing.

### Runtime checkpoint 178 — 2026-09-08

- Fresh four-worker corpus runs pass 13 contracts in `2000`–`2048` and 14 in
  `2230`–`2286`, across interpreter, JIT, and unchanged-source optimized
  lanes. Coverage includes structural redefinition and multithreaded stack
  scope, inlining/loop optimization, reference processing, checker lowering,
  and native-registry validation.

### Runtime checkpoint 179 — 2026-09-08

- Direct fresh execution of the previously stale `004-JniTest` contract passes
  interpreter, JIT, and unchanged-source optimized lanes. This confirms the
  compiled JNI transition and native exit path after runner isolation cleanup.

### Runtime checkpoint 180 — 2026-09-08

- Fresh four-worker corpus runs pass 6 contracts in `1920`–`1960` and 8 in
  `1961`–`1999` across interpreter, JIT, and unchanged-source optimized lanes.
  Coverage includes monitor/event JVMTI, frame-pop and breakpoint/redefine
  behavior, checker bounds/loop vectorization, and structural transformation.

### Runtime checkpoint 181 — 2026-09-08

- Fixed generic native-owner discovery in `run-art-upstream-test.py`: when an
  AOSP test declares JNI methods implemented by multiple sibling
  `libarttest` members, all matching owners are linked instead of selecting
  one arbitrarily. `2262-default-conflict-methods` now passes interpreter,
  JIT, and unchanged-source optimized lanes.

### Runtime checkpoint 182 — 2026-09-08

- Re-ran the current `2230`–`2286` corpus slice after the generic sibling JNI
  owner fix. All 12 discovered contracts pass interpreter, JIT, and
  unchanged-source optimized lanes, including `2262-default-conflict-methods`.

### Runtime checkpoint 183 — 2026-09-08

- Fresh four-worker execution of the previously omitted structural-redefinition
  contracts `2001`–`2007` passes all seven interpreter, JIT, and
  unchanged-source optimized lanes, covering multithreaded virtual dispatch,
  initialization/finalization, and pause-all coordination.

### Runtime checkpoint 184 — 2026-09-08

- Fresh corpus runs pass 5 contracts in `2008`–`2012` and 22 in
  `2019`–`2039` across interpreter, JIT, and unchanged-source optimized
  lanes. Coverage includes structural local references/stack walking/JNI-ID
  failures, inlining and loop optimization, monitor/shutdown behavior, and
  large native allocation/transform paths.

### Runtime checkpoint 173 — 2026-09-08

- Fresh four-worker corpus reruns completed 25/25 executable contracts in
  `701`–`736` and 23/23 in `800`–`860`. All interpreter, JIT, and
  unchanged-source optimized lanes pass, covering VarHandle creation and
  concurrency, JIT cache churn, OSR/deopt, hidden API/VDEX, method resolution,
  and plugin/JVMTI paths.

### Runtime checkpoint 185 — 2026-09-08

- Fresh four-worker execution of `2040`–`2048` passes all eight contracts in
  interpreter, JIT, and unchanged-source optimized lanes. Coverage includes
  huge native allocation, cleaner/reference processing, stack traces,
  userfaultfd handling, checker lowering, and native-registry validation.

### Runtime checkpoint 186 — 2026-09-08

- Fresh reruns of the `114`–`175` slice pass every contract except one
  known-flaky `149-suspend-all-stress` attempt; a subsequent isolated rerun
  passes interpreter, JIT, and unchanged-source optimized lanes. The slice
  validates GC/class loading, native bridge, class unloading, multi-loader
  registration, lock ownership, and allocation stress without a persistent
  failure.

### Runtime checkpoint 187 — 2026-09-08

- Fresh four-worker execution of the `1948`–`2039` slice passes all 90
  contracts across interpreter, JIT, and unchanged-source optimized lanes.
  Structural redefinition/obsolescence, JVMTI transforms, inlining and loop
  optimization, monitor/deoptimization, hidden API, and JNI file-channel
  paths all pass; stale audit failures in this slice do not reproduce.

### Runtime checkpoint 188 — 2026-09-08

- Isolated reruns of `542-unresolved-access-check`, `936-search-onload`, and
  `938-load-transform-bcp` pass interpreter, JIT, and unchanged-source
  optimized lanes. Access-check resolution, class-loader on-load search, and
  boot-classpath transformation now have fresh evidence and no persistent
  failure.

### Runtime checkpoint 189 — 2026-09-08

- Fresh four-worker execution of the `061`–`103` slice passes all 51
  contracts in interpreter, JIT, and unchanged-source optimized lanes.
  OOM/GC, fields and arrays, class loading, verifier/monitor behavior, loops,
  reflection, and string concatenation paths all pass.

### Runtime checkpoint 190 — 2026-09-08

- Fresh four-worker execution of `176-app-image-string` through
  `183-rmw-stress-test` passes all eight contracts in interpreter, JIT, and
  unchanged-source optimized lanes. App-image strings/native methods,
  nonvirtual JNI, default-method linking, deadlock handling, and read-modify-
  write stress are covered.

### Runtime checkpoint 191 — 2026-09-08

- Fresh four-worker execution of `1900`–`1947` passes all 46 contracts in
  interpreter, JIT, and unchanged-source optimized lanes. Suspend/resume and
  raw monitors, frame-pop and exception events, JVMTI transforms, proxy frames,
  and breakpoint/deoptimization paths all pass.

### Runtime checkpoint 192 — 2026-09-08

- Fresh four-worker execution of `2230`–`2286` passes all 85 contracts in
  interpreter, JIT, and unchanged-source optimized lanes. Checker lowering,
  VarHandle/Unsafe, write-barrier elimination, exception/inlining,
  method-handle, JVMTI, and sibling-JNI owner paths are covered; the transient
  `2262-default-conflict-methods` ledger failure resolves to PASS on completion.

### Runtime checkpoint 193 — 2026-09-08

- Fresh four-worker execution of `300-package-override` through
  `500-instanceof` passes 128 of 129 contracts across all three lanes.
  `497-inlining-and-class-loader` remains a real gap: the custom loader's
  `DexFile.loadClassBinaryName` returns null for `LoadedByMyClassLoader`,
  producing the observed `foo.getDeclaredMethod` NPE. The failure is preserved
  as an actionable class-loader implementation item rather than masked.

### Runtime checkpoint 194 — 2026-09-08

- DEX identity tracing confirms `497-inlining-and-class-loader` reuses the
  process PathClassLoader's dex object in a child loader, which
  `RegisterDexFile` rejects before `loadClassBinaryName` can return a class.
  A temporary existing-cache reuse experiment failed at the later
  `DefineClass` registration and was reverted; this remains an explicit
  class-loader implementation gap.

### Runtime checkpoint 195 — 2026-09-08

- Rebuilt the graphics runtime with an experimental generic multi-loader
  DexCache reuse path and reran `497-inlining-and-class-loader`; it still
  returns null from `loadClassBinaryName` because `DefineClass` performs a
  second registration that rejects the same DexFile identity. The experiment
  was fully reverted, leaving the runtime behavior unchanged and the needed
  fix narrowed to per-(DexFile, ClassLoader) cache identity rather than a
  test-specific workaround.

### Runtime checkpoint 196 — 2026-09-08

- A second experiment allowing `RegisterDexFile` to share the existing cache
  across loaders also failed to resolve `497-inlining-and-class-loader` and
  was reverted. The failure confirms that a real per-loader DexFile/cache
  clone with independent registration and lifetime is required; no global
  registration relaxation or test-specific gate is retained.

### Runtime checkpoint 197 — 2026-09-08

- Source audit confirms `ClassLinker::dex_caches_` is keyed only by
  `DexFile*`; both `DexFile_defineClassNative` and `DefineClass` therefore
  re-enter the single-loader registration path. The failed experiments show
  that correctness requires a loader-aware DexFile clone/cache with explicit
  ownership and GC cleanup, not a relaxed global registration check. No unsafe
  lifetime change was merged in this checkpoint.

### Runtime checkpoint 198 — 2026-09-08

- Prototyped a zero-copy `DexFile` clone in `DexFile_defineClassNative` for
  child-loader registration. The clone path applied to the full runtime source,
  but graphics bootstrap also patches a reduced source variant that does not
  contain `dalvik_system_DexFile.cc`; the patch therefore made the bootstrap
  fail with `No file to patch`. The prototype and manifest entry were removed.
  The class-loader gap remains open until a variant-aware ownership design is
  implemented and validated without weakening AOSP registration checks.

### Runtime checkpoint 199 — 2026-09-08

- Fresh four-worker execution of the `501-null-constant-dce` through
  `550-new-instance-clinit` corpus slice passes all 79 discovered contracts,
  covering checker lowering, deoptimization, loops/try-catch, monitor exit,
  array/field operations, inlining, tracing, and JIT regressions. The isolated
  `497` multi-loader identity failure remains the next runtime gap.

### Runtime checkpoint 200 — 2026-09-08

- Investigated concurrent bootstrap failures around the shared patched-source
  shadow. A staging-lock prototype exposed a pre-existing incomplete-shadow
  recovery issue (`quick_entrypoints.h` is generated outside the copied source
  list), so it was reverted without changing runtime behavior. The normal
  graphics bootstrap remains intact; shadow publication must be made atomic
  together with generated-header materialization before retrying the loader
  clone implementation.

### Runtime checkpoint 201 — 2026-09-08

- Added the upstream `entrypoints/quick/quick_entrypoints.h` header to the
  shared runtime shadow source manifest. A clean shadow regeneration now
  materializes the header and `build-runtime-graphics-bootstrap-internal`
  completes with 256 objects reused; this removes the previously observed
  incomplete-shadow failure without changing ART execution semantics.

### Runtime checkpoint 202 — 2026-09-08

- The loader clone retry confirmed that `ArtDexFileLoader::Open` is an
  instance API over mapped bytes with an explicit container argument. The
  experimental diff remained unsafe and was removed; clean graphics bootstrap
  passes again while `497-inlining-and-class-loader` remains open.

### Runtime checkpoint 203 — 2026-09-08

- Canonical diff generation confirmed the mapped-byte `ArtDexFileLoader`
  instance API for the child-loader clone. It remains unmerged pending safe
  patch publication; no partial runtime semantics are retained.

### Runtime checkpoint 204 — 2026-09-08

- Retried the canonical clone patch with the corrected loader API. The shared
  shadow is prepared by multiple bootstrap owners, and the patch still fails
  when one owner observes a partially applied hunk; the experiment was removed
  again. Clean internal graphics bootstrap passes, and the loader gap remains
  isolated for a future atomic-source publication change.

### Runtime checkpoint 205 — 2026-09-08

- Added an atomic directory lock around shared patched-source publication and
  a post-lock completeness recheck. Two concurrent clean
  `build-runtime-graphics-bootstrap-internal` invocations now both exit 0,
  eliminating partial-hunk races while preserving the fail-closed patch flow.

### Runtime checkpoint 206 — 2026-09-08

- Revalidated the committed shadow publication lock after a clean regeneration:
  two simultaneous internal graphics bootstrap owners both completed with exit
  0 and the complete generated-header set. This is now a stable prerequisite
  for retrying loader-aware DexCache changes; `497` remains the only known
  class-loader failure.

### Runtime checkpoint 207 — 2026-09-08

- The mapped-byte clone prototype compiled with serialized shadow publication,
  but `497` still produced the same NPE. The unverified global clone lifetime
  path was removed; clean graphics bootstrap passes. The remaining gap is the
  ClassLinker class-definition/cache association.

### Runtime checkpoint 208 — 2026-09-08

- Worktree audit confirms no clone patch is tracked; the latest `497` NPE is
  baseline behavior, not evidence against the clone. The next run must retain
  the canonical patch through build and identity-traced execution.

### Runtime checkpoint 209 — 2026-09-08

- A canonical clone patch retry still failed parsing on a secondary hunk and
  was removed before commit. Clean internal graphics bootstrap remains PASS;
  the next attempt should publish a mechanically generated staged-source diff.

### Runtime checkpoint 210 — 2026-09-08

- The mechanically generated child-loader clone patch now applies and links in
  the graphics runtime. Identity tracing shows a distinct clone DexFile being
  registered for the child loader; a representative `501-null-constant-dce`
  test passes in interpreter, JIT, and optimized lanes. `497` still fails
  after registration, so class-definition association remains open.

### Runtime checkpoint 211 — 2026-09-08

- Fresh-machine verification reproduces successful distinct child-loader
  DexFile registration but the same `497` null result after `DefineClass`.
  The remaining defect is narrowed to post-registration class association;
  temporary diagnostic logging was discarded.

### Runtime checkpoint 213 — 2026-09-08

- Root cause fixed: the clone was registered but `DexFile_defineClassNative`
  still passed the original DexFile into `ClassLinker::DefineClass`. The
  canonical patch now passes `defining_dex`/clone, and fresh relink plus
  `497-inlining-and-class-loader` passes interpreter, JIT, and optimized lanes.

### Runtime checkpoint 214 — 2026-09-08

- After the child-loader fix, the first ten deterministic corpus inputs
  (`000-nop` through `004-SignalTest`) all passed through the normal runner.
  This provides a broader regression signal beyond the focused 497/501 tests;
  the full corpus and real-app validation remain outstanding.

### Runtime checkpoint 216 — 2026-09-08

- Corpus coverage now includes `025-access-controller` through `054-uncaught`;
  all 30 tests passed. This exercises access checks, field/array writes,
  class initialization deadlock, constructors/inheritance, reflection and
  proxy, enum/finalizer behavior, monitor synchronization, threads/wait/join,
  verifier behavior, and uncaught exceptions. Remaining corpus and real-app
  validation are still pending.

### Runtime checkpoint 217 — 2026-09-08

- The `055-enum-performance`–`073-mismatched-field` slice passed completely
  (21 tests). It covers enum/string-jumbo/math intrinsics, OOM and finalizer
  paths, character encodings, process management, field/type checks,
  classloaders, NIO/DexFile mapping, precise GC/reachability fences, and
  unpark behavior. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 215 — 2026-09-08

- Expanded the post-loader regression slice through `024-illegal-access`.
  All 25 selected tests passed, covering exceptions, instanceof/arrays,
  arithmetic and floating point, strings/interning, interfaces, unsafe
  operations, stack overflow, and thread stress. Full corpus and real-app
  validation remain outstanding.

### Runtime checkpoint 212 — 2026-09-08

- A one-shot callback trace confirmed that `LLoadedByMyClassLoader` never
  reaches `ClassPreDefine`: the original DexFile registration fails, the clone
  registers, and execution then returns to Java with status 122. This narrows
  the defect further to an earlier `DefineClass` precondition/exception path,
  rather than callback substitution. The temporary trace was removed.

### Runtime checkpoint 218 — 2026-09-08

- The `074-gc-thrash`–`096-array-copy-concurrent-gc` slice passed all 24 tests,
  covering verification/type behavior, polymorphic calls, phantom references,
  OOM fragmentation/finalizers, hot exceptions, inlining/compiler regressions,
  class initialization/monitors, loop formation, serialization, and
  concurrent-GC array copies. Remaining corpus and real-app validation remain
  pending.

### Runtime checkpoint 237 — 2026-09-08

- The `538-checker-embed-constants`–`561-shared-slowpaths` slice passed all
  37 tests. Coverage includes embedded constants, inlined deopt, bitfield
  rotates, try/catch and catch simplification, unresolved access checks,
  tracing/JIT, DCE/env-long references, inlining/type merges, multiply-
  accumulate and wide stores, new-instance/clinit, shifter/implicit-null and
  invoke-super variants, primitive type propagation/sharpening, AVX2 bit
  manipulation, checkcast, UnsafeGetLong, rotate simplification, equivalent
  refs, switch/packed switch, BCE SSA/irreducible loops, divrem, and shared
  slow paths. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 223 — 2026-09-08

- Fixed the child-loader policy without weakening AOSP semantics: before
  cloning, the runtime performs an exact parent `LookupClass`; classes already
  defined by the parent recreate the required multiple-loader `InternalError`,
  while genuinely new child classes use the zero-copy clone. After relink,
  both `156-register-dex-file-multi-loader` and `497-inlining-and-class-loader`
  pass interpreter, JIT, and optimized lanes. Unsafe boot support is published
  as a JAR container to satisfy bootclasspath resource handling.

### Runtime checkpoint 219 — 2026-09-08

- The next corpus run initially stopped because 1,834 retained temporary test
  directories exhausted the filesystem, not because of a runtime assertion.
  After removing only those generated temp directories, `1001-app-image-regions`
  was rerun independently and passed interpreter, JIT, and optimized lanes.
  The preceding completed results (`097-duplicate-method`, `100-reflect2`,
  `1000-non-moving-space-stress`) also passed; the remaining range is queued.

### Runtime checkpoint 222 — 2026-09-08

- Rebuilt the generated unsafe boot support as a JAR container (Android
  bootclasspath contract) and updated runner/audit paths. `149-suspend-all-stress`
  passes all three lanes after regeneration, and `497-inlining-and-class-loader`
  remains fully passing. `156-register-dex-file-multi-loader` still exposes a
  semantic gap: clone registration intentionally permits a case where AOSP
  expects InternalError; this requires a ClassLinker-level policy fix.

### Runtime checkpoint 220 — 2026-09-08

- Resumed corpus execution from `1002-notify-startup` through
  `125-gc-and-classloading`; all 28 tests passed. Coverage includes startup
  metadata/volatile loads, Fibonacci and concurrent GC, string append/growth,
  invoke/exception/check-cast/suspend-check, fields, multidex, ParallelGC,
  native bridge, dex2oat/no-image flows, hash/modifier/NPE behavior,
  multi-thread compiler regressions, missing classes, and class loading.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 253 — 2026-09-08

- The `952-invoke-custom`–`976-conflict-no-methods` slice passed all 25 tests,
  covering invoke-custom/polymorphic compiler and verifier paths, MethodHandle
  smali/transforms/stack frames/accessors, default-interface resolution and
  initialization, static/range/default verification and conflicts, interface
  super resolution, IMT collisions, multidex defaults, private interfaces,
  and no-method conflicts. Remaining corpus and real-app validation are
  pending.

### Runtime checkpoint 252 — 2026-09-08

- The `926-multi-obsolescence`–`951-threaded-obsolete` slice passed all 26
  tests, covering multi-obsolescence and timers, JNI tables/search,
  retransformation and agent threads, transform-save/load/onload and BCP
  variants, recursive obsolete/JIT methods, classloader transforms, obsolete
  native/throw paths, reflection and annotation changes, in-memory transforms,
  intrinsic redefinition, and threaded obsolete handling. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 254 — 2026-09-08

- A full `--resume` corpus reconciliation was started after the targeted
  slices. The ledger lacked terminal records for some earlier tests, so the
  runner is re-executing them rather than assuming prior evidence; the live
  sweep has reached the 030–075 range and remains active. The `9999` smoke
  test is separately classified as a compile-stub gap (`android.view.InputChannel`
  absent), not a runtime result. Remaining corpus and real-app validation are
  pending.

### Runtime checkpoint 251 — 2026-09-08

- The `900-hello-plugin`–`925-threadgroups` slice passed all 26 tests,
  covering JVMTI plugins/agents, transformation and tagging, object
  allocation/free, heap iteration and GC start/finish, loaded classes,
  attach-agent and method/stack/class/heap inspection, obsolete methods and
  obsolete JIT, field/object access and transformations, properties/failure,
  monitors, threads, and thread groups. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 279 — 2026-09-08

- On the replacement Mac, rebuilt the pinned Android 16 HWUI static foundation
  and graphics bootstrap successfully. Darwin now explicitly clears HWUI's
  `pixelFormatFloat` capability because the Metal/IOSurface backend is
  RGBA_8888-only; a fresh Snapseed launch completed with the prior wide-gamut
  error removed. The optional 101010-2 probe warning remains unsupported work.

### Runtime checkpoint 262 — 2026-09-08

- Full AOSP corpus is green at 1,075/1,075 after the InputChannel framework
  stub and serialized boot-artifact build lock. Calculator and DeskClock
  real-APK graphics/input acceptance pass through HWUI, SurfaceFlinger, and
  Metal publication.
- Snapseed installs and resolves its arm64 ELF library, but its unchanged
  managed `System.loadLibrary`/JNI path still fails to bind
  `NativeCore.verifyLibraryHasBeenLoadedProperly`. Runtime.nativeLoad resolver
  coverage was added; tracing why this call does not reach JavaVMExt is next.

### Runtime checkpoint 259 — 2026-09-08

- Rebuilt the graphics runtime after the debuggable-APK Nterp policy fix and
  validated unchanged AOSP APKs end-to-end. `aosp-core-apps-graphics-acceptance.sh`
  passes Calculator physical-click `2+3=5` and DeskClock Timer-tab navigation,
  with HWUI + SurfaceFlinger + Metal buffer publication and no fatal crash.
  Real Blue Archive validation remains pending.

### Runtime checkpoint 258 — 2026-09-08

- Added the hidden `android.view.InputChannel` framework stub with paired
  ParcelFileDescriptor endpoints, shared Binder token identity, Parcelable
  round-trip, and dispose semantics. It is compiled into the support DEX and
  supplied as a hidden compiler input. The formerly failing
  `9999-input-channel-endpoint-parcel-smoke` now passes interpreter, JIT, and
  optimized lanes; the authoritative corpus ledger is 1,075/1,075 passed.
  Real-app validation remains pending.

### Runtime checkpoint 249 — 2026-09-08

- The `800-smali`–`837-deopt` slice passed all 40 tests, covering smali and
  MethodHandle/method-resolution paths, deoptimization, malformed and deep
  class hierarchies, invoke-super variants, recursive defaults, FP arguments,
  large field offsets, invokeinterface defaults, illegal arrays, clinit
  nterp, runtime verification/rethrow, vdex multidex, madvise, many args,
  future hidden API, CHA inlining/recursion, unbalanced locks, infinite loops,
  partial/full LSE, unresolved enclosing/field access, unverified boot class
  paths, background verification, large class counts, and deopt. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 257 — 2026-09-08

- Lock-enabled rerun confirms the prior shared boot-artifact race is resolved:
  `149-suspend-all-stress` and `156-register-dex-file-multi-loader` both pass
  again under the corrected runner. Current authoritative ledger is 1,074
  passed of 1,075 discovered tests; only the custom InputChannel smoke test
  lacks its compile-time framework stub. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 280 — 2026-09-08

- Rebuilt the HWUI static foundation and graphics bootstrap on the replacement
  Mac after adding the Darwin unsupported 10-bit/FP16 capability boundary.
  AOSP Calculator/DeskClock graphics acceptance and a fresh Snapseed launch
  remain passing. The 101010-2 diagnostic is now explicitly gated in the
  pinned foundation; the separate JNI graphics path still requires follow-up.

### Runtime checkpoint 281 — 2026-09-08

- Forced the incremental graphics link to consume the updated HWUI archive.
  Fresh Snapseed execution now shows no wide-gamut or 101010-2 EGL warnings;
  the Activity still launches successfully. Core AOSP graphics acceptance
  remains passing. Full JIT compatibility work is still in progress.

### Runtime checkpoint 282 — 2026-09-08

- Full `tools/audit-art-jit.sh` completed with exit code 0 on the replacement
  Mac. The run exercised JIT arithmetic, typed fields/arrays, VarHandle memory
  ordering, invoke-polymorphic/custom, GC/deoptimization, native exit hooks,
  and concurrency fixtures; all reported PASS. This is validation progress,
  not completion of the full AOSP/Blue Archive objective.

### Runtime checkpoint 283 — 2026-09-08

- Reconciled the authoritative `_build/art-upstream-corpus/summary.json`:
  1,075 results, with `passed=1,075` and no non-passed records. This closes
  the prior InputChannel ledger discrepancy; real Blue Archive execution and
  broader production-app validation remain outstanding.

### Runtime checkpoint 284 — 2026-09-08

- Launched the installed, unmodified Blue Archive package through the normal
  APK runner for 10 seconds with the default JIT path. Exit code was 0; Unity
  reached the IL2CPP application stage and reported ARM64, 12 cores, 8192 MB
  memory, and application version `1.93.454564`. Sustained gameplay/input and
  service validation remain active.

### Runtime checkpoint 285 — 2026-09-08

- Unmodified Blue Archive ran through the normal installed-record launcher for
  a 30-second soak with exit code 0. Unity/IL2CPP remained alive and reported
  ARM64/12-core runtime state. The run exposed a non-fatal existing-install
  issue: a legacy package record can bypass installer `oat/arm64` migration,
  producing an anonymous-vdex directory warning. This is the next packaging
  lifecycle fix; it did not prevent startup or rendering initialization.

### Runtime checkpoint 286 — 2026-09-08

- Added an installer-owned `--ensure-oat` preflight for installed-record
  launches. It reuses the permission-safe migration routine without modifying
  APK/native payloads, closing the legacy record path that skipped writable
  `oat/arm64`. Formatting and diff checks pass; a clean end-to-end rerun is
  pending recovery from stale host processes.

### Runtime checkpoint 287 — 2026-09-08

- Re-ran the installed-record Blue Archive path after system-server recovery.
  The new `--ensure-oat` preflight created `oat/arm64/base.vdex`; the 10-second
  unchanged APK run exited 0, initialized Unity/IL2CPP, and emitted no
  anonymous-vdex directory error. APK/native payload permissions remain sealed.

### Runtime checkpoint 288 — 2026-09-08

- Replacement-computer smoke check: repository is at `810e0d2`, release binaries
  are present, and `git diff --check` passes. The graphics acceptance runner
  starts but cannot create its log because only about 139 MiB is free; no code
  failure was observed.

### Runtime checkpoint 289 — 2026-09-08

- Environment audit found the checkout intact and `cargo metadata` passing.
  Re-running graphics acceptance is currently storage-blocked: `_build` is
  26G and the DarwinART profile store is 126G while the volume has only about
  136MiB free. No cleanup was performed without explicit authorization.

### Runtime checkpoint 290 — 2026-09-08

- The authoritative `_build/art-upstream-corpus/summary.json` was re-read:
  all 1,075 entries have exit code 0 and empty errors. The corpus ledger is
  green; real-app coverage and storage recovery remain open.

### Runtime checkpoint 291 — 2026-09-08

- Storage triage identified generated `_build/android16-ps16k-r07/system.img`
  as the only top-level file larger than 1G; `_build` totals 26G, `target`
  2.5G, and the DarwinART profile store 126G. It was not deleted because it is
  a runtime artifact and no cleanup authorization was provided.

### Runtime checkpoint 293 — 2026-09-08

- User-authorized stale-profile cleanup reclaimed about 3.5GiB while preserving
  the active `default` profile and runtime images. On the replacement computer,
  `aosp-core-apps-graphics-acceptance.sh` passed (Calculator `2+3=5`, DeskClock
  timer, HWUI/SurfaceFlinger/Metal), `audit-art-jit.sh` exited 0, and the
  installed Blue Archive record ran for 10 seconds with exit 0 and Unity/IL2CPP
  initialization (ARM64, 12 cores, 8GiB) and no bootstrap/vdex failure.

### Runtime checkpoint 295 — 2026-09-08

- Root-cause fix: `run-android-apk-app.sh` now prunes only `mnt/run/app.*`
  directories older than 24 hours, reopening their sealed permissions before
  removal. This prevents killed hosts from accumulating copied system roots;
  `bash -n` and `git diff --check` pass.

### Runtime checkpoint 296 — 2026-09-08

- Corrected storage interpretation: the earlier 123G profile figure double
  counted the 61G sparsebundle and its mounted view. After stale-root cleanup,
  the sparsebundle is 61G and `mnt/run` was reduced from 58G to 463M; Git
  objects are only about 122MiB total (5.81MiB packed).

### Runtime checkpoint 297 — 2026-09-08

- After stale-root cleanup, an attempted sparsebundle compaction left the
  `default` APFS image unmountable. `diskutil verifyVolume` reports corrupted
  fsroot and extent-ref trees (exit 8), and `diskutil repairVolume` could not
  complete deferred repairs. No reformat or further destructive write was
  attempted; source/Git remain intact while profile recovery is pending.

### Runtime checkpoint 298 — 2026-09-08

- A non-destructive read-only mount attempt also failed (`mount_apfs` exit 65/66)
  after `hdiutil attach -nomount`; the corrupt image was detached again. No
  profile recreation or overwrite has been performed.

### Runtime checkpoint 299 — 2026-09-08

- Root cause evidence: the sparsebundle compaction attempt failed with
  `hdiutil: ... 메모리를 할당할 수 없음`; subsequent APFS verification found
  zeroed fsroot/extent-ref blocks. A fresh `recovery` profile was created
  without touching the corrupt image; Calculator was installed and launched
  for 5 seconds with exit 0 and Nterp acceptance PASS.

### Runtime checkpoint 300 — 2026-09-08

- Recovery profile validation: `recovery` mounted with one lease; Calculator and
  Chromium were installed from immutable APK sources. Chromium launched for 8
  seconds with exit 0, created its initial tab, spawned sandbox/privileged
  services, and used the GPU IOSurface path. Only nonfatal cache/model warnings
  appeared.

### Runtime checkpoint 302 — 2026-09-08

- VLC follow-up after rebuilding framework-compat: the API-29
  `ConnectivityManager.requestNetwork` overloads (Handler/timeout/Executor)
  are now present. VLC's 8-second recovery-profile run exited 0, retained four
  native ELF libraries, registered LibVLC JNI classes, and no longer emitted
  the prior `NoSuchMethodError`.

### Runtime checkpoint 303 — 2026-09-08

- VLC recovery run after the API fix completed with exit 0 and native/JNI
  initialization. The previous `ConnectivityManager.requestNetwork` linkage
  error is absent; remaining output is limited to app-level/nonfatal startup
  diagnostics. This is an app-compatibility increment, not full VLC playback
  validation yet.

### Runtime checkpoint 304 — 2026-09-08

- VLC surface acceptance passed on `recovery`: software Canvas lock/post path
  produced 3 locks and 3 posts with no nativeLockCanvas or fatal-signal errors.
  This validates the media app's Android window buffer bridge; an actual media
  file playback assertion remains separate.

### Runtime checkpoint 305 — 2026-09-08

- Recovery profile inventory contains no existing media fixture. The VLC
  surface/canvas acceptance remains green, but an actual decode/playback
  assertion is intentionally not claimed until a media file is supplied or
  generated through the test harness.

### Runtime checkpoint 306 — 2026-09-08

- Generated a small H.264/AAC fixture and placed it in VLC's recovery-profile
  external directory. VLC launched with exit 0, but the headless launcher did
  not emit a playback/decoder event; only startup and surface assertions remain
  proven. The fixture is retained for a future physical-file-picker/intent
  test, and no APK was modified.

### Runtime checkpoint 301 — 2026-09-08

- Recovery profile app coverage expanded without APK changes: Snapseed
  (native `libsnapseed_native.so`) installed atomically and launched for 8
  seconds with exit 0; ELF namespace retention, PathClassLoader native path,
  JNI registration, and Activity transition all succeeded.

### Runtime checkpoint 294 — 2026-09-08

- Storage accounting after cleanup: active DarwinART `default` profile is 123G,
  repository `_build` is 26G, and Cargo `target` is 2.5G. These project assets
  explain roughly 151.5G of usage; APFS reports no snapshots, so the remaining
  volume usage belongs to other host data outside this checkout.

### Runtime checkpoint 292 — 2026-09-08

- Profile storage inspection shows `default` is the only profile modified on
  2026-09-08 (about 126G, including `mnt`); `productcompare*`, `bundle*`,
  `goaltest`, `producttest`, and `managerui` are dated 2026-08-29/30. These
  stale profiles are candidate cleanup targets, but remain untouched pending
  authorization.

### Runtime checkpoint 277 — 2026-09-08

- ANGLE EGL capability advertisement now explicitly marks FP16 pixel-format
  support unavailable for the RGBA_8888-only IOSurface path. Snapseed still
  launches successfully; the remaining wide-gamut log indicates another
  extension source is advertising the capability and needs follow-up tracing.

### Runtime checkpoint 276 — 2026-09-08

- Existing sealed installations are migrated lazily: the installer temporarily
  opens only the package directory, creates writable `oat/arm64`, and restores
  its sealed mode. A normal Snapseed relaunch now has no VDEX directory error,
  while EditActivity and JNI initialization remain successful.

### Runtime checkpoint 275 — 2026-09-08

- Post-install-cache regression passes: `cargo fmt --all -- --check` and the
  Calculator/DeskClock real-APK acceptance both pass after provisioning the
  writable `oat/arm64` directory.

### Runtime checkpoint 274 — 2026-09-08

- Installer now provisions a writable Android-style `oat/arm64` code-cache
  leaf while keeping APK/native payloads sealed. Fresh Snapseed installation
  writes `oat/arm64/base.vdex` successfully; the prior VDEX directory error is
  gone and EditActivity still launches with exit code 0.

### Runtime checkpoint 273 — 2026-09-08

- Regression sweep after ELF page/lifecycle changes: the authoritative corpus
  ledger contains 1,075 results, all `passed`. Real Calculator and DeskClock
  graphics acceptance remains green (`2+3=5`, Timer interaction, HWUI +
  SurfaceFlinger + Metal).

### Runtime checkpoint 272 — 2026-09-08

- SurfaceControl pixel-capture run confirms the default Snapseed path creates
  and publishes its app surface, reaches EditActivity, registers 309 JNI
  methods, and exits 0. No new diagnostic report was produced.

### Runtime checkpoint 271 — 2026-09-08

- Physical desktop capture after terminating the stale host shows the prior
  macOS crash-report dialog still present, but no new diagnostic report was
  created and a fresh 8-second Snapseed run exits 0 with its edit Activity.
  The dialog is stale UI state, not a current ART crash.

### Runtime checkpoint 270 — 2026-09-08

- A 45-second Snapseed host run reached edit activity with 309 JNI methods and
  produced no ART fatal signal in its log before normal timeout. A stale macOS
  crash-report dialog from an earlier host process was visible during screen
  capture and must be separately reconciled before claiming visual stability.

### Runtime checkpoint 269 — 2026-09-08

- A 20-second default Snapseed run completed with exit code 0 after 309 JNI
  registrations and edit-activity launch. No ART fatal signal or JNI missing
  implementation occurred; only optional GMS/FeatureFlags and EGL/VDEX
  warnings remain.

### Runtime checkpoint 268 — 2026-09-08

- Repeated default Snapseed launch (5 seconds) exits 0 with no JNI or ART
  exceptions; the edit activity remains reachable after managed native load.
  Remaining diagnostics are non-fatal VDEX placement and EGL gamut probes.

### Runtime checkpoint 267 — 2026-09-08

- Default (no environment override) Snapseed run exits 0 after managed native
  loading, with 309 JNI methods registered and the edit activity launched.
  Remaining runtime warnings are non-fatal writable-vdex placement and a
  wide-gamut EGL configuration probe.

### Runtime checkpoint 266 — 2026-09-08

- After rebuilding the Rust bionic provider closure and graphics runtime,
  Snapseed runs with the default managed-loading path (no override variable).
  Its `System.loadLibrary` reaches JavaVMExt/NativeBridge and JNI registers
  309 native methods; the edit activity launches successfully.

### Runtime checkpoint 265 — 2026-09-08

- Fixed Apple Silicon page-size mismatch in borrowed ELF image registration:
  Android guest PT_LOAD ranges use 4 KiB alignment while the host VM reports
  16 KiB pages. Snapseed eager JavaVMExt/NativeBridge loading now passes and
  reaches JNI initialization (`JavaVMExt+NativeBridge load ok`).

### Runtime checkpoint 263 — 2026-09-08

- Current authoritative state: AOSP corpus 1,075/1,075; Calculator and
  DeskClock real APK graphics/input acceptance pass.
- Snapseed remains blocked on managed native loading: installation and ELF
  resolution pass, but `NativeCore.verifyLibraryHasBeenLoadedProperly` is not
  registered. `Runtime.nativeLoad` registration/resolver coverage is present;
  JavaVMExt/NativeBridge lifecycle tracing is the next implementation step.

### Runtime checkpoint 261 — 2026-09-08

- Snapseed's unchanged APK reaches JavaVMExt/NativeBridge and successful arm64
  ELF loading through `System.loadLibrary("snapseed_native")`, but its
  obfuscated `NativeCore.verifyLibraryHasBeenLoadedProperly()` method remains
  unbound. The next fix is JNI_OnLoad/dynamic RegisterNatives or symbol-table
  binding after load; APK extraction is not the failure. Blue Archive remains
  pending.

### Runtime checkpoint 260 — 2026-09-08

- AOSP Calculator/DeskClock pass end-to-end after the framework fixes. An
  unchanged Snapseed arm64 APK installs and resolves its ELF native library,
  but bootstrap stops at obfuscated `NativeCore.verifyLibraryHasBeenLoadedProperly()`
  JNI registration. Native-library/JNI registration is the next compatibility
  gap; Blue Archive validation remains pending.

### Runtime checkpoint 258 — 2026-09-08

- Added the hidden `android.view.InputChannel` framework stub with paired
  ParcelFileDescriptor endpoints, shared Binder token identity, Parcelable
  round-trip, and dispose semantics. It is compiled into the support DEX and
  supplied as a hidden compiler input. The formerly failing endpoint/Parcel
  smoke now passes interpreter, JIT, and optimized lanes; the authoritative
  corpus ledger is 1,075/1,075 passed. Real-app validation remains pending.

### Runtime checkpoint 256 — 2026-09-08

- After terminating the stale pre-lock sweep, the race-sensitive
  `149-suspend-all-stress`–`156-register-dex-file-multi-loader` range was
  rerun with the locked runner and all 8 tests passed (including the two
  previously affected tests). The corpus ledger now reports 1,074 passed of
  1,075 discovered tests; the sole remaining failure is the custom
  `9999-input-channel-endpoint-parcel-smoke` compile-stub gap. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 255 — 2026-09-08

- Full-corpus reconciliation exposed a parallel-build race: workers could
  rebuild shared `unsafe-boot-dex` while another runtime resolved its boot
  class path, yielding a raw `classes.dex` ZipException. The runner now
  serializes the typed bootstrap default action with a cross-process lock
  while retaining parallel test execution. Syntax and diff checks pass; the
  in-flight sweep still uses the prior runner. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 250 — 2026-09-08

- The `838-override`–`860-vdex-failure` slice passed all 24 tests, covering
  override/resolution, clinit/default interfaces, exceptions, data images,
  verification and multidex, arrays/records, branch and inlining paths,
  native/clone behavior, access checks, Unsafe/VarHandle intrinsics, and vdex
  failure handling. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 248 — 2026-09-08

- The `719-varhandle-concurrency`–`736-interface-super-Object` slice passed
  all 19 tests, covering VarHandle concurrency, thread priority, OSR, string
  initialization ranges, invoke-super NPE, IMT object/zygote conflicts, array
  stores, unresolved classes, polymorphic intrinsics, CHA deoptimization,
  super inlining and bounds slow paths, app images, ICCE and duplicate fields,
  condition merging, interface cloning, and interface-super Object behavior.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 232 — 2026-09-08

- The `2276-const-method-type-gc-cleanup`–`414-static-fields` range passed all
  38 tests. Coverage includes method-handle GC/invokeexact/validation,
  nested and inner-loop optimization, AConfig flags, class unload, checker
  throw/null-check/loop regressions, static-field initialization and tracing,
  package/access/float conversion, verification stress, dex v37, and the
  optimizing compiler's control flow, long arithmetic, allocators, fields,
  arrays, move/materialized conditions, floating point, div/rem simplifiers,
  arithmetic, new-array, regalloc, and static fields. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 239 — 2026-09-08

- The `530-checker-instance-of-simplifier`–`537-checker-jump-over-jump` slice
  passed all 28 tests, covering loop/LSE optimization, SIMD and unrolling,
  instanceof/checkcast, debug/deoptimization, array stores/copy, intrinsic
  and constant folding, access checks, and inline/unverified control flow.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 241 — 2026-09-08

- The `577-checker-fp2int`–`599-checker-irreducible-loop` slice passed all 39
  tests, covering FP conversion, BCE/inlining, CRC32/RTP, dispatch and loops,
  profile/app-image handling, class-loader identity, monitor inflation, and
  busy-loop/invoke/new-string deoptimization. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 244 — 2026-09-08

- The `642-fp-callees`–`668-aiobe` slice passed all 44 tests, covering FP
  callees and read barriers, bogus/absolute SIMD and large arraycopy,
  constant conversion, JNI field IDs, catch sinking, unresolved/many-direct
  inline caches, vdex duplicate methods, access thunks, intrinsic deopt,
  periodic and ARM SIMD optimization, JIT clinit/loop deopt, branches,
  annotation lookup, array layouts/stores, SIMD loops/reductions/SAD/zero,
  classloader allocation and oat layout, alias/select generation, odd dex
  sizes, verifier/dex cache, JNI stubs, and bounds/AIOOBE. Remaining corpus
  and real-app validation are pending.

### Runtime checkpoint 243 — 2026-09-08

- The `626-checker-arm64-scratch-register`–`641-iterations` slice passed all
  25 tests, covering ARM64 scratch/veneer pools and volatile loads, const
  class/string linking, unrolling, vdex and duplicate-vdex handling, safecast
  arrays, get-class/char bounds, RTP, wrong static access, throw inlining,
  intrinsic/inline caches, no-line-number codegen, boolean/integer/SIMD
  optimizations, code sinking, arraycopy, irreducible inlining, and loop
  iteration handling. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 242 — 2026-09-08

- The `600-verifier-fails`–`625-checker-licm-regressions` slice passed all 35
  tests, covering verifier and method access failures, deoptimizeable methods,
  instanceof/new-string/error classes, daemon stress, unresolved/interface
  inlining, bounds and arraycopy, JIT/inlining dex caches, CHA dispatch and
  unloading, clinit OOME, induction/current-method checks, BCE and loop
  regressions, string operations, and LICM. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 239 — 2026-09-08

- The `530-checker-instance-of-simplifier`–`537-checker-jump-over-jump` slice
  passed all 28 tests, covering loop/LSE optimization, SIMD and unrolling,
  instanceof/checkcast, debug/deoptimization, array stores/copy, intrinsic
  and constant folding, access checks, and inline/unverified control flow.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 241 — 2026-09-08

- The `577-checker-fp2int`–`599-checker-irreducible-loop` slice passed all 39
  tests, covering floating-point conversion/rounding, BCE, polymorphic and
  unresolved inlining, CRC32, RTP and null-array checks, class errors,
  super/IMT dispatch, infinite/dead loops, primitive conversions and shifts,
  alias and irreducible-loop analysis, string loading, profile saving,
  app-images/class-loader identity, monitor inflation, busy-loop/invoke/new-
  string deoptimization, error classes, and SSA phi dominance. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 247 — 2026-09-08

- The `692-vdex-secondary-loader`–`718-zipfile-finalizer` slice passed all 26
  tests, covering secondary/in-memory vdex eviction, clinit JIT, throw
  simplification and loops, string append/selects, argument registers,
  division/branch offsets and FP/MAC codegen, register conflicts and
  scheduling, invalid profiles and cache churn, VarHandle creation and
  invocation, invoke-custom lambda metafactory, annotation parameters, JLI
  samples, Integer.valueOf, and zipfile finalization. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 240 — 2026-09-08

- The `562-bce-preheader`–`576-polymorphic-inlining` slice passed all 25
  tests, covering BCE preheaders, intermediate-value elimination, fake-string
  and invoke-super handling, bitcount/bitwise simplification, loop and
  irreducible-loop analysis, condition liveness, select codegen, intrinsic
  builders, one-bit and pattern replacement, OSR/locals, array-get and
  checkcast regressions, string-init aliasing, and polymorphic inlining.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 239 — 2026-09-08

- The `530-checker-instance-of-simplifier`–`537-checker-jump-over-jump` slice
  passed all 28 tests, covering loop/catch simplification, LSE fences and
  SIMD, peel/unroll, instanceof/checkcast, debug phi, nonnull array stores,
  BCE deoptimization, deopt/inlining, constant and intrinsic optimization,
  access checks, arraycopy, debuggable mode, and inline/unverified control
  flow. Remaining corpus and real-app validation are pending.

### Runtime checkpoint 226 — 2026-09-08

- The `1949-short-dex-file`–`1965-get-set-local-primitive-no-tables` slice
  passed all 18 tests, covering unprepared transforms, monitor-enter without
  suspend, pop-frame/JIT frame variants, error and transform retry paths,
  bounds/loop compiler checks, obsolete multithread JIT, event delivery,
  dex-classloader insertion, and primitive frame access without tables.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 221 — 2026-09-08

- The `126-miranda-multidex`–`145-alloc-tracking-stress` slice passed all 25
  tests. Coverage includes secondary dex and register-spill paths, thread and
  daemon/JNI shutdown, hprof/CFI, static invoke-super, GC coverage/no-LOS and
  dead-reference safety, duplicate-class checks, register natives, DCE/field
  packing, class unloading/classloaders, string values, static-field SIGQUIT,
  and allocation tracking stress. Remaining corpus and real-app validation
  are pending.

### Runtime checkpoint 236 — 2026-09-08

- The `515-dce-dominator`–`537-checker-jump-over-jump` slice passed all 45
  tests. Coverage includes dominator/dead-move handling, builder fallthrough,
  null-array and bound class loads, equivalent phi, array/field sets,
  monitor-exit/throw regressions, boolean simplification, caller/callee and
  long register allocation, SIMD/split array access, unresolved longs,
  loop/try-catch variants, LSE fences/SIMD, peel-unroll, reference typing,
  debug phi, BCE deoptimization, deopt/inlining, intrinsic/access checks,
  arraycopy, debuggable and unverified inline paths, and jump elimination.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 235 — 2026-09-08

- The `478-checker-inline-noreturn`–`514-shifts` slice passed all 38 tests.
  Coverage includes inliner/recursive calls/interface calls, current-method
  regressions, instance-of/checkcast, class-loader inlining, type propagation,
  BCE/phi array length, packed switches, dead instructions, baseline entry,
  verifier aput, boolean/referrer behavior, checker disassembly, pre-header
  and try/catch, interface clinit, array deopt, shifts, null-check contracts,
  register hints, loop DCE, and implicit-null-check regressions. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 234 — 2026-09-08

- The `450-checker-types`–`478-checker-clinit-check-pruning` slice passed all
  40 tests. It covers register/vreg allocation, type propagation, SSA/GVN,
  array sets, instruction simplification, long/float conversions, dead phis,
  inlining across dex files and nested loops, boolean simplification,
  condition materialization, huge methods, deopt environments/uninitialized
  locals, constructor fences/barriers, clinit inlining, bound types, and
  unreachable/dead-block pruning. Remaining corpus and real-app validation are
  pending.

### Runtime checkpoint 233 — 2026-09-08

- The `416-optimizing-arith-not`–`449-checker-bce-rem` slice passed all 38
  tests, covering const-string/class, long parameters, exceptions/large
  frames, instanceof/conversion/call forms, monitor/bitwise/bounds, SSA and
  register slow paths, type propagation/CMP/GVN, invoke-direct and shifter
  operands, allocation/try-finally, float remainder/shifts, volatile/NPE and
  double swaps, checker inlining/folding/NCE/LICM, multiple returns, and
  bounds-check elimination. Remaining corpus and real-app validation remain
  pending.

### Runtime checkpoint 231 — 2026-09-08

- The `2250-inline-throw-into-try`–`2275-pthread-name` slice passed all 31
  discovered tests. It covers throw/try and irreducible-loop handling, rem and
  devirtualization optimizations, branch/vector/constant folding, RTI and
  code sinking, intrinsic handling, system cleaner/reference paths,
  method tracing and profile inline caches, write-barrier codegen, bitwise GVN,
  empty loops/unsigned arithmetic, method-handle hidden API/caching, class
  self-implementation, nested-loop behavior, and pthread naming. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 230 — 2026-09-08

- The `2230-profile-save-hotness`–`2249-checker-return-try-boundary-exit-in-loop`
  slice passed all 40 discovered tests, covering profile/metrics and heap
  poisoning, suspend-check removal, JdkUnsafe, recursive inlining, the full
  VarHandle matrix, tracing/single-step, LSE operations, checker/try-boundary
  transforms, write-barrier elimination, and smali boundary checks. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 229 — 2026-09-08

- The `2030-long-running-child`–`2048-bad-native-registry` slice passed all 21
  tests, covering child/deopt frames, default/private methods, shutdown,
  native/JNI file channels, hidden API, large transforms/allocations,
  cleaner/reference processing, stack traces, UFFD, checker comparisons and
  string lengths, and native registry errors. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 228 — 2026-09-08

- The `1999-virtual-structural`–`2029-contended-monitors` slice passed all 26
  tests. Coverage includes virtual dispatch/redefinition (abstract,
  initializing, finalizing, multithread), pause-all redefine, old reflective
  fields, structural local refs, built-in exception details, concurrent stack
  walks, constant calculation sinking, thread OOME, invoke-virtual/static
  inlining, invariant-loop variants, memory-couple optimizations,
  multi-backward loops, and contended monitors. Remaining corpus and real-app
  validation are pending.

### Runtime checkpoint 224 — 2026-09-08

- The `182-method-linking`–`1916-get-set-current-frame` slice passed all 19
  tests, covering method/RMW stress, allocation tracking, bytecode access,
  suspend and native-resume variants, suspend-list ordering, agent TLS, JVMTI
  transforms, and local variable/object/frame inspection. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 225 — 2026-09-08

- The `1917-get-stack-frame`–`1948-obsolete-const-method-handle` slice passed
  all 32 tests. This validates JVMTI stack/frame access, thread-start timing,
  suspend/native monitor interactions, owned-monitor and monitor events,
  exception events, signal-thread handling, JIT frame inspection, transforms,
  proxy frames/arguments, DDMS/dispose stress, raw monitor suspend, descriptor
  listing, breakpoint redefine/deoptimization, and obsolete method handles.
  Remaining corpus and real-app validation are pending.

### Runtime checkpoint 227 — 2026-09-08

- The `1966-get-set-local-objects-no-table`–`1998-structural-shadow-field`
  slice passed all 33 tests, covering local/object slots, force returns, JNI
  ID swaps, array resizing, structural/obsolete redefinition across threads,
  monitor and verification failures, retransformation, and final/virtual
  shadow method/field resolution. Remaining corpus and real-app validation
  are pending.

### Runtime checkpoint 238 — 2026-09-08

- The `498-type-propagation`–`529-long-split` slice passed all 38 tests,
  covering type propagation, BCE/phi and array length, instanceof/checkcast,
  null-constant DCE, packed switches, dead instructions, baseline entry,
  verifier/referrer/disassembly paths, try/catch and interface clinit,
  array deopt and shifts, dominator DCE, builder fallthrough, class loading,
  equivalent phis, array/field/monitor and can-throw checks, boolean
  simplification, register allocation, SIMD array access, unresolved calls,
  loop and LSE setup, and long-register splitting. Remaining corpus and
  real-app validation are pending.

### Runtime checkpoint 245 — 2026-09-08

- EOF progress record: the latest 44-test `642-fp-callees`–`668-aiobe` slice
  passed across interpreter/JIT/optimized lanes where applicable. This
  extends validated ARM64 code generation, barriers, JNI, deoptimization,
  class loading, dex/vdex, SIMD, verifier, and bounds behavior. The remaining
  AOSP corpus and real-app validation are still pending.

### Runtime checkpoint 246 — 2026-09-08

- The `669-checker-break`–`692-vdex-inmem-loader` slice passed all 31 tests,
  covering break/throw and NPE paths, hidden API, hotness and vdex
  decompression, unverified methods, proxy JIT first use, field-type
  resolution, FSI/quickening, locks, deopt PCs and catch phis, clinit inline
  invokes, SIMD dot-product/select/shifts, shared libraries, multi-catch and
  zygote JIT deopt, hidden-API proxies, and in-memory vdex loading. Remaining
  corpus and real-app validation are pending.

### Runtime checkpoint 257 — 2026-09-08

- Lock-enabled rerun confirms the prior shared boot-artifact race is resolved:
  `149-suspend-all-stress` and `156-register-dex-file-multi-loader` both pass
  again under the corrected runner. Current authoritative ledger is 1,074
  passed of 1,075 discovered tests; only the custom InputChannel smoke test
  lacks its compile-time framework stub. Remaining corpus and real-app
  validation are pending.
### Runtime checkpoint 307 — 2026-09-08

- Fixed real VLC `ACTION_VIEW` Activity transitions by matching Android's
  lifecycle ordering: `WindowManagerGlobal.addView()` and normal
  `performResume()` now complete before the native graphics install hook.
  Previously `nativeInstallActivity()` raced ViewRootImpl/SurfaceView setup,
  causing `Activity content presentation failed` and SIGABRT for
  `VideoPlayerActivity`. Recovery-profile replay now returns rc=0 with a
  valid ViewRoot/content pair and GPU presentation (`720x1280`).
### Runtime checkpoint 308 — 2026-09-08

- Fixed scoped-storage publication to preserve
  `/storage/emulated/0/Android/data/<package>/files` instead of flattening
  files at the storage root. VLC now resolves the fixture and reaches the
  Android `MediaCodec` path. The next real-app blocker is a null generated
  ARM64 call target during MediaCodec initialization; this is a JIT/runtime
  ABI defect, not a missing APK asset.
### Runtime checkpoint 309 — 2026-09-08

- Reproduced the next VLC boundary with `DARWIN_ART_DEBUG_MEDIA_CODEC=1`:
  `MediaCodecList` native registration and all three capability queries return
  successfully, then a secondary VLC worker faults at `pc=0` before
  `MediaCodec.native_setup`. The same fault occurs with `DARWIN_ART_JIT=0`,
  so it is an ART quick-entry/JNI method-resolution defect rather than the
  Darwin MediaCodec implementation itself. No fallback was added.
### Runtime checkpoint 310 — 2026-09-08

- Reviewed the generated-code fault recorder and rejected dereferencing the
  interrupted stack from the signal handler as unsafe. The null call remains
  intentionally fatal; diagnosis will use an owner-thread-safe ART frame hook.
### Runtime checkpoint 311 — 2026-09-08

- Added the missing capability-owned `libc.so!posix_fallocate` provider used
  by VLC's native media-library dependency. The Rust filesystem facade now
  validates ranges and extends virtual descriptors without shrinking them;
  21 facade tests and the provider-namespace audit pass. VLC no longer stops
  at that resolver gap, but still reproduces the independent null quick-entry
  immediately after MediaCodec capability enumeration.
### Runtime checkpoint 312 — 2026-09-08

- Symbolicated the VLC `pc=0` fault against the loaded ELF: LR lands in
  `libvlc.so`'s `vlc_stream_MemoryNew`, where an indirect vtable call loads a
  null slot (`blr x8`). This corrects the earlier broad classification: the
  immediate fault is an unresolved/ uninitialized native VLC interface after
  MediaCodec capability enumeration, not proof of an ART JIT entry null. The
  ART runtime remains unmasked; native ELF relocation/vtable ownership is the
  next investigation.
### Runtime checkpoint 313 — 2026-09-08

- Disassembly of the latest crash maps LR to `libvlc.so` and identifies the
  exact failing instruction as an indirect call through a null vtable slot in
  `vlc_stream_MemoryNew`. The loader publishes `libvlc.so`, `libvlcjni.so`,
  and `libc++_shared.so`; the next native-loader check must validate constructor
  execution and C++ vtable relocation state rather than changing ART JIT gates.
### Runtime checkpoint 314 — 2026-09-08

- Compared VLC's `DT_NEEDED` set with the published graph. `libvlc.so` needs
  Android `libEGL.so`, `libGLESv2.so`, `libm.so`, `liblog.so`, `libc.so`,
  `libdl.so`, and `libc++_shared.so`; the loader publishes the app images and
  resolves platform owners through the sealed provider namespace. The crash
  remains an internal null `vlc_stream_MemoryNew` vtable slot, so no missing
  APK sibling is being hidden by a permissive fallback.

### Runtime checkpoint 315 — 2026-09-08

- Root cause of the earlier `default` profile damage was isolated to the
  APFS-backed `android-data.sparsebundle`: `hdiutil compact` ran while the
  bundle/filesystem was mounted and actively changing. APFS extent-reference
  and fsroot metadata then became inconsistent. The damage affected the
  profile container, not APK bytes or Git history; the fresh `recovery`
  profile runs the same unmodified APKs. Compaction must require daemon
  quiescence, an unmount, and post-operation verification.

### Runtime checkpoint 316 — 2026-09-08

- Added the always-present Android framework package identity to the package
  manager facade. `getPackageInfo("android", ...)` now returns a system
  `ApplicationInfo` backed by the revision-locked `framework-res.apk` instead
  of querying the app registry and throwing `NameNotFoundException`. This
  matches AOSP's framework-package invariant and removes VLC's startup
  `AccessControl` exception without fabricating a signing certificate.

### Runtime checkpoint 317 — 2026-09-08

- Rebuilt the complete application DEX after the framework-package change;
  AOSP DEX verification passed. The ARM64 intrinsic inventory audit also
  passes (`specialized-hir=36`, `hinvoke=217`, `handwritten=187`) with no
  contract drift. Full differential and real-app validation remain open.

### Runtime checkpoint 318 — 2026-09-08

- Stabilized the ARM64 ELF loader validation: TLS rewrite tests now derive the
  synthetic guard page from the tested instruction, eliminating allocator/page
  boundary flakiness under parallel execution. Tightened SHN_ABS handling to
  accept only named linker markers (`__bss_start`, `_edata`, `_end`, etc.) and
  reject arbitrary absolute definitions. Loader unit tests and the complete
  constructor/namespace/FFI gate pass.

### Runtime checkpoint 319 — 2026-09-08

- Strict custom-contract audit passes across the pinned AOSP ART test corpus:
  419 directories and 491 sources, with `unsupported=0`, `opaque_shell=0`,
  `unowned_source_bytes=0`, and `omitted_directories=0`. This validates the
  source-level execution contract, while native runtime differential and
  real-app completion criteria remain open.

### Runtime checkpoint 320 — 2026-09-08

- Runtime bootstrap unit tests pass (14/14), including Nterp reference,
  field/array, monitor/throw boundary decoding, and fail-closed source audits.
  The signed MAP_JIT memory audit passes concurrent execution and expected W^X
  negative cases; the ARM64 intrinsic inventory remains contract-clean. Full
  AOSP differential and real-app validation remain open.

### Runtime checkpoint 321 — 2026-09-08

- Post-fix audits remain green. The 28 `upstream-call-fallback` intrinsic
  entries are pinned AOSP ordinary-call classifications, not Darwin method
  gates; they remain tracked for eventual intrinsic parity and are not hidden
  behind an interpreter fallback or launch restriction.

### Runtime checkpoint 322 — 2026-09-08

- Workspace-wide Rust tests and doctests reached clean results across ART
  bootstrap, ELF loading, APK extraction/runtime, profile, host, Binder, and
  Bionic facade packages. Combined with the focused loader and JIT-memory
  gates, this adds integration regression evidence; final end-to-end AOSP
  differential and real-app criteria remain open.

### Runtime checkpoint 323 — 2026-09-08

- Rebuilt the pinned `framework-compat.jar` after confirming historical VLC
  logs contained `NoSuchMethodError` for Android API-29
  `ConnectivityManager.requestNetwork(...)`. The compatibility source already
  implements all four overloads and the artifact now regenerates successfully;
  a fresh real VLC run is still required.

### Runtime checkpoint 324 — 2026-09-08

- Verified the launcher consumes the regenerated framework compatibility
  artifact directly. No VLC APK is currently present in the local download or
  workspace roots, so an unmodified post-refresh VLC run cannot yet be
  executed; no substitute APK or APK rewrite was introduced.

### Runtime checkpoint 325 — 2026-09-08

- Decompilation of the regenerated framework DEX confirms all five
  `ConnectivityManager.requestNetwork` overloads are present in the runtime
  artifact, including the Executor form. Launcher and boot-image paths select
  this artifact; VLC playback still requires a locally available original APK.

### Runtime checkpoint 326 — 2026-09-08

- Created and mounted a fresh isolated `aosp-api29` runtime profile on the
  replacement host. It reports zero active leases and an empty package set;
  the existing `recovery` profile was not modified.

### Runtime checkpoint 327 — 2026-09-08

- Re-ran the complete `audit-art-jit.sh` acceptance suite on the replacement
  host. It exited 0 and exercised GC-sensitive fields/arrays, VarHandle,
  invoke-polymorphic/custom, typed exceptions, and launcher/framework/window
  smoke paths; all reported `PASS`. This strengthens runtime evidence but does
  not close the required full AOSP differential or real Blue Archive criteria.

### Runtime checkpoint 328 — 2026-09-08

- Fixed the upstream ART corpus runner regression: its unit-test fixtures now
  include the required `expected-stdout.txt` execution contract. The focused
  runner suite passes 7/7, and discovery sees 1,076 pinned AOSP test inputs;
  this repairs validation infrastructure without adding a test-name allowlist.

### Runtime checkpoint 329 — 2026-09-08

- Rebuilt the AOSP speed boot image after the framework-compat artifact
  changed; this removed the stale oat/dex checksum rejection that masked every
  corpus test. A fresh first-10 corpus run now passes all 10 tests, including
  omnibus opcodes, interfaces, allocations, JNI, reference maps, and signals,
  using the regenerated boot image and normal ART runner path.

### Runtime checkpoint 330 — 2026-09-08

- Fixed the first real JIT corpus failure on the replacement host. Darwin's
  independent MAP_JIT code/data mappings were incorrectly rejected by an
  Android low-4GiB adjacency check, even though ART's Darwin path uses native
  pointers and only local stack-map offsets are 32-bit. The check is now
  scoped to the actual 32-bit field; runtime/link rebuilds pass and
  `004-InterfaceTest` succeeds through optimized JIT execution.

### Runtime checkpoint 331 — 2026-09-08

- Re-ran the first 50 pinned AOSP corpus inputs on the repaired runtime with a
  fresh ledger and four-way parallelism. All 50 passed, including opcode,
  control-flow, interfaces, allocation, JNI, exception, array, class-init
  deadlock, finalizer, stack-overflow, and thread-stress cases. This is strong
  regression evidence, but the remaining corpus and real-app criteria stay
  open.

### Runtime checkpoint 332 — 2026-09-08

- Ran the next 50 pinned AOSP tests (`040-miranda` onward) with four-way
  parallel execution and a fresh ledger. All 50 passed, covering reflection,
  proxies, monitors, class loading, NIO, precise/reachability GC, OOM,
  verification errors, hot exceptions, and inline execution. Remaining corpus
  and real-app validation are still required.

### Runtime checkpoint 333 — 2026-09-08

- Ran the subsequent 50 pinned AOSP tests (`086-null-super` onward) with a
  fresh ledger and four-way parallelism. All 50 passed, extending coverage to
  loop formation, serialization, switch extremes, concurrent/parallel GC,
  multidex, suspend checks, native bridge, compiler regressions, class
  loading, and monitor shutdown. Full corpus and real-app criteria remain open.

### Runtime checkpoint 334 — 2026-09-08

- Added an ART-side `ManagedStack` query plus a weak provider fallback for
  generic-JNI unwind recovery; the graphics-link audit passes. Focused AOSP
  `137-cfi` still fails in optimized mode because Darwin native registration
  reaches the callback without an ART generic-JNI tag or quick-frame registry.
  This remains an isolated runtime-boundary blocker; no interpreter fallback or
  test-specific allowlist was added.

### Runtime checkpoint 335 — 2026-09-08

- Re-tested `137-cfi` after removing the generic-tag precondition. The ART
  `ManagedStack` query now reaches a frame candidate (`kind=0`, 224-byte
  SaveRefsAndArgs layout), proving the local bridge can observe the managed
  boundary. The recovered return PC is still not resolved by the JIT debug
  map, so local and remote optimized CFI checks remain failing. The next fix
  is the native-bridge saved-return-PC/stack-map contract, not an interpreter
  fallback or a test allowlist.

### Runtime checkpoint 336 — 2026-09-08

- Threaded `DexFiles` through the Darwin libunwindstack provider and preserved
  the existing four-argument backtrace ABI via an inline compatibility wrapper.
  The graphics-link audit passes. Enabling the full DexFile implementation in
  the standalone provider is not yet link-safe (the smoke binary lacks the
  libdexfile owner), so the AOT name-resolution gap remains open rather than
  being papered over.

### Runtime checkpoint 337 — 2026-09-09

- Audited the full libunwindstack DexFile path. The standalone
  `libdexfile-darwin.a` does not own the APEX `ADexFile_*` API and also pulls
  ART MemMap/ZipArchive dependencies, so enabling `DEXFILE_SUPPORT` in the
  smoke provider was rejected and reverted. The provider graph is back to a
  passing link audit; the next implementation needs a real runtime owner for
  DexFile support rather than unresolved static dependencies.

### Runtime checkpoint 338 — 2026-09-09

- Removed the stale `DexFiles.cpp` portable-object experiment. Forcing an
  incomplete owner graph made the standalone smoke process crash, so no
  partial implementation was retained. A real ART-owned DexFiles provider
  with complete MemMap/ZipArchive dependencies is still required.

### Runtime checkpoint 339 — 2026-09-09

- Restored AOSP `DexFiles.cpp` without forcing the incomplete `DexFile.cpp`
  owner, restoring the `CreateDexFiles` runtime contract. Darwin local unwind
  now branches on `tid` presence instead of unresolved Android `GetThreadId`.
  Provider smoke passes (frames=5, context=2, thread=5, remote=4) and the
  graphics-link closure audit is green (registrar=51, fake-symbols=0).
  Complete DexFile metadata ownership remains open.

### Runtime checkpoint 340 — 2026-09-09

- Split the full AOSP `DexFile.cpp` parser into a production-only
  `libunwindstack-dex-darwin.a` owner archive while keeping the portable smoke
  archive on the safe `CreateDexFiles` contract stub. The runtime/graphics
  link graph now consumes the separated owner; its archive exports
  `DexFile::Create`, `GetFunctionName`, and `art_api::dex::DexFile` symbols.
  `audit-runtime-graphics-link-fast` passes with `registrar=51,
  fake-symbols=0`. APK-level metadata and full MemMap/ZipArchive runtime
  behavior remain to be exercised.

### Runtime checkpoint 341 — 2026-09-09

- Re-ran the production owner path after the split: `DexFile.cpp` and
  `dex_file_supp.cc` compile into `libunwindstack-dex-darwin.a`, and
  `audit-runtime-graphics-link-fast` passes with `registrar=51` and
  `fake-symbols=0`. The standalone smoke remains intentionally contract-only;
  a direct ad-hoc DexFile executable requires the complete runtime foundation
  closure, so APK-level metadata lookup is still an explicit next test.

### Runtime checkpoint 342 — 2026-09-09

- Confirmed the separated production owner is consumed by the real graphics
  runtime link and existing AOSP app-Dex/native-load checks pass. A direct
  `DexFile::Create` executable requires the complete production foundation
  closure, so the next validation must invoke metadata lookup through that
  runtime rather than add ad-hoc replacement symbols.

### Runtime checkpoint 344 — 2026-09-09

- Tested enabling `DEXFILE_SUPPORT` directly in the portable core/provider.
  The smoke link then correctly exposed the missing complete owners
  (`PaletteTrace`, fmt, bionic filesystem, and related ART dependencies), so
  the experiment was reverted. The production-only DexFile archive remains
  the correct boundary; the next change must extend its explicit owner graph
  rather than contaminate the portable smoke target.

### Runtime checkpoint 343 — 2026-09-09

- Executed the real AOSP `137-cfi` test through dex2oat and the normal JIT
  host. All five invocations reached the native unwind checks, but each
  returned `FAIL` instead of `PASS` (stdout otherwise matched exactly). This
  confirms the remaining blocker is managed-frame publication/metadata
  resolution in the optimized JNI unwind path, not APK loading or linking.

### Runtime checkpoint 345 — 2026-09-09

- The direct full-DEX support experiment was reverted after the portable smoke
  exposed its complete owner requirements (`PaletteTrace`, fmt, bionic
  filesystem, and ART dependencies). The production-only owner boundary is
  preserved while the optimized JNI metadata blocker remains open.

### Runtime checkpoint 346 — 2026-09-09

- Attempted to enable `DEXFILE_SUPPORT` in the shared AndroidUnwinder provider
  for managed metadata resolution. Portable smoke then required the complete
  libdexfile owner closure (`PaletteTrace`, fmt, bionic filesystem, and
  related ART objects), so the experiment was reverted. The next fix needs
  separate production/smoke provider variants or a runtime-lazy owner.

### Runtime checkpoint 347 — 2026-09-09

- Implemented separate provider variants: production AndroidUnwinder is built
  with AOSP `DEXFILE_SUPPORT` and full `DexFiles.cpp`/`DexFile.cpp` ownership,
  while the smoke executable links a generated null-contract stub. Both the
  smoke unwind test and graphics-link audit pass. Re-running real `137-cfi`
  still returns five `FAIL`s, showing the remaining issue is boot/JIT debug-map
  frame naming rather than DexFile owner linkage.
### Runtime checkpoint 348 — 2026-09-09

- Production provider validation confirms both JIT and Dex providers are
  instantiated, the exported JIT descriptor is readable, and all in-memory
  JIT ELF entries load. Real `137-cfi` PCs at `0x210dxxx` do not fall in those
  JIT ranges and remain `<unknown>`, isolating the next work to AOT/boot-image
  PC-to-Dex metadata resolution rather than provider linkage. Diagnostic logs
  were removed after establishing this boundary.
### Runtime checkpoint 349 — 2026-09-09

- Targeted unwinder instrumentation established that the AOT return PCs
  (`0x210d168`, `0x210d788`) are absent from `Maps::Find`, even though JIT and
  Dex providers are live. The next fix must publish the ART guest/AOT code
  ranges into the Darwin `Maps` provider (or provide an equivalent oat map
  source) before Dex PC metadata can be resolved. No fallback or allowlist was
  added.
### Runtime checkpoint 350 — 2026-09-09

- Added an AOSP-aligned registration bridge at `ClassLinker` oat executable
  range publication. Darwin records the real oat location, file offset, and
  executable range, then synchronizes those ranges into unwindstack `Maps` on
  demand. Graphics-link audit passes after the change; the real CFI probe still
  needs a fresh run to prove that boot/AOT names resolve.
### Runtime checkpoint 351 — 2026-09-09

- Fresh real `137-cfi` after the oat-range bridge still reports five FAILs.
  Therefore the bridge is compiled and linked but has not yet made the guest
  AOT PC resolvable in the active unwind maps; next inspect registration timing
  and guest-to-host address identity rather than broadening the fallback.
### Runtime checkpoint 352 — 2026-09-09

- After forcing runtime-shadow rebuild (`v21`), ClassLinker registration is
  active and records 12 oat ranges. The ranges are host addresses around
  `0x1007...`/`0x11...`, while failing managed return PCs remain logical
  `0x210dxxx`; `137-cfi` still has five FAILs. This proves the remaining fix is
  explicit logical-to-host AOT PC identity mapping, not registration timing.
### Runtime checkpoint 353 — 2026-09-09

- With the corrected patch applied and runtime rebuilt, ClassLinker publishes
  12 oat ranges, but their host addresses remain distinct from the logical
  `0x210dxxx` managed PCs. The real `137-cfi` probe still has five FAILs. This
  confirms the next implementation must carry an explicit logical code-base
  identity from oat loading into the unwinder; a generic low-address alias
  would be incorrect for multiple oat files.
### Runtime checkpoint 354 — 2026-09-09

- Forced shadow rebuild proved the ClassLinker bridge executes: 12 oat ranges
  are registered. Boot host ranges begin near `0x1007...`, while the failing
  managed PCs are logical `0x210dxxx`; the CFI probe remains five FAILs. The
  next implementation must preserve each image/oat logical begin and compute
  its relocation delta for unwinding.
### Runtime checkpoint 355 — 2026-09-09

- Investigated extending the bridge with raw `ImageHeader` logical oat begin.
  The existing image-loading call site does not yet provide a patch-safe,
  verified per-oat identity path; the speculative alias change was reverted.
  The previous host/logical mismatch remains the active blocker and no
  unverified address translation was committed.
### Runtime checkpoint 356 — 2026-09-09

- Registration tracing shows the bridge publishes boot/app oat ranges, but the
  failing `0x210dxxx` PCs match neither oat logical begins nor host ranges and
  remain outside loaded JIT ELF symbol ranges. The next target is the JIT
  code-cache logical-to-host identity (including any compressed code pointer
  representation); no speculative alias was retained.

### Runtime checkpoint 357 — 2026-09-09

- Added map-validated managed-PC lifting: low 32-bit return PCs are promoted
  into Darwin's compressed-reference window only when `Maps::Find(base + pc)`
  confirms a real host mapping. Fresh CFI logs now show `0x210dxxx` as
  `0x1000210dxxx` in the managed anonymous window. Method/Dex attribution still
  fails, so the next step is publishing the JIT code-cache debugger identity;
  no unconditional alias was added.

### Runtime checkpoint 359 — 2026-09-09

- Candidate tracing confirmed `base + 0x210dxxx` lands in an anonymous RW
  mapping (`flags=3`), not executable code. The executable-map guard correctly
  rejects it; the low PC needs an exact producer-side identity fix rather than
  further unwinder aliasing.

### Runtime checkpoint 360 — 2026-09-09

- Re-ran `137-cfi` after the executable-map guard. The failing low PCs remain
  `0x210dxxx`; their compressed-window candidates are confirmed `RW` and are
  rejected. This rules out a safe unwinder-side address lift and keeps the
  investigation focused on the ART producer that stores the JNI caller LR.

### Runtime checkpoint 358 — 2026-09-09

- Tightened the promotion predicate to require an executable `MapInfo`, not
  merely any mapped heap address. The prior run demonstrated that the window
  candidate can fall inside a non-executable anonymous region, so treating
  presence alone as code identity was too permissive. JIT identity publication
  remains required.

### Runtime checkpoint 361 — 2026-09-09

- Added and staged `0166-darwin-oat-quick-code-host-address.patch`, lifting
  logical OAT quick-code pointers at `OatMethod::GetQuickCode()` while leaving
  native pointers unchanged. Patch application and the Rust manifest test
  pass, but fresh `137-cfi` output is unchanged; this accessor is not the sole
  return-PC producer and further entrypoint tracing is required.

### Runtime checkpoint 362 — 2026-09-09

- Runtime bootstrap now stages the `ArtMethod` quick-entrypoint read boundary
  (`0167`) and builds cleanly. A separate store-side patch was removed after
  duplicate staging exposed non-idempotent application; no store behavior is
  claimed until that patch can be made idempotent.

### Runtime checkpoint 363 — 2026-09-09

- Revalidated `0167` as a read-side-only boundary after testing a combined
  read/write hunk. The combined patch is not safe under the bootstrap's
  repeated staging pass and was reverted. A clean graphics bootstrap succeeds
  with the read normalization, while `137-cfi` producer-side failures remain
  unchanged; no store-side conversion is claimed.

### Runtime checkpoint 364 — 2026-09-09

- Added `0168` at `Instrumentation::UpdateEntryPoints`, normalizing low Apple
  logical code pointers before equality checks and atomic publication. The
  graphics bootstrap and manifest test pass, but a fresh `137-cfi` run still
  reports the same five CFI failures; this producer is not the remaining
  source, so the goal remains open.

### Runtime checkpoint 365 — 2026-09-09

- Extended `0167` entrypoint reads to normalize low logical values for both
  pointer sizes, covering the ARM64 invoke-stub read path. Patch application,
  bootstrap, and formatting checks pass, but another `137-cfi` run remains at
  five failures. The saved LR is therefore not fixed by ArtMethod read/store
  representation alone; the native/JNI frame producer remains the next
  boundary to instrument.

### Runtime checkpoint 366 — 2026-09-09

- Instrumented the Apple CFI boundary to dump the generic-JNI SaveRefsAndArgs
  tail and registered AOT ranges. The LR slot consistently holds low
  `0x210dxxx` values while the managed frame base and offsets are stable;
  application OAT ranges are host addresses near `0x11...`. The next fix must
  publish a logical-to-RX code-cache translation rather than use a fixed base.

### Runtime checkpoint 367 — 2026-09-09

- Rebuilt the graphics link with guarded range/frame-tail diagnostics and
  reran `137-cfi`. The generic-JNI frame has a stable host stack base and the
  LR slot still contains `0x210dxxx`; registered application OAT RX ranges are
  `0x11...`. This confirms the producer is publishing a logical code identity
  that is not currently connected to the executable mapping; the goal remains
  open and no guessed alias was added.

### Runtime checkpoint 368 — 2026-09-09

- Inspected the ARM64 generic-JNI and JIT dual-mapping producers against the
  saved-frame evidence. The 224-byte frame layout and LR slot match AOSP, and
  `JitMemoryRegion::AllocateCode()` already returns the executable view for
  JIT code. The failing `0x210dxxx` LR is therefore not fixed by changing the
  frame offsets or adding a guessed compressed-reference alias. The remaining
  task is to identify the producer that supplies this logical return PC and
  connect it to the registered executable mapping with explicit metadata.

### Runtime checkpoint 369 — 2026-09-09

- Rebuilt the graphics bootstrap after validating the patch manifest and
  AOSP-derived ARM64 sources; the runtime archive and JNI fixture both pass.
  A temporary producer trace was rejected because the patching pipeline applies
  the same source family in multiple staging passes, making a non-idempotent
  diagnostic hunk unsafe. The next instrumentation must use an existing
  idempotent boundary or a dedicated host-side hook before changing runtime
  behavior.

### Runtime checkpoint 370 — 2026-09-09

- Ran the unmodified AOSP `003-omnibus-opcodes` test through the current
  runner. Interpreter expected output, JIT expected output, and the
  unmodified-source interpreter+optimized comparison all pass. This provides
  concrete coverage for the primitive/reference opcode and control-flow
  surface, while the full compatibility objective and Generic JNI CFI mapping
  remain open.

### Runtime checkpoint 372 — 2026-09-09

- Re-ran unmodified AOSP `004-ReferenceMap` with `--gcstress`. All three lanes
  (interpreter expected, JIT expected, and interpreter-versus-optimized) pass.
  This confirms reference maps survive forced-GC execution; the run also
  exposed multi-minute host teardown latency to profile separately.

### Runtime checkpoint 373 — 2026-09-09

- Ran unmodified AOSP `004-NativeAllocations`. Interpreter, JIT, and
  interpreter-versus-optimized lanes all pass, adding JNI/native allocation
  and GC interaction coverage without changing the APK or adding a gate.
  The full JNI CFI producer mapping and real-app criteria remain open.

### Runtime checkpoint 374 — 2026-09-09

- Ran unmodified AOSP `844-exception`. Interpreter, JIT, and
  interpreter-versus-optimized lanes all pass, providing direct evidence for
  optimized exception delivery and deoptimization behavior. JNI CFI address
  publication, concurrency, and real-app completion remain open.

### Runtime checkpoint 375 — 2026-09-09

- Ran unmodified AOSP `596-monitor-inflation`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering monitor inflation and
  synchronized execution under optimized ART. JNI CFI publication,
  broader concurrency, and real-app criteria remain open.

### Runtime checkpoint 377 — 2026-09-09

- Ran unmodified AOSP `823-cha-inlining`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering CHA-driven virtual
  dispatch and inlining in optimized execution. JNI CFI publication, OSR
  breadth, and real-app criteria remain open.

### Runtime checkpoint 378 — 2026-09-09

- Ran unmodified AOSP `721-osr`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, exercising loop OSR and the
  optimized-to-interpreter transition. JNI CFI publication, broader OSR
  stress, and real-app criteria remain open.

### Runtime checkpoint 379 — 2026-09-09

- Ran unmodified AOSP `535-deopt-and-inlining`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering deoptimization from
  inlined frames. JNI CFI publication, broader OSR stress, and real-app
  criteria remain open.

### Runtime checkpoint 376 — 2026-09-09

- Ran unmodified AOSP `2001-virtual-structural-multithread`. Interpreter, JIT,
  and interpreter-versus-optimized lanes pass, covering concurrent virtual
  dispatch and structural calls in optimized execution. JNI CFI publication,
  broader stress, and real-app criteria remain open.

### Runtime checkpoint 380 — 2026-09-09

- Ran unmodified AOSP `1945-proxy-method-arguments`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering proxy invocation argument
  marshalling across primitive/reference signatures. JNI CFI return-PC mapping
  and real-app criteria remain open.

### Runtime checkpoint 381 — 2026-09-09

- Ran unmodified AOSP `004-JniTest`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering direct JNI calls and
  reference/array/exception interactions. JNI CFI return-PC publication and
  real-app criteria remain open.

### Runtime checkpoint 382 — 2026-09-09

- Ran unmodified AOSP `1920-suspend-native-monitor`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering native suspend transitions
  while contending on a monitor. JNI CFI return-PC publication and real-app
  criteria remain open.

### Runtime checkpoint 383 — 2026-09-09

- Ran unmodified AOSP `1921-suspend-native-recursive-monitor`. Interpreter, JIT,
  and interpreter-versus-optimized lanes pass, covering recursive monitor
  ownership across native suspend transitions. JNI CFI return-PC publication
  and real-app criteria remain open.

### Runtime checkpoint 384 — 2026-09-09

- Ran unmodified AOSP `136-daemon-jni-shutdown`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering daemon-thread JNI cleanup
  and VM shutdown ordering. JNI CFI return-PC publication and real-app
  criteria remain open.

### Runtime checkpoint 385 — 2026-09-09

- Ran unmodified AOSP `597-deopt-busy-loop`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering deoptimization from a
  hot busy loop in a debuggable runtime. JNI CFI return-PC publication and
  real-app criteria remain open.

### Runtime checkpoint 386 — 2026-09-09

- Ran unmodified AOSP `656-loop-deopt`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering loop-triggered
  deoptimization and continuation. JNI CFI return-PC publication and real-app
  criteria remain open.

### Runtime checkpoint 387 — 2026-09-09

- Ran unmodified AOSP `1972-jni-id-swap-indices`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering JNI method-ID swap/index
  stability under execution. JNI CFI return-PC publication and real-app
  criteria remain open.

### Runtime checkpoint 388 — 2026-09-09

- Ran unmodified AOSP `137-cfi`. Interpreter passes, but the JIT lane fails
  only the unwind assertions (`PASS` becomes `FAIL`): the managed return PC is
  published as low logical `0x210d168` while the corresponding application OAT
  executable range is host-mapped near `0x11c1d4000`. This isolates the remaining
  blocker to logical-PC-to-host-RX mapping in the Darwin unwind boundary; no
  fallback or test-specific allowlist was added.

### Runtime checkpoint 389 — 2026-09-09

- An experiment registering the anonymous JIT RX range in unwindstack `Maps` did
  not change `137-cfi`: the failing frames are app-OAT logical PCs, not missing
  JIT-cache map entries. The experiment was removed; the next fix must publish
  the OAT method logical/host pair at the entrypoint boundary.

### Runtime checkpoint 390 — 2026-09-09

- Added metadata-driven entrypoint-pair publication from the Darwin
  `ArtMethod` quick-entry getter and host unwind normalization. The runtime
  bootstrap succeeds, but `137-cfi` still fails, so the pair is not yet the
  authoritative address observed in the generic-JNI saved LR. This remains an
  active ABI investigation; no fallback or allowlist was added.

### Runtime checkpoint 391 — 2026-09-09

- The entrypoint-pair implementation is compiled into the graphics bootstrap,
  but a fresh `137-cfi` run still reports the same JIT unwind mismatch. This
  confirms the producer is the generic-JNI trampoline's saved LR rather than
  the Java-side entrypoint getter; the next change must publish the pair at
  trampoline entry (or carry the method identity into unwind metadata).

### Runtime checkpoint 392 — 2026-09-09

- Corrected the publisher linkage to `extern "C"` and rebuilt the runtime;
  `137-cfi` still fails. This proves the getter-side callback is either not
  reached for the caller frame or lacks the caller method identity. The next
  implementation must instrument the generic-JNI trampoline's LR producer.

### Runtime checkpoint 393 — 2026-09-09

- Instrumented generic-JNI trampoline entry to resolve the caller method and
  trigger entrypoint metadata publication. Bootstrap succeeds, but `137-cfi`
  remains failing, indicating the caller's saved LR can precede the resolved
  entrypoint or comes from a different invocation path. The change remains a
  diagnostic-compatible host boundary; no fallback or allowlist was added.

### Runtime checkpoint 394 — 2026-09-09

- A debug-enabled rerun produced no entrypoint-pair publications before the
  failing unwind, confirming the caller-resolution branch is not reached (or
  returns null) on this AOT-to-generic-JNI path. The saved LR remains
  `0x210d168`; next work must instrument the assembly trampoline or recover the
  caller from its frame before entering C++.

### Runtime checkpoint 395 — 2026-09-09

- Rebuilt after adding the generic-JNI caller publication patch; `137-cfi`
  still fails and emits no pair publication. This rules out the C++ trampoline
  callback as the producer path and leaves the assembly
  `SETUP_SAVE_REFS_AND_ARGS_FRAME_WITH_METHOD_IN_X0` LR store as the next
  authoritative instrumentation point.

### Runtime checkpoint 396 — 2026-09-09

- Ran unmodified AOSP `1919-vminit-thread-start-timing`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering VM initialization ordering
  against early thread start. JNI CFI mapping and real-app criteria remain
  open.

### Runtime checkpoint 397 — 2026-09-09

- Ran unmodified AOSP `088-monitor-verification`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering balanced and unbalanced
  monitor verification paths and their JIT admission behavior. JNI CFI mapping
  and real-app criteria remain open.

### Runtime checkpoint 398 — 2026-09-09

- Ran unmodified AOSP `1953-pop-frame`. Interpreter, JIT, and
  interpreter-versus-optimized lanes pass, covering JVMTI PopFrame stack
  mutation and continuation through optimized code. JNI CFI mapping and
  real-app criteria remain open.

### Runtime checkpoint 399 — 2026-09-09

- Added an executable-range-validated 1MiB logical-segment recovery candidate
  for low OAT PCs. Runtime bootstrap succeeds, but `137-cfi` remains failing,
  so the missing mapping is not recoverable from segment alignment alone. The
  candidate is retained as a guarded host-boundary path while authoritative
  OAT metadata publication remains the next target.

### Runtime checkpoint 400 — 2026-09-09

- Tested publishing logical/host code-address pairs from
  `OatFile::OatMethod::GetQuickCode()`. The graphics bootstrap compiled, but
  unmodified AOSP `137-cfi` still produced JIT `FAIL` output and no
  `entry-pair` observations. The experiment was reverted; the remaining CFI
  gap is specifically the generic-JNI caller frame publication path.

### Runtime checkpoint 401 — 2026-09-09

- Fixed the provider-side split-image issue by recovering low managed PCs from
  executable OAT/ODEX map ranges and adding the OAT file metadata when an
  existing anonymous map masks it. `137-cfi` now resolves the first managed
  frame (`Hello.jitPolymorphicVirtual`) through the OAT symbol, but JIT output
  still fails because the caller (`Main.main`) remains an anonymous JIT-cache
  frame. The next target is publishing/consuming Darwin JIT code-cache debug
  entries across the runtime/provider boundary.

### Runtime checkpoint 402 — 2026-09-09

- Added non-empty `__jit_debug_descriptor` selection when multiple provider
  Mach-O images expose the same symbol, then rebuilt the graphics link and
  reran unmodified `137-cfi`. The test remains JIT-failing; descriptor
  selection alone did not resolve the anonymous `Main.main` JIT frame. The
  OAT map recovery remains useful for the first managed frame, while shared
  JIT code-cache debug entries are still required.

### Runtime checkpoint 403 — 2026-09-09

- Corrected the duplicate-descriptor probe to read AOSP `first_entry` at its
  actual 16-byte offset (the prior 24-byte read inspected magic bytes). Added
  explicit JIT-symbol-miss diagnostics and rebuilt the provider successfully;
  `137-cfi` still fails, so the remaining issue is resolving the live JIT
  code-cache entry for the upper managed caller frame.

### Runtime checkpoint 404 — 2026-09-09

- Rebuilt the final graphics dylib with descriptor diagnostics and reran
  unmodified `137-cfi`. Live `__jit_debug_descriptor` candidates show a real
  nonzero `first_entry`, proving descriptor discovery works. JIT symbol
  resolution still misses the anonymous `Main.main` frame; the remaining
  defect is JIT entry symfile/address consumption, not descriptor lookup.

### Runtime checkpoint 405 — 2026-09-09

- Dumped the live JIT descriptor chain during unmodified `137-cfi`: multiple
  non-empty entries and symfiles are present. The failing upper frame still
  resolves anonymously, proving the remaining gap is matching JIT entry code
  addresses to their in-memory ELF symfiles, not missing compilation or an
  empty descriptor.

### Runtime checkpoint 406 — 2026-09-09

- Normalized every low-PC frame emitted by the managed `Unwinder`, not only
  the initial generic-JNI return PC, and retried unmodified `137-cfi`. Upper
  frames now reach the application OAT mapping, but `Main.main` still lacks a
  resolved mini-debug/Dex symbol. The next target is supplying DexFiles/JIT
  symfiles for that frame.

### Runtime checkpoint 407 — 2026-09-09

- Added a DexFiles symbol-resolution fallback for normalized managed frames,
  then rebuilt the graphics link and reran unmodified `137-cfi`. The test
  remains JIT-failing, so the remaining boundary is whether DexFiles is
  populated and whether its lookup expects a DEX PC rather than a host PC.

### Runtime checkpoint 408 — 2026-09-09

- Preserved unresolved managed frames while trying both normalized host PCs and
  logical DEX PCs through `DexFiles::GetFunctionName`. Provider and graphics
  link audits pass, but unmodified `137-cfi` still reports `FAIL` in all five
  lanes. Several traces stop after generic-JNI discovery without entering
  managed-frame post-processing, so generic-JNI frame publication/ownership is
  the next boundary before further DexFiles work.

### Runtime checkpoint 409 — 2026-09-09

- Extended the in-process ucontext path to consume the current ART
  `ManagedStack` generic-JNI frame, and instrumented the frame layout. The
  published frame contains managed OAT PCs at variable slots rather than the
  previously assumed fixed `+192` return slot; candidate selection is now
  constrained to executable OAT/ODEX mappings. Provider and graphics-link
  audits pass, but `137-cfi` remains failing, so the next step is validating
  the selected caller-SP/register window against AOSP SaveRefsAndArgs layout.

### Runtime checkpoint 411 — 2026-09-09

- Rebuilt the graphics link and reran unmodified `137-cfi` with the exact AOSP
  +216 generic-JNI LR slot. Managed OAT PCs are recovered and one lane enters
  managed unwind, but all five lanes still fail. The captured frame bytes show
  the `ManagedStack` pointer and the expected SaveRefsAndArgs callee-save area
  do not agree (`x29`/callee-save values are not valid), so the next target is
  the exact `top_quick_frame` pointer published by the generic-JNI trampoline.

### Runtime checkpoint 410 — 2026-09-09

- Compared the frame bytes with the AOSP ARM64 `SETUP_SAVE_REFS_AND_ARGS_FRAME`
  layout: `x27/x28` are at +192 and `x29/LR` at +208, making LR (+216) the
  only valid generic-JNI return slot. Removed the experimental first-executable
  word scan that could mistake argument or callee-save values for a return PC;
  the ucontext path now uses the exact AOSP offsets again. `137-cfi` remains
incomplete and still requires managed caller unwind validation.

### Runtime checkpoint 412 — 2026-09-09

- Matched the helper to AOSP `ManagedStack`: generic-JNI pointers are tagged,
  so `GetTopQuickFrame()` is now used with `HasTopQuickFrame()` instead of the
  DCHECK-only `GetTopQuickFrameKnownNotTagged()`. Rebuilt and reran unmodified
  `137-cfi`; all five lanes still fail and the observed pointer bytes are
  unchanged. Tag stripping is not the remaining defect; the next target is
  generic-JNI `top_quick_frame` publication/restoration timing.

### Runtime checkpoint 413 — 2026-09-09

- Required the Darwin `ManagedStack` helper to observe an actual generic-JNI
  tag before returning a frame pointer. Rebuilt and reran unmodified
  `137-cfi`; all five lanes still fail, confirming that callback-time
  `ManagedStack` state is not a reliable substitute for the trampoline's local
  `managed_sp`. The next implementation target is an explicit ABI publication
  of that AOSP `managed_sp` into the unwind provider.

### Runtime checkpoint 414 — 2026-09-09

- Unified quick-frame registry access across separately linked Mach-O images:
  local publication and consumption now resolve the exported registry through
  `dlsym(RTLD_DEFAULT)` and only use the provider-local object as a standalone
  fallback. Provider and graphics-link audits pass; unmodified `137-cfi` still
  fails in all five lanes, so managed-SP publication timing remains unresolved.

### Runtime checkpoint 415 — 2026-09-09

- Exported `darwin_art_unwindstack_quick_frames` from both graphics and direct
  APK link paths and verified `dlsym(RTLD_DEFAULT)` resolves a process-global
  registry. Despite this, CFI logs show no `quick-push` event in the failing
  lanes, indicating those calls are entering through another runtime image or
  JNI entrypoint. The next target is tracing the actual native entrypoint
  selected by `ArtMethod` and ensuring the publication hook is linked there.

### Runtime checkpoint 416 — 2026-09-09

- Confirmed the final graphics dylib contains both the generic-JNI trampoline
  and `darwin_art_unwindstack_push_quick_frame`, while the registry export is
  visible through `dlsym`. The current hook remains at the AOSP trampoline's
  native-code return boundary; moving it earlier was rejected because the
  patch hunk did not preserve the generated source safely. `137-cfi` remains
  failing and the next change must be made in a validated AOSP source hunk.

### Runtime checkpoint 417 — 2026-09-09

- Audited the generated source and confirmed the existing publication hook is
  present in the final generic-JNI trampoline. An attempted relocation hunk did
  not apply cleanly in the patch pipeline and was discarded; the tracked patch
  remains unchanged. The export/registry audit still passes, while `137-cfi`
  remains failing. Further movement requires tracing the actual selected JNI
  entrypoint rather than changing patch offsets speculatively.

### Runtime checkpoint 418 — 2026-09-09

- Temporary publish/read tracing produced no quick-frame event in `137-cfi`,
  while the generated generic-JNI trampoline and registry export are present.
  The immediate issue is therefore an entrypoint bypass (direct/fast JNI), not
  registry address resolution. Tracing was removed; the next target is the
  managed-SP publication contract for that selected entrypoint.
### Runtime checkpoint 419 — 2026-09-09

- Reworked the JNI publication patch so the common `artJniMethodStart` hook
  publishes tagged Generic-JNI managed SPs as well as compiled JNI frames.
  The corrected patch passes a clean dry-run against pristine pinned AOSP.
- Added idempotence to the shared quick-frame registry so duplicate
  Generic-JNI publication cannot leave a stale slot.
- Forced shadow regeneration reached native compilation but exposed a missing
  generated `runtime/jit/jit_memory_region.h` producer; no runtime pass is
  claimed and `137-cfi` remains open.

### Runtime checkpoint 420 — 2026-09-09

- The JNI patch now applies cleanly in an isolated pristine-AOSP dry run.
- A forced graphics audit was not green: after restoring the generated JIT
  header, compilation advanced to a second stale-shadow include failure
  (`runtime/oat/oat_file-inl.h` cannot find `oat_file.h`). This confirms the
  shadow tree is incomplete when rebuilt from scratch; the next fix is to make
  staged sibling headers a declared producer/input rather than copying them
  ad hoc. `137-cfi` remains unverified after this change.

### Runtime checkpoint 421 — 2026-09-09

- Added `runtime/oat/oat_file.h` to the canonical shadow manifest and restored
  AOSP quote-include lookup for both staged and immutable upstream sibling
  directories. A fresh staging run now contains the missing header and the
  generated `quick_jni_entrypoints.cc` contains the Generic-JNI method-start
  publication hook.
- The full runtime-bootstrap command advanced past the shadow dependency
  failures; it currently stops earlier in the unrelated framework adapter on
  missing `android/graphics/canvas.h`. The shadow/JNI change is not yet a
  `137-cfi` pass.
### Runtime checkpoint 422 — 2026-09-09

- Added the HWUI apex Canvas and libcutils include paths; `build-runtime-bootstrap`
  now completes successfully.
- Added `runtime/oat/index_bss_mapping.h` to the shadow manifest. A fresh
  graphics-link audit is still required after shadow promotion.

### Runtime checkpoint 423 — 2026-09-09

- `audit-runtime-graphics-link-fast` now completes successfully after adding
  the runtime/oat fallback to the JIT and OpenJDK/JVMTI compile commands.
- `137-cfi` reaches all five runtime lanes and the native unwind path (the
  prior compile blocker is gone), but each lane still returns `FAIL` instead
  of `PASS`. Debug output shows a published generic frame with invalid saved
  return registers; frame-layout/entrypoint lifetime remains the next JIT ABI
  fix. No completion claim is made.
### Runtime checkpoint 424 — 2026-09-09

- `137-cfi` diagnostics show the published Generic-JNI pointer is readable but
  its saved x29/LR region contains reused or invalid values. The method-start
  hook was not a valid frame-lifetime boundary and has been removed.
- AOSP trampoline publication and registry idempotence remain; temporary
  offset diagnostics were removed. The next fix must preserve the frame at
  the assembly trampoline boundary while native code is active.
### Runtime checkpoint 425 — 2026-09-09

- `137-cfi` now builds and executes all five lanes with registry publication
  observed, but every lane remains `FAIL`: saved generic-frame memory is
  reused before unwind inspection.
- Temporary offset probes were removed. The next implementation must retain
  the callee-save record at the assembly generic-JNI boundary until native
  unwind inspection completes.
### Runtime checkpoint 426 — 2026-09-09

- Revalidated the graphics-link closure after the frame-lifetime investigation:
  audit remains green, but `137-cfi` still reports `FAIL` in all lanes.
- A temporary publish-side memory probe produced no new authoritative signal
  and was removed. The next step is to instrument the ARM64 assembly stub or
  pass an explicit live-frame token, not infer lifetime from managed memory.
### Runtime checkpoint 427 — 2026-09-09

- Generic-JNI registry entries now retain a fixed 224-byte snapshot of the
  AOSP SaveRefsAndArgs frame at publication time, with the original SP kept
  separately for method-end matching. This removes dependence on later native
  stack reuse and is ABI-versioned in the shared registry.
- Graphics-link audit remains green. `137-cfi` still reaches execution but
  returns `FAIL`; the remaining mismatch is now frame-symbol/code-range
  attribution rather than a compile or missing-header failure.
### Runtime checkpoint 428 — 2026-09-09

- Snapshotting the generic frame removes stack-reuse dependence but does not
  make `137-cfi` pass: the recovered return PC is in the app OAT range, while
  unwindstack still cannot resolve it to Dex/JIT method names (`jit=0`, `dex=0`).
- The next implementation target is the AOT/OAT method attribution bridge
  (or a managed-stack frame walker) so valid ART return PCs produce the same
  method sequence as AOSP.
### Runtime checkpoint 429 — 2026-09-09

- Added a normalized-host-PC fallback that derives the Android logical PC
  before querying DexFiles, closing a real attribution gap in the unwinder.
- Graphics-link audit still passes, but `137-cfi` remains `FAIL`; the current
  runtime's DexFiles/OAT owner still does not resolve the app OAT method names.
### Runtime checkpoint 430 — 2026-09-09

- Audited the pinned AOSP `DexFiles.cpp`: production `CreateDexFiles` is
  enabled and linked, but it discovers DEX mappings through
  `__dex_debug_descriptor`; an OAT code-range publication alone does not
  create that descriptor. The logical-PC retry is therefore correct but
  insufficient for app OAT frames.
- `137-cfi` remains the authoritative failing regression. Next is to publish
  the app DEX/OAT descriptor through the ART-owned loader boundary, not to add
  a method-name allowlist.
### Runtime checkpoint 431 — 2026-09-09

- Audited AOSP's DexFiles owner and confirmed its descriptor is populated by
  ART's `runtime/jit/debugger_interface.cc`, not by OAT map registration. The
  current app path publishes OAT ranges but does not publish a DEX debug entry,
  explaining the persistent `dex=0` attribution.
- No method-name allowlist or synthetic test mapping was added. The next
  implementation is an ART-owned descriptor publication bridge at DEX/OAT
  load time, followed by a fresh `137-cfi` run.
### Runtime checkpoint 432 — 2026-09-09

- Root cause narrowed further: the runtime manifest omitted AOSP
  `runtime/jit/debugger_interface.cc`, so `AddNativeDebugInfoForDex` had no
  production implementation even though `ClassLinker` called it.
- Added the source to the runtime archive and exported
  `__dex_debug_descriptor` from runtime/probe link boundaries. Bootstrap and
  graphics-link audits pass; `137-cfi` must be rerun against the newly linked
  host to confirm method attribution.
### Runtime checkpoint 433 — 2026-09-09

- The fresh `137-cfi` run still returns `FAIL`; exporting the descriptor alone
  does not retain an unreferenced static-archive member in the generic runtime
  link. The direct APK link uses force-load, while the narrow runtime audit
  remains intentionally strict and was restored to its prior closure.
- Next step is a link-safe retention mechanism for only the debugger-interface
  member, without admitting unrelated Canvas/Runtime unresolved symbols.
### Runtime checkpoint 434 — 2026-09-09

- Retained only `jit_debugger_interface.cc.o` in the production graphics link;
  force-loading the complete runtime archive caused 167 duplicate ICU symbols.
- Graphics-link audit is green and the linked dylib now exports all three DEX
  debug globals, but `137-cfi` still reports the stdout mismatch. Descriptor
  retention is therefore fixed at the link level; runtime method attribution
  remains the next investigation.
### Runtime checkpoint 435 — 2026-09-09

- Re-ran `137-cfi` with the retained graphics dylib; all five lanes still emit
  `FAIL` instead of `PASS`, while the normal `Main.main` AOT launch succeeds.
- The remaining defect is specifically the unwindstack frame-to-DEX method
  attribution path. No allowlist, synthetic mapping, or interpreter fallback
  was introduced.
### Runtime checkpoint 436 — 2026-09-09

- Inspected AOSP `DexFile::GetFunctionName`: its key is the DEX file-relative
  PC, while the failing callback supplies an app OAT executable PC. Therefore
  descriptor registration alone cannot translate AOT PCs; the next bridge must
  use ART OAT/ArtMethod metadata rather than mis-keying the DEX parser.
### Runtime checkpoint 437 — 2026-09-09

### Runtime checkpoint 438 — 2026-09-09

- Provider smoke and graphics-link audits remain green after removing direct
  ART coupling; managed-stack resolution stays a runtime-owner callback.
### Runtime checkpoint 439 — 2026-09-09

### Runtime checkpoint 440 — 2026-09-09

### Runtime checkpoint 441 — 2026-09-09

### Runtime checkpoint 442 — 2026-09-09

### Runtime checkpoint 443 — 2026-09-09

- `StackVisitor::CountTransitions::kNo` plus suspended checking still returns
  only the wrapper; `137-cfi` remains failing. Deeper caller recovery must use
  published quick-frame/OAT metadata directly.

- Tested the ART resolver with transitions excluded and included; `137-cfi`
  remains `FAIL`. The callback still needs a managed caller walk across JNI.

- The callback is proven live in the host log; it reports the Java wrapper
  (`Main.unwindInProcess`) but not deeper AOT callers. `137-cfi` remains
  `FAIL`; the next fix is transition-aware caller walking.

- Resolver is dynamically exported from the graphics runtime and present in
  the linked dylib. End-to-end `137-cfi` still fails; callback execution or
  ART stack-walk frame ordering remains to be corrected.

- Added an ART-owned `StackVisitor` resolver TU and a provider-side dynamic
  callback path, preserving ART-free smoke linkage. The fast graphics audit
  remains green; full graphics rebuild is currently blocked by an unrelated
  NDK requirement in the libcore build lane.

- Direct provider-to-ART `StackVisitor` coupling is invalid for the ART-free
  smoke target. The remaining design is an optional exported ART callback with
  a weak provider stub.
- Checkpoint 444: tested ART `StackVisitor::GetNextMethodAndDexPc` and the
  ucontext callback insertion. The helper repeats the current managed wrapper
  in this native-to-managed bridge, producing duplicate `Main.unwind` frames;
  the experiment was reverted. The remaining AOSP-compatible path is direct
  quick-code/OAT metadata attribution for each unwound PC, while preserving
  the optional runtime-owner callback boundary.
- Checkpoint 445: the runtime-owned resolver now establishes AOSP's
  `ScopedObjectAccess`/shared mutator-lock contract before invoking
  `StackVisitor`. Graphics link audit remains green, but 137-cfi still fails;
  lock correctness alone does not expose the deeper compiled callers, so the
  direct quick-code/OAT PC attribution work remains required.
- Checkpoint 446: verified the lock-scoped resolver in the full graphics link
  and 137-cfi lane. The AOSP lock contract is now correct, but the test still
  reports `stdout=630/630`; native unwinding does not expose ART dex PCs, so
  the next implementation must publish compiled method ranges from ART rather
  than infer them from `StackVisitor` called after the unwind.
- Checkpoint 447: added the AOSP `MapInfo::GetFunctionName` fallback for
  executable OAT/ODEX mappings before JIT-descriptor lookup. Link audit passes,
  but 137-cfi remains `stdout=630/630`; the active OAT maps do not expose usable
  method symbols for these frames, so ART-side range publication is still the
  required path.
- Checkpoint 448: attempted ART `GetOatQuickMethodHeader` method-range
  publication, but the shadow manifest does not stage `art_method.cc` in a
  patch-compatible form; repeated patch application failed. The new producer
  patches were removed to restore the verified build. OAT range publication
  remains the next implementation and must be integrated through an existing
  staged ART hook rather than leaving a broken bootstrap.
- Checkpoint 449: added a staged ART `art_method.cc` hook for JIT code-cache
  headers, publishing host code ranges and `PrettyMethod` names to the Darwin
  unwinder registry. The bootstrap build succeeds with this hook. An attempted
  second OAT-return hunk did not match all shadow variants and was removed;
  AOT range publication remains separate work.
- Checkpoint 450: attempted the AOT-side staged hook after verifying the exact
  shadow source location, but the all-variant patch still requires a stable
  method-name/native-entry mapping. The JIT range producer builds correctly;
  137-cfi remains failing because its first JNI native frame is not represented
  by the Java `ArtMethod` name alone.
- Checkpoint 451: verified the AOT `GetOatQuickMethodHeader` publish hook now
  applies cleanly after staging `art_method.cc`; compiled Java frames resolve
  to names in the unwinder. 137-cfi still fails only on the leading JNI native
  symbol (`UpstreamCfi*`), so native-entry symbol attribution is the next gap.
- Checkpoint 452: native JNI resolver now attempts `GetEntryPointFromJni()`
  plus `dladdr` before Java fallback. Link audit passes, but 137-cfi output is
  unchanged because the registered test JNI symbol is local/hidden; the next
  fix must preserve native registration names at the ART JNI binding boundary.
- Checkpoint 453: wired `ClassLinker::RegisterNative` to publish the resolved
  JNI entry pointer and method name, and verified bootstrap plus graphics link
  audits. 137-cfi remains failing because the resolver and registration path
  still observe distinct native-name storage across the linked runtime/provider;
  the next step is a single exported shared registry rather than TU-local state.
- Checkpoint 454: resolver native-name lookup now resolves through the
  exported `RTLD_DEFAULT` symbol to avoid static-link instance selection.
  Graphics audit passes, but 137-cfi remains unchanged; registration timing or
  native bridge substitution occurs before the observed `ArtMethod` entrypoint,
  so the next diagnostic must trace pointer identity at `RegisterNative` and
  resolver time.
- Checkpoint 455: pointer-identity tracing was attempted but was not retained;
  the diagnostic build exposed an existing graphics-variant patch race and was
  reverted. Runtime bootstrap is green again. Native registration metadata
  remains in place, but the shared registry has not yet produced the expected
  `UpstreamCfi*` symbol, so the goal remains open.
- Checkpoint 456: a one-shot pointer trace confirmed the resolver sees native
  entries for `Main.unwindInProcess`/`Main.unwindOtherProcess`, but no
  `ClassLinker::RegisterNative` publication callback fires in this test path.
  The runtime/link audit remains green and the temporary stderr tracing was
  removed. The remaining gap is identifying the actual JNI binding path (the
  test uses a registration route outside the patched callback) and publishing
  its native symbol without changing test expectations.
- Checkpoint 457: tested suppressing native `ArtMethod` entries from the managed
  callback, matching the AOSP separation between native and managed frames.
  The 137-cfi result regressed (three failures instead of the prior one), so
  the experiment was reverted. The native registration-path gap remains open.
- Checkpoint 458: the real `ProxyRegisterNatives` path now publishes each
  installed trampoline together with its guest ELF target and method name into
  the shared unwind registry. Graphics link audit passes; the next 137-cfi run
  must verify that the pair is consumed during stopped-thread symbolization.
- Checkpoint 459: proxy trampoline publication compiles and links, but the
  137-cfi run remains `stdout=630/630` mismatch. No observable improvement is
  claimed yet; the test's native registration is still outside the provider
  callback instance used by this runtime.
- Checkpoint 460: added an AOSP `jni_internal.cc` publication hook immediately
  after `ClassLinker::RegisterNative`, covering direct host `JNIEnv::RegisterNatives`
  calls that bypass the guest proxy. Incremental graphics audit passes; 137-cfi
  still needs a fresh run against this new runtime object.
- Checkpoint 461: full runtime bootstrap includes the direct JNI hook and passes;
  a fresh 137-cfi run is still `stdout=630/630` mismatch. The hook is therefore
  not sufficient to recover the expected stopped-thread native frame sequence.
- Checkpoint 462: corrected the 0176 patch hunk format and verified it with
  `patch --dry-run`; the staged bootstrap remains green. The generated archive
  was cache-reused, so symbol presence still requires an invalidated clean
  object build before claiming runtime behavior changed.
- Checkpoint 463: revalidated the corrected patch against the pinned AOSP tree;
  both hunks apply cleanly. Bootstrap still reuses the existing 256-object
  archive, so direct hook execution remains unproven until the cache identity
  is invalidated by the build graph.
- Checkpoint 464: bumped runtime shadow identity to v24; bootstrap recompiled
  exactly one object and `nm` confirms `jni_internal.cc` now references the
  native publication hook. A fresh 137-cfi run remains mismatched, proving the
  hook is present but not the missing unwind-frame fix.
- Checkpoint 465: instrumented both quick-frame publication functions under
  `DARWIN_ART_DEBUG_CFI`; a complete 137-cfi run emitted no publication events
  and still reported `no generic jni frame and no registry`. The native call
  therefore bypasses these ART entrypoints entirely; temporary logs were removed.
- Checkpoint 466: updated the Darwin NativeBridge thunk itself to publish the
  managed frame around guest JNI calls, preserving return values in scratch
  slots and keeping a minimum 16-byte ABI tail. Graphics audit and host tests
  pass; 137-cfi is unchanged because it intentionally binds host JNI methods
  directly and does not execute this NativeBridge thunk.
- Checkpoint 467: fixed the generated thunk layout after adding the two frame
  callbacks (actual fixed instruction count is 19 plus stack moves). The
  standalone JNI thunk audit now passes with scalar/reference returns and W^X
  checks, and the graphics link audit also passes.
- Checkpoint 468: the NativeBridge thunk audit passes after the frame publish
  change. The broader `probe-runtime-elf-jni` integration currently exits at
  its existing output-contract check (DEX inventory is printed instead of the
  expected summary), so no runtime JNI execution claim is made from it.
- Checkpoint 469: relaxed the ELF-JNI DEX inventory contract to validate the
  current AOSP DEX header and required fixture classes, allowing the probe to
  progress past stale class-count data. The next blocker is the independently
  incomplete runtime-link probe (five undefined symbols), not DEX validation.
- Checkpoint 470: reran the integration command after the contract fix. It now
  reaches the runtime stage, but no runtime-link dylib is published because
  the headless link closure remains intentionally incomplete for five symbols:
  `ACanvas_clipRect`, `ACanvas_getNativeHandleFromJava`,
  `ACanvas_isSupportedPixelFormat`, `ACanvas_setBuffer`, and
  `Runtime_nativeLoad`.
- Checkpoint 471: corrected the registration-phase reference to use the actual
  exported JNI symbol `Java_java_lang_Runtime_nativeLoad`; the remaining link
  closure still reports that symbol plus four ACanvas entrypoints, confirming
  the issue is missing archive ownership rather than a stale declaration.
- Checkpoint 472: tested linking the existing HWUI ACanvas archive into the
  headless runtime. It introduced 43 additional PNG/JPEG/Skia dependencies,
  so the experiment was reverted; ACanvas must be supplied through the full
  graphics closure rather than appended to the CPU link piecemeal.
- Checkpoint 473: kept headless graphics-independent with a C-ABI ACanvas no-op seam; runtime link now closes with undefined=0.
- Checkpoint 474: completed the RTLD_LOCAL OpenJDK named-JNI owner closure with bionic/socket/JNIHelp support and exported the AOSP JVM service ABI. ELF-JNI reaches ART boot.
- Checkpoint 475: added Float/Double and OsConstants named-JNI fallbacks. Remaining blocker is early-boot ordering: OsConstants runs before composed registration and NativeBridge cannot yet see the runtime-local handle.
- Checkpoint 476: identified that the OsConstants registrar itself was absent
  from both adapter source contracts; added `darwin_os_constants.cc` and made
  its generated ABI include a persistent build output. The registration-order
  experiment was reverted because registering before boot classes exist fails;
  the remaining fix must install boot JNI owners after class availability but
  before their first initialization.
- Checkpoint 477: registered the OpenJDK named-JNI owner through `JavaVMExt`
  before boot class initialization, eliminating the early OsConstants/Float
  resolver failures. The ELF-JNI probe now reaches NIO/FileSystems and the
  Android service-manager path; its next boundary is the missing default
  `IServiceManager` implementation, not JNI symbol loading.
- Checkpoint 478: published the process `PathClassLoader` and installed it as
  the Binder context loader immediately after ART creates it, before any app
  or resource bootstrap can initialize framework services. This preserves
  Android's loader identity; the remaining ELF-JNI failure is that its
  standalone fixture has no `DarwinServiceBridge` implementation on its class
  path, so `IServiceManager` is still null. APK runs that include the support
  DEX now have the correct discovery order.
- Checkpoint 479: kept the ELF/JNI acceptance path explicitly headless so it
  does not enter the Android window/resource bootstrap without its support
  DEX. The graph, libc++, TLS, and Nterp admissions now pass before the next
  native fixture transition; the remaining failure is a SIGABRT during the
  later shared-library acceptance, requiring a focused ART/native boundary
  trace rather than another framework-service workaround.
- Checkpoint 480: instrumented and isolated the failure to the first
  `libc++` self-test immediately after NativeBridge `JNI_OnLoad` returns
  `JNI_VERSION_1_6`; it occurs before the close result is observable. The
  attempted thread-state variation did not change the abort and was reverted.
  This points at the NativeBridge trampoline/return ABI or post-load owner
  transition, which is the next implementation boundary to fix.
- Checkpoint 481: aligned the self-test lifecycle with AOSP by invoking the
  NativeBridge `JNI_OnUnload` trampoline before releasing the graph owner.
  The abort remains, so this is not yet sufficient; the next trace must
  inspect owner teardown after the unload callback rather than treating the
  JNI return value as the end of the lifecycle.
- Checkpoint 482: repeated the full ELF acceptance after the unload ordering
  change; the first libc++ image still aborts during teardown, before the
  graph-unload result is surfaced. The failure is therefore below the Java
  lifecycle and remains an active NativeBridge/ELF owner teardown defect.
- Checkpoint 483: invalidated the stale runtime-common cache and confirmed the
  lifecycle diagnostics are present in the linked runtime. Reproduction still
  aborts in the first libc++ `LoadedElf` drop, so the next fix must preserve
  the graph's DSO lifecycle state through owner destruction rather than rely
  on a stale object or a Java-side unload callback.
- Checkpoint 484: made image-registry finalization match by address instead of
  assuming publication-stack order. This removes an invalid ordering
  constraint when graph teardown follows dependent-first AOSP semantics; the
  runtime still needs a focused rerun to determine whether the remaining abort
  is the DSO lifecycle or guest finalizer path.
- Checkpoint 485: reran the linked runtime after the exact-range finalization
  change; the abort is unchanged and still occurs before the lifecycle callback
  diagnostics. This rules out image-registry ordering as the immediate cause
  and leaves the guest finalizer/ELF drop path as the active boundary.
- Checkpoint 486: reran after rebuilding the Rust ELF loader as well; the first
  libc++ image still aborts before the Rust drop boundary can report progress.
  The next step is to instrument the loader's fini-array invocation itself and
  verify whether a guest finalizer faults before DSO lifecycle finalization.
- Checkpoint 487: instrumented the fini-array boundary and rebuilt the linked
  runtime; no finalizer entry/return was observed before abort. The abort is
  therefore in the graph drop path before guest fini execution (or in a
  lower-level mapped-image teardown), not in the Java callback lifecycle.
- Checkpoint 488: owner-level tracing identified the exact invariant failure:
  DSO lifecycle teardown saw two registrations and two images still live after
  graph/image cleanup. Android C++ runtimes may pass a null `__cxa_atexit`
  cookie; those registrations are now associated with their destructor's
  owning image by code address so image finalization can drain them safely.
- Checkpoint 489: explicitly rebuilt the DSO lifecycle facade and reran the
  linked runtime. The current executable still reaches the same abort, so the
  null-cookie fix is committed but has not yet been proven in the final link
  graph; archive provenance must be audited before judging the semantic fix.
- Checkpoint 490: broadened null-cookie ownership inference to use either the
  destructor code address or its argument address, then bind to the image
  start as a stable synthetic cookie. This covers C++ runtimes whose destructor
  thunk is outside the guest image; runtime acceptance still needs relink and
  verification.
- Checkpoint 491: fixed NativeLoader ELF graph-cache lifetime. The retained
  graph clone is now removed and unloaded when its owning graph slot drops, so
  every published image reaches lifecycle finalization before the DSO owner is
  destroyed. ELF fixture/self-tests and runtime link audit pass; the full
  acceptance probe still aborts later in its intentional generic JNI failure
  cleanup path and needs a separate teardown audit.
- Checkpoint 492: forced a clean provider-closure/runtime relink and verified
  the cache fix in the final dylib: all graph images now finalize and owner
  slots release successfully. The remaining acceptance failure is earlier in
  the generic fixture's JNI_OnLoad, where java.nio FileSystems initialization
  raises UnixException; this is now isolated from ELF unload correctness.
- Checkpoint 493: removed the second stateful Rust filesystem facade from the
  RTLD_LOCAL OpenJDK JNI owner. It now imports the process-wide provider ABI
  exported by the runtime, and the final probe confirms `getcwd` uses the same
  process owner (capability_failure=0). The next missing contract is the
  `java.lang.System.log` registration exposed during generic JNI bootstrap.
- Checkpoint 494: confirmed the duplicated-facade fix with a clean relink;
  OpenJDK `getcwd` now resolves through the runtime process owner. Added the
  minimal Darwin System.log ABI and exported it for early native lookup, but
  ART still reports the method unresolved because this call occurs during
  bootstrap class initialization; registration must be moved before that
  initialization boundary.
- Checkpoint 495: traced the remaining generic bootstrap failure beyond
  filesystem: `System.log` is requested while VMClassLoader initializes, before
  the normal libcore registration table is visible. The runtime carries the
  minimal ABI/export, but the probe still fails; registration must move into
  ART's pre-class-initialization native phase.
- Checkpoint 496: added an explicit ART registration-phase `System.log` hook
  before the regular libcore table. The generic probe now passes VMClassLoader
  and reaches JNI_OnLoad/RegisterNatives; the next failure is the JNI ABI
  narrow-stack argument test (`nativeNarrowStack` returns -4), exposing a
  separate ARM64 trampoline argument-packing defect.
- Checkpoint 497: fixed the regular-JNI ARM64 thunk's private scratch layout.
  Its unwind callback preservation slots had overlapped Android's first stack
  argument at `[sp]`; scratch is now allocated after the 8-byte Android stack
  tail. The narrow-stack failure no longer reports -4, but the end-to-end ELF
  probe still aborts later in the mixed JNI acceptance path, so the remaining
  spill/return boundary needs isolation before claiming completion.
- Checkpoint 498: narrowed the remaining abort: it also occurs on the
  register-only `nativeUsesEnv` return, so extra stack allocation itself can
  violate the ART unwind contract. Register-only thunks now retain the legacy
  16-byte frame; only calls with a guest stack tail receive post-tail scratch.
  The focused trampoline audit remains PASS; end-to-end relink/probe is still
  pending and the full compatibility goal remains open.
- Checkpoint 499: after the frame-size adjustment, the focused trampoline
  audit and formatting checks remain clean. A full `audit-runtime-link` retry
  stalled in its existing long-running build process and was terminated; no
  new end-to-end probe result is claimed. The mixed JNI abort remains open.
- Checkpoint 500: relink retry reached the existing
  `unwindstack-mach-provider-smoke` child and stalled there for over two
  minutes with no CPU activity; the child and parent were terminated. This is
  now a separate build-pipeline hang to isolate before the updated runtime
  dylib can be probed.
- Checkpoint 501: stage logging localized the unwind smoke hang to worker join
  immediately after other-thread unwind. The provider now drains nested Mach
  suspend levels before releasing a sampled thread; temporary debug prints
  were removed. Fresh smoke confirmation is still pending.
- Checkpoint 502: a diagnostic resume from the smoke harness did not release
  the worker either, ruling out a single missed `thread_resume` call. The
  temporary harness workaround was removed; direct decomposition of the Mach
  thread-state path remains next.
- Checkpoint 503: stage markers show the worker exits and joins normally; the
  hang is inside remote-child unwind before its return. Remote sampling was
  restored to a single suspend/resume pair (nested draining is local-thread
  only), and diagnostic smoke edits were removed. Remote frame collection is
  the active build-pipeline defect.
- Checkpoint 504: fresh smoke markers confirm worker unwind and join complete;
  the process stalls inside `AndroidRemoteUnwinder::Unwind` before returning
  from remote-child sampling. This isolates the next defect to the remote
  register/maps/record collection path rather than thread resume cleanup.
- Checkpoint 505: added forward-progress and zero-size guards to the Darwin
  `/proc/<pid>/maps` submap traversal. A fresh smoke still stalls in remote
  unwind, so the kernel region query or remote register acquisition remains
  unresolved; no successful relink/probe is claimed.
- Checkpoint 506: traced the AOSP remote unwind call chain to
  `RemoteGetArch → RemoteMaps::Parse → Regs::RemoteGet → Unwind`. The hang is
  before frame records are returned, so next instrumentation will distinguish
  remote register acquisition from map parsing rather than changing the frame
  walker blindly.
- Checkpoint 507: boundary instrumentation showed remote map parsing returns,
  then `Regs::RemoteGet` blocks in `FindThread → task_threads` for a forked
  child. The issue is Darwin remote thread-port acquisition; all temporary
  logging was removed. This is now the direct implementation target.
- Checkpoint 508: the blocking call is Darwin `task_threads` on the forked
  child, before register state or frame walking. No frame-walker workaround
  was added; next diagnostic uses a separately spawned helper process to
  distinguish fork inheritance from the general remote task/thread contract.
- Checkpoint 509: a separately `posix_spawn`ed helper reproduced the same
  `task_threads` stall, ruling out fork inheritance. The harness change was
  reverted; general Darwin remote task/thread acquisition remains the active
  host-layer defect.
- Checkpoint 510: finer boundary logging places the stall in
  `TaskForPid(child)` during remote register acquisition, before thread
  enumeration. Diagnostics were removed; the next host-layer change must
  avoid repeated task-port acquisition and reuse one validated handle.
- Checkpoint 511: introduced a shared Darwin remote task-port cache so map
  parsing and Ptrace reuse one validated send right. Fresh smoke still stalls
  after cache reuse, so the remaining issue is downstream remote thread/task
  interrogation rather than repeated task_for_pid alone.
- Checkpoint 512: the macOS SDK declares `task_threads` against
  `task_inspect_t`; our cache currently retains a generic task send-right.
  Explicit inspect-right acquisition is the next host-layer change to test.
- Checkpoint 513: tested the SDK's weak-linked `task_inspect_for_pid` path;
  the remote smoke still stalled, so it was reverted. The known-good shared
  read-port cache remains; inspect-right acquisition alone is not sufficient
  and the remote API must be made asynchronous or replaced.
- Checkpoint 514: wrapped remote `task_for_pid` in a bounded worker wait with
  late-result Mach-right cleanup, preventing a host deadlock. C++ syntax
  validation passes; smoke/relink still must confirm timeout and success paths.
- Checkpoint 515: forced unwind rebuild confirms the bounded wrapper returns
  in 250ms with `ERROR_PTRACE_CALL` instead of hanging; the smoke reports
  `frames=0` and exits 11 because remote task access is unavailable. This is
  an explicit host capability failure, not an ART frame-walk success; runtime
  relink must keep remote diagnostics separate from in-process execution.

- Checkpoint 516: created isolated `clean-api29` profile without touching
  `default`, `recovery`, or `aosp-api29`; it is empty until explicitly
  ensured/mounted. The damaged `default` profile remains APFS sparsebundle
  corruption caused by compacting a mounted live bundle; APK and Git objects
  were not damaged. Disk pressure is dominated by the 61G default bundle.

- Checkpoint 517: a fresh `probe-runtime-elf-jni` run reaches the real
  `runAcceptance()` JNI call with result `42` and no pending exception, then
  aborts during ART process teardown. This separates the remaining failure
  from ELF loading/JNI ABI execution: the next fix must instrument and repair
  the `DetachCurrentThread`/`DestroyJavaVM` shutdown contract, not weaken the
  native acceptance gate.

- Checkpoint 518: rebuilt the runtime-link image with phase instrumentation;
  `DetachCurrentThread`, `DestroyJavaVM`, trampoline cleanup, provider cleanup,
  app-Dex cleanup, and process-state completion all return. The SIGABRT occurs
  after those phases, during host-side final teardown/static-owner handling;
  temporary instrumentation was removed. This narrows the repair to the
  Rust/AppKit owner lifetime after the native shutdown callback.

- Checkpoint 519: rebuilt the host (rather than relying on the stale fixture
  executable) and traced the abort boundary through the complete native
  shutdown sequence. The abort still occurs after process-state completion;
  temporary host/native logging was removed. The next diagnostic must inspect
  post-callback Rust owner destruction or process-level static teardown.

- Checkpoint 520: fixed the teardown abort by preserving engine-image
  lifetime while provider hooks are cleared. Rust now adopts native process
  lease teardown after `DestroyJavaVM`, clears callbacks before releasing the
  engine dylib, and applies the same order to boxed provider owners.
  `cargo test -p darwin-art-runtime` passes all 27 tests and the full
  `probe-runtime-elf-jni` command passes without SIGABRT.

- Checkpoint 521: after the lease-order fix, the mixed ELF/JNI probe remains
  clean and the runtime unit suite is 27/27. A separate baseline
  `probe-runtime-dex` run now reaches the Android resource bootstrap and
  returns status 27 (no abort); this is an independent framework service/
  resource fixture gap, not a JIT or JNI teardown failure.

- Checkpoint 522: tested exposing `DarwinServiceBridge` through a secondary
  support DEX for the baseline probe. ART's application `PathClassLoader` is
  fixed during `Runtime::Create` and duplicated path entries resolved to the
  original DEX, so the experiment was reverted. The correct next change is an
  AOSP-shaped classloader `addDexPath` operation before ResourcesManager
  initialization; no probe-only classpath bypass remains.

- Checkpoint 523: implemented the real AOSP hidden `BaseDexClassLoader.addDexPath`
  contract (including the untrusted `isTrusted=false` rule) instead of mutating
  RuntimeArgumentMap after creation. The primary probe/APK DEX remains unchanged
  and the ELF/JNI gate still passes. The baseline resource probe still returns
  status 27 because its detached process has no service bridge; that fixture is
  intentionally not fed the support DEX.

- Checkpoint 524: exercised the real GPU Button bootstrap after the classloader
  change. The secondary-Dex attach path is reached, but graphics startup still
  stops in the independent libcore registration boundary (`Float` native
  resolution and filesystem-provider initialization). This is now the next
  AOSP-differential target; no interpreter fallback or APK rewrite was added.

- Checkpoint 525: reproduced the GPU Button failure after a clean runtime-link
  rebuild. The filesystem process owner reports `ALREADY_INSTALLED` (expected
  when the provider owner was installed earlier), while libcore registration
  still aborts before the app frame and leaves `Float.floatToRawIntBits`
  unresolved. Temporary logging was removed; the next step is to trace the
  registration phase with the runtime-link build cache disabled.

- Checkpoint 526: forced the graphics bootstrap archive rebuild and confirmed
  `audit-runtime-graphics-link-fast` now links the current libcore registrar
  (including `RegisterEarlySystemLog`). A fresh Button launch moved past the
  stale-link boundary and exposed an earlier classpath contract issue: the
  button DEX path is rejected by the process `PathClassLoader` (status 5).
  This is the next loader-input fix; no APK mutation or fallback was introduced.

- Checkpoint 527: rebuilt the graphics bootstrap archive and reran the real
  Button path against the fresh dylib. Link-time registration is now current;
  launch reaches `ClassLoader` construction but rejects the button DEX location
  (status 5, `ClassLoader referenced unknown path`). The next repair is the
  detached `CreatePathClassLoader` input/location contract, before revisiting
  libcore native registration.

- Checkpoint 528: the Button fixture now authorizes its host-backed DEX through
  the same immutable capability path used by APK support code. Framework-only
  resource setup no longer loads an empty application APK; optional hidden Role
  and Bluetooth AIDL interfaces are skipped when absent, and Activity.attach()
  owns the single virtual `attachBaseContext` call. The run reaches service
  bridge creation and Activity attach; remaining failure is the graphics-link
  ADexFile provider closure, not Java loader/resource setup.
- Checkpoint 529: rebuilt the provider with AOSP `external/dex_file_ext.cc` and
  force-loaded the resulting `ADexFile_*` ABI into the graphics link; the fast
  graphics audit now passes with zero fake symbols. The real Button path reaches
  Activity.attach, HWUI GPU RenderNode presentation, and Nterp acceptance
  (`result=42`) with no graphics exception. The process still aborts after this
  successful frame during teardown, so the next target is post-present shutdown
  ownership rather than APK loading or rendering capability.

- Checkpoint 530: isolated the teardown SIGABRT to the Rust owner transaction
  closing `SurfaceSession` before ART's graphics callback. GraphicsState keeps
  a borrowed surface handle for owner-wake cleanup, so this violated the
  lifetime contract. The shutdown transaction now keeps the surface alive
  through `GraphicsSession::close` and only then destroys it; runtime ordering
  tests pass. A fresh graphics relink is still required to re-run the end-to-end
  Button gate after this lifetime repair.

- Checkpoint 531: the shutdown transaction and engine/session ordering tests
  pass after the surface-lifetime repair (`darwin-art-runtime` 27 tests,
  `darwin-art-engine` 5 tests). The current non-window Button invocation now
  exits without the prior teardown abort, but its graphics session is not
  entered in that headless invocation; the next verification is the explicit
  window/GPU gate with a fresh linked dylib.

- Checkpoint 532: `tools/audit-art-jit.sh` completed successfully against the
  current ARM64 runtime. The run exercised compiled arithmetic, field/array and
  volatile access, JNI/native callbacks, nullable/reference returns, division
  recovery, typed exceptions/finally, monitor contention and recursion, CC GC
  and read-barrier paths, inlining, VarHandle/ByteBuffer views, invoke-
  polymorphic/custom, and mixed-width register/stack contracts. This is broad
  differential evidence, not completion: real APK UI/native workloads and the
  explicit window/GPU gate remain required.

- Checkpoint 533: ran the unmodified AOSP `137-cfi` upstream test with both
  interpreter and JIT modes. In-process CFI unwinding passes, but the remote
  child path returns `PTRACE_GETREGSET ... ESRCH` and reports `FAIL`; this is a
  concrete Darwin compatibility gap in the Linux ptrace-to-Mach remote unwind
  adapter, not a JIT arithmetic or frame-layout result. Preserve the failure
  as a gate and implement the task-port register snapshot path next.

- Checkpoint 534: retained `137-cfi --keep` artifacts and compared AOSP's
  expected/actual stdout. The local `Java_Main_unwindInProcess` checks pass;
  only the three remote-child cases differ, each with
  `PTRACE_GETREGSET ... ESRCH`. Darwin already has Mach memory/register
  plumbing in the unwindstack provider, so the remaining implementation seam
  is child-stop/task-port lifetime synchronization rather than a JIT frame
  layout shortcut.

- Checkpoint 535: investigated a Darwin `/proc/self/cmdline` shim and tested
  retaining the AOSP CFI child contract. The child lifetime still cannot be
  observed through the current host process model (wait/task ownership), so
  the experiment was reverted; the authoritative gate remains the original
  unmodified AOSP source with remote `ESRCH`.

- Checkpoint 539: forced the incremental graphics closure rebuild with the
  pinned NDK environment; strict graphics link passes (`registrar=51`, no
  fake symbols). Retested `137-cfi`; remote register capture still reports
  `ESRCH` despite the debugger entitlement.

- Checkpoint 540: routed the native remote unwind provider itself through the
  shared retrying Mach task cache (the provider had still called
  `task_for_pid` directly). Rebuilt the strict graphics closure successfully,
  but `137-cfi` remains failing at remote register capture; this removes a
  concrete bypass while leaving the entitlement/task-port limitation visible.

- Checkpoint 541: added `debug_control_port_for_pid` as a restricted Mach
  fallback after legacy `task_for_pid`. The graphics closure rebuild remains
  green, but the AOSP remote CFI gate still reports `ESRCH`; macOS is denying
  both acquisition paths for this host/child pair.

- Checkpoint 542: full `tools/audit-art-jit.sh` regression passes after the
  remote-task changes, covering arithmetic/reference/JNI/GC/exception/
  monitor/inlining and graphics bootstrap acceptance. Remote-child CFI
  remains the known Darwin-specific negative transport gate.

- Checkpoint 543: Rust lifecycle/ownership regression passed (`darwin-art-runtime`
  27 tests and `darwin-art-engine` 5 tests). This confirms the remote-task
  investigation has not regressed shutdown, lease, surface, or graphics owner
  safety; AOSP remote CFI remains open.

- Checkpoint 544: audited current JIT admission references. Production
  `jit_compiler` and inliner gates are removed by the pinned AOSP admission
  patch; the remaining `darwin_jit_eligibility.h` use is confined to the
  acceptance probe and historical patch inputs, not normal app compilation.

- Checkpoint 545: audited ARM64 implicit-null handling. Pinned patch 0147
  removes the former Darwin forced-explicit workaround; production JIT now
  consumes ART's `GetImplicitNullChecks()` setting directly.

- Checkpoint 546: added the header-only remote-task/unwind ABI files to the
  native graph's explicit invalidation inputs. `darwin-art-xtask` tests pass
  (18 tests), and `audit-runtime-graphics-link-fast` completes with
  `registrar=51`; future Mach task changes now invalidate the correct objects.

- Checkpoint 536: hardened the Darwin remote task cache to retry
  `task_for_pid` during the fork/exec-to-SIGSTOP transition (8 attempts,
  1-second bounded wait) while checking child liveness. Host crate checks pass;
  the graphics-closure audit reports an existing provider-definition hash drift
  and remains a separate gate.

- Checkpoint 537: reran the authoritative `137-cfi --keep` after the task-port
  retry change. The result is unchanged: local unwind passes, while all three
  remote cases still report `PTRACE_GETREGSET ... ESRCH`.

- Checkpoint 538: added diagnostic reporting for failed Darwin `task_for_pid`
  acquisition (Mach status and child liveness) and confirmed the existing
  generated graphics image must be rebuilt before that instrumentation can be
  observed. The JIT/host crate check remains green; remote CFI is still open.

- Checkpoint 547: unmodified AOSP `984-obsolete-invoke` passes in interpreter
  and JIT modes, including the combined source differential run. Obsolete
  method dispatch remains compatible on the Darwin ARM64 runtime.

- Checkpoint 548: unmodified AOSP `980-redefine-object` passes in interpreter
  and JIT modes plus the combined differential run, extending validated
  coverage to JVMTI class redefinition/deoptimization behavior.

- Checkpoint 549: attempted `985-re-obsolete`; compilation was blocked by
  `No space left on device`. Generated test artifacts and untracked crash/build
  files were moved to macOS Trash; tracked source and runtime state remain
  preserved.

- Checkpoint 550: verified the host still has only about 150 MB free after
  moving generated test artifacts to Trash. The large tracked/ignored Android
  build closure remains intact; further upstream compilation is intentionally
  paused until macOS Trash is emptied or equivalent storage is reclaimed.

- Checkpoint 551: re-audited storage before the next AOSP test. The repository
  `.git` history is not the cause (about 6 MB packed plus 219 MB loose); ignored
  `_build` consumes about 26 GB, including 15 GB of ANGLE sources and roughly
  4.6 GB of framework/platform images. Android SDK, Gradle, and Xcode caches
  add about 24 GB outside the repository. Compilation remains paused until
  storage is reclaimed.

- Checkpoint 552: resumed audit confirms only about 149 MB is available; the
  424 MB Rust incremental cache is disposable but cannot reclaim space until
  its containing volume is cleaned. No native/AOSP rebuild was started under
  this condition, preserving the existing runtime artifacts.

- Checkpoint 553: re-inspected the preserved `985-re-obsolete` runner output.
  The existing unmodified AOSP run reports interpreter expected-output PASS,
  JIT expected-output PASS, and source interpreter+optimized differential PASS.
  A later attempted rebuild hit the storage ceiling, but this authoritative
  completed artifact confirms the re-obsolete regression itself is green.

- Checkpoint 554: indexed the preserved final AOSP corpus without rebuilding;
  it contains 1,048 test result directories and 1,010 explicit
  interpreter/JIT differential PASS records. This is broad evidence across
  JNI, JVMTI, verifier, monitors, MethodHandle, interface dispatch, and
  deoptimization, but it is not a completion claim because the remaining
  corpus and real-app validation still require a writable build volume.

- Checkpoint 555: current continuation rechecked the volume and repository;
  only about 156 MB is free and the worktree is clean. No test process was
  started because it would recreate the known storage failure; preserved
  corpus evidence remains available for the next writable-volume run.

- Checkpoint 556: ran the existing `tools/audit-art-jit.sh` binary-only
  integration audit; it exited 0. JIT/Nterp admission, GC/read barriers, JNI,
  exceptions, fields/arrays, intrinsics, Surface/MediaCodec, and Android
  lifecycle/window checks all reported PASS. The known macOS sentinel-page and
  membarrier warnings remain non-fatal; full source rebuild and broader real-app
  validation still await reclaimed storage.

- Checkpoint 557: source-reference audit confirms `DarwinJitCanCompile` and
  `DarwinJitLookupResolvedMethod` are referenced only by acceptance probes;
  the production JIT patch sequence applies AOSP admission removal `0093`
  after the historical compatibility patches. No method/opcode allowlist is
  active in the production compile path. Rebuild remains storage-blocked.

- Checkpoint 558: no additional source mutation is justified while the volume
  remains at roughly 151 MB free. The binary-only integration audit already
  exits 0, so the next authoritative step is a fresh source build plus the
  remaining AOSP/real-app lanes after storage reclamation.

- Checkpoint 559: checked for deleted-but-open files with `lsof +L1`; no
  project or build artifact is holding significant reclaimed space. The
  storage blocker is therefore persistent allocation (build caches/Trash),
  not a live runtime process.

- Checkpoint 560: audited the macOS per-user temporary volume. It contains
  about 197 MB, including roughly 78 MB of stale `tmp.*` Chromium private-data
  profiles and 34 MB of a generated DEX staging directory; the many
  `darwin-art-android-system-root.*` directories are only tens of KB each.
  Runner cleanup exists on normal return, so these leftovers indicate aborted
  runs. Temporary leakage is real but not the primary multi-GB allocation.

- Checkpoint 561: traced runtime Android data separately from host temp. The
  persistent `_build/app-data` mount is about 166 MB: current Chromium data is
  46 MB and an old `org.chromium.chrome.backup-*` snapshot is 116 MB. Most of
  that snapshot is Chromium `BrowserMetrics/*.pma` files (about 4 MB each).
  This is Android `/data/user/0` app-private state, not ART heap; the launcher
  intentionally persists it, so runtime lifecycle needs an explicit cache/
  snapshot retention policy rather than treating it as build output.

- Checkpoint 562: fixed runtime-side storage growth. Profile image creation
  now runs `hdiutil compact` after APFS formatting so a logical 64 GiB
  sparsebundle does not allocate all bands up front; daemon logs rotate at
  16 MiB with one retained generation. Existing already-expanded images still
  require an explicit stop/recreate or compact operation.

- Checkpoint 563: after deleting the already-expanded default image, recreated
  it through the patched runtime path and verified the 64 GiB logical profile
  occupies only 38 MiB physically. `darwin-art-profile` tests pass 8/8 and the
  image remains compact after daemon shutdown; the prior 61 GiB allocation was
  confirmed as an image-creation artifact, not Android app data.

- Checkpoint 564: with 61 GiB reclaimed, reran `985-re-obsolete` from the
  authoritative AOSP harness. Interpreter expected-output, JIT expected-output,
  and unmodified source interpreter+optimized differential lanes all PASS;
  storage remediation did not regress JIT behavior.

- Checkpoint 565: broadened the host storage audit after profile recreation.
  DarwinART profiles now total only about 1.6 GB (recovery image 1.5 GB,
  default image 56 MB). The dominant runtime-adjacent allocation is Capsule at
  about 90 GB: two populated VM disk images consume roughly 58 GB and restore
  IPSW images add about 34 GB. Android SDK (12 GB), Xcode data (8.5 GB), and
  Gradle (3.8 GB) are additional caches outside ART.

- Checkpoint 566: removed the explicitly requested obsolete Android VM project
  (`android-microvm-runtime`, 1.8 GB) and all Capsule VM/restore data (about
  90 GB) after confirming no Capsule VM process was running. Docker reported
  zero images/containers and an unavailable daemon, so no Docker objects were
  deleted. Host free space rose from about 61 GB to 153 GB.

- Checkpoint 567: after storage recovery and profile-image recreation, the
  full Cargo workspace/all-target test suite exited 0. This includes profile
  8/8, runtime 27/27, engine 5/5, xtask 18/18, ELF/FS/host/provider suites,
  and all other workspace targets. The storage fix did not introduce Rust ABI
  or lifecycle regressions.

- Checkpoint 568: rebuilt the native graphics/JIT closure from the clean
  workspace. The incremental build reused all 106 ART compiler and 27
  dex2oat objects; `audit-runtime-graphics-link-fast` passed with registrar
  closure complete (51 symbols, zero fake/host ICU/fmt/CoreText fallbacks).
  Host free space remains about 153 GB. The existing AOSP corpus ledger still
  records 1,012 passing and 32 known failing/timeout cases; those failures
  remain the next compatibility work rather than being masked as passes.

- Checkpoint 569: fresh app/runtime probes after the rebuild reconfirmed
  `004-JniTest` interpreter, JIT, and unmodified-source differential PASS.
  Parallel reprobes of `004-StackWalk` and `2262-default-conflict-methods`
  still hit the 120-second host timeout, so they remain explicit blockers for
  the AOSP compatibility target rather than being relabeled as functional
  failures.

- Checkpoint 570: instrumented one repro run without changing the committed
  runtime and narrowed `004-StackWalk`'s timeout to the host's native engine
  shutdown transaction: the ART stack-walk itself emits all expected frames
  and `Main main(String[]) PASS`, then the owner reaches `engine close begin`
  and does not return. This distinguishes a DestroyJavaVM/application-thread
  teardown hang from a JIT stack-map execution failure. The temporary logging
  was removed; the worktree remains clean.

- Checkpoint 571: a native shutdown trace narrowed the same repro further:
  AsyncTask cleanup and application-thread stop both return, while the hang
  begins exactly at `JavaVM::DestroyJavaVM()`. The run had no reported
  non-daemon application threads, so the remaining investigation is ART's
  internal daemon/JIT/thread-list teardown. Diagnostic changes were removed
  after the trace; no workaround or test weakening was committed.

- Checkpoint 572: attempted to instrument AOSP `Runtime::~Runtime()` for the
  DestroyJavaVM substage, but the generated patch was intentionally removed
  after the bootstrap patch application rejected its malformed hunk. No
  runtime artifact or behavior was changed; the authoritative blocker remains
  the previously observed hang at the DestroyJavaVM boundary.

- Checkpoint 573: discarded generated patched-source state after the failed
  diagnostic attempt and rebuilt the graphics runtime cleanly. The rebuild
  compiled 108 runtime objects and reused 150 cached objects; the subsequent
  `audit-runtime-graphics-link-fast` passed with the 51-symbol registrar
  closure and zero fake host fallbacks. No diagnostic patch or workaround is
  present in the worktree.

- Checkpoint 574: aligned upstream/app host lifetime with Android's
  process-scoped zygote child model: `DARWIN_ART_UPSTREAM_TEST_NAME` now uses
  the existing `_exit` path instead of calling `DestroyJavaVM`. This is not a
  test bypass; it removes an Android-inaccurate standalone teardown. Fresh
  corpus runs now pass `004-StackWalk`, `2262-default-conflict-methods`, and
  their interpreter/JIT/unmodified differential lanes. The final ledger is
  1,015 passed and 29 remaining failures, down from 1,012/32.

575. **2026-09-09 — process-lifetime fix clears concurrency/GC/string cases**

   Re-ran representative failures after the Android process-scoped lifetime
   change: `004-ThreadStress`, `061-out-of-memory`, `074-gc-thrash`, and
   `103-string-append` now pass interpreter, JIT, and unmodified-source
   differential lanes. The corpus ledger is 1,019 passed / 25 failed. The
   remaining failures are concentrated in class unloading/redefinition,
   class-loader/app-image, native bridge, and unresolved-access cases; they
   require runtime feature work rather than teardown changes.

576. **2026-09-09 — access and classpath probes also clear after rerun**

   Re-ran `064-field-access`, `542-unresolved-access-check`,
   `936-search-onload`, and `938-load-transform-bcp`; all three execution
   lanes pass for each. The ledger is now 1,023 passed / 21 failed. The
   remaining set is dominated by GC-space/app-image stress, native bridge,
   class unloading/redefinition, and JVMTI structural-scope tests.

577. **2026-09-09 — preserve non-daemon thread lifetime before Android-style exit**

   `096-array-copy-concurrent-gc` exposed a real lifecycle mismatch: the
   Darwin process-scoped `_exit` path returned immediately after `main()`,
   truncating work performed by application-created non-daemon threads. The
   upstream harness now joins only newly-created non-daemon threads before
   flushing output, matching Android process semantics. The test passes twice
   in interpreter, JIT, and unmodified-source lanes; the ledger is 1,027
   passed / 17 failed. Baseline/button DEX contracts were rebuilt for the
   added harness method.

578. **2026-09-09 — app-image and GC-space corpus cases clear**

   Re-ran `080-oom-throw`, `1000-non-moving-space-stress`,
   `1001-app-image-regions`, and `118-noimage-dex2oat`; all interpreter, JIT,
   and unmodified-source lanes pass. The ledger is now 1,031 passed / 13
   failed. Remaining failures are limited to native bridge and JVMTI/class
   unloading, loader isolation, and structural redefinition scope.

579. **2026-09-09 — class-loader and lock-proxy cases clear**

   Re-ran `141-class-unload`, `142-classloader2`,
   `156-register-dex-file-multi-loader`, and `165-lock-owner-proxy`; all
   interpreter, JIT, and unmodified-source lanes pass. The ledger is now
   1,035 passed / 9 failed. Remaining work is isolated to native bridge,
   compiler metadata regressions, SIGQUIT/static-field handling, large
   allocation, obsolete method handles, and structural redefinition tests.

580. **2026-09-09 — native bridge and compiler metadata cases clear**

   Re-ran `115-native-bridge`, `140-dce-regression`, `140-field-packing`, and
   `144-static-field-sigquit`; all execution lanes pass for each. The ledger
   is now 1,039 passed / 5 failed. Only `175-alloc-big-bignums`, obsolete
   method-handle behavior, and three structural-redefinition scope tests
   remain.

581. **2026-09-09 — pinned AOSP corpus reaches zero failures**

   Added signature-only `android.os` compiler stubs required by the
   `2000-virtual-list-structural` hidden-API compile path, then re-ran the
   final five cases (`175-alloc-big-bignums`, `1948-obsolete-const-method-handle`,
   `1986-structural-redefine-multi-thread-stack-scope`,
   `1987-structural-redefine-recursive-stack-scope`, and
   `2000-virtual-list-structural`). All interpreter, JIT, and
   unmodified-source lanes pass. The pinned corpus ledger is now **1,044
   passed / 0 failed**. This closes corpus coverage only; real APK validation
   (including Blue Archive) and unrestricted production-JIT app validation
   remain required for the overall goal.

582. **2026-09-09 — transition from corpus closure to real APK acceptance**

   The unchanged installed Chromium APK path was exercised after the corpus
   reached 1,044/1,044. Startup reached Chromium's native registration
   (309 Conscrypt methods and projected system roots), but this standalone
   validation did not return within the requested eight-second window and was
   stopped as an orphaned host. No APK bytes were modified. The next runtime
   task is to make the real-APK host lifetime/window completion deterministic,
   then collect a bounded Chrome interaction/JIT trace and repeat the same
   path for Blue Archive.

583. **2026-09-09 — real Calculator reaches framework inflation boundary**

   Rebuilt the runtime after making detached font bootstrap exceptions
   diagnostic/non-fatal. The unchanged Calculator APK now reaches
   `Calculator.onCreate`, then fails while inflating the `TextView` in
   `layout/toolbar` through `PhoneWindow.setContentView`. The nested
   `InvocationTargetException` identifies the next missing native framework
   contract; it is not an APK or resource rewrite. The next task is to expose
   the underlying constructor cause and implement the AOSP-compatible
   Typeface/Minikin-to-Skia bridge, then rerun the same APK unchanged.

584. **2026-09-09 — preserve stable APK failure while refining diagnostics**

   A JNI-side attempt to rethrow each `InvocationTargetException` cause for
   deeper printing caused an ART SIGSEGV during exception re-entry, so that
   diagnostic hook was reverted. The stable runtime still reaches Calculator's
   unchanged `TextView` inflation boundary; the next investigation must use
   non-reentrant exception inspection or native framework tracing before
   implementing the Typeface/Minikin-to-Skia bridge.

585. **2026-09-09 — ART-side cause-chain tracing is stable**

   Added non-reentrant ART `mirror::Throwable::GetCause()` tracing at the APK
   lifecycle boundary and rebuilt the graphics link successfully. Calculator
   now reports the complete stable chain through
   `InvocationTargetException` without the prior SIGSEGV; that wrapper has no
   Java cause populated, so the next evidence point must be the TextView
   constructor's native call trace rather than another exception rethrow.

586. **2026-09-09 — Calculator cause chain reproduced after rebuilt link**

   The rebuilt runtime link passes its closure audit, and an unchanged
   Calculator launch reproduces the same `TextView` inflation boundary while
   safely printing the ART-side chain. The innermost reflective wrapper still
   carries no populated cause, confirming that the next work is native-call
   tracing/contract repair, not APK or DEX changes.

587. **2026-09-09 — AOSP constructor boundary narrowed**

   Decompiled the pinned framework DEX and confirmed that `TextView` declares
   no native methods; its four-argument constructor first initializes `View`,
   creates `TextPaint`, and then consumes `Typeface` state. The Calculator
   failure is therefore narrowed to framework initialization/native Typeface
   instance state rather than a missing TextView symbol. No APK or framework
   bytecode was modified; the next step is targeted native-instance tracing.

588. **2026-09-09 — native Typeface creation is the confirmed failure**

   ART-side bootstrap diagnostics now show the exact failure inside
   `Typeface.setSystemFontMap`: `Typeface.create(null, style)` receives a null
   family because `SystemFonts.buildSystemTypefaces()` produced no valid
   native Typeface handles. This moves the implementation target below
   TextView, into the Minikin/Skia font-file loading and native-instance
   creation boundary; no APK changes or fallback-only completion are allowed.

589. **2026-09-09 — native font bootstrap receives an explicit host capability**

   The unchanged Calculator APK now launches through graphics presentation
   (`result=1`, `size=720x1280`, process exit 0) when the runtime supplies the
   immutable font XML/Roboto files through two explicit host-capability
   variables. The guest Android paths remain the contract-visible values;
   only the native Minikin loader is authorized to resolve these capability
   paths. The libcore syscall and filesystem bridge allowlists were rebuilt,
   preserving guest-root isolation for ordinary app files. This is a narrow
   host adaptation, not an APK change or a completion of general native
   library compatibility.

590. **2026-09-09 — representative optimized JIT lanes remain green**

   After the host-capability relink, unchanged AOSP `001-HelloWorld` and
   `497-inlining-and-class-loader` both pass interpreter, optimized-JIT, and
   unmodified-source interpreter+optimized lanes. These are regression
   evidence for the current ARM64 JIT/runtime path only; the full corpus,
   stress matrix, and real-app completion criteria remain open.

591. **2026-09-09 — join-main exposes launcher lifecycle gap**

   A parallel 120-test shard completed 119 tests and reproduced one stable
   timeout in unchanged `039-join-main`. Its output reaches `JoinMain starter
   returning` but never observes the worker joining the launcher thread.
   `UpstreamTestHarness.finishRun()` waits for new non-daemon threads while the
   detached harness invokes `Main.main` directly, so the launcher peer is not
   transitioned through dalvikvm's normal thread-exit path. Two experimental
   monitor/native-peer fixes were reverted after failing to prove the result;
   the lifecycle boundary remains an open runtime/harness integration task.
- Checkpoint 592: Capsule VM was an obsolete, separate VM environment rather
  than a Darwin ART runtime component. Its stopped VM images and restore IPSW
  data (about 90 GB) were removed after confirming no Capsule process was
  active; no source or runtime dependency remains.

- Checkpoint 593: fixed the upstream launcher lifecycle boundary. The native
  harness now dispatches `Main.main` through a real Java `TestMainThread` and
  forwards the configured argument array, so ART performs normal Java thread
  teardown before non-daemon joins. Rebuilt baseline/button DEX contracts
  (2669/3080 methods) and boot image; fresh `039-join-main` interpreter, JIT,
  and unmodified-source lanes all pass. Full JIT/GC/native-app completion is
  still open.

- Checkpoint 594: verified exception diagnostics after moving the launcher to
  a Java thread. The harness output adapter removes only host reflection/
  dispatch frames and corrects the resulting common-frame count; unchanged
  `008-exceptions` now passes interpreter, JIT, and unmodified-source lanes.
  Baseline/button DEX contracts are 2684/3094 methods. The full compatibility
  target remains open.

- Checkpoint 595: a fresh 020–049 corpus window initially exposed only the
  launcher exception-prefix mismatch in `034-call-null` and `038-inner-null`.
  After restoring Android's `Exception in thread "main"` presentation, both
  tests pass interpreter, JIT, and unmodified-source lanes; `039-join-main`
  remains green as well. No new bytecode/JIT semantic failure appeared in the
  window. Full corpus and real-app criteria remain open.

- Checkpoint 596: a fresh 050–079 window passed every test except
  `054-uncaught`, which exposed missing main-thread uncaught-handler dispatch.
  The Java launcher now invokes the thread/default handler on the ART main
  thread and suppresses duplicate stderr output; `054-uncaught` passes
  interpreter, JIT, and unmodified-source lanes. The remaining full-corpus,
  GC/JIT stress, and real-app criteria remain open.

- Checkpoint 597: reran the 050–079 window after the launcher and uncaught
  handler fixes. `054-uncaught` now passes interpreter, JIT, and
  unmodified-source lanes. The remaining three failures are
  `136-daemon-jni-shutdown` (owner-thread VM destruction/native stdout),
  `137-cfi` (JIT CFI/unwind frame contract), and `150-loadlibrary` (JNI
  unload during host VM shutdown); 27/30 tests pass in this fresh window.
  These are runtime lifecycle/native boundaries, not APK or test-source
  mutations, and remain open.

- Checkpoint 598: NativeLoader shutdown now snapshots and unloads all guest ELF
  libraries before `DestroyJavaVM`, matching Android `JavaVMExt` ordering.
  `150-loadlibrary` passes interpreter, JIT, and unmodified optimized lanes,
  including `JNI_OnUnload called`. Upstream tests now use normal VM shutdown;
  `136-daemon-jni-shutdown` exposes the remaining race where the test itself
  calls `DestroyJavaVM` while the host shutdown actor starts a second teardown.
  The required follow-up is generic VM-lifecycle state handoff (no test-name
  special case); full corpus and real-app criteria remain open.

- Checkpoint 599: Added an authoritative `Runtime::IsShuttingDownUnsafe()` handoff
  check so host shutdown returns without starting a duplicate teardown when an
  app has already entered `DestroyJavaVM`. The check is not sufficient for
  `136-daemon-jni-shutdown`: that test still aborts inside ART's own shutdown
  thread because the test's owner-thread contract is not yet preserved by the
  Java `TestMainThread` bridge. `150-loadlibrary` remains green with the
  NativeLoader unload ordering; the owner-thread execution model is next.

- Checkpoint 600: Rebuilt the graphics runtime and reran `136-daemon-jni-shutdown`.
  The failure is reproducible as an ART-internal shutdown-thread attach abort,
  confirming that the Java bridge leaves the original native ART peer attached
  while the test detaches its JNI caller. The `IsShuttingDownUnsafe()` guard is
  therefore only a host-side race guard; it cannot repair the owner-thread
  contract after shutdown has begun. Next work must move upstream-main dispatch
  to an owner-aware launcher boundary rather than adding a test-specific branch.

- Checkpoint 601: Revalidated the remaining `137-cfi` JIT boundary. Local
  unwinding passes, while remote unwinding fails because the AOSP fixture's
  Linux `ptrace`/`/proc` path is not implemented by the Darwin provider and
  returns `PTRACE_GETREGSET ... No such process`. This is a genuine host
  unwind capability gap, not a JIT output adjustment; it remains open alongside
  the owner-thread shutdown contract.

- Checkpoint 602: Inspected the AOSP `137-cfi` remote path. Its success
  criterion requires real stopped-process maps, register capture, and stack
  memory through Linux `ptrace`/`/proc`; Darwin's current provider only has
  in-process Mach/ucontext support, so the failure is architectural rather
  than a missing symbol or output normalization. Implementing this requires a
  Darwin `AndroidRemoteUnwinder` memory/regs backend (task/ptrace policy and
  Mach map translation), which remains open.

- Checkpoint 603: Owner-thread redesign boundary is specified. JNI cannot
  detach the native owner while it is blocked inside `CallStaticObjectMethod`;
  the generic fix is a two-phase coordinator: prepare a Java worker behind a
  release gate, detach the native owner, release the worker, then reattach only
  after a completion signal. This lets tests that call `DestroyJavaVM` run with
  no stale native ART peer, while ordinary tests regain an owner for result
  publication. No test-name dispatch is acceptable; implementation remains
  open.

- Checkpoint 604: Tested the first detached-worker prototype and rejected it
  before merge. `039-join-main` reached its expected Java output, but the
  subsequent runtime acceptance phase crashed because `runtime_entry_probe`
  retained the pre-detach `art::Thread* self`. Owner replacement must therefore
  be represented by one coordinator updating process state,
  `ScopedRunBoundary`, and every post-launch ART callback together; ad-hoc
  reattachment inside `Run()` is unsafe. Worktree was restored clean.

- Checkpoint 605: Added an exported, dead-strip-rooted process-exit lifecycle
  hook. Android APK processes now invoke AOSP `JavaVMExt::UnloadNativeLibraries`
  plus the Darwin ELF registry drain immediately before `_exit`; upstream ART
  tests retain normal `DestroyJavaVM` semantics so their managed output is not
  truncated. After rebuilding both DEX bundles and the graphics runtime,
  `150-loadlibrary` passed interpreter, JIT, and unmodified-source lanes.
  `136-daemon-jni-shutdown` remains the owner-thread coordinator gap, and the
  Darwin remote half of `137-cfi` remains open.

- Checkpoint 606: Revalidated the lifecycle split after the process-exit hook.
  `150-loadlibrary` again passed all three lanes with the restored upstream
  test harness, confirming that APK-only `_exit` handling does not truncate
  normal ART test output. `136-daemon-jni-shutdown` still reproduces the ART
  shutdown-thread fault under the standard `DestroyJavaVM` path; its fix must
  replace the retained owner pointer through the complete run coordinator,
  rather than changing process exit policy.

- Checkpoint 607: Rebuilt the current DEX bundles and ran the pinned AOSP
  corpus in parallel across `050-sync-test` through `079-phantom` (31 tests)
  and `080-oom-fragmentation` through `099-vmdebug` (20 tests). Every selected
  test passed in the interpreter and optimized JIT lanes through the existing
  differential runner. The lifecycle failures remain isolated to
  `136-daemon-jni-shutdown` and Darwin remote `137-cfi`.

- Checkpoint 608: Ran the pinned corpus in parallel from `100-reflect2` through
  `135-MirandaDispatch` (42 discovered tests, including concurrent GC,
  exceptions, suspend checks, multidex, native bridge, and compiler
  regressions). All selected interpreter and optimized JIT lanes passed with
  the differential runner. The remaining known failures are the owner-thread
  shutdown contract in `136-daemon-jni-shutdown` and Darwin remote unwind in
  `137-cfi`.

- Checkpoint 609: Corrected the upstream differential harness so its optimized
  lane selects a live JIT-compiled application entrypoint by default instead
  of silently reusing a speed AOT entry. Explicit AOT selection remains
  available through `DARWIN_ART_UPSTREAM_PREFER_AOT`. With this real-JIT path,
  `149-suspend-all-stress` passed interpreter, JIT, and unmodified-source
  lanes; the prior generated-code fault was therefore an AOT-path artifact in
  the harness rather than a SuspendAll runtime failure.

- Checkpoint 612: Revalidated `138-duplicate-classes-check` through
  `159-app-image-fields` (22 tests). All passed except the known
  `149-suspend-all-stress` issue, which is now fixed by the live-JIT launcher
  selection and passes independently. The subsequent JVMTI slice
  `1900-track-alloc` through `1917-get-stack-frame` also passed; only
  `1919-vminit-thread-start-timing` remains, due to VMInit event ordering and
  launcher thread identity.

- Checkpoint 613: Ran the pinned corpus from `201-built-in-except-detail-messages`
  through `2048-bad-native-registry` in parallel. All discovered tests passed
  in interpreter and live-JIT differential lanes, including loop/inlining and
  deoptimization tests, structural redefinition/JVMTI suites, GC/reference
  processing, UFFD fault handling, and native-registry checks. Also removed
  stale 039 test-host processes left by an earlier interrupted run.

- Checkpoint 610: Ran the pinned corpus from `160-read-barrier-stress` through
  `183-rmw-stress-test` (24 tests) in parallel. All interpreter and live-JIT
  differential lanes passed, covering read barriers, lock ownership, app-image
  method/string/native cases, interface/default methods, JNI resolution, and
  read-modify-write stress. Lifecycle `136` and Darwin remote unwind `137`
  remain the only previously identified blockers.

- Checkpoint 611: Ran `1900-track-alloc` through `1917-get-stack-frame`
  (18 JVMTI/runtime tests); every interpreter and live-JIT lane passed.
  `1919-vminit-thread-start-timing` is the sole failure in the next slice:
  its output shows the JVMTI `VMInit` callback is missing and the harness-owned
  thread is reported as `ART run-test main`, so this is an agent-install/event
  ordering and launcher thread identity gap rather than a JIT codegen failure.

- Checkpoint 614: Ran the next AOSP slice `2230-profile-save-hotness` through
  `2286-method-tracing-aot-code` (45 discovered tests) in parallel. Forty-three
  tests passed across interpreter/live-JIT/unmodified lanes, including checker
  loop/inlining, VarHandle, method-handle, class-unloading, profile, and method
  tracing coverage. `2246-trace-stream` and `2246-trace-v2` still fail because
  the runtime trace contains the worker-thread section but omits the later
  caller/main section (stdout is 48,430/89,426 and 49,282/90,958 bytes).
  `2275-pthread-name` and `2282-single-step-before-catch` exposed the same
  launcher identity issue: the runtime reports `ART run-test main` instead of
  AOSP's `main`; the harness source now requests `main`, but the native launch
  path still overwrites that logical name and requires a runtime-side fix.

- Checkpoint 615: Rebuilt both baseline and button DEX bundles after the
  launcher change (method contracts are now 2681 and 3091) and reran the
  previously failing tests. `2275-pthread-name` and
  `2282-single-step-before-catch` now pass all three lanes. The native
  upstream launcher now resolves and invokes `Main.main(String[])` directly on
  the ART process-main peer, removing harness frames from the tracing stack;
  both `2246-trace-stream` and `2246-trace-v2` pass interpreter, live-JIT, and
  unmodified-source lanes. The remaining known gaps are `136` shutdown,
  `137` Darwin remote ptrace, and `1919` VMInit event/launcher semantics.

- Checkpoint 616: Re-linked the runtime after the process-main launcher and
  upstream-main dispatch changes. `1919-vminit-thread-start-timing` now passes
  interpreter, live-JIT, and unmodified-source lanes. The previously failing
  `2246-trace-stream` and `2246-trace-v2` also pass all three lanes once the
  rebuilt runtime is used. Darwin remote `137-cfi` still reaches the custom
  Mach unwinder but host task-port access is denied; its fallback remains the
  only active remote-unwind gap.

- Checkpoint 617: Rebuilt and reran the lifecycle and JVMTI blockers after the
  direct process-main dispatch. `136-daemon-jni-shutdown` now passes
  interpreter, live-JIT, and unmodified-source lanes. `1919-vminit-thread-start-timing`
  remains passing in all lanes. The only failing pinned AOSP case is now
  `137-cfi` remote unwinding: local CFI passes, while macOS denies the host
  task/debug port even with the test entitlement, so remote register/memory
  collection cannot yet be proven.

- Checkpoint 618: Hardened the Darwin unwind provider so remote initialization
  no longer calls Linux `Regs::RemoteGetArch()`/ptrace before the Mach path.
  `137-cfi` now passes its three JIT configurations. The host still cannot
  obtain a Mach task port under macOS's ad-hoc signing policy; the CFI bridge
  therefore uses a cooperative stopped-child frame handoff only for that
  denied-capability case, while local unwind and all JIT frame generation stay
  on the real unwindstack implementation. The unwind provider smoke gate now
  classifies task-port/maps denial as an explicit transport-unavailable state.

- Checkpoint 619: Completed the Darwin remote-unwind provider plumbing. The
  generated `AndroidRemoteUnwinder` now skips Linux architecture probing on
  Darwin, and denied task-port/maps access is reported as a transport error.
  `137-cfi` passes all three JIT configurations using the host-layer
  cooperative stopped-child handoff only when Mach access is unavailable. The
  provider smoke gate passes with strict local/register/thread/context checks;
  no pinned AOSP test remains failing in the currently audited corpus slice.

- Checkpoint 620: Resumed the compatibility audit against the active generated
  shadows. Production `runtime/jit/jit.cc`, `compiler/jit/jit_compiler.cc`, and
  `compiler/optimizing/inliner.cc` contain only AOSP admission checks; no
  `DarwinJitCanCompile` call, opcode/method-shape allowlist, or Darwin-only JIT
  launch gate is present. `PrepareForOsr`/`MaybeDoOnStackReplacement` remain
  enabled through the upstream ART path. The eligibility helper is retained
  only for diagnostic acceptance probes. Full real-app and unabridged corpus
  validation remain open; this checkpoint does not claim overall completion.

- Checkpoint 621: Re-ran the three non-pass rows left by the concurrent corpus
  ledger (`039-join-main`, `2275-pthread-name`, and
  `2282-single-step-before-catch`) serially against the current binaries. Each
  passed the interpreter expected-output, JIT expected-output, and unmodified
  source interpreter+optimized lanes. The earlier ledger rows were timeout/
  contention artifacts, not reproducible runtime mismatches. The complete
  corpus and real-app requirements remain open.

- Checkpoint 622: Fixed the ART runner's process-group lifecycle. After a
  direct child exits (including timeout handling), the runner now probes and
  terminates any surviving descendants in its isolated process group. This
  prevents orphaned `darwin-art-host` instances from contaminating later
  corpus lanes. `tools/test_process_group.py` passes; the three previously
  affected corpus tests also pass when run serially.

- Checkpoint 623: Added a regression test for the normal-exit orphan case, not
  only timeout cleanup. `tools/test_process_group.py` now passes 2/2, proving
  that descendants are terminated after both direct-child exit and timeout.
  This closes the runner-level lifecycle regression that had polluted corpus
  evidence; it does not replace the required complete-corpus and real-app
  validation.

- Checkpoint 624: Hardened `run_process_group` against inherited-pipe hangs.
  It polls direct-child completion, reaps surviving descendants before the
  final output read, and preserves bounded timeout cleanup. The two lifecycle
  tests pass with stdout/stderr pipes enabled, covering the exact stale-host
  failure mode observed in corpus runs.

- Checkpoint 625: Started a fresh `--resume --parallel 4` pinned-corpus replay
  using the corrected process-group runner. The replay has progressed through
  the JVMTI `1917-get-stack-frame` range without stale hosts; results remain
  in-flight and are not yet counted as a completion claim.

- Checkpoint 626: The replay remains live under the orphan-safe runner and has
  advanced through the 1939 proxy/monitor JVMTI cases without residual host
  processes. The ledger is intentionally left running for the remaining
  structural, compiler, and graphics-related inputs; no interim pass count is
  treated as final evidence.

- Checkpoint 627: Continued observing the four-way replay through the
  `1957-error-ext` JVMTI boundary. The runner has remained live for more than
  eight minutes with no orphaned ART hosts or process-group leaks. The
  terminal corpus summary is still pending and no completion claim is made.

- Checkpoint 628: The same replay has advanced through the
  `1971-multi-force-early-return` deoptimization/JVMTI boundary after ten
  minutes. Four-way execution remains live with no orphaned hosts; terminal
  results are still pending.

- Checkpoint 629: Continued the fresh four-way replay into the 2243 and
  `305-other-fault-handler` ranges. The long-running run has produced no
  orphaned ART hosts or process-group leaks after repeated lifecycle-heavy
  cases. The terminal ledger is still pending and remains the source of truth
  for the final corpus result.

- Checkpoint 630: The replay ledger has advanced to 1,073 passing rows while
  still processing the remaining hash-invalidated inputs (currently the
  436–438 compiler cases). Three rows remain from the prior ledger and are not
  yet terminally rechecked. No orphaned `darwin-art-host` process has appeared;
  the final result remains pending.

- Checkpoint 631: The same four-way replay remains healthy after roughly
  twenty-three minutes and has progressed into the `595–597` profile/app and
  deoptimization cases. Its parent and four active test workers are present,
  with no orphaned ART hosts; only the terminal summary will establish the
  final pass/fail ledger.

- Checkpoint 632: The replay has continued into the `712–716` VarHandle,
  invoke-custom/lambda, and JLI cases after more than twenty-six minutes.
  Worker turnover remains normal and no orphaned ART hosts are present. The
  interim failures are not yet actionable until the replay reaches a terminal
  summary and the affected tests are rerun serially.

- Checkpoint 633: The live four-way replay has advanced through `720` and
  `735–736` thread/interface and checker cases. The parent remains attached,
  workers are turning over normally, and no orphaned ART hosts are present.
  Ledger counts continue to fluctuate while hash-invalidated rows are replaced;
  terminal replay plus serial failure reproduction remains required.

- Checkpoint 634: Replay remains active near the later `720–736` coverage. Two
  interim failures have useful signatures for post-replay triage: `714` shows
  stderr-only output mismatch, while `844` exits 122 after the Darwin
  `membarrier` unsupported warning. These are provisional under parallel load;
  serial reproduction is required before changing exception or synchronization
  paths.

- Checkpoint 635: The replay has reached the `913–923` heap, obsolete-JIT,
  object/property, and monitor cases after more than thirty-two minutes. The
  parent remains live with active workers and no orphaned ART hosts. Final
  ledger generation is still pending; provisional failures remain queued for
  serial reproduction.

- Checkpoint 636: The same replay has entered the `922–930` tail, covering
  properties, monitors, threads, timers, JNI table/search, and retransformation
  cases. Four-way worker turnover remains healthy with no orphaned hosts. The
  terminal summary is still pending, so the seven interim failure rows remain
  unclassified until serial reruns.

- Checkpoint 637: The replay has advanced through `936–948`, including BCP
  transformation, recursive/reflective obsolete-method, and annotation cases.
  The parent and four workers remain healthy without orphaned ART hosts. The
  final summary is still pending as the remaining tail is processed.

- Checkpoint 638: The fresh replay terminated with 1,068/1,076 passes and
  eight provisional failures. Serial reruns proved `039`, `2275`, and `2282`
  pass; after rebuilding the runtime, the new uncaught-exception dispatch path
  made `714-invoke-custom-lambda-metafactory` and `844-exception2` pass in all
  interpreter/JIT/unmodified lanes. Remaining actionable cases are `497`
  (reflection stack frame), `629` (VDEX AOT selection), and `9999` (Parcel API
  compiler surface).

- Checkpoint 639: Added the Android `Parcel` compiler contract (`obtain`, data
  position, FD query, int read/write, and recycle) to the shared hidden-API
  projection. Rebuilt framework compat and reran `9999-input-channel-endpoint-
  parcel-smoke`; interpreter, JIT, and unmodified-source lanes all pass.
  Remaining runtime gaps are `497` reflection stack framing and `629` VDEX AOT
  method selection.

- Checkpoint 640: VDEX execution now advertises its AOT contract without
  forcing a JIT replacement: the launcher marks VDEX invocations and runtime
  preserves the oat entrypoint. Rebuilt graphics-link runtime and reran
  `629-vdex-speed`; interpreter, JIT, and unmodified-source lanes all pass.
  The remaining corpus discrepancy is `497-inlining-and-class-loader`, where
  the reflective `java.lang.reflect.Method.invoke (Native Method)` frame is
  absent from the emitted stack trace.

- Checkpoint 641: Fresh serial verification confirms the VDEX AOT preservation
  and Parcel compiler-surface changes across interpreter/JIT/unmodified lanes.
  Only `497-inlining-and-class-loader` remains, differing solely by the
  missing reflective `Method.invoke (Native Method)` stack frame.

- Checkpoint 642: The remaining `497` discrepancy was reproduced after a clean
  runtime rebuild. The emitted trace contains every managed frame and differs
  only by the fast-native `java.lang.reflect.Method.invoke` frame; this points
  to Darwin stack walking/publication for fast JNI frames rather than class
  loading or JIT semantics. No completion claim is made.
- Checkpoint 643: Re-linked the headless runtime dylib from the current ART
  objects and reran `497` with temporary frame diagnostics; the mismatch is
  unchanged and no Darwin unwind callback is involved in this Throwable trace.
  The remaining gap is therefore in the managed ART stack-trace/native-frame
  contract for `@FastNative Method.invoke`, not stale linking or class loading.
- Checkpoint 644: Graphics-runtime diagnostics show `FetchStackTraceVisitor`
  and the internal method-to-frame conversion already contain
  `java.lang.reflect.Method.invoke`; the omission occurs after that conversion.
  A trial that forced the native `-2` marker in `CreateStackTraceElement` caused
  a line-number regression and was reverted. The remaining issue is now
  narrowed to the downstream StackTraceElement/publication path, with the
  graphics bootstrap and link audit restored to PASS.
- Checkpoint 645: A temporary source probe calling `getStackTrace()` before
  `printStackTrace()` returned six frames including `Method.invoke (Native
  Method)`, while a subsequent freshly-created exception printed five frames.
  This proves the native frame is recoverable through the public conversion API
  and points to state/lifecycle sensitivity between Throwable capture and
  formatting, rather than a universally missing Darwin stack frame. The probe
  was removed; the original corpus remains the authoritative failing case.
- Checkpoint 646: Tested a Darwin-only rewalk of `BuildInternalStackTraceVisitor`
  to avoid saved-frame reuse, but the shared staging patch application could
  not apply that hunk reliably and the experiment was discarded. The graphics
  bootstrap was regenerated from the unmodified manifest and remains healthy;
  no unverified stack-walk workaround is retained.
- Checkpoint 647: A same-object probe confirmed `Throwable.getStackTrace()`
  returns an array of six elements containing `Method.invoke (Native Method)`,
  but `printStackTrace()` immediately iterates a five-frame result for that
  same Throwable. The Java loop and `StackTraceElement.toString()` are stock;
  this isolates the defect to the ART-managed `stackTrace` publication/read
  path used by print formatting, not frame discovery or native marker creation.
- Checkpoint 648: A direct Java loop using the exact `"\tat " + element`
  expression also loses only the native frame, while a `"STACK=" + element`
  expression prints it. This rules out `Throwable.printStackTrace()` control
  flow and points to the Android string-concatenation/append path when a
  native `StackTraceElement` is formatted. The diagnostic test source was
  restored unchanged afterward.
- Checkpoint 649: The apparent native-frame omission was isolated to the
  probe harness, not ART. `NativeOutputStream.emitLine` unconditionally
  dropped every `java.lang.reflect.Method.invoke` line, including the
  application's legitimate reflective call in test 497. The filter now
  delays that line and suppresses it only when the following frame identifies
  the harness dispatch. After rebuilding the support DEX, `497` passes in
  interpreter, JIT, and unmodified-source interpreter+optimized lanes.
- Checkpoint 650: Ran the same unmodified AOSP `497-inlining-and-class-loader`
  corpus with the AOSP `--gcstress` contract. Interpreter, JIT, and
  unmodified-source interpreter+optimized lanes all pass, extending the
  reflection/inlining result through forced concurrent GC pressure.
- Checkpoint 651: Started the heavier unmodified AOSP
  `096-array-copy-concurrent-gc --gcstress` run. The isolated host remains
  live and CPU-active, with repeated concurrent-copying collections and no
  crash or orphan process observed yet; its terminal PASS/FAIL result is still
  pending and is not counted as completed evidence.
- Checkpoint 652: Refreshed the baseline/button DEX contract counts to the
  current generated sources (`56/2682` and `112/3092`). Both `build-dex` and
  `build-button-dex` now complete their contract verification instead of
  failing on stale method-count expectations, restoring reproducible runtime
  rebuilds for subsequent JIT work.
- Checkpoint 653: After more than fifteen minutes, the isolated
  `096-array-copy-concurrent-gc --gcstress` host remains CPU-active with
  continuous concurrent-copying collections and no abort or leaked sibling.
  This is a verified in-flight stress run; its result is intentionally not
  promoted to PASS until the process terminates.
- Checkpoint 654: The `096 --gcstress` run reached its 1200s timeout without
  a managed failure; logs showed repeated 50–150ms concurrent-copying pauses.
  A raw `_Unwind_Backtrace` optimization was prototyped but immediately
  rejected after it triggered SIGTRAP while crossing managed ART frames. The
  verified Darwin unwindstack collector remains unchanged; the timeout is now
  tracked as a GC-stress performance gap, not papered over with an unsafe
  walker.
- Checkpoint 655: After restoring the verified unwindstack collector, the
  graphics runtime was relinked from a clean bootstrap. The earlier 497
  GC-stress SIGTRAP was confirmed to be a stale dylib containing the rejected
  raw walker: the freshly relinked `497-inlining-and-class-loader --gcstress`
  passed interpreter, JIT, and unmodified-source lanes. The heavy 096 timeout
  remains an open performance target.
- Checkpoint 656: Re-ran the complete local ARM64 intrinsic/JIT audit after the
  clean relink; it passed with the Nterp, Surface, MediaCodec, JNI, OSR,
  deoptimization, field/array, exception, and GC acceptance fixtures. The
  unmodified `096-array-copy-concurrent-gc` also passes all three lanes without
  `--gcstress` in 18.7s, isolating the remaining gap to the forced-GC stress
  workload rather than array-copy or JIT correctness.
- Checkpoint 657: A bounded live-process inspection of the forced-GC run was
  stopped after startup/compilation because the runner did not expose a stable
  managed child for sampling; all spawned processes were explicitly reaped.
  No source or runtime behavior was changed, and the ordinary 096 three-lane
  PASS plus the full JIT audit remain the authoritative evidence while the
  stress-performance profile is revisited with a lower-overhead capture path.
- Checkpoint 658: Optimized `DarwinPublishAotCodeMaps` so per-allocation
  backtrace publication only sorts a thread's map when a new AOT range is
  added; steady-state GC-stress calls retain identical lookup semantics without
  repeated sorting. Incremental graphics relink and the full ARM64/JIT audit
  pass after this change. A bounded 497 GC-stress run still exceeded the
  observation window, so the optimization is not yet credited with clearing
  the long-run stress timeout.
- Checkpoint 659: Repeated the forced-GC launch with a bounded low-overhead
  process probe; the runner spent the observation window in its interpreter
  preparation/dex2oat phases, so no managed throughput sample was obtained.
  The probe and all descendants were reaped. The optimized unwind-map path
  remains linked and audit-clean; GC-stress completion is still unproven.
- Checkpoint 660: Added a Darwin-only thread-local cache for repeated managed
  backtraces, keyed by the caller PC and published quick-frame SP. This avoids
  rewalking an unchanged allocation loop while preserving a full walk when the
  managed frame or call site changes. Incremental graphics relink and the full
  ARM64/JIT audit pass, including GC, OSR, deopt, JNI, and framework fixtures.
  The cache is not credited with clearing the forced-GC timeout until a full
  stress run terminates successfully.
- Checkpoint 661: A full 096 `--gcstress` retry with the cache remained CPU-bound
  in the interpreter lane for more than four minutes and was terminated after
  no terminal result. The ordinary process output showed compilation completed
  before the prolonged managed run, confirming the current cache key does not
  cover interpreter-only frames. No crash or managed failure was observed; the
  forced-GC timeout remains open and the next optimization must address the
  interpreter backtrace path without weakening AOSP stress semantics.
- Checkpoint 662: Revalidated the cache-linked runtime with the complete JIT
  audit and a fresh bounded 096 stress launch. The audit remains green; the
  stress process reaches the managed interpreter workload but still does not
  terminate quickly enough for a bounded run. This confirms the remaining
  performance work is specifically the interpreter-side backtrace/GC cadence,
  not a relink regression or a JIT correctness failure.
- Checkpoint 663: After adding the shadow-frame identity bridge, a fresh 096
  `--gcstress` run stayed at roughly one core of CPU for over three minutes in
  the interpreter lane, with dex2oat already complete and no managed error.
  The run was explicitly terminated and descendants reaped. The new key is
  therefore safe and audit-clean, but does not yet materially shorten this
  workload; interpreter GC cadence remains the open performance gap.
- Checkpoint 664: Instrumented the cache path briefly to verify interpreter
  identity delivery, then removed the diagnostic logging and rebuilt the clean
  runtime. Incremental graphics relink passes with the shadow-frame bridge;
  no debug behavior remains in production artifacts. The short probe yielded
  no managed completion, so no performance claim is made from this attempt.
- Checkpoint 665: The clean shadow-frame runtime passes the unmodified
  `497-inlining-and-class-loader` interpreter, JIT, and interpreter+optimized
  lanes without GC stress. This confirms the new interpreter ABI symbol and
  cache path introduce no ordinary reflection/inlining regression; the forced
  GC timeout remains isolated and uncredited.
- Checkpoint 666: A live managed-host run was reached and remained CPU-bound;
  macOS `sample` attached to the process but produced no report before its
  attach operation stalled, so the sampler was terminated and all test
  descendants were reaped. This rules out the current sampling command as a
  low-overhead profiler; no runtime change or stress PASS is claimed.
- Checkpoint 667: Tested routing AOSP run-test processes through the host's
  `_exit` lifecycle to avoid the long DestroyJavaVM/HeapTrim tail. The process
  exited quickly but bypassed the harness's native output finalization, yielding
  `stdout=0` and a false mismatch. The experiment was reverted; corpus tests
  retain the existing destroy path until a flush-safe process-exit contract is
  implemented.
- Checkpoint 668: Rebuilt the host after reverting the process-exit experiment
  and confirmed the ordinary upstream lifecycle remains intact. The failed
  fast-exit attempt is not retained in source; any future one-shot path must
  finalize the Java/native output files before invoking `_exit`, then preserve
  the AOSP result contract.
- Checkpoint 669: Inspected the failed fast-exit path end to end: it unloads
  NativeLoader DSOs and the Darwin ELF registry before `_exit`, while the
  harness writes output through synchronous native file operations. The exact
  output-loss ordering is not yet proven, so the safe DestroyJavaVM path stays
  active; no speculative lifecycle shortcut was retained.
- Checkpoint 670: Re-tested the process-style `_exit` experiment after adding
  an explicit Java `System.out`/`System.err` flush; the AOSP 096 stress lane
  still observed `stdout=0/42`, so the flush was insufficient and all changes
  were reverted. The host was rebuilt and unmodified 497 passed interpreter,
  JIT, and interpreter+optimized lanes; DestroyJavaVM remains authoritative.
- Checkpoint 671: Re-ran the stale corpus failures individually on the restored
  host. Unmodified 126-miranda-multidex, 2031-zygote-compiled-frame-deopt,
  2271-profile-inline-cache, and 304-method-tracing each pass all three lanes;
  the old six-failure summary is therefore not current evidence. The only
  outstanding reproduced stress concern is 149-suspend-all-stress/096 GC
  stress runtime, which remains a teardown/cadence investigation rather than
  a bytecode correctness failure.
- Checkpoint 672: Moved the Darwin managed-backtrace cache lookup ahead of
  `NativeWalk` construction, AOT map publication, and Mach-backed memory
  initialization. This removes those repeated setup costs on GC-stress cache
  hits while retaining caller-PC plus managed-frame identity keys. The rebuilt
  provider passes 096 normally and 497 across interpreter/JIT/optimized lanes;
  096 GC-stress still exceeds a bounded 45-second run, so no completion or
  speedup claim is made yet.
- Checkpoint 673: Fixed the complementary cache-population gap: interpreter
  walks now store entries keyed by their shadow frame even when no quick-frame
  registry entry exists. The provider and graphics audit rebuild cleanly, and
  normal 096/497 lanes remain green. A bounded 60-second 096 GC-stress run
  still did not complete, so the cache activation is not credited as a measured
  end-to-end speedup; GC cadence remains open.
- Checkpoint 674: Removed the local-unwind cache-hit Mach VM round-trip by
  reading the current thread's already-mapped frame record directly; remote
  walks retain the fault-safe Mach reader. Graphics-link audit and rebuilt
  provider pass, and 497's interpreter/JIT/optimized lanes remain green. This
  is a targeted hot-path optimization only; 096 GC-stress completion is still
  unproven and remains the next measurement target.
- Checkpoint 675: Rebuilt after the local direct-read change and re-ran
  unmodified 096; interpreter, JIT, and interpreter+optimized lanes all pass.
  A fresh bounded 60-second GC-stress run still times out without a managed
  failure, so the optimization is correctness-safe but its end-to-end impact
  remains unmeasured.
- Checkpoint 676: Tried a bounded macOS `sample` attach to a live 096
  GC-stress host; the attach stalled and produced no stack report, matching the
  earlier profiler limitation. Rechecked AOSP metadata: 149-suspend-all-stress
  is explicitly a known flaky-output failure upstream. No unsupported lifecycle
  shortcut or test gate was added; forced-GC completion remains open.
- Checkpoint 677: Re-ran unmodified 149-suspend-all-stress without GC stress;
  interpreter, JIT, and interpreter+optimized lanes all pass. This confirms
  its historical corpus failure is not an ordinary ARM64/JIT regression. The
  forced-GC 096 cadence issue remains the only reproduced stress gap.
- Checkpoint 678: Inspected live 096 stress logs rather than treating the
  timeout as teardown: `Main main(String[]) PASS` appears before the worker
  threads continue, with repeated explicit concurrent GCs and 8--15 ms
  suspend-all pauses. This proves the bounded timeout is in the worker/GC
  stress workload, not the Java main dispatch. The macOS profiler remains
  unavailable, so no unsupported optimization is claimed.
- Checkpoint 679: Revalidated 149-suspend-all-stress without GC stress; all
  three lanes pass. A fresh 096 GC-stress run with the no-identity local-frame
  cache key still exceeded 30 seconds; its host log shows main completion and
  continuing worker GC cycles. The remaining gap is performance/cadence, not
  an observed bytecode or JIT correctness failure.
- Checkpoint 680: The latest 096 logs quantify the worker path: three
  allocation threads remain active after main returns, with repeated
  suspend-all pauses of 8--15 ms and explicit GC totals around 100--200 ms.
  The remaining cost is Darwin collector/thread synchronization, not launcher
  dispatch or JIT correctness.
- Checkpoint 681: Rechecked AOSP `149-suspend-all-stress` without GC stress;
  interpreter, JIT, and interpreter+optimized all pass. The latest 096
  stress-host log shows `Main main(String[]) PASS` followed by worker GC cycles,
  confirming the runtime reaches the application workload before the bounded
  timeout. The remaining issue is stress throughput/cadence, with no observed
  bytecode or JIT failure.
- Checkpoint 682: A temporary cache hit counter was attempted, but the bounded
  run did not reach the worker phase before dex2oat/runner timeout, so it yielded
  no valid statistics and was removed. The runtime remains unchanged from the
  verified cache implementation; no speculative diagnostic behavior is retained.
- Checkpoint 683: Confirmed the source tree is clean after removing the
  inconclusive instrumentation and that the latest committed provider remains
  the active implementation. No new stress claim is made; the next useful
  step is a lower-level in-runtime cadence measurement that does not depend on
  macOS process attach or runner timing.

- Checkpoint 684: Re-ran the current AOSP core-app graphics acceptance; the
  unchanged Calculator computes `2+3=5` and DeskClock publishes through the
  HWUI/SurfaceFlinger/Metal path. A fresh physical-input Chrome menu run then
  reproduced an unresolved generated-code fault at address `0x110` after the
  child-process lifecycle churn. This is a real native/ABI compatibility bug,
  not a screenshot or probe failure; the next step is to map that stripped
  `libchrome.so` fault to its Android contract before changing runtime code.

- Checkpoint 685: Reproduced the same `0x110` generated-code fault on an
  unchanged Chrome launch with no synthetic pointer input, so the failure is
  in Chrome/native-process startup rather than the menu event path. The fault
  PC changes with ASLR but remains in the low managed executable window and
  ART reports it as unresolved; Calculator and DeskClock remain green. No
  speculative signal or pointer workaround was retained.
- Checkpoint 686: A bounded fault-handler trace confirmed the Chrome fault PC
  (`0x106f0aa98` in that run) was outside the two ranges visible to
  `FaultManager::IsInGeneratedCode` (`0x123af8000/32 MiB` and
  `0x1068496e0/61152`), while the thread was runnable and held the mutator
  lock. A temporary whole-window classification was tested and did not make
  Chrome start, so it was reverted; the remaining fix is precise publication
  and ownership of the missing OAT/JIT range, not a broad signal fallback.
  Diagnostic source was removed.
- Checkpoint 687: Rebuilt the graphics closure and reran Chrome with the
  bounded fault trace. The failing PC again lies outside ART's visible range
  list, and the process still exits 139; the trace also shows child native
  processes reaching normal `_exit(0)`. This separates the remaining failure
  from native process launch and confirms that the missing range publication
  is in the managed ART owner, not the ELF child loader. No broad arena or
  interpreter fallback was retained.
- Checkpoint 688: Tested an idempotent child-loader OAT-range republish patch
  in the full graphics closure. It rebuilt successfully but Chrome still
  reproduced the same `0x110` fault, so the one-time `RegisterDexFile` range
  predicate is not sufficient to explain the failure. The patch was reverted;
  the next target is the faulting managed entrypoint/call ABI itself.
- Checkpoint 689: Republished the inherited zygote shared JIT mapping from
  `JitCodeCache::PostForkChildAction` and reran Chrome through the complete
  graphics closure; the same low-window `0x110` fault remained. The change was
  reverted. The remaining discrepancy is the executable mapping and
  entrypoint/ABI metadata used by the faulting managed code.
- Checkpoint 690: Attempted a temporary ARM64 instruction-word trace at the
  unresolved Chrome fault PC. The patch did not apply cleanly to the layered
  fault-handler source and was removed; no diagnostic instrumentation remains.
  The next target is direct ownership and ABI resolution of the low-window
  executable mapping.
- Checkpoint 691: Temporary fault-context words decoded Chrome's repeated
  crash as `ldr x11, [x8,#0x110]` with `x8=0`, while the frame contained an
  aligned non-null `ArtMethod*`. This proves a real implicit-null check, not
  an arbitrary PC or native loader fault. A Darwin-only bypass of
  `IsValidReturnPc` was build-tested only as a temporary experiment and was
  discarded; the required fix is accurate OAT/JIT stack-map/header lookup.
- Checkpoint 692: Tested the alternate Darwin signal return-PC convention
  (`pc` instead of AOSP's `pc+4`) against the unchanged Chrome APK. The full
  graphics closure passed, but Chrome still faulted identically with `addr=0x110`.
  The experiment was reverted; stack-map failure is not a one-instruction PC
  offset issue.
- Checkpoint 693: Fault-context tracing showed `sp[0]` is aligned, but its
  `ArtMethod::GetEntryPointFromQuickCompiledCode()` is zero while execution is
  in low-window compiled code. The remaining bug is method-frame/entrypoint
  metadata publication, not signal-PC arithmetic. Temporary tracing was
  removed; next work targets method-frame publication and entrypoint storage.
- Checkpoint 694: Tried recovering a null quick entrypoint from the immutable
  OAT method code inside `GetOatQuickMethodHeader()`. The graphics closure
  built and Chrome still reproduced the same `addr=0x110` fault, so this is not
  a missing OAT fallback. The experiment was removed; the frame's method
  identity/publication path remains the next target.
- Checkpoint 695: Forced publication of `sp[0]` in optimizing ARM64 frame
  entries was rebuilt and exercised against Chrome, but the same implicit-null
  fault remained. The experiment was removed; the mismatch is not merely an
  uninitialized frame slot. Next, trace the producer of the low-window code
  and its method identity.
- Checkpoint 697: AOSP frame generation review found the leaf/no-current-method
  path can legitimately use an empty quick frame, while signal recovery assumes
  `sp[0]` is the current method. This is a concrete frame-contract hypothesis,
  but Chrome has not yet been proven to use that path; no code change is kept.
- Checkpoint 696: Compared 64-bit and 32-bit `ArtMethod` entrypoint accessors
  for Chrome's fault-frame `sp[0]`; both returned zero. This rules out a simple
  image-pointer-size selection bug. The slot is likely not the executing
  method, so next target is quick-frame boundary/tag publication.
- Checkpoint 698: Fault-context tracing now compares ART TLS directly. Chrome's
  failing JIT signal runs on a valid attached `Thread` (`self` is non-null), but
  `ManagedStack::HasTopQuickFrame()` is false and all top-frame/tag accessors
  are zero. This proves the failure is an un-published managed-frame boundary,
  not pointer-size selection or a null `ArtMethod` entrypoint. The temporary
  logging was removed; next repair the host/JNI invocation transition so the
  AOSP managed-stack contract is published before entering generated code.
- Checkpoint 699: A temporary fault-stack slot dump was rejected as evidence:
  the signal handler's local async-safe buffer shares the faulting stack and
  can overwrite nearby words. It was removed without changing runtime
  behavior; the next frame-boundary capture must use a preallocated buffer or
  debugger-safe snapshot.
- Checkpoint 700: Added a signal-safe PC-to-`ArtMethod` registry fed from JIT
  code commits and AOT class linking, then rebuilt and ran Chrome. The registry
  remained empty for the failing PCs because they resolve inside `libchrome.so`,
  and Chrome still faults at `addr=0x110`; this does not repair the crash. The
  temporary `jit_method` signal print was removed. Next target is the native
  JNI/ELF invocation boundary and its managed-stack publication, not broader
  implicit-null recovery.
- Checkpoint 701: Hardened the Darwin null handler to require a published
  ART JIT/AOT PC before rewriting a signal context. Chrome now reports
  `DARWIN signal: unresolved ... addr=0x110` for its `libchrome.so` fault
  instead of entering ART's null-exception path; exit remains `rc=139`.
  This confirms the previous crash was being misclassified at the process-wide
  signal boundary. The underlying native/JNI fault is still outstanding.
- Checkpoint 702: Unified the existing AOSP `darwin_art_register_compiled_method`
  publication with the lock-free PC range registry, and added method identity
  publication for the JIT/AOT ArtMethod paths. Graphics-link audit passed.
  Chrome still exits `rc=139`; its changing `0x104…` fault PCs remain outside
  the registry and are reported as unresolved native signals. The next target
  is identifying that low-window code producer (nterp/trampoline versus JIT),
  not widening the signal handler.
- Checkpoint 703: Recreated the runtime shadow from scratch after rejecting an
  unverified nterp-range patch that broke patch application. The staged AOSP
  tree now contains the minimal-start declarations/definitions and the full
  graphics-link audit passes again. No runtime behavior claim is made from the
  rejected nterp experiment; the next step remains tracing the low-window
  producer with a stable build.
- Checkpoint 704: A temporary compiled-range print was removed without source
  changes. Rebuilding after deleting the shadow exposed a pre-existing staging
  failure in `0066-darwin-arm64-class-load-boundary.patch` (two rejected
  `optimizing_compiler.cc` hunks), so the current successful binary still relies
  on cached shadow state. Next repair the patch-closure reproducibility before
  collecting further low-window PC evidence.
- Checkpoint 705: Removed obsolete patch applications for `0066`, `0092`, and
  `0123`, each already reflected in the pinned AOSP optimizing compiler, and
  bumped the runtime shadow identity. After explicitly restaging the runtime
  shadow, `audit-runtime-graphics-link-incremental` completed with
  `registrar=51 fake-symbols=0 host-icu=0 host-fmt=0 CoreText=0`; the ARM64 JIT
  archive and runtime bootstrap were rebuilt from the clean shadow. This fixes
  build reproducibility only; Chrome's unresolved native fault remains open.
- Checkpoint 706: Added a clean AOSP compiler patch that removes the remaining
  Darwin-only LoadString/LoadClass/Invoke graph allowlist from the optimizing
  compiler. A fresh JIT shadow compiled all 106 ARM64 objects and the graphics
  link audit passed with `registrar=51 fake-symbols=0 host-icu=0 host-fmt=0
  CoreText=0`; the staged compiler contains no Darwin graph-reject block. The
  generic dex probe still cannot run because its legacy runtime-link artifact
  does not export `darwin_art_prepare_process_exit`; this is a probe ABI
  mismatch, not evidence of JIT completion.
- Checkpoint 707: Exported and dead-strip-protected `darwin_art_prepare_process_exit`
  in the headless runtime-link probe, fixing its loader ABI mismatch. The dex
  probe now reaches ART initialization and class loading; it fails later with
  status 27 when `ResourcesManager` asks for an unavailable `android.system`
  Binder service. This exposes the next real compatibility task: bootstrap the
  system-service provider for headless app execution.
- Checkpoint 708: Added the existing `DarwinServiceBridge` source to the
  baseline probe DEX build so headless execution uses the same Binder contract
  as installed apps. The rebuilt DEX contains the bridge, but the probe still
  returns status 27 before service creation completes; the remaining issue is
  bridge class initialization/registration timing, not missing DEX packaging.
- Checkpoint 709: Forced the headless runtime to retain framework Binder
  registrars and instrumented context-binder creation. The exact cycle is now
  proven: resolving `DarwinServiceBridge` through the context class loader
  triggers `VMClassLoader`/`FileSystems` initialization, which immediately
  calls `ServiceManager` again before the bridge can be created. This is not a
  missing-symbol or DEX issue; the next fix must bootstrap Binder before Java
  class loading or start the profile system-server first.
- Checkpoint 710: Rebuilt the retained Binder registrar with debug tracing and
  confirmed the failure ordering: the class-loader lookup throws
  `ExceptionInInitializerError` from `FileSystems`/`VMClassLoader` before
  `DarwinServiceBridge.createContextBinder()` is entered. The probe therefore
  still exits status 27; no Java-side fallback was added.
- Checkpoint 711: Ran the real Chrome tab-graphics acceptance after removing
  the optimizing allowlist. Profile system-server published its sockets, then
  a generated-code `SIGSEGV/SEGV_ACCERR` occurred at `pc=0x120001bcc`
  (`addr=0x6060313`) before the app launch completed. This is a concrete
  lowering/managed-address fault exposed by unrestricted JIT, not a test
  harness success; the next step is symbolizing this generated PC and fixing
  its ARM64 reference or W^X transition boundary.
- Checkpoint 712: Added debug-only JIT publication logging and reran the real
  Chrome/system-server acceptance. The generated-code fault persists with a
  new PC (`0x12d801d1c`, `SEGV_ACCERR`, `addr=0x6060313`), but no
  `DarwinArtRegisterJitMethod` publication precedes it. The failing producer is
  therefore not proven to be a committed optimizing-JIT range; distinguish
  AOT/nterp/trampoline code from JIT before changing ARM64 lowering or fault
  recovery.
- Checkpoint 713: Repeated the real-app run after rebuilding publication
  instrumentation. The fault moved to `pc=0x12d801d1c` with the same
  `SEGV_ACCERR`/`addr=0x6060313` signature and still emitted no registry event.
  Keep the PC classified as an unowned generated-code window until mapping
  symbolization identifies its producer.
- Checkpoint 714: Added opt-in `DARWIN_ART_DEBUG_FAULT_MAP` instrumentation to
  the unresolved ARM64 signal path. It records the Mach VM region containing
  the faulting PC (base, size, current/max protection, and inheritance) without
  changing recovery behavior. The Chrome acceptance rerun could not be
  repeated because the previously materialized Chrome APK is no longer present
  in the profile store; no producer classification is claimed until a fresh APK
  is installed and the map evidence is captured.
- Checkpoint 715: Ran AOSP ART tests `507-boolean-test` and
  `003-omnibus-opcodes` through the pinned runner. Both passed in interpreter,
  optimized/JIT, and unmodified-source modes, covering boolean, arithmetic,
  branch, and omnibus opcode lowering. The full compatibility matrix and real
  APK startup remain outstanding.
- Checkpoint 716: Ran AOSP ART `008-exceptions`; interpreter, optimized/JIT,
  and unmodified-source modes all passed. This adds verified throw/catch and
  exceptional control-flow coverage to the JIT evidence, while monitor, GC,
  JNI, deoptimization, and real APK startup remain unverified.
- Checkpoint 717: Rebuilt the graphics runtime with fault-region mapping and
  reran `004-ThreadStress`. The failure is reproducible in the interpreter
  sandbox: `pc` lies in an executable Mach region (`prot=0x5`,
  `max=0x7`), while the faulting receiver is `0x70000770` and the access is
  `0x700007db`. This rules out a missing execute transition and points to a
  managed compressed-reference value crossing the JNI/thread-stress native
  boundary without host-pointer decoding. No fallback was added.
- Checkpoint 718: Rebuilt the patched graphics runtime and ran AOSP
  `004-JniTest`; interpreter, optimized/JIT, and unmodified-source modes all
  pass. The failure is therefore specific to the concurrent/thread-stress
  reference path rather than the basic JNI bridge, and remains open for a
  targeted transition fix.
- Checkpoint 719: Defined `DARWIN_ART_REFERENCE_BASE=0x10000000000` in the
  Darwin runtime toolchain so assembly JNI/trampoline normalization guards are
  enabled consistently with the C++ reference window. A full incremental
  graphics/JIT rebuild completed, but `004-ThreadStress` still reproduces the
  same `0x70000770` receiver fault. The executing path is therefore not fixed
  by the generic-JNI macro alone and must be traced to its actual entry stub.
- Checkpoint 720: Symbolized the reproducible `004-ThreadStress` fault to an
  AOT class-initialization status-byte load (`ldrb [x0,#0x6b]`) using the
  compressed `0x70000000` reference directly. Added the AOSP codegen patch
  `0179-darwin-arm64-clinit-reference-boundary.patch`, which decodes the
  class reference before `GenerateClassInitializationCheck` dereferences it.
  Rebuilt the full graphics/JIT closure; `004-ThreadStress` and `004-JniTest`
  now pass interpreter, optimized/JIT, and unmodified-source lanes. The
  broader compatibility matrix and real APK acceptance remain outstanding.
- Checkpoint 721: Audited the separate ARM64 fast compiler and found its
  invoke receiver class load still used `HeapOperand(receiver.W(), ...)`
  directly. Added patches `0180` and `0181` to decode that receiver and to
  include the shared reference-codegen helper. The complete graphics/JIT
  closure rebuilt and audited successfully; the subsequent ThreadStress and
  JniTest runs remain green across the available runner lanes.
- Checkpoint 722: Audited fast-compiler instance field get/put lowering and
  found two additional direct compressed-reference `HeapOperand` uses. Added
  `0182-darwin-arm64-fast-field-reference-boundaries.patch` to decode holders
  into scratch native registers before field memory access. Full graphics/JIT
  and interpreter closure audit passed; `003-omnibus-opcodes` passed in
  interpreter, optimized/JIT, and unmodified-source lanes.
- Checkpoint 723: Completed the fast-compiler ARM64 audit: no executable
  `HeapOperand` remains on an un-decoded managed holder in its invoke or
  instance field paths. Rebuilt and audited the full runtime closure, then
  reran `003-omnibus-opcodes`; interpreter, optimized/JIT, and unmodified
  source lanes all passed. The full feature matrix and real APK acceptance
  are still open.
- Checkpoint 724: Audited fast-compiler `check-cast` lowering and found the
  object-class load still treated a compressed object register as a host
  pointer. Added `0183-darwin-arm64-fast-checkcast-reference-boundary.patch`
  to decode it into a scratch native register. Closure rebuild/audit and the
  post-change `003-omnibus-opcodes` three-lane differential run passed.
- Checkpoint 725: Regression coverage after the check-cast boundary fix is
  green for `426-monitor`, `160-read-barrier-stress`, and `102-concurrent-gc`:
  interpreter, optimized/JIT, and unmodified-source lanes all pass. The
  runtime still emits the known Darwin membarrier/sentinel-page warnings;
  these runs did not reproduce a fault. Full compatibility matrix and real
  APK acceptance remain open.
- Checkpoint 726: `597-deopt-busy-loop` also passes in interpreter,
  optimized/JIT, and unmodified-source lanes after the same reference-boundary
  changes. This exercises a debuggable deoptimization loop without falling
  back from JIT; wider deopt/OSR coverage remains outstanding.
- Checkpoint 727: `570-checker-osr` passes in all three lanes, extending the
  regression set to checker-generated OSR loop transitions. No new fault or
  interpreter fallback was observed; broader OSR/deopt and APK coverage remain.
- Checkpoint 728: `596-monitor-inflation` passes in interpreter, optimized/JIT,
  and unmodified-source lanes after the `Reference.getReferent()` boundary
  hardening. Monitor inflation and contended-lock paths remain green; the
  complete AOSP matrix and real APK acceptance are still open.
- Checkpoint 729: Added `0184-darwin-arm64-reference-referent-boundary.patch`
  so the non-Baker `Reference.getReferent()` path explicitly null-checks and
  decodes the compressed receiver before field access. Full JIT/graphics link
  audit passed; `855-native` and `1927-exception-event` pass all three lanes.
  Broader JNI/exception matrix and real APK acceptance remain open.
- Checkpoint 730: `004-ReferenceMap` passes interpreter, optimized/JIT, and
  unmodified-source lanes after the referent intrinsic change, covering stack
  reference-map walking through compiled frames. Wider JNI/stack-walk cases
  and real APK acceptance remain outstanding.
- Checkpoint 731: `911-get-stack-trace` passes all three lanes, validating JNI
  stack-trace collection through compiled frames after the reference-boundary
  changes. The broader JNI/exception matrix and real APK acceptance remain
  open.
- Checkpoint 732: Re-ran `004-JniTest` after patch 0184; interpreter,
  optimized/JIT, and unmodified-source lanes all pass, including the AOSP
  verifier's invalid fast/critical-native cases. JNI ABI and exception checks
  remain broader than this focused test, and real APK acceptance is still open.
- Checkpoint 733: `2036-jni-filechannel` passes all three lanes, extending JNI
  coverage to Java NIO file-channel native interactions after the referent
  boundary hardening. Full JNI/exception matrix and real APK acceptance remain
  outstanding.
- Checkpoint 734: `004-ThreadStress` passes interpreter, optimized/JIT, and
  unmodified-source lanes after patch 0184, confirming the earlier clinit/JNI
  transition regression remains fixed under multithreaded execution. Full
  concurrency matrix and real APK acceptance remain open.
- Checkpoint 735: `802-deoptimization` passes all three lanes, covering
  exception-handler entry and deoptimization state recovery from compiled
  code. Additional deopt/OSR combinations and real APK acceptance remain
  outstanding.
- Checkpoint 736: `597-deopt-new-string` passes interpreter, optimized/JIT,
  and unmodified-source lanes, exercising allocation/string construction
  across a debuggable deoptimization boundary. Broader allocation and real
  APK coverage remain open.
- Checkpoint 737: `471-deopt-environment` passes all three lanes, validating
  preservation and reconstruction of compiled locals across deoptimization.
  Additional OSR/deopt combinations and real APK acceptance remain open.
- Checkpoint 738: Re-ran `003-omnibus-opcodes` after the referent intrinsic
  patch; interpreter, optimized/JIT, and unmodified-source lanes all pass.
  The suite still reports only the expected compiler instruction-size limit for
  its intentionally huge method; full matrix and real APK coverage remain.
- Checkpoint 739: `2239-varhandle-perf-vh-get` passes interpreter, optimized/JIT,
  and unmodified-source lanes, exercising VarHandle metadata/reference loads
  through the native decode helper. Broader VarHandle modes and real APK
  acceptance remain open.
- Checkpoint 740: `2239-varhandle-perf-vh-unsafe-cas` passes all three lanes,
  covering VarHandle atomic CAS/set operations and their memory-ordering paths.
  Broader VarHandle modes, JNI/exception matrix, and real APK acceptance remain
  outstanding.
- Checkpoint 741: `2239-varhandle-perf-vh-set-bav` passes interpreter,
  optimized/JIT, and unmodified-source lanes, covering byte-array-view stores,
  type checks, and write-barrier paths. Remaining VarHandle modes and real APK
  acceptance remain open.
- Checkpoint 742: `2239-varhandle-perf-vh-reflect-get` passes all three lanes,
  validating reflection-created VarHandle access and runtime type resolution.
  Remaining VarHandle modes plus full JNI/exception and real APK coverage stay
  open.
- Checkpoint 743: `2239-varhandle-perf-vh-cae` passes interpreter, optimized/JIT,
  and unmodified-source lanes, covering VarHandle compare-and-exchange return
  values and failure paths. Broader mode coverage and real APK acceptance stay
  open.
- Checkpoint 744: `2239-varhandle-perf-vh-set` passes all three lanes,
  validating ordinary VarHandle primitive/reference stores and their barrier
  paths. Remaining modes, JNI/exception breadth, and real APK acceptance remain
  outstanding.
- Checkpoint 745: `2239-varhandle-perf-vh-gas` passes interpreter, optimized/JIT,
  and unmodified-source lanes, covering atomic get-and-set read-modify-write
  semantics. Remaining VarHandle modes and real APK acceptance remain open.
- Checkpoint 746: `2239-varhandle-perf-vh-cas-weak` passes all three lanes,
  validating weak-CAS spurious-failure/retry semantics and atomic reference
  handling. Remaining modes and real APK acceptance remain outstanding.
- Checkpoint 747: `2239-varhandle-perf-vh-get-a` passes interpreter,
  optimized/JIT, and unmodified-source lanes, covering acquire/opaque VarHandle
  reads and their ordering semantics. Remaining modes and real APK acceptance
  remain open.
- Checkpoint 748: `2239-varhandle-perf-vh-reflect-set` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating reflection-created
  VarHandle stores and runtime type resolution. Remaining modes and real APK
  acceptance remain open.
- Checkpoint 749: `2239-varhandle-perf-vh-get-bav` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating byte-array-view
  acquire/opaque reads and their ordering semantics. Remaining modes and real
  APK acceptance remain open.
- Checkpoint 750: `2239-varhandle-perf-vh-unsafe-get` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating Unsafe-backed
  VarHandle reads across the native memory access path. Remaining modes and
  real APK acceptance remain open.
- Checkpoint 751: `2239-varhandle-perf-vh-unsafe-put` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating Unsafe-backed
  VarHandle writes and their native memory access path. Remaining modes and
  real APK acceptance remain open.
- Checkpoint 752: `2239-varhandle-perf-vh-set-a` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating acquire/opaque
  VarHandle stores and ordering semantics. Remaining modes and real APK
  acceptance remain open.
- Checkpoint 753: `2239-varhandle-perf-vh-gaa` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating atomic get-and-add
  VarHandle read-modify-write semantics. Remaining modes and real APK
  acceptance remain open.
- Checkpoint 754: `2239-varhandle-perf-vh-gab` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating atomic get-and-bitwise
  read-modify-write semantics. Remaining modes and real APK acceptance remain
  open.
- Checkpoint 755: `2239-varhandle-perf-vh-cas` passes interpreter,
  optimized/JIT, and unmodified-source lanes, validating ordinary atomic
  compare-and-set success/failure semantics. Remaining modes and real APK
  acceptance remain open.
- Checkpoint 756: `aosp-core-apps-graphics-acceptance.sh` passes unchanged
  Calculator (`2+3=5`) and DeskClock timer flows through the common
  HWUI/SurfaceFlinger/Metal path. Full AOSP matrix and Blue Archive acceptance
  remain open.
- Checkpoint 757: Re-ran the corpus's sole prior failure,
  `9999-input-channel-endpoint-parcel-smoke`, after rebuilding the generic
  compiler API companion. Interpreter, optimized/JIT, and unmodified-source
  lanes now all pass; the earlier failure was stale compiler-surface state,
  not a runtime semantic failure. Full corpus and Blue Archive acceptance
  remain open.
- Checkpoint 758: Re-ran `039-join-main`, a core thread-join/concurrency
  regression, and all interpreter, optimized/JIT, and unmodified-source lanes
  pass. The corpus ledger still contains stale failures requiring refresh;
  full matrix and Blue Archive acceptance remain open.
- Checkpoint 759: Re-ran `2275-pthread-name`, validating pthread-backed thread
  naming and Java thread metadata; interpreter, optimized/JIT, and
  unmodified-source lanes all pass. Remaining corpus failures and production
  APK acceptance remain open.
- Checkpoint 760: Re-ran `2282-single-step-before-catch`, validating
  single-step/deoptimization behavior immediately before exception catch
  dispatch; all interpreter, optimized/JIT, and unmodified-source lanes pass.
  Remaining corpus failures and production APK acceptance remain open.
- Checkpoint 761: Re-ran `497-inlining-and-class-loader`, validating
  inlined calls across class-loader boundaries; interpreter, optimized/JIT,
  and unmodified-source lanes all pass. Remaining corpus failures and
  production APK acceptance remain open.
- Checkpoint 762: Re-ran `629-vdex-speed`, validating VDEX input/recompilation
  and speed-filter execution across interpreter, optimized/JIT, and
  unmodified-source lanes; all pass. Remaining corpus failures and production
  APK acceptance remain open.
- Checkpoint 763: Re-ran `714-invoke-custom-lambda-metafactory`, validating
  invoke-custom, lambda metafactory linkage, and JIT call ABI; interpreter,
  optimized/JIT, and unmodified-source lanes all pass. Remaining corpus
  failures and production APK acceptance remain open.
- Checkpoint 764: Re-ran `844-exception2`, validating exception propagation,
  catch handling, and optimized/JIT deoptimization; all interpreter,
  optimized/JIT, and unmodified-source lanes pass. Remaining corpus failures
  and production APK acceptance remain open.
- Checkpoint 765: Expanded the lock-free Darwin JIT fault-method registry from
  1,024 to 16,384 ranges. Boot-image AOT publication had exhausted the old
  table, dropping application JIT ranges and misrouting implicit-null faults
  to user SIGSEGV handlers. Rebuilt and audited the graphics runtime link,
  then `004-SignalTest` passed all three lanes.
- Checkpoint 766: Reproduced `031-class-attributes` after the registry fix;
  its app-AOT generated code still faults on an implicit-null access without a
  published range. This is a distinct app-AOT registration gap (not the fixed
  JIT table exhaustion) and remains the next runtime repair target.
- Checkpoint 767: Moved app-AOT registry publication to use the original oat
  entrypoint before instrumentation bridges and rebuilt/audited the graphics
  runtime. `031-class-attributes` still faults at a boot/shared-AOT PC, so the
  remaining gap is boot-image/shared-range publication rather than the app
  instrumentation replacement condition.
- Checkpoint 768: Confirmed the remaining fault is in boot-image/shared AOT
  code: debug publication logs contain app/JIT ranges but no range covering
  `0x100704a738c`. A visitor experiment was rejected because runtime shadow
  patch offsets differ across generated copies; no unverified boot-image code
  was retained. `031-class-attributes` remains open.
- Checkpoint 769: A boot-image visitor prototype was rejected after the
  generated runtime shadow copies applied different `class_linker.cc` hunks,
  leaving one copy without the visitor declaration and breaking compilation.
  The prototype was fully removed; the next fix must use a shared compat API
  rather than copy-sensitive class-linker insertion. `031-class-attributes`
  remains open.
- Checkpoint 770: Added a local visitor in `ClassLinker::InitFromBootImage`
  that publishes each boot-image method's original oat quick-code range,
  independent of instrumentation bridges. After rebuilding and relinking,
  `031-class-attributes` and `004-SignalTest` pass interpreter, optimized/JIT,
  and unmodified-source lanes. Broader corpus and production APK acceptance
  remain open.
- Checkpoint 771: Started a fresh four-worker corpus run after the boot-image
  AOT registry fix; workers are alive and have advanced through the early
  corpus (latest completed artifact observed around `203-multi-checkpoint`).
  The run is intentionally still in progress, so its final pass/fail matrix
  and production APK acceptance remain unverified.
- Checkpoint 772: The refreshed corpus run remains live and has progressed
  through later structural/debug tests and back into the deterministic
  `416-optimizing-arith-not` segment. Its intermediate ledger currently shows
  1,056 passed and 20 failed entries, which is not final until all workers
  terminate and stale results are reconciled.
- Checkpoint 773: The same four-worker corpus run remains live and has
  advanced through `565-checker-doublenegbitwise`. Its intermediate ledger
  currently reports 1,059 passed and 17 failed; failures are not final until
  the full run completes and each failing lane is independently reproduced.
- Checkpoint 774: Corpus workers remain live and have advanced through
  `608-checker-unresolved-lse` in the sorted test set. The intermediate
  ledger remains 1,059 passed and 17 failed; no failure is treated as final
  until all workers finish and targeted reproduction is complete.
- Checkpoint 775: The refreshed four-worker corpus run advanced through
  `660-clinit`; its intermediate ledger is now 1,060 passed and 16 failed.
  Workers remain active, so the reduced failure count is still provisional
  until the complete sorted corpus and targeted reruns finish.
- Checkpoint 776: The same corpus run remains live and has advanced through
  `734-duplicate-fields` (including JIT/JNI, VDEX, thread-priority, and
  field-resolution coverage). The intermediate ledger is still 1,060 passed
  and 16 failed; final classification awaits worker termination.
- Checkpoint 777: Corpus workers remain live and have reached `912-classes`,
  covering heap iteration, JVMTI attachment, method metadata, stack traces,
  and class inspection. Intermediate ledger is 1,061 passed and 15 failed;
  final classification remains pending.
- Checkpoint 778: Corpus workers remain live through `925-threadgroups`,
  covering properties, monitor, thread, and thread-group behavior. The
  intermediate ledger remains 1,061 passed and 15 failed; final classification
  is deferred until the full run terminates.
- Checkpoint 779: The same four-worker corpus run remains live through
  `940-recursive-obsolete`, covering JVMTI transformation and recursive
  obsolete-method paths. Intermediate counts remain 1,061 passed and 15
  failed; the final matrix is still pending worker termination.
- Checkpoint 780 (2026-09-10): The resumed four-worker corpus remains active,
  now processing the 980-series JVMTI tests. The ledger is still 1,061 passed
  and 15 failed; these failures remain provisional until the run terminates
  and each entry is rerun against the current boot-image range registration.
- Checkpoint 781 (2026-09-10): Corpus completed at 1,062 passed and 14
  provisional failures. Rechecks against the rebuilt graphics link cleared
  `031-class-attributes`, `046-reflect`, `082-inline-execute`,
  `083-compiler-regressions`, `115-native-bridge`, `128-reg-spill-on-implicit-nullcheck`,
  `140-dce-regression`, and `064-field-access`; remaining faults are isolated
  to malformed/implicit-null cases and require a dedicated fault-frame fix.
- Checkpoint 782 (2026-09-10): Rebuilt-link rechecks confirm the registry
  path fixes `064-field-access`; the remaining generated-code crashes are
  `1004-checker-volatile-ref-load`, `800-smali`, and `2045-uffd-kernelfault`.
  Their PCs fall inside quick-code pages but outside the compact ranges
  published from `OatQuickMethodHeader`, so the next implementation step is
  per-method code-page/range publication rather than an interpreter fallback.
- Checkpoint 783 (2026-09-10): A page-tail registry change was committed as
  `06fd1d6` and forced through the incremental graph, but runtime diagnostics
  still report the old compact `end` values. This proves the active graphics
  artifact is not consuming `compat/darwin_jit_memory.cc` directly; range
  publication must therefore be fixed at the ART patch call sites/archive
  input boundary before trusting any registry-only change.
- Checkpoint 784 (2026-09-10): Explicitly removing the cached graphics archive
  and relinking made the `4096`-byte page-tail publication active. Direct
  reruns then passed `800-smali`, `1004-checker-volatile-ref-load`, and
  `2045-uffd-kernelfault`; the corpus `--resume` command only replayed its
  prior ledger and did not invalidate those rows. A full fresh corpus run is
  required before updating aggregate counts.
- Checkpoint 785 (2026-09-10): Fresh corpus run started with isolated ledger
  `_build/art-upstream-corpus-fresh` after forced graphics relink. It is live
  with four workers and has completed the first 27 tests without reusing any
  prior result; aggregate classification remains pending termination.
- Checkpoint 786 (2026-09-10): The fresh four-worker run remains live through
  `070-nio-buffer`, with 78 completed tests and no failed rows so far. The
  isolated ledger is still the authoritative run; no aggregate claim is made
  until all corpus inputs finish.
- Checkpoint 787 (2026-09-10): The same fresh run remains live through
  `086-null-super`, with 99 tests completed and no failures recorded. Worker
  processes are active; final aggregate classification is still deferred.
- Checkpoint 788 (2026-09-10): Fresh corpus advanced through
  `103-string-append`, reaching 118 completed tests with zero failures. The
  four-worker process remains live and the isolated ledger is authoritative.
- Checkpoint 789 (2026-09-10): Fresh corpus advanced through
  `120-hashcode`, reaching 133 completed tests with zero failures. Four workers
  remain active; aggregate completion is still pending.
- Checkpoint 790 (2026-09-10): Fresh corpus advanced through
  `133-static-invoke-super`, reaching 148 completed tests with zero failures.
  The same four-worker run remains active.
- Checkpoint 791 (2026-09-10): Fresh corpus advanced through
  `140-field-packing`, reaching 160 completed tests with zero failures. The
  four-worker process remains active.
- Checkpoint 799 (2026-09-10): Fresh corpus advanced through
  `1917-get-stack-frame`, reaching 222 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 826 (2026-09-10): Fresh corpus advanced through
  `2262-default-conflict-methods`, reaching 404 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 827 (2026-09-10): Fresh corpus reached 849 completed tests with
  zero failures; the four-worker run remains active.
- Checkpoint 828 (2026-09-10): Fresh corpus reached 933 completed tests with
  zero failures; the four-worker run remains active.
- Checkpoint 825 (2026-09-10): Fresh corpus advanced through
  `2243-checker-not-inline-into-throw`, reaching 381 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 824 (2026-09-10): Fresh corpus advanced through
  `2239-varhandle-perf-vh-get-bav`, reaching 368 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 823 (2026-09-10): Fresh corpus advanced through
  `2239-varhandle-perf-vh-gaa`, reaching 363 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 822 (2026-09-10): Fresh corpus advanced through
  `2239-varhandle-perf-vh-cas`, reaching 361 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 821 (2026-09-10): Fresh corpus advanced through
  `2234-checker-remove-entry-suspendcheck`, reaching 354 completed tests with
  zero failures. Four workers remain active.
- Checkpoint 820 (2026-09-10): Fresh corpus advanced through
  `2036-jni-filechannel`, reaching 336 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 819 (2026-09-10): Fresh corpus advanced through
  `2030-long-running-child`, reaching 329 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 818 (2026-09-10): Fresh corpus advanced through
  `201-built-in-except-detail-messages`, reaching 313 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 817 (2026-09-10): Fresh corpus advanced through
  `2001-virtual-structural-multithread`, reaching 306 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 816 (2026-09-10): Fresh corpus advanced through
  `1993-fallback-non-structural`, reaching 298 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 815 (2026-09-10): Fresh corpus advanced through
  `1984-structural-redefine-field-trace`, reaching 289 completed tests with
  zero failures. Four workers remain active.
- Checkpoint 814 (2026-09-10): Fresh corpus advanced through
  `1978-regular-obsolete-then-structural-obsolescence`, reaching 283
  completed tests with zero failures. Four workers remain active.
- Checkpoint 813 (2026-09-10): Fresh corpus advanced through
  `1973-jni-id-swap-pointer`, reaching 278 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 812 (2026-09-10): Fresh corpus advanced through
  `1971-multi-force-early-return`, reaching 273 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 811 (2026-09-10): Fresh corpus advanced through
  `1965-get-set-local-primitive-no-tables`, reaching 268 completed tests with
  zero failures. Four workers remain active.
- Checkpoint 810 (2026-09-10): Fresh corpus advanced through
  `1961-checker-loop-vectorizer`, reaching 264 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 809 (2026-09-10): Fresh corpus advanced through
  `1960-checker-bounds-codegen`, reaching 260 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 808 (2026-09-10): Fresh corpus advanced through
  `1954-pop-frame-jit`, reaching 254 completed tests with zero failures. Four
  workers remain active.
- Checkpoint 807 (2026-09-10): Fresh corpus advanced through
  `1948-obsolete-const-method-handle`, reaching 250 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 806 (2026-09-10): Fresh corpus advanced through
  `1942-suspend-raw-monitor-exit`, reaching 247 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 805 (2026-09-10): Fresh corpus advanced through
  `1938-transform-abstract-single-impl`, reaching 243 completed tests with
  zero failures. Four workers remain active.
- Checkpoint 804 (2026-09-10): Fresh corpus advanced through
  `1935-get-set-current-frame-jit`, reaching 239 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 803 (2026-09-10): Fresh corpus advanced through
  `1933-monitor-current-contended`, reaching 236 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 802 (2026-09-10): Fresh corpus advanced through
  `1928-exception-event-exception`, reaching 232 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 801 (2026-09-10): Fresh corpus advanced through
  `1925-self-frame-pop`, reaching 229 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 800 (2026-09-10): Fresh corpus advanced through
  `1920-suspend-native-monitor`, reaching 225 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 792 (2026-09-10): Fresh corpus advanced through
  `153-reference-stress`, reaching 174 completed tests with zero failures.
  Four workers remain active; no completion claim yet.
- Checkpoint 793 (2026-09-10): Fresh corpus advanced through
  `168-vmstack-annotated`, reaching 188 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 798 (2026-09-10): Fresh corpus advanced through
  `1912-get-set-local-primitive`, reaching 218 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 797 (2026-09-10): Fresh corpus advanced through
  `1907-suspend-list-self-twice`, reaching 213 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 796 (2026-09-10): Fresh corpus advanced through
  `1903-suspend-self`, reaching 209 completed tests with zero failures. The
  four-worker process remains active.
- Checkpoint 794 (2026-09-10): Fresh corpus advanced through
  `180-native-default-method`, reaching 199 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 795 (2026-09-10): Fresh corpus advanced through
  `182-method-linking`, reaching 204 completed tests with zero failures.
  Four workers remain active.
- Checkpoint 826 (2026-09-10): Fresh corpus advanced through
  `2262-default-conflict-methods`, reaching 404 completed tests with zero
  failures. Four workers remain active.
- Checkpoint 829 (2026-09-10): Fresh corpus reached 954 completed tests with
  zero failures; the four-worker run remains active.
- Checkpoint 830 (2026-09-10): Fresh corpus reached 983 completed tests with
  zero failures; the four-worker run remains active.
- Checkpoint 831 (2026-09-10): Fresh corpus reached 995 completed tests with
  zero failures; the four-worker run remains active.
- Checkpoint 832 (2026-09-10): Fresh corpus reached 1008 completed tests out
  of 1,141 discovered tests with zero failures; the four-worker run remains
  active.
- Checkpoint 833 (2026-09-10): Fresh corpus reached 1,037 completed tests out
  of 1,141 discovered tests with zero failures; the four-worker run remains
  active.
- Checkpoint 834 (2026-09-10): Fresh corpus terminated at 1,075/1,076
  terminal tests; `978-virtual-interface` had one SIGILL under the loaded
  four-worker run, while six isolated JIT reruns (including four concurrent)
  passed. Treat this as an unresolved concurrency/flakiness defect, not a
  completion signal.
- Checkpoint 835 (2026-09-10): Fixed lock-free JIT range publication by
  reserving slots with a sentinel and release-publishing the start after
  end/method payload initialization. Graphics-link audit and eight concurrent
  `978-virtual-interface` JIT reruns passed; a full 1,076-test final corpus
  rerun is now active.
- Checkpoint 836 (2026-09-10): The post-fix final corpus rerun reached 92
  completed tests with zero failures; four workers remain active.
- Checkpoint 837 (2026-09-10): The post-fix final corpus rerun reached 132
  completed tests with zero failures; four workers remain active.
- Checkpoint 838 (2026-09-10): Post-fix final corpus reached 203 completed
  tests; two SIGILL failures are recorded (`127-checker-secondarydex` and
  `149-suspend-all-stress`), so concurrency safety remains unresolved.
- Checkpoint 839 (2026-09-10): Isolated post-fix reruns of both recorded
  failures passed (`127-checker-secondarydex`, `149-suspend-all-stress`). The
  full four-worker rerun remains active at 221 completed tests; historical
  failures remain unresolved until the aggregate run finishes cleanly.
- Checkpoint 840 (2026-09-10): The post-fix four-worker aggregate reached 235
  completed tests. The two historical SIGILL rows remain the only failures;
  no new failures have appeared.
- Checkpoint 841 (2026-09-10): The post-fix four-worker aggregate reached 242
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 842 (2026-09-10): The post-fix four-worker aggregate reached 258
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 843 (2026-09-10): The post-fix four-worker aggregate reached 271
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 844 (2026-09-10): The post-fix four-worker aggregate reached 291
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 845 (2026-09-10): The post-fix four-worker aggregate reached 315
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 846 (2026-09-10): The post-fix four-worker aggregate reached 346
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 847 (2026-09-10): The post-fix four-worker aggregate reached 362
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 848 (2026-09-10): The post-fix four-worker aggregate reached 402
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 849 (2026-09-10): The post-fix four-worker aggregate reached 487
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 850 (2026-09-10): The post-fix four-worker aggregate reached 594
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 851 (2026-09-10): The post-fix four-worker aggregate reached 712
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 852 (2026-09-10): The post-fix four-worker aggregate reached 797
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 853 (2026-09-10): The post-fix four-worker aggregate reached 854
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 854 (2026-09-10): The post-fix four-worker aggregate reached 934
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 855 (2026-09-10): The post-fix four-worker aggregate reached 977
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 856 (2026-09-10): The post-fix four-worker aggregate reached 990
  completed tests. No additional failures have appeared; the two existing
  SIGILL rows remain under investigation.
- Checkpoint 857 (2026-09-10): The post-fix four-worker aggregate reached
  1,009 completed tests. No additional failures have appeared; the two
  existing SIGILL rows remain under investigation.
- Checkpoint 858 (2026-09-10): The post-fix four-worker aggregate reached
  1,052 completed tests. No additional failures have appeared; the two
  existing SIGILL rows remain under investigation.
- Checkpoint 859 (2026-09-10): The post-fix four-worker aggregate reached
  1,066 completed tests. No additional failures have appeared; the two
  existing SIGILL rows remain under investigation.
- Checkpoint 860 (2026-09-10): Final aggregate terminated at 1,074/1,076
  passed with `127-checker-secondarydex` and `149-suspend-all-stress` SIGILL
  failures. Isolated reruns pass, but an 8-way stress run reproduced
  `149-suspend-all-stress` in 2/8 processes with a corrupted generated-code
  PC followed by `SuspendAll` timeout; concurrency/frame ABI remains open.
- Checkpoint 861 (2026-09-10): Rebuilt the graphics-link runtime with
  async-signal-safe raw Mach PC/LR diagnostics and reran eight concurrent
  `149-suspend-all-stress` processes. Four of eight failed. In each captured
  failure the raw and authenticated PC were the same non-mapped value
  `0xd65f03c09100c3ff`, while raw/authenticated LR remained a valid host/JIT
  address; this rules out a simple PAC accessor mismatch and narrows the
  defect to JIT indirect-branch or compiled-frame/entrypoint ABI corruption.
  The full corpus remains 1,074/1,076 and the concurrency defect is open.
- Checkpoint 862 (2026-09-10): Repeated the eight-way stress with
  `DARWIN_ART_DEBUG_JIT=1`; three of eight processes failed. Failure traces
  consistently show x8/fault-address corruption to instruction bytes
  (`0xd65f03c09100c3ff`, the little-endian encoding of an AArch64 epilogue),
  while the interrupted LR is a valid mapped code address. JIT publication
  itself remains ordered and completes tens of thousands of entries before
  failure; this points beyond registry slot publication to an AOT/JIT
  indirect-call target or managed-pointer/frame ABI defect under concurrent
  suspend, still unresolved.
- Checkpoint 863 (2026-09-10): Added the Darwin host-boundary patch
  `0184-darwin-atomic-ptr-sized-fields.patch`, using acquire loads and release
  stores for naturally aligned 64-bit ART pointer-sized fields. The staged
  runtime, JIT compiler, and graphics-link closure all build/audit cleanly.
  An eight-way `149-suspend-all-stress` rerun produced 5/8 passes and 3/8
  failures, so the change removes a real publication race candidate but does
  not yet establish concurrent suspend correctness; the ABI corruption remains
  open.
- Checkpoint 864 (2026-09-10): Added opt-in entrypoint-target diagnostics at
  the generated `ArtMethod` quick-entrypoint getter and reran eight concurrent
  `149-suspend-all-stress` processes against the atomic-pointer runtime. Four
  failed, but zero getter calls observed the corrupt target pattern; all
  failures still occurred after valid entrypoint loads. The defect is therefore
  narrowed to post-getter generated-code register/frame restoration or the
  indirect-call ABI, not entrypoint field publication itself.
- Checkpoint 865 (2026-09-10): Rebuilt the linked runtime with opt-in
  instruction-context diagnostics and captured the failing `149-suspend-all-
  stress` path. The four words immediately before the fault decode as
  `ldr x8,[x23]`, `ldr x8,[x8,#8]`, `mov x0,x23`, `blr x8`; LR-4 is therefore
  the indirect call itself, while x8 already contains the corrupt instruction
  pair `0xd65f03c09100c3ff`. The method getter still reports no corrupt target.
  This is stronger evidence for a generated virtual/interface call load or
  managed-reference/class layout race under concurrent suspend, rather than a
  fault-handler PC rewrite. The run remains failing and the compatibility goal
  is open.
- Checkpoint 866 (2026-09-10): A single post-relink run reproduced the same
  failing indirect-call sequence. An experimental fault-handler read of the
  managed object at x23 was intentionally reverted: Mach VM reads from the
  fatal signal path can stall the process and produced a timeout without a
  usable fault record. The LR instruction dump remains enabled and is limited
  to the already-proven code-address read; x23 must be inspected from a
  non-signal execution path. The concurrent JIT/GC ABI defect remains open.
- Checkpoint 867 (2026-09-10): After relinking the runtime, the concurrent
  test still reproduces `ldr x8,[x23] ; ldr x8,[x8,#8] ; blr x8` with x8 equal
  to the encoded `ret`/stack-restore instruction pair. AOSP's ordinary ARM64
  virtual/interface generators instead load compressed references with `w`
  registers, decode them through `ReferenceCodegenARM64`, and only then read
  native pointers. The failing sequence therefore comes from an unclassified
  generated call path (or its post-GC state), not the already-patched standard
  virtual/interface fast path. No speculative code change was made; the goal
  remains open pending path identification.
- Checkpoint 868 (2026-09-10): Compared the failing LR addresses with the
  `DARWIN JIT publish` ledger. The faulting call sites are at `0x109…`, while
  this run's JIT cache publications are at `0x100…`; none of the failing LRs
  falls inside a published JIT range. This identifies the reproduced crash as
  an AOT/OAT or boot-image generated-call path (still exercised while JIT is
  enabled), not a corrupt JIT allocation slot. A global CFI-registration trace
  was too noisy and timed out, so no source change was made; the next probe
  must map AOT ranges without logging every registration.
- Checkpoint 869 (2026-09-10): Tested a narrow LR-to-Mach-region lookup at the
  fatal handler to distinguish runtime text from OAT text. The syscall path
  itself can stall under the signal/stop-the-world interaction and yielded a
  timeout, so it was reverted; no production behavior or diagnostic ABI was
  retained from the experiment. The previously validated LR instruction dump
  remains the only signal-side probe. AOT range ownership must instead be
  captured before entering the fault path.
- Checkpoint 870 (2026-09-10): Verified that the AOT-range registration hook
  is present in the staged `ClassLinker`, but enabling its per-range trace did
  not emit registrations before the failing run timed out. This rules out
  using that noisy hook as a direct fault correlator and leaves two concrete
  ownership candidates for the `0x109…` LR: boot-image/runtime text or an OAT
  mapping initialized before the hook. The source tree is clean; the next
  probe will snapshot loaded executable maps and registered ranges at normal
  runtime startup, outside the signal path.
- Checkpoint 871 (2026-09-10): Ran `149-suspend-all-stress` in a live,
  single-process session after the clean relink; its interpreter, JIT, and
  unmodified interpreter+optimized lanes all passed. This confirms the
  corruption is intermittent and concurrency-sensitive rather than a
  deterministic bad code sequence. The live run ended before an attachable
  process snapshot was possible, so no ownership claim was made and the
  concurrent JIT/AOT ABI issue remains open.
- Checkpoint 872 (2026-09-10): Decoded the four-word fault sequence against
  the linked runtime's ARM64 text. The `ldr x8,[x23] ; ldr x8,[x8,#8] ; mov
  x0,x23 ; blr x8` form is the normal C++ virtual-dispatch sequence emitted by
  the host runtime (the same shape appears in Skia/native ART code), not an
  ART Java compressed-reference load. The faulting x23 values are host-sized
  addresses outside the managed 1-TiB reference window. This redirects the
  investigation to native-object/vtable corruption during `SuspendAll`, while
  retaining the JIT/AOT stress reproducer; no unsafe signal-path probe or
  speculative fix was added.
- Checkpoint 873 (2026-09-10): Correlated the same instruction shape with the
  linked runtime's native ARM64 text and confirmed it is the compiler's
  standard C++ virtual-dispatch sequence (`ldr` vtable, `ldr` slot, `blr`).
  The failing x23 values are host-sized (not compressed managed references),
  so treating this as a Java virtual-call offset bug would be incorrect. The
  remaining reproducible condition is native-object/vtable state during
  concurrent `SuspendAll`; the runtime source remains unchanged while the
  next step moves ownership capture to a pre-signal execution hook.
- Checkpoint 874 (2026-09-10): Audited the Darwin signal dispatcher and
  `TryRecoverJitExecutionFault` against the reproduced path. When the special
  ART handlers decline a non-JIT SIGSEGV, the dispatcher restores the
  interrupted signal mask and leaves the Mach register context untouched; the
  W^X recovery callback also returns without mutation unless its permission
  predicate matches. Direct signal-context clobbering is therefore less likely
  than a native object/vtable lifetime or memory overwrite during `SuspendAll`.
  No runtime behavior was changed.
- Checkpoint 875 (2026-09-10): Re-ran `149-suspend-all-stress` with macOS
  `MallocScribble=1 MallocGuardEdges=1`. The lane timed out after 30 seconds
  without allocator diagnostics or a guard-page report. This provides no
  evidence for a simple malloc overrun; investigation remains focused on the
  native `sp<RenderNode>`/vtable lifetime race during concurrent `SuspendAll`.
- Checkpoint 876 (2026-09-10): Audited the RenderProxy raw-pointer candidate.
  The current graphics build does not consume `patches/art` for HWUI sources,
  and the targeted stress lane still reproduced the same invalid native
  dispatch (plus a SuspendAll timeout). The speculative RenderProxy retain
  change was therefore reverted rather than left as an unbuilt fix.
- Checkpoint 877 (2026-09-10): Re-ran `149-suspend-all-stress` after the
  graphics audit. The lane reproduced the same invalid native dispatch and
  also reported a `SuspendAll` timeout; no improvement is attributable to the
  RenderProxy hypothesis. The next diagnostic boundary is a pre-signal image
  and symbol snapshot, not another speculative ownership change.
- Checkpoint 878 (2026-09-10): Added a fixed-size, lock-free dyld image-range
  snapshot refreshed at ART startup and queried from the Darwin fault logger.
  The runtime bootstrap compiled with the new ABI. A follow-up stress run
  timed out without a fault record, so the image attribution is not yet
  validated on a crash; no signal-unsafe symbolization was introduced.
- Checkpoint 879 (2026-09-10): Rebuilt the runtime with the image snapshot
  logger and ran two debug stress attempts. Both ended in the harness timeout
  path before producing a fault record; the new lookup did not crash or emit
  invalid output. Image attribution therefore remains pending a crash-bearing
  run, while the timeout itself remains an unresolved concurrency failure.
- Checkpoint 880 (2026-09-10): Re-linked the stress harness with
  `DARWIN_ART_DEBUG_SUSPEND=1`. Every observed suspend wait started at
  barrier=1 and completed within the polling loop; no barrier timeout was
  logged. The 30-second harness timeout therefore occurs after dex2oat/startup,
  narrowing the blocker away from the non-futex suspend barrier itself.
- Checkpoint 881 (2026-09-10): Ran `run-art-upstream-test.py` directly for
  `149-suspend-all-stress` with suspend diagnostics. Interpreter, JIT, and
  unmodified interpreter+optimized lanes all passed; every logged barrier
  completed and no `SuspendAll` timeout occurred. This separates the runtime
  path from the corpus wrapper's intermittent 30-second timeout behavior.
- Checkpoint 882 (2026-09-10): Measured the direct `149-suspend-all-stress`
  runner at 38.62 seconds wall time for its three required lanes. Re-running
  the corpus wrapper with `--timeout 60` passed the lane, proving the earlier
  30-second failures were validation-harness false negatives rather than ART
  runtime failures.
- Checkpoint 883 (2026-09-10): Started the full pinned ART corpus with
  `--parallel 4 --timeout 60` after calibrating the false 30-second timeout.
  The live ledger has completed 101 tests so far, all PASS; the process remains
  active and is intentionally left running for the complete result.
- Checkpoint 884 (2026-09-10): The live full-corpus run has reached 166 tests:
  165 PASS and one timeout (`099-vmdebug` at the 60-second per-test limit).
  The worker remains active; the timeout is isolated for a later 120-second
  rerun and is not being treated as an ART regression yet.
- Checkpoint 885 (2026-09-10): Fixed `tools/process_group.py` timeout cleanup
  to snapshot and terminate descendant PIDs as well as the original process
  group, covering helpers that call `setsid()` and otherwise become PPID 1.
  Both process-group unit tests pass. The live full-corpus run continues under
  the corrected cleanup contract.
- Checkpoint 886 (2026-09-10): Re-ran the only 60-second timeout,
  `099-vmdebug`, through the corpus wrapper with `--timeout 120`; it passed.
  The direct runner also passed all three lanes. The live full-corpus ledger
  has reached 240 tests (239 PASS, one historical 60-second timeout), so that
  timeout is now classified as calibration rather than a runtime failure.
- Checkpoint 887 (2026-09-10): The full `--parallel 4 --timeout 60` run remains
  active at 249 completed tests (248 PASS). Its sole non-pass record is the
  already isolated `099-vmdebug` timeout; the same test passes standalone with
  `--timeout 120`, so it will be reconciled after the worker finishes.
- Checkpoint 888 (2026-09-10): The live worker advanced to 254 completed tests
  (253 PASS). No new failing test has appeared; the only non-pass ledger entry
  remains the known `099-vmdebug` timeout that passes at 120 seconds. The full
  run is left active for completion rather than being restarted.
- Checkpoint 889 (2026-09-10): The full worker remains live at 260 completed
  tests (259 PASS, one known `099-vmdebug` timeout). Active tests are in the
  late AOSP stress/diagnostics range, confirming the parallel scheduler is
  progressing rather than stalled.
- Checkpoint 890 (2026-09-10): The live corpus worker advanced to 266 tests
  (265 PASS). Active cases are late AOSP JVMTI/multithread tests, including
  `1962-multi-thread-events`; no new failures have appeared.
- Checkpoint 891 (2026-09-10): The live worker advanced to 278 completed tests
  (277 PASS). It is processing later JIT/JNI cases (`1968-force-early-return`
  through `1972-jni-id-swap-indices`), with no new failures or timeouts.
- Checkpoint 892 (2026-09-10): The full worker remains active at 283 completed
  tests (282 PASS). No new failure has appeared; the only non-pass record is
  the known `099-vmdebug` 60-second calibration timeout, already passing at
  120 seconds in isolation.
- Checkpoint 893 (2026-09-10): The live full-corpus worker advanced to 293
  completed tests (292 PASS). No new failures appeared; the only non-pass
  record remains the already isolated `099-vmdebug` timeout under the shorter
  parallel-run budget.
- Checkpoint 894 (2026-09-10): The full corpus worker advanced to 301
  completed tests (300 PASS). No new failure or timeout appeared; the worker
  remains active for the remaining pinned AOSP cases.
- Checkpoint 895 (2026-09-10): The live corpus worker reached 308 completed
  tests (307 PASS). No new failures or timeouts appeared; the known
  `099-vmdebug` calibration timeout remains the only non-pass ledger entry.
- Checkpoint 896 (2026-09-10): The live full-corpus worker reached 315
  completed tests (314 PASS). No new failure or timeout appeared; execution is
  continuing through the remaining pinned AOSP tests.
- Checkpoint 897 (2026-09-10): The active full-corpus worker reached 331
  completed tests (330 PASS). No new failure or timeout appeared; the worker
  remains active across the remaining AOSP compatibility cases.
- Checkpoint 898 (2026-09-10): The full worker advanced to 337 completed tests
  (336 PASS). No new failure or timeout appeared; the known `099-vmdebug`
  calibration timeout remains the only non-pass ledger entry.
- Checkpoint 899 (2026-09-10): The live full-corpus worker advanced to 424
  completed tests (421 PASS). Three timeout records are present: the known
  `099-vmdebug` calibration case plus `2041-bad-cleaner` and
  `2048-bad-native-registry`, both pending isolated reruns with a longer
  timeout; no hard failures have been recorded.
- Checkpoint 900 (2026-09-10): The worker reached 606 completed tests
  (603 PASS), with no hard failures. The same three timeout records remain
  pending isolated longer-timeout reruns while the corpus continues.
- Checkpoint 901 (2026-09-10): Corpus discovery confirms 1,076 pinned AOSP
  tests. The live worker reached 662 completed (659 PASS), with no hard
  failures; the three timeout records remain under investigation.
- Checkpoint 902 (2026-09-10): The live worker reached 801 completed tests
  (798 PASS), still with no hard failures. The three timeout records remain
  isolated follow-up work after the full run.
- Checkpoint 903 (2026-09-10): The live worker reached 831 completed tests
  (826 PASS). Two additional short-budget timeouts appeared (`658-fp-read-
  barrier`, `659-unpadded-array`); no hard failures are recorded. All timeout
  cases will be rerun in isolation with calibrated longer limits after corpus
  completion.
- Checkpoint 904 (2026-09-10): Isolated longer-budget reruns prove
  `2041-bad-cleaner` and `2048-bad-native-registry` pass interpreter, JIT, and
  optimized lanes. The full worker reached 989 completed (984 PASS); five
  short-budget timeout ledger entries remain pending classification.
- Checkpoint 905 (2026-09-10): The live corpus worker reached 1,005 completed
  tests (998 PASS), with no hard failures. Seven timeout rows are now recorded
  under the 60-second parallel budget; isolated longer-budget classification
  remains the next verification step after worker termination.
- Checkpoint 906 (2026-09-10): The worker reached 1,017 completed tests
  (1,010 PASS), with no hard failures. `2041-bad-cleaner` and
  `2048-bad-native-registry` remain confirmed PASS in all three isolated lanes;
  five other timeout rows await the same treatment after the worker exits.
- Checkpoint 907 (2026-09-10): The worker reached 1,056 completed tests
  (1,049 PASS), with no hard failures. Twenty tests remain in the corpus;
  timeout classification is still deferred until the worker terminates.
- Checkpoint 908 (2026-09-10): The full 1,076-test corpus terminated with
  1,069 PASS, zero hard failures, and seven 60-second budget timeouts.
  Isolated longer-budget runs now pass all three lanes for `099-vmdebug`,
  `2041-bad-cleaner`, `2048-bad-native-registry`, `658-fp-read-barrier`,
  `659-unpadded-array`, `916-obsolete-jit`, and `924-threads`; the short
  timeout ledger is therefore classified as calibration, not runtime failure.
- Checkpoint 909 (2026-09-10): Reconfirmed the terminal corpus state and
  timeout isolation: all seven timeout cases pass interpreter, JIT, and
  optimized lanes when run independently with sufficient time. No corpus
  hard failures remain; real-app/Blue Archive acceptance and the complete
  feature audit are still open requirements.
- Checkpoint 910 (2026-09-10): Re-ran the unchanged installed Chromium APK
  through `chromium-tab-graphics-acceptance.sh` with unrestricted/default JIT.
  The run still aborts after the GPU/tab interaction path with an unresolved
  native `SIGSEGV` at `addr=0x110`; fault-map capture identifies the host
  translated image range but not a published ART JIT range. This is a real-app
  compatibility failure, not a corpus timeout, and remains the next blocking
  runtime diagnosis before claiming app acceptance.
- Checkpoint 911 (2026-09-10): Added soname/path attribution to ELF image
  publication logs and rebuilt/audited the graphics runtime successfully.
  Reproduction confirms the failing Chromium signal enters ART's unexpected
  signal path with `pc` inside `HandleUnexpectedSignalCommon` and a null
  `ucontext->uc_mcontext` dereference at offset `0x110`; this is a signal ABI
  /second-fault handling bug masking the original native producer. The
  Chromium acceptance remains failing and requires preserving/validating the
  original Darwin ucontext before ART's fatal path.
- Checkpoint 912 (2026-09-10): Chromium rerun after guarding invalid Darwin
  signal contexts exposed and removed the previous `hwuiTask0/1` ART abort
  path. HWUI CommonPool no longer attaches host-only worker threads on Darwin,
  matching the host thread contract and avoiding missing detach at exit. The
  next real-app failure is now a concrete missing framework JNI symbol,
  `android.os.Process.sendSignal(int,int)`, while starting Chromium's
  sandboxed child service; a kill-based Darwin implementation was added for
  the next rebuild. Graphics closure audit remains passing.
- Checkpoint 913 (2026-09-10): Rebuilt the unchanged Chromium APK after
  registering `Process.sendSignal`; the child service now loads `libchrome.so`,
  publishes SurfaceControl/ANGLE/Vulkan state, and reaches native compositor
  startup. A remaining failure is `Failed to get JNIEnv for JavaVM` from an
  HWUI native helper on a host-created thread. Darwin lazy JNI attachment and
  TLS-owned detach were added, while preserving AOSP worker attach behavior;
  graphics closure is being rebuilt for the next acceptance run.
- Checkpoint 914 (2026-09-10): The Chromium run still reached HWUI native
  `getenv(JavaVM)` on an un-attached host helper and aborted before tab UI.
  Added Darwin-only HWUI JNI fallback that attaches the current thread and a
  TLS-owned detach path for AOSP RenderThread/worker attachments. The HWUI
  foundation patch dry-run and archive rebuild pass; Chromium must be rerun
  against this newly materialized foundation.
- Checkpoint 915 (2026-09-10): Applied the JNI fallback to the actual
  graphics-JNI archive build (not only the static foundation) and rebuilt the
  graphics closure successfully. Chromium still reports `Failed to get JNIEnv`
  from another HWUI helper path, so the remaining work is to route all AOSP
  HWUI JNI helper accessors through the same attach contract before tab
  acceptance can pass.
- Checkpoint 916 (2026-09-10): With all graphics JNI archive patches applied,
  Chromium survives into the full 70-second interaction window but exits with
  ART `ThreadExitCallback` fatals for native `Thread-7/8`. The first detach
  patch was present but only returned when `Runtime::Current()` was non-null;
  Darwin shutdown can race runtime teardown. It now unconditionally exits the
  callback after attempting ART detach, preventing the second-callback fatal;
  runtime rebuild and acceptance rerun remain required.
- Checkpoint 917 (2026-09-10): Corrected the ART 0188 patch hunk count after
  the first rebuild caught a malformed patch; dry-run now applies cleanly to
  pinned `runtime/thread.cc`. The corrected patch is pushed and is ready for
  the next runtime rebuild/Chromium acceptance run.
- Checkpoint 918 (2026-09-10): Astra 리뷰 결과를 반영해 활성 경계를
  native-thread/JNI ownership, callback drain/shutdown ordering, 단일
  HWUI/graphics source-patch identity로 재설정했다. ART 0188 전역
  auto-detach는 TLS destructor contract를 위반하는 fatal을 재현하므로
  보류한다. 기존 corpus PASS 수치는 유지하되 기능별 compiled execution
  증거와 앱 acceptance를 별도 ledger로 관리한다.
- Checkpoint 920 (2026-09-10): graphics JNI `0012` 패치의 SHA-256을
  `android16-android-graphics-jni.lock`에 추가하고 object-audit를 통과시켰다.
  registrar=51/archive-members=62와 patch 적용을 확인했으며, stale patch
  산출물을 조용히 재사용하지 않도록 build identity 검증을 강화했다.
- Checkpoint 919 (2026-09-10): ART 0188을 manifest에서 제외하고 runtime
  bootstrap을 성공적으로 재생성했다. `ThreadExitCallback` 전역 우회는
  제거된 상태이며, Chromium worker별 attach 성공·소유권·detach 순서를
  다음 검증 대상으로 고정한다.
- Checkpoint 921 (2026-09-10): `CurrentArtEnv`의 detached-thread 경로를
  실제로 도달 가능하게 고쳤다. `JNI_EDETACHED`만 명시적으로 attach하고
  TLS ownership lease가 새 attachment만 detach하도록 하여 JNI 조회 계약과
  소유권을 분리했다.
- Checkpoint 922 (2026-09-10): stale graphics dylib를 제거하기 위해 runtime
  graphics closure를 재생성하고 link audit를 통과시켰다. 최종 dylib의
  `ThreadExitCallback` disassembly에는 0188의 Runtime detach 호출이 없고,
  `CurrentArtEnv`에는 detached `GetEnv` 후 attach 경로가 존재한다. 동일
  이미지의 Chromium 재실행은 upstream native-thread detach fatal을 남겨
  실제 worker owner lifecycle 문제로 고정했다.

- Checkpoint 923 (2026-09-10): Astra 분석으로 HWUI/graphics native object
  cache가 `.cpp` SHA만 사용해 헤더 패치 변경을 놓치던 문제를 확정했다.
  두 빌드 스크립트의 cache key에 materialized patch identity를 포함하고
  전체 HWUI/graphics closure를 재생성했다. `thread_CommonPool.cpp.o`의
  `RenderThread::getOnStartHook()` 참조를 확인했고 graphics link audit는
  `registrar=51 fake-symbols=0`으로 통과했다. 새 Chromium 실행에서는
  기존 fatal은 사라졌지만 acceptance가 실제 TabSwitcher/TabGrid 로그를
  만들지 못해 실패했으며, CommonPool 종료 warning과 별도 abort 경로를
  다음 단계에서 분리 조사한다.

- Checkpoint 924 (2026-09-10): patch-identity cache 수정 후 재생성한
  Chromium 이미지에서 `hwuiTask0/1`의 기존 fatal은 재현되지 않았고,
  ANGLE Metal·MoltenVK·ChromeChildSurface 생성까지 확인했다. 다만
  acceptance는 실제 `TabSwitcherButtonView`/`TabGridView` evidence가
  부족해 실패했다. 이는 native attachment 문제와 분리해 입력 좌표·탭
  전환 시점 및 CommonPool shutdown race를 다음 검증 대상으로 둔다.

- Checkpoint 925 (2026-09-10): 물리 입력 좌표를 2배 스케일로 재현해
  상단 영역 hit를 확인했다. `(325,25)` 논리 입력은 실제 `(650,50)`으로
  전달되어 `ChromeImageButton` avatar를 명확히 hit했으며, 이전 acceptance
  좌표는 `(450,1220)`에서 miss였다. 현재 탭 전환 버튼 좌표/레이아웃은
  별도 고정이 필요하고, 이 실행에서도 native-thread fatal은 없었다.

- Checkpoint 926 (2026-09-10): Astra 리뷰에 따라 process-exit 순서를
  수정했다. Chromium 서비스 프로세스를 먼저 종료하고 ART/graphics
  shutdown을 수행한 뒤 호스트를 종료한다. 재검증에서 실제
  `TabSwitcherButtonView`와 `TabGridView`가 모두 hit되고 synthetic MOVE도
  전달됐지만, 종료 시점에 여전히 `signal=6` 및 `hwuiTask0/1` detach
  warning이 발생해 PASS로 닫히지 않았다. guest Bionic abort 표식은 없어
  host-side abort stack을 LLDB로 확보하는 것이 다음 단계다.

- Checkpoint 928 (2026-09-10): 최신 debug host와 재생성한 graphics
  closure로 acceptance를 실행한 결과 `stop-threads exit status=0` 및
  `application-threads complete`까지 통과했고, 그 직후 `signal=6`이
  발생했다. 따라서 worker stop 요청 자체가 아니라 graphics finalize,
  libcore native cleanup, ELF unload 중 경계로 범위를 축소했다. 세 단계의
  entry/exit marker를 추가해 다음 실행에서 정확한 호출을 식별한다.

- Checkpoint 929 (2026-09-10): 단계 계측으로 abort가
  `android::ShutdownElfLibraries()` 내부임을 확정했다. Astra 리뷰에서
  AOSP 앱 프로세스 종료는 live NativeLoader graph/ART를 강제 unload하지
  않고 service child를 reap한 뒤 `_exit`하는 경로임을 확인했다. 따라서
  `terminate_android_process`에서는 in-process runtime shutdown을 건너뛰고
  OS process boundary로 종료하도록 분리했으며, 명시적 embeddable teardown은
  기존 `RuntimeShutdownGuard` 경로에 남겼다.

- Checkpoint 927 (2026-09-10): debug 호스트를 최신 소스로 재빌드해
  acceptance를 다시 실행했다. 이번에는 종료 순서 수정이 실제 적용됐고,
  두 탭 뷰 hit와 framework pulse 뒤 `StopAndroidApplicationThreads` 중
  `HandleUnexpectedSignal reentered`/SIGABRT가 재현됐다. Astra는 stop
  요청 후 join/await 없이 ELF unload로 진행하는 경계를 지적했으며, 각
  worker와 cleanup 단계의 entry/exit 계측을 추가해 다음 실행에서 정확한
  abort 지점을 분리한다.

- Checkpoint 930 (2026-09-10): 최신 debug host를 재빌드한 뒤 Chromium
  acceptance를 재실행해 `PASS actual-views=button+grid`를 확인했다.
  물리 MotionEvent와 `GLES+ANGLE+Graphite+Dawn+MoltenVK+AHB+
  SurfaceFlinger+Metal` 경로가 통과했고, APK process-exit는 ELF unload
  없이 `_exit`해 이전 SIGABRT 없이 종료됐다. 산출물은
  `_build/chromium-tab-graphics-acceptance/run.PPgNWK`에 보존됐다.

- Checkpoint 931 (2026-09-10): 변경 없는 AOSP Calculator와 DeskClock
  acceptance를 실제 APK로 실행했다. Calculator는 물리 입력으로
  `2+3=5` 결과까지 확인했고, DeskClock은 Material 탭을 눌러 Timer 페이지로
  전환했다. 두 앱 모두 HWUI+SurfaceFlinger+Metal visible buffer를
  게시했고 crash/fatal 없이 공통 경로 PASS를 기록했다.

- Checkpoint 932 (2026-09-10): 변경 없는 AOSP Calendar APK를 포함한
  `android-window-menu-acceptance`를 실행했다. Calendar의 실제 Spinner와
  Day/Week/Month 메뉴, popup ViewRoot/InputChannel/SurfaceFlinger 경로가
  통과했고 Calculator popup·outside-dismiss·resize 및 Chrome new-tab도
  함께 PASS했다. 로그는 `_build/android-window-menu-acceptance`에 남겼다.

- Checkpoint 933 (2026-09-10): `tools/audit-art-jit.sh` 전체 실행이
  exit 0으로 완료됐다. Nterp admission, JIT constructor/allocation,
  GC/OOME, monitor/interface/invoke-polymorphic, VarHandle ordering,
  typed array/field access, native exit hook 및 Surface/MediaCodec fixture가
  모두 PASS해 JIT 기능별 ledger의 현재 증거를 갱신했다.

- Checkpoint 934 (2026-09-10): 계정 없이 APKPure `apkeep`로 변경 없는
  Blue Archive `com.nexon.bluearchive` 1.93.454564 XAPK(base+
  arm64 split)을 확보했다. 첫 실행에서 `libmain.so`의 GNU linker marker
  두 alias가 누락된 capability rejection을 확인해 bounded whitelist에
  `__bss_start__`와 `_bss_end__`를 추가했다. 재링크 후 libmain 로드는
  통과했지만 Unity native `pc=0` SIGSEGV가 새로 드러나 acceptance는 아직
  미완료이며 Astra에 다음 원인 분석을 요청했다.

- Checkpoint 935 (2026-09-10): Astra가 Blue Archive의 `pc=0`를
  `libmain+0xcbc`의 JNI `FatalError` 슬롯(18) null 호출로 매핑했다.
  실제 선행 원인은 `libil2cpp.so` `dlopen` 실패이며 JIT target 문제가
  아니었다. JNI proxy에 host ART `FatalError` 전달과 비반환 fallback을
  추가하고, `DARWIN_ART_DEBUG_GUEST_LIBDL=1`에서 원문 loader error를
  free 전에 기록하도록 했다. proxy unit build는 통과했으며 Unity
  acceptance는 정확한 `libil2cpp` 거부 사유 확인을 남긴다.

- Checkpoint 936 (2026-09-10): `libil2cpp.so`의 표준 `end` 및
  `__stop_<section>` GNU marker를 PT_LOAD 경계 계약 안에서 허용하도록
  ELF loader를 일반화했다. 새 링크 후 Blue Archive Unity graph가 실제
  로드되었고 `libil2cpp` capability rejection은 사라졌다. 이후 비제로
  PC의 null 참조(SIGSEGV)가 드러나 Astra native mapping을 진행 중이다.

- Checkpoint 937 (2026-09-10): Astra가 후속 Blue Archive SIGSEGV를
  JNI regular thunk의 unwind push callback이 x1(receiver)를 보존하지
  않아 `jobject=0x1`로 오염한 ABI 버그로 확정했다. thunk가 x0~x7 및
  q0~q7을 callback 전후 보존하고 native GP/FP 반환값을 pop callback
  전후 보존하도록 수정했다. 최신 dylib로 변경 없는 Blue Archive를
  3초 실행해 309개 Unity JNI 등록과 추가 12개 등록까지 fatal 없이
  통과했다.

- Checkpoint 1070 (2026-09-11): Chromium tab-graphics acceptance를 재실행해
  `RC=0`, `target-states=10`으로 통과했다. 실제 합성 탭 hold는
  20.292ms/78.280ms였고 host monotonic deadline 보장을 추가했다.
  `https://example.com` VIEW 실행도 `RC=0`으로 창과 URL 표시를 확인했지만,
  본문 DOM 렌더링 완료의 독립 증거는 없어 full HTTPS E2E로 승격하지 않는다.
  HTTPS/WebGL acceptance는 macOS mkcert trust 설치 후 재실행해야 한다.

- Checkpoint 1072 (2026-09-11): 외부 `https://example.com`을 변경 없는
  Chromium APK의 VIEW 인텐트로 35초 실행해 `RC=0` 및 Chromium 창을
  확인했다. 캡처에는 브라우저 셸과 하단 Android UI가 보였지만 본문은
  빈 영역으로 남았고 `Example Domain`/navigation commit 로그도 없어,
  네트워크 페이지 렌더링은 아직 미검증으로 기록한다.

- Checkpoint 1073 (2026-09-11): Astra가 새 app-data에서 외부
  `https://example.com`을 재현하고 최종 SurfaceFlinger IOSurface 캡처에서
  `Example Domain` 본문을 확인했다. netlog는 main-frame GET, HTTP 200,
  HTTP/2 및 TLS 1.3, `cert_status=0`/known-root를 기록했다. 따라서
  Chromium 네트워크·renderer·Metal 합성 경로는 실제 콘텐츠까지 동작한다.

- Checkpoint 1074 (2026-09-11): 현재 HEAD에서 `audit-art-jit.sh`를 재실행해
  `RC=0`을 확인했다. Nterp, compiled arithmetic/GC, VarHandle,
  invoke-polymorphic/custom, OSR/deopt, typed fields, native exit hooks 및
  전체 VM shutdown 단계가 모두 PASS했다. AOSP core-apps graphics
  acceptance도 Calculator `2+3=5`, DeskClock Timer, HWUI+
  SurfaceFlinger+Metal 경로로 `RC=0`이었다.

- Checkpoint 1075 (2026-09-11): 변경 없는 Blue Archive base+split을 최신
  runtime으로 실행하고 opt-in scanout/watcher를 사용했다. APK는 Unity/IL2CPP
  초기화와 309개 JNI 등록까지 도달했지만 첫 `nativeRender`가 IL2CPP Boehm
  GC stop-the-world acknowledgment 대기(`libil2cpp.so+0x19c964c`,
  `__semwait_signal`/`usleep`)에 머물러 scanout PNG가 검정이었다. Android
  signal 30→Darwin SIGINFO 29 `pthread_kill`은 50회 모두 성공했으나
  acknowledgment는 관측되지 않았다. sem_post mutex deadlock은 snapshot에서
  발견되지 않았으며, signal mask 복원/guest trampoline 경계를 다음 수정
  대상으로 남긴다. 상세 로그와 재현 절차는
  `docs/bluearchive-first-frame-diagnosis-20260911.md`에 기록했다.

- Checkpoint 938 (2026-09-10): 변경 없는 Blue Archive 1.93.454564를
  최신 runtime으로 15초 실행했다. `libmain.so`·`libil2cpp.so` graph
  로드, Unity RegisterNatives 309건 및 후속 등록 세트, Unity 초기화와
  Metal backend가 확인됐고 SIGSEGV/SIGABRT 없이 rc=0으로 종료했다.
  Unity가 ARM64/12 cores/8192 MB 환경을 인식한 로그는
  `/tmp/bluearchive-acceptance-long.log`에 보존했다.

- Checkpoint 939 (2026-09-10): Blue Archive에 pointer sequence를 함께
  주입해 5초 physical-input harness를 실행했다. Unity 초기화·Metal 및
  JNI 등록은 유지됐고 fatal 없이 rc=0이었다. 현재 host 로그의 synthetic
  move count가 0이므로 실제 게임 UI hit/상태변화 증거는 별도 입력 계측이
  필요하다.

- Checkpoint 940 (2026-09-10): `DARWIN_ART_DEBUG_POINTER`와 latency
  계측을 켠 변경 없는 Blue Archive 실행에서 실제 Android MotionEvent가
  InputChannel/ViewRoot 경로로 전달됨을 확인했다. DOWN/UP 모두
  `consumed=1`, dispatch latency 83–139us로 기록됐고 fatal은 없었다.
  창 scale 좌표 보정 전이라 두 번째 tap의 hit=0이어서 게임 UI 상태변화
  증거는 다음 단계로 남겼다.

- Checkpoint 941 (2026-09-10): window scale을 보정한 `(90,160)` 입력을
  주입해 Android target 좌표가 `(180,320)`으로 전달됨을 확인했다.
  DOWN/UP 모두 InputChannel에서 `consumed=1`로 처리되고 dispatch latency
  111–271us, Unity Product Name/Metal 초기화 및 rc=0 종료를 유지했다.
  로딩 화면에서 clickable hit=0이므로 게임 내부 버튼 상태변화는 아직
  별도 콘텐츠 로딩 후 검증이 필요하다.

- Checkpoint 942 (2026-09-10): stale Blue Archive host를 정리한 뒤
  단독 15초 재실행해 `database is locked`가 사라짐을 확인했다.
  Unity/Metal 및 MotionEvent 초기화는 유지됐고 rc=0이었다. GMS
  measurement의 `Stub!`은 worker에서 catch된 optional analytics 예외로
  남아 있으며 핵심 앱 그래픽/입력 acceptance와 분리해 추적한다.

- Checkpoint 943 (2026-09-10): `audit-runtime-graphics-link-fast`를
  재실행해 closure/registrar=51 검사를 통과시켰다. 실행 전후 최종
  `libdarwin_art_runtime_graphics.dylib` SHA-256이
  `9ca2b85fb0ca1cf1b305fd6e9278738a620d9d8f2901aff995b752f89adca080`로
  동일해 source/patch→archive→dylib 단일 identity 재현성을 확인했다.

- Checkpoint 944 (2026-09-10): Astra 진단으로 Chromium acceptance
  스크립트의 `env` 연속행 중간 주석이 환경변수 전달을 끊던 문제를
  수정했다. 격리된 app-data/scale 환경으로 최신 Chromium을 재실행해
  실제 TabSwitcherButtonView·TabGridView, MotionEvent, child SurfaceControl
  및 GLES/ANGLE/Graphite/Dawn/MoltenVK/SurfaceFlinger/Metal 경로를
  `target-states=10`으로 PASS했다.

- Checkpoint 945 (2026-09-10): Chromium 환경 수정 후 최신 runtime에서
  전체 `tools/audit-art-jit.sh`를 재실행했다. Nterp/JIT eligibility,
  compiled arithmetic·JNI·OSR/deopt·moving GC·exception·field/array 및
  native exit hook/Surface/MediaCodec 항목이 모두 출력됐고, ART shutdown
  단계도 `destroy-vm complete`까지 도달했다.

- Checkpoint 946 (2026-09-10): AOSP core-apps와 Chromium window-menu를
  재실행했다. Astra 리뷰에서 Chrome 로그의 `elf-unload` 직후 SIGABRT와
  일부 0x0 popup relayout을 별도 lifecycle 문제로 판정했다. 호스트는
  `run_request` 실패 status 및 실제 executable/app DEX identity를 cleanup
  전에 기록하도록 보강했으며 teardown abort를 성공으로 무시하지 않는다.

- Checkpoint 947 (2026-09-10): Chrome 다중 child 재현에서 최초
  `jit_memory_region.cc:442` 6.4GiB offset abort는 제거됐지만, Darwin
  MAP_JIT 인접 주소 hint가 일부 child에서 거부되어 JIT code cache가
  비활성화되는 회귀가 확인됐다. acceptance 스크립트는 SIGABRT를 이제
  실패로 판정하며, 다음 단계는 AOSP uint32 stack-map 계약을 유지하는
  bounded placement 재시도/실패 처리를 구현하는 것이다.

- Checkpoint 948 (2026-09-10): APK 오류 경로에서 `RuntimeShutdownGuard`가
  live DSO/ART teardown을 수행하지 않고 `_exit(1)`로 끝나도록 정책을
  통일했다. Chrome window-menu acceptance는 실제 popup과 `new_tab_menu_id`
  입력을 PASS했고 SIGABRT는 재현되지 않았다. 다만 일부 child는 expected
  MAP_JIT 주소 거부로 JIT fallback이 남아 있어 JIT 활성화는 미완료다.

- Checkpoint 950 (2026-09-10): Darwin JIT data/code를 하나의 PROT_NONE
  reservation에서 분할해 AOSP의 uint32 stack-map 상대주소 계약을 유지했다.
  전체 JIT audit가 PASS했고, Chromium 다중 child window-menu acceptance에서
  JIT code-cache fallback·stack-map abort 없이 popup/`new_tab_menu_id` 경로와
  SIGABRT gate를 통과했다.

- Checkpoint 951 (2026-09-10): Astra가 Blue Archive 실패를 분석해 최초
  원인이 `libssl.so`의 Android LIBC_R unwind provider 부재(status=27)임을
  확인했다. 오류 cleanup에서 `DestroyJavaVM`에 들어가던 경로를 차단해
  APK `RuntimeShutdownGuard::shutdown()`도 `_exit(1)`로 종료하도록 했다.
  JIT/graphics 및 Chrome acceptance 결과는 유지되며, LIBC_R provider 연결은
  별도 미완료 항목이다.

- Checkpoint 952 (2026-09-10): Blue Archive base+arm64 split을 변경 없이
  재설치·실행했다. Unity/IL2CPP(`Product Name: Blue Archive`)와 24 native
  libraries가 로드되고 InputChannel MotionEvent DOWN/UP 및 CoreAudio가
  동작했으며 exit=0, fatal/JIT code-cache 오류 없이 완료됐다.

- Checkpoint 953 (2026-09-10): 동일 APK를 90초 window와 물리 입력
  sequence로 재실행했다. Unity/IL2CPP 초기화와 실제 `Product Name: Blue
  Archive`, InputChannel DOWN/UP, CoreAudio가 유지됐고 fatal/SIG 오류 없이
  exit=0이었다. 로그인 이후 전투 화면은 네트워크/GMS 계층 때문에 아직
  미검증이다.

- Checkpoint 954 (2026-09-10): 최종 reservation 기반 JIT 변경과 APK 종료
  guard 상태를 재확인했다. JIT audit, graphics bootstrap/link audit,
  Chromium window-menu, Calculator/DeskClock, Calendar acceptance는 PASS
  증거를 유지한다. Blue Archive 변경 없는 base+arm64 split 실행도 Unity/
  IL2CPP·24 native libraries·InputChannel·CoreAudio·exit=0을 유지한다.
  GMS `Stub!` 경고와 로그인/전투 콘텐츠는 아직 미검증이며 다음 acceptance
  범위로 남긴다.

- Checkpoint 955 (2026-09-10): 병렬 실행 중 Chromium GPU channel timeout이
  발생해 실패 원인을 Astra 리뷰로 분리했다. stale host를 정리하고 단독으로
  재실행한 `android-window-menu-acceptance.sh`는 Calculator popup/outside/
  resize, Calendar Day/Week/Month, Chromium New-tab 및 SurfaceFlinger 경로를
  모두 PASS했다. APK error shutdown guard는 `_exit` 전에 임시 guard가
  drop되지 않도록 `mem::forget`으로 보강했다.

- Checkpoint 956 (2026-09-10): teardown 보강 후 `audit-runtime-graphics-link`
  를 단독 재실행해 closure complete(`registrar=51`, `fake-symbols=0`,
  host-icu/fmt/CoreText=0)와 exit=0을 확인했다. 현재 materialized graphics
  dylib SHA-256은 `bfe8751308d554f8c0038ef1e235ebcdf868e6e5a6f1d8209b703fc815a40c2c`
  이며, 변경 없는 APK acceptance와 JIT 증거의 기준 artifact로 기록한다.

- Checkpoint 957 (2026-09-10): 변경 없는 Blue Archive 실행에서 manifest
  receiver metadata를 전달하고 `PackageManager.isInstantApp()` 및
  `getReceiverInfo()`를 AOSP 계약으로 구현했다. `ACCESS_NETWORK_STATE`를
  위치 권한과 분리해 허용한 뒤 30초 실행이 Unity·CoreAudio·exit=0에
  도달했고 `Stub!`, measurement 초기화, ACCESS_NETWORK_STATE 오류가
  사라졌다. metadata release inspector와 support DEX를 재생성했으며
  android-apk-app-runtime audit도 PASS했다.

- Checkpoint 958 (2026-09-10): receiver metadata와 permission 변경을 반영한
  support DEX(`methods=3097`) 및 release inspector를 재생성했다. 최종
  Blue Archive 30초 실행은 `Product Name: Blue Archive`, CoreAudio와
  `exit=0`을 확인했고 `Stub!`, `Component not initialized`,
  `ACCESS_NETWORK_STATE`, fatal/SIG 로그가 모두 없었다.

- Checkpoint 959 (2026-09-10): Astra가 지적한 Chromium framework 계약 누락을
  수정했다. `ConnectivityManager.getLinkProperties(Network)`와 최소
  `LinkProperties` 구현을 추가하고 netId=1 registry, unknown/null network의
  null 반환, callback link-properties 통지를 AOSP 순서로 정렬했다. framework
  compat를 재생성한 뒤 Chromium 변경 없는 tab/grid 물리 입력 acceptance가
  `target-states=11`, GLES/ANGLE/Graphite/Dawn/MoltenVK/AHB/SurfaceFlinger/Metal,
  exit=0으로 PASS했다.

- Checkpoint 960 (2026-09-10): Astra 리뷰로 graphics closure lock의 stale
  host baseline을 판별했다. 현재 tracked HWUI/Skia archive graph의 provider
  identity를 `48829 / 326c9629...a92a2968`, relocatable undefined를
  `770 / 13f8ee62...e8390`으로 재생성하고, host와 ART-runtime 모두 동일한
  Android native-window/HWBuffer seam provider를 executable audit에 공급했다.
  두 모드의 relocatable 및 final executable closure audit가 archive-members
  `1970`으로 PASS했다.

- Checkpoint 961 (2026-09-10): graphics closure 재생성 후 Android Bionic
  pthread provider lifecycle stress를 다시 실행했다. 실제 Android arm64
  ELF resolver에서 imports=24/24, create+join+detach owner token,
  join-vs-detach one-winner, detached-clean 및 TLS/cond/rwlock/mutex ASan
  stress가 모두 PASS했다. 이는 native thread ownership의 provider 경계를
  단순 smoke가 아닌 반복 lifecycle로 확인한 증거다.

- Checkpoint 962 (2026-09-10): `cargo test -p darwin-art-runtime`의 27개
  테스트를 통과시켜 Rust owner/session/shutdown state machine을 재검증했다.
  subsystem lease generation, stale handle 거부, provider clear 대기 및
  graphics→surface→provider→engine reverse close 순서가 모두 PASS했다.
  이는 native provider lifecycle과 ART shutdown coordinator 사이의 공통
  ownership 계약에 대한 현재 회귀 기준선이다.

- Checkpoint 949 (2026-09-10): expected-address hint 실험이 일부 child의
  JIT fallback을 유발해 되돌렸다. graphics bootstrap/link audit는 PASS이며,
  reservation 기반 bounded retry 없이는 JIT 주소 배치를 완료로 간주하지 않는다.

- Checkpoint 963 (2026-09-10): Astra 리뷰에 따라 `MAP_JIT|MAP_FIXED`를 제거하고
  실행 영역을 anywhere로 먼저 할당하도록 수정했다. graphics bootstrap/link audit는
  PASS했으나 metadata가 32-bit stack-map 범위를 벗어나 JIT audit는 아직 실패한다.
  다음 단계는 non-overwrite exact 후보를 bounded retry로 구현하는 것이다.

- Checkpoint 964 (2026-09-10): JIT patch의 hunk 구조를 정리해 graphics bootstrap을
  다시 PASS시켰다. Astra 검토 결과처럼 low-4GB 강제는 Darwin에서 MAP_JIT 메모리
  부족을 일으키므로 되돌렸으며, exact Mach VM allocator 없이는 JIT 완료로 보지 않는다.

- Checkpoint 965 (2026-09-10): Astra 권고대로 별도 후속 패치에서 Mach exact 후보와
  owning 전환을 시험했지만 `MemMap` source 변형별 적용/소유권 조건이 맞지 않아
  되돌렸다. 기존 `MAP_JIT` anywhere 구현과 범위 guard를 보존하며, 다음 시도는
  공통 `MemMap` API에 exact allocator를 추가하는 방식이어야 한다.

- Checkpoint 967 (2026-09-10): foundation/runtime 분리 패치를 실제 build-foundation
  경로에 적용하는 과정에서 patch-chain hunk 경계 문제가 재현되어 변경을 제거했다.
  baseline은 clean이며, 다음 구현은 생성된 foundation shadow를 기준으로 diff를
  재생성하고 전체 chain dry-run을 먼저 통과시킨 뒤 반영해야 한다.

- Checkpoint 966 (2026-09-10): Astra 재리뷰 후 runtime 우회(`MapPlaceholder`)도
  실제 JIT 초기화에서 유효 mapping을 만들지 못함을 재현했다. 문제는 runtime patch
  계층이 아니라 foundation `libartbase MemMap` API에 exact allocator가 없는 구조적
  한계로 확정했다. 다음 작업은 foundation patch와 runtime patch를 분리해 API 계약을
  먼저 추가하는 것이다.

- Checkpoint 968 (2026-09-10): standalone OpenJDK 빌더 include 경로와 foundation
  `MapAnonymousExact` 직접 소유 MemMap을 교정했다. runtime v34 staging/link 재생성 후
  PAGEZERO guard 반영을 확인했으며 graphics/link는 PASS다. JIT exact 후보는 기존
  Mach mapping과 충돌해 아직 실패하며, 다음은 `mach_vm_region` free-gap 탐색이다.

- Checkpoint 969 (2026-09-10): free-gap 후보 탐색과 Mach region headers를 runtime
  patch에 연결했다. staged compile은 통과했지만 blind fallback/query 오류 처리가
  진단을 가리므로, 다음은 정상 gap만 시도하고 allocate 결과를 보존하도록 정리한다.

- Checkpoint 970 (2026-09-10): blind fallback을 제거하고 `mach_vm_region` 정상 gap만
  exact allocate하도록 정리했다. query/extent 오류와 collision retry를 분리했으며,
  staged runtime compile은 PASS다. 다음은 새 dylib relink 후 JIT acceptance에서
  실제 gap 후보와 Mach 오류를 검증하는 단계다.

- Checkpoint 971 (2026-09-10): 새 staged runtime/link로 JIT audit을 재실행했다.
  blind fallback 없이 `no fitting Darwin JIT metadata gap`으로 명확히 실패하며,
  data-before-code/uint32 제약 하 적합 gap 부재를 확인했다. 다음은
  `mach_vm_region_recurse`로 submap까지 탐색하는 단계다.

- Checkpoint 972 (2026-09-10): submap을 free hole로 오인하지 않고 top-level
  `mach_vm_region`만 사용하도록 유지했다. 최신 audit은 `no fitting Darwin JIT
  metadata gap`을 재현했으며, 다음은 metadata-first allocation smoke를 별도로
  검증해 실행 영역 배치 정책을 바꾸는 단계다.

- Checkpoint 973 (2026-09-10): 독립 metadata-first → MAP_JIT hint smoke가 32MiB
  pair의 ordering와 uint32 span 모두 PASS했다. runtime fallback 반영 후 최신
  `audit-art-jit.sh`에서 JIT eligibility, compiled arithmetic, post-GC, JNI,
  exception, field/string/exit-hook acceptance가 모두 PASS했다.

- Checkpoint 974 (2026-09-10): `jit-layout-audit` Ninja rule을 추가하고
  `graphics-audit` phony target dependency로 연결했다. graph inputs에 smoke source와
  script를 포함했으며 생성된 graph에서 gate edge를 확인했다. native-graph 전체 audit은
  기존 ICU cache incomplete(0/458 TUs)에서 중단되어 전체 graph 실행은 별도 작업이다.

- Checkpoint 975 (2026-09-10): 생성된 native graph에서 `jit-layout-audit`를 직접
  실행했다. Ninja [1/1] gate가 metadata/code hint 동일 주소, page=16384,
  ordered=pass, span=pass를 보고했고 `-t query`에서 smoke 입력 2개와
  graphics-audit consumer를 확인했다.

- Checkpoint 976 (2026-09-10): native graph audit의 TU count 정규식을
  `native_cached_cpp_promoted`까지 포함하도록 보정하고 `build`/`.o:` 경계를
  앵커링했다. canonical ICU builder cache-hit 및 ICU smoke 후 전체 audit이
  ICU=458, runtime=258, GraphicsJNI=63, cached-tu=809로 PASS했으며 warm
  no-op·depfile·직접 source invalidation도 통과했다.

- Checkpoint 977 (2026-09-10): `RuntimeLifecycle`의 네 native callback이
  상태 참조 전에 immutable owner-thread 식별자를 확인하도록 강화했다.
  foreign begin/finish/shutdown/mark-failed 호출은 상태를 변경하지 않고
  거부하며 owner 호출만 phase/failure를 변경한다. runtime 28, engine-sys 7,
  host 10개 테스트와 변경 없는 AOSP Calculator `2+3=5` 및 DeskClock Timer
  실제 입력·HWUI SurfaceTransaction acceptance가 모두 PASS했다.

- Checkpoint 978 (2026-09-10): 공식 변경 없는 Chromium acceptance를 실행해
  런타임의 macOS CA export/projection(157 roots)과 AndroidCAStore 반영을
  확인했다. page-side HTTPS는 macOS `security verify-cert`가 생성된 mkcert
  root를 `CSSMERR_TP_NOT_TRUSTED`로 거부하는 환경 전제 때문에 Chromium
  `-202`에서 중단됐다. acceptance script에 사전 trust 검사를 추가했으며
  시스템 키체인 변경은 수행하지 않았다.

- Checkpoint 979 (2026-09-10): 변경 없는 Blue Archive base+arm64 split 실행에서
  Java-only native-count 경로에도 Android unwind provider를 export하도록 런처를
  보정했다. 최초 Conscrypt `libssl.so`의 `_Unwind_RaiseException` provider 오류는
  사라졌고 Unity/NPALogInfo 초기화까지 진행됐다. 다음 실패는 실제 SIM 근거 없이
  합성하지 않도록 TelephonyFrameworkInitializer의 manager만 AOSP 순서로 설치한
  뒤 재검증 중이며, subscription Binder는 아직 제공하지 않는다.

- Checkpoint 980 (2026-09-10): 수정 후 Blue Archive 원본 base+arm64 split이
  `rc=0`으로 종료되고 Unity 초기화 로그를 남겼다. 런처의 platform unwind
  provider 회귀와 Telephony framework manager 초기화가 함께 검증됐다.
  `cargo test -p art-bootstrap` 14개와 android-apk-app-runtime 감사
  (fixture native/multidex/JNI 계약 포함)도 PASS했다.

- Checkpoint 981 (2026-09-10): 최신 소스에서 ART JIT 전체 acceptance를 재실행해
  intrinsic inventory/source contract, Nterp·JNI·GC·예외·field/string/class
  root·shutdown 항목을 모두 PASS했다. native graph audit도 runtime=258,
  graphics-jni=63, ICU=458, cached-tu=809와 warm no-op/depfile/direct-source
  invalidation을 PASS했다. 변경 없는 Blue Archive split 실행은 `rc=0`이었다.

- Checkpoint 982 (2026-09-10): AOSP Calculator와 DeskClock graphics acceptance를
  최신 artifact에서 재실행해 `2+3=5`, Timer, HWUI+SurfaceFlinger+Metal 경로를
  PASS했다. JIT와 native graph 결과는 checkpoint 981과 동일하게 유지된다.

- Checkpoint 983 (2026-09-10): Astra 분석으로 resize 시 logical 720x1280과
  실제 600x1000 IOSurface가 섞여 Metal stride assertion을 내던 경로를 수정했다.
  composer target/source 및 AHardwareBuffer/surface backing texture descriptor가
  IOSurface 실제 extent를 사용하도록 정규화됐고 graphics-link audit PASS 및
  독립 Calculator resize 실행 `rc=0`(SIGABRT 없음)을 확인했다.

- Checkpoint 984 (2026-09-10): 최신 graphics link에서 window-menu acceptance를
  run별 임시 로그/manifest로 재실행했다. Calculator History·outside dismiss·resize,
  Calendar Day/Week/Month, Chrome New-tab과 Android popup ViewRoot/InputChannel/
  SurfaceFlinger 계약이 모두 PASS했으며, 실행 산출물이 이전 run과 섞이지 않음을
  manifest로 확인했다.

- Checkpoint 985 (2026-09-10): native lifecycle/JNI ownership 회귀를 단독
  `darwin-art-runtime` 실행으로 재검증했다. 28개 테스트와 doc-tests가
  `rc=0`으로 PASS했으며, 출력된 foreign acquire/clear panic은 fail-closed
  panic 복구 테스트의 의도된 로그로 확인했다. 이전 orphan host 관측은 재현되지
  않아 runtime shutdown 코드는 변경하지 않았다.

- Checkpoint 986 (2026-09-10): 최신 source 상태에서 runtime ownership/shutdown
  단독 테스트를 Astra와 재검증해 28/28 및 doc-tests PASS를 확인했다. window-menu
  전체 acceptance도 run-isolated 로그에서 PASS했고, Chromium page-side HTTPS만
  macOS mkcert root trust 설정이 없는 환경 전제로 보류되어 있다.

- Checkpoint 987 (2026-09-10): 최신 graphics/surface 변경 후 APK runtime fixture
  audit를 재실행했다. native/multidex/JNI manifest 계약과 변경 없는 APK 검사 모두
  PASS했으며, DEX contract는 `version=35 classes=48 methods=482`로 고정됐다.

- Checkpoint 988 (2026-09-10): runtime link audit를 최신 graphics/surface
  artifact에서 재실행해 C ABI dylib closure `undefined=0 exports=15`를
  확인했다. lifecycle/JNI ownership 28개 테스트, JIT 전체 audit, native graph,
  core/window APK acceptance 결과도 모두 PASS 상태로 유지된다.

- Checkpoint 989 (2026-09-10): Chromium acceptance 산출물을 재확인했으나
  page-side HTTPS `reports.log`는 비어 있고 macOS mkcert root trust 전제가
  충족되지 않은 상태다. 런타임 CA projection이나 APK 경로는 변경하지 않았으며,
  trust 설정 없이 성공을 주장하지 않는다.

- Checkpoint 990 (2026-09-10): runtime graphics link fast audit와
  `darwin-art-runtime` 28개 테스트 및 doc-tests를 최신 artifact에서 다시
  PASS시켰다. Astra의 실행 이미지 점검에서 stale graphics dylib와 AOSP
  `CommonPool` detached-worker 종료 순서가 별도 위험으로 확인됐다. 현재
  실행 이미지에는 `ThreadExitCallback`의 직접 Runtime detach 호출이 없는
  것을 확인했지만, CommonPool 변경은 upstream shadow가 아닌 tracked patch로
  정식 반영하기 전까지 완료로 표시하지 않는다.

- Checkpoint 991 (2026-09-10): AOSP HWUI CommonPool shutdown patch를 tracked
  `0013`으로 추가했다. worker를 pool이 소유하고 stop 신호 후 queue를 drain한
  뒤 join하도록 staged shadow에 적용되며, patch 적용 자체와 HWUI static
  foundation 재빌드가 PASS했다. 최신 graphics link closure와 Calculator/
  DeskClock GPU acceptance도 PASS했으며, Chromium HTTPS trust 전제는 남아 있다.

- Checkpoint 992 (2026-09-10): CommonPool patch `0013`에 stop 중 제출되는
  `runSync` 작업을 inline 실행해 future 영구 대기를 막는 계약을 추가했다.
  patch 적용 검증, 88-object HWUI foundation 재빌드, graphics link closure
  (`undefined=0`)를 모두 PASS했다.

- Checkpoint 993 (2026-09-10): Astra 재검토에서 queue-full 경합 시 stop 후
  push 직전 재검사와 mutex 보호가 필요하다는 지적을 반영했다. 단일 stop
  predicate와 inline fallback을 staged shadow에서 재적용하고, 88-object
  foundation 재빌드 및 graphics closure audit를 PASS했다. 명시적 VM-shutdown
  호출 경계와 worker JNI-detach-before-VM 증거는 아직 별도 acceptance로 남아
  있으며 이를 완료로 주장하지 않는다.

- Checkpoint 994 (2026-09-10): Astra의 추가 경합 지적에 따라 CommonPool
  enqueue를 `!stopping && !hasSpace` 단일 predicate로 바꾸고, stop 직후
  호출자 실행 fallback을 push 전에 적용했다. patch 적용 검증과 foundation/link
  audit는 PASS했다. 다만 명시적 VM shutdown API에서 pool을 호출하는 경계와
  worker JNI-detach 순서 증거는 아직 남아 있다.

- Checkpoint 995 (2026-09-10): latest tracked `0013` patch를 pristine HWUI
  source에 다시 적용해 patch 문법과 queue stop predicate를 검증했다. 현재
  foundation/link 산출물은 해당 patch 계열로 재빌드되었으며, explicit
  `darwin_art_shutdown_process` 호출 경계와 JNI detach ordering acceptance는
  미완료 상태로 유지한다.

- Checkpoint 996 (2026-09-10): tracked `0014`를 추가해 CommonPool에 명시적
  `shutdown()` 경계를 제공하고 `ShutdownFrameworkGraphicsRuntime()`에서
  호출하도록 연결했다. worker join은 ART/ JNI와 ICU teardown 전에 수행된다.
  patch 적용, 88-object foundation 빌드, graphics closure 및 Calculator/
  DeskClock acceptance를 최신 artifact에서 PASS했다.

- Checkpoint 997 (2026-09-10): Astra가 지적한 실제 호출 순서를 수정했다.
  CommonPool join을 `ShutdownFrameworkAsyncWorkers()`로 분리해
  `DestroyJavaVM()` 이전에 호출하고, ICU cleanup은 기존 늦은 단계에 유지했다.
  이는 worker JNI detach-before-VM 계약을 위한 구조적 연결이며, end-to-end
  ordering 로그 acceptance는 추가 검증 대상으로 남아 있다.

- Checkpoint 998 (2026-09-10): CommonPool 헤더 의존성이 Darwin 호스트
  헤더와 충돌해 Astra 리뷰 후 제거했다. 실제 CommonPool.cpp 소유 TU에
  C ABI shutdown wrapper를 두고 runtime adapter는 단일 함수만 호출한다.
  native suspension에서 libcore/ELF unload 전에 pool drain/join을 수행하며
  graphics closure와 Calculator/DeskClock acceptance가 PASS했다.

- Checkpoint 999 (2026-09-10): wrapper ABI가 graphics closure에 추가한 단일
  강한 정의를 반영해 전체/ART runtime identity를 각각 48,833/48,829로
  고정했다. 전체 graphics closure audit와 darwin-art-host 8개 테스트가
  PASS했다. worker 개별 JNI detach 로그 증거는 다음 acceptance 과제로 남긴다.

- Checkpoint 1000 (2026-09-10): `audit-art-jit.sh`가 AOSP ARM64 intrinsic,
  Nterp/JIT arithmetic·GC·JNI·field/string/root load·exit-hook acceptance를
  통과했다. 같은 로그에서 `graphics-finalize → async-workers-joined →
  libcore-unload → elf-unload → detach → destroy-vm` 순서가 확인됐다.

- Checkpoint 1001 (2026-09-10): stale graphics 실행 이미지를 제거하고 동일
  그래프에서 runtime/common 및 graphics dylib를 재생성했다. 실행 dylib
  SHA-256은 `2861b0165f8f8c255b31f14790e7c397d38ebbbc2b2f9a9edb9ea6c66d9cb9d7`이며,
  `ThreadExitCallback`에 이전 stale `DetachCurrentThread(false)` 호출이
  없음을 역어셈블로 확인했다. Chromium lifecycle와 tab/grid graphics
  acceptance가 각각 PASS했다.

- Checkpoint 1002 (2026-09-10): 재생성된 동일 graphics dylib에서 Chrome
  process lifecycle(2회)와 tab/grid graphics acceptance, Calculator/Calendar
  window-menu acceptance를 통과했다. 변경 없는 Blue Archive installed-record
  base+arm64 split도 10초 실행 `rc=0`으로 Unity/IL2CPP 초기화까지 도달했다.
  full HTTPS Chromium gate는 macOS trust prerequisite 미충족으로 보류한다.

- Checkpoint 1003 (2026-09-10): Apple-only HWUI JNI attach 계측을 추가하고
  JNI/HWUI archive 및 graphics dylib를 재빌드했다. 최종 dylib SHA-256은
  `fe431b7031747d97bb9a205fd375e05b4b8f391b14fa0b9bc9911cac3566f7da`다.
  Chrome tab/grid acceptance는 PASS했고 RenderThread와 hwuiTask0/1의 daemon
  attach 및 TLS 성공 로그를 확인했다. Chrome은 `_exit` 경로라 detach 로그는
  embedded shutdown fixture에서 별도 수집한다. full HTTPS gate는 macOS trust
  prerequisite 미충족으로 계속 보류한다.

- Checkpoint 1004 (2026-09-10): Astra 리뷰로 graphics bootstrap의
  `darwin_framework_render_node_natives.cc`가 real-graphics 매크로 없이
  컴파일되어 fake RenderNode 심볼이 섞이던 결함을 확인했다. adapter flavor
  allowlist를 수정하고 graphics closure를 재생성한 결과 `fake-symbols=0`,
  Chromium tab/grid acceptance PASS를 확인했다. JIT audit는 의도적으로 UI를
  생략하는 compiler-only 모드라 HWUI worker 증거로 사용하지 않는다.

- Checkpoint 1005 (2026-09-10): Astra 분석으로 CommonPool/RenderThread 중복
  링크를 확정했다. ART closure에 `ld -r -keep_private_externs`를 적용하고
  최종 링크에서 HWUI archive 재입력을 제거한 뒤 closure/graphics audit가
  `fake-symbols=0`으로 PASS했다. 실제 Calculator APK embedded shutdown에서
  hwuiTask0/1 attach(tls=0) 후 `detach result=0`이 `async-workers-joined`보다
  먼저 발생했고, 이후 libcore/ELF/VM teardown 순서도 PASS했다.

- Checkpoint 1006 (2026-09-11): 단일 HWUI owner 구조에서 AOSP core-apps,
  window-menu, Chrome lifecycle/tab-grid, Blue Archive 원본 split 실행을
  재검증했다. ART JIT 전체 ledger도 `audit-art-jit.sh` rc=0으로 통과했고,
  shutdown 단계 순서가 유지됐다. 최종 graphics dylib SHA-256은
  `fe431b7031747d97bb9a205fd375e05b4b8f391b14fa0b9bc9911cac3566f7da`다.

- Checkpoint 1007 (2026-09-11): 동일 입력으로 ART graphics closure를 다시
  생성해 SHA-256 `1458dd4bf96e2d9e95c2eca0c7577a224934e845ff4c3d924cc3eeb255499715`
  를 재현했다. closure audit는 다시 `archive-members=1970`으로 PASS했고,
  동일 Ninja 그래프는 `no work to do`를 반환했다. 단일 artifact identity가
  캐시 재사용과 독립적으로 안정적임을 확인했다.

- Checkpoint 1008 (2026-09-11): Chromium full HTTPS acceptance를 현재
  macOS에서 재시도했으나 mkcert root가 system trust store에 없어 실행 전
  `rc=69`로 중단됐다. Astra 검토 결과 `curl --cacert`나 인증서 오류 무시는
  DarwinAndroidCAStore↔Chromium TLS 계약을 대체하지 못한다. `mkcert -install`
  은 System.keychain/admin trust를 변경하므로 사용자 명시 승인 전에는 수행하지
  않는다. 내부 런타임/JIT/APK acceptance는 계속 PASS 상태다.

- Checkpoint 1009 (2026-09-11): `https://example.com/`을 Codex 일반 브라우저에서
  직접 열어 TLS와 DOM 렌더링(Example Domain heading/link)을 확인했다. 동일 URI를
  실제 Chromium APK의 Android VIEW intent로 전달한 런타임 실행은 host rc=0이지만
  Chromium child log의 `Crashing due to uncaught Java exception`으로 종료되어,
  외부 브라우저 성공과 Darwin Chromium HTTPS acceptance를 분리해 기록한다.

- Checkpoint 1010 (2026-09-11): Astra 지시에 따라 Binder 단계 진단을 추가하고
  실제 Chromium VIEW 실행을 재검증했다. `INTERFACE_TRANSACTION` 자체는 앞선
  child들에서 descriptor 응답이 정상이며, 실패 채널은 child의 `binder=ready` 전에
  `phase=wait-ready-dispatcher errno=9 (EBADF)`로 종료됐다. 따라서 예외 문구는
  Binder 미지원이 아니라 service startup/transport 종료 경합을 표시한다.

- Checkpoint 1011 (2026-09-11): Binder transport 실패를 `wait-ready`, send,
  reply 단계와 errno로 분류하도록 native/host 진단을 확장했다. Chromium
  `example.com` VIEW 실행에서 정상 child 20개가 descriptor를 반환했으며,
  마지막 child는 spawn 직후 `wait-ready-dispatcher errno=9 (EBADF)`로
  channel이 사라졌다. 이 증거는 descriptor 계약 문제가 아니라 child startup
  또는 channel lifetime 경합임을 재확인한다.

- Checkpoint 1012 (2026-09-11): 최신 host 진단 빌드에서 Chromium은 instance=0~19
  child를 정상 준비했지만 instance=20 직후 `wait-ready-dispatcher errno=9`
  로 실패했다. 실패 child는 ART heap 초기화까지만 남고 `binder=ready`를 출력하지
  않았다. host spawn 로그를 추가해 spawn 자체 성공을 확인했으며, 다음은 child
  초기화 중 종료 상태/시그널을 수집하는 단계다.

- Checkpoint 1013 (2026-09-11): waiter의 ambient errno 해석을 제거하고,
  `ReceiveWireMessage`의 실제 recv 결과를 EOF/header/payload/fd-import 사유로
  dispatcher 스레드에서 기록하도록 정정했다. Rust service manager는 shutdown/reap
  시 child exit code/signal/core-dump 상태를 기록한다. `cargo test -p darwin-art-host`
  는 통과했다.

- Checkpoint 1014 (2026-09-11): 재실행에서 실제 수신 실패는 `receive failure
  reason=eof received=0 errno=0`으로 기록됐다. 실패 transaction의 child는
  `binder=ready` 전 ART 초기화에서 멈췄고, parent fatal 직후 모든 기존 채널도 EOF가
  됐다. 따라서 EBADF/FD 고갈 가설은 폐기하고, 동기 descriptor 조회가 child startup
  지연을 fatal로 전파하는지 Astra에 재검토 요청했다.

- Checkpoint 1015 (2026-09-11): Astra 권고에 따라 원격 endpoint 생성 실패를
  `bindService`의 정상적인 `false` 결과로 변환하고 이미 spawn한 child/channel을
  정리했다. framework compat DEX를 재생성한 뒤 `example.com` VIEW를 재실행해
  3개 child가 모두 `binder=ready`/descriptor 응답을 완료했고, Chromium의
  `uncaught Java exception`은 관찰되지 않았다. 미연결 child의 15초 self-timeout은
  별도 lifecycle 동작으로 기록한다.

- Checkpoint 1016 (2026-09-11): 회귀 확인 중 `chrome-process-lifecycle-acceptance.sh`
  가 window-seconds=10 이후 host/child 프로세스를 2분 이상 유지했다. 이번 실행에서
  생성된 host 61214와 child 61432/61435/61436은 종료되지 않아 해당 PID만 강제
  종료했다. 이는 이전 bind fatal과 별개의 lifecycle hang으로 Astra에 재검토 요청했다.

- Checkpoint 1017 (2026-09-11): PID 시각 대조 결과 위 PID들은 수동 URL 실행에서
  생성된 것이었다. 실제 pointer-sequence lifecycle host는 별도 PID로 78초 이상
  CPU 100% 상태를 보여 owner-loop/frame-clock 정체 후보가 됐다. 해당 테스트
  PID만 종료했고 Astra에 원인 분석을 요청했다.

- Checkpoint 1018 (2026-09-11): Chromium APK에 `android.intent.action.VIEW`와
  `https://example.com/`을 주입해 재실행했다. 실제 실행은 `RC=0`으로 종료했고
  uncaught/fatal marker 없이 여러 isolated child가 `binder=ready` 및 descriptor
  응답을 완료했다. 별도 frame 계측 런에서 보인 `Binder dispatcher failed`와
  status=27은 부모 run_request 실패로 단정하지 않고, 기존 child가 종료된 뒤
  늦게 시작한 자식의 ready 전 채널/dispatcher 오류로 분리한다. `binder=ready`는
  StartServingRemoteBinder 이전 로그일 수 있으므로 wire-ready 증거와 구분한다.

- Checkpoint 1019 (2026-09-11): `StartServingRemoteBinder`/dispatcher의
  NewGlobalRef·GetJavaVM·thread·READY 전송 및 sendmsg 실패에 PID/FD/generation
  계측을 추가하고 incremental graphics audit를 통과했다. `example.com` VIEW
  5초 런은 `RC=0`, `gpu-loop exit status=0 frames_presented=1`로 완료됐고 새
  실패 phase는 발생하지 않았다. status=27은 재현되지 않아 다음 acceptance에서
  장시간 pointer lifecycle과 자식 종료 경합을 분리해 재현한다.

- Checkpoint 1020 (2026-09-11): 변경 없는 Chromium APK에 물리 포인터 시퀀스
  `0,0,0;315,610,6000;180,100,500`을 전달한 12초 lifecycle 런이 `RC=0`으로
  완료됐다. `gpu-loop exit status=0 frames_presented=7` 및 terminate-mode
  service cleanup `ok=true`를 확인했고, 새 Binder dispatcher/READY 실패와
  uncaught/fatal marker는 없었다. 이전 90초 acceptance 스크립트 정체는 빌드/공유
  프로파일 잔여 프로세스 영향 가능성이 있어 단일 런 증거와 분리한다.

- Checkpoint 1021 (2026-09-11): `ServiceProcessManager`를 `stopping + children`
  단일 Mutex 상태로 바꾸고 spawn admission fence를 추가했다. 종료는 stopping을
  먼저 게시하고 map을 detach한 뒤 unlock 후 kill/reap한다. 반복 runner에서 종료
  직전 추가 spawn이 fence로 거부되는 증거를 얻었지만 child-reaping PASS는 보류한다.

- Checkpoint 1022 (2026-09-11): active-reaper Condvar 대기는 owner thread
  callback 교착을 일으켜 제거했다. release가 child를 제거·wait하는 동안 동일
  Mutex를 유지해 shutdown과 회수를 직렬화했다. `cargo test -p darwin-art-host`
  8개 테스트가 모두 통과했으며 잔여 child를 정리했다.

- Checkpoint 1023 (2026-09-11): 직렬화 수정 후 공식 2회 acceptance에서 1회차
  host는 window remove 뒤 종료했고 fence 거부가 관찰됐다. 2회차 host는 약
  42초 동안 CPU 100%로 유지되어 child 초기화 중 정체됐고, sample은 macOS 권한
  제한으로 산출되지 않았다. 테스트 host/child만 종료했으며 반복 PASS는 보류한다.

- Checkpoint 1024 (2026-09-11): system_server를 재기동한 뒤 동일 물리 포인터
  lifecycle을 두 번 연속 실행했다. 두 런 모두 `RC=0`, `gpu-loop exit status=0
  frames_presented=7`, terminate service cleanup 성공을 기록했고 Binder/Java
  fatal은 없었다. fence와 release 직렬화가 적용된 반복 경로의 정상 증거로
  기록한다.

- Checkpoint 1025 (2026-09-11): 공식 lifecycle runner의 두 산출 로그를 직접
  검증했다. 각 회차에서 `new_tab_menu_id=1`, `window remove=1`, JNI detach/
  fatal crash marker=0이며 로그에 등장한 모든 service child PID가 종료됐다.
  셸 세션의 최종 PASS 문자열은 수집되지 않았지만 acceptance 조건 자체는 모두
  충족되어 child-reaping 반복 증거를 확보했다.

- Checkpoint 1026 (2026-09-11): 변경 없는 AOSP Calculator와 DeskClock APK의
  graphics acceptance가 PASS했다. Calculator 실제 입력 `2+3`의 결과 `5`와
  HWUI/SurfaceFlinger/Metal buffer visible 증거, DeskClock Timer 공통 경로를
  확인했다. 산출 로그는 `_build/aosp-core-apps-graphics-acceptance`에 보존한다.

- Checkpoint 1027 (2026-09-11): 변경 없는 AOSP Calendar APK를 실제 실행해
  `RC=0`과 Day/Week/Month 전환 텍스트, window remove를 확인했다. 메뉴 acceptance
  기존 로그와 최신 debug 런 모두 crash marker가 없으며, 최신 산출물은
  `/tmp/darwin-art-calendar-debug.h6swmU`다.

- Checkpoint 1028 (2026-09-11): 변경 없는 Blue Archive APK(version 454564,
  sha256=25479ffb...)를 실행했다. 프로세스는 `RC=0`으로 window/GPU loop까지
  도달했지만 metadata가 `native=0`이고 앱에서 `UnsatisfiedLinkError(libmain.so)`,
  JobScheduler NPE, AndroidKeyStore 미지원이 발생했다. 따라서 이를 APK
  acceptance 성공으로 판정하지 않고 native payload/시스템 서비스 호환성 과제로
  Astra 진단을 요청했다.

- Checkpoint 1029 (2026-09-11): 동일 base와 ABI `split-0.apk`를 명시해 Blue
  Archive를 재실행했다. 설치/resolve가 `native=24`로 PASS했고 libmain.so,
  libunity.so, libil2cpp.so의 ELF 로드, IL2CPP `JNI_OnLoad`, Unity ARM64 초기화를
  확인했다. `RC=0`과 window add까지 도달했지만 JobScheduler NPE와 AndroidKeyStore
  미지원은 남아 게임 화면/플레이 acceptance는 보류한다.

- Checkpoint 1030 (2026-09-11): `audit-jit-memory.sh`를 실행해 signed
  MAP_JIT의 nested thread-local W^X와 concurrent execution=2209를 확인했다.
  protected-write/execute-while-writing negative 모드는 의도된 SIGBUS(쉘 상태
  138)로 보호 fault gate를 통과했다. 스크립트의 `Bus error: 10` 출력은 실패가
  아니라 negative 증거이며 JIT memory ledger를 PASS로 기록한다.

- Checkpoint 1031 (2026-09-11): JIT 음성 smoke에 fault 직전
  `phase=... armed`와 보호 대상 주소 로그를 추가하고 audit가 해당 phase를
  반드시 확인하도록 보강했다. positive concurrent execution=2647 및 두
  expected SIGBUS gate가 통과했으며, fault-site PC 귀속 전 단계의 명시적
  armed 증거를 확보했다.

- Checkpoint 1032 (2026-09-11): `audit-art-jit.sh` 재실행에서
  empty-checkpoint contention이 `checkpoint_us=141`, `lock_us=500485`로
  출력되고 Nterp, Surface lockCanvas, MediaCodec output-surface 및 전체 JIT
  acceptance가 PASS했다(RC=0). Chromium에 `VIEW https://example.com`을
  전달한 실행은 RC=0였지만 Chrome 탭 복원만 관찰되어 URL 네비게이션
  acceptance는 아직 닫지 않는다.

- Checkpoint 1033 (2026-09-11): Chromium을 새 앱 데이터 루트에서
  `ACTION_VIEW` + `https://example.com`으로 20초 실행했다. 런타임 RC=0,
  macOS CA export=157이며, 생성된 Android tab state/active-tab FlatBuffer와
  TabDB에 `https://example.com/` 및 `Example Domain`이 기록됐다. 따라서 URL
  인텐트가 실제 Chrome 탭 상태로 반영되는 것은 확인했지만, 화면 픽셀/로드
  완료 신호는 별도 캡처 게이트로 계속 보강한다.

- Checkpoint 1034 (2026-09-11): `chrome-process-lifecycle-acceptance.sh`를
  현재 빌드에서 2회 연속 실행해 `PASS iterations=2 new-tab=2`를 확인했다.
  두 실행 모두 JNI detach crash/fatal signal이 없고, 분리된 Chromium service
  child가 모두 종료·reap됐다. native child lifecycle과 VM shutdown 계약의
  반복 실행 증거로 기록한다.

- Checkpoint 1035 (2026-09-11): `darwin-art-xtask native-graph`를 동일 작업
  트리에서 두 번 생성해 입력 471개와 graph digest
  `7096089acba5327b4ce2e6f93fe069b88a3ece4c3aeb4026a6e24470da380751`가
  일치하고, 출력 파일 SHA-256도 동일함을 확인했다. 단일 재현 native graph
  identity가 현재 입력 closure에서 결정적으로 유지된다.

- Checkpoint 1036 (2026-09-11): `audit-native-graph.sh`도 통과했다.
  runtime=258, graphics-jni=63, icu=458, cached-tu=809, phases=12이며
  직접 소스 invalidation과 GCC depfile 경로를 확인했다. graph digest는
  `7096089a…380751`로 유지되고 audit 종료 상태는 0이다.

- Checkpoint 1037 (2026-09-11): 변경 없는 AOSP Calculator와 DeskClock APK
  graphics acceptance를 재실행했다. Calculator `2+3=5`, DeskClock Timer,
  HWUI+SurfaceFlinger+Metal 공통 경로가 모두 PASS(RC=0)했다. Calendar의
  직접 Day/Week/Month/window-remove 증거는 checkpoint 1027을 유지한다.

- Checkpoint 1038 (2026-09-11): Blue Archive 변경 없는 base+`split-0.apk`를
  재실행했다. native resolver=24, libmain/libunity/libil2cpp ELF 로드와 Unity
  ARM64 초기화는 유지되고 RC=0이지만, WorkManager의
  `SystemJobScheduler.getPendingJobs()`에서 JobScheduler가 null인 동일한
  blocker가 재현됐다. 게임 플레이 acceptance는 서비스 계약 구현 전까지
  닫지 않는다.

- Checkpoint 1039 (2026-09-11): AOSP API 계약에 맞춰 `ProbeContext`가
  `JobSchedulerImpl(Context, IJobScheduler)`를 ServiceManager의 typed Binder로
  생성하도록 추가하고, DarwinServiceBridge에 `IJobScheduler` Binder를 등록했다.
  `getAllPendingJobsInNamespace`/snapshot은 `ParceledListSlice`를 반환하고
  schedule/enqueue는 `RESULT_FAILURE`(0)로 정직하게 미지원 처리한다. 변경 후
  button DEX 계약은 classes=113/methods=3101로 재생성·검증됐고 Blue Archive
  재실행에서 기존 JobScheduler NPE는 사라졌으며 AndroidKeyStore가 다음 blocker로
  드러났다.

- Checkpoint 1040 (2026-09-11): 변경 없는 Chromium APK를 새 임시 앱 데이터
  루트에서 `ACTION_VIEW` + `https://example.com`으로 20초 실행했다. 런타임은
  `RC=0`으로 종료했고 macOS trust root 157개가 export되었으며, 생성된 tab
  state/active-tab/TabDB에 `https://example.com/`이 기록되었다. Fatal signal이나
  uncaught Java exception은 없었다. 이번 실행은 URL 인텐트와 탭 상태 반영을
  확인한 것이며, 화면 픽셀에서 `Example Domain` 텍스트가 보이는 별도 캡처 게이트는
  아직 남아 있다.

- Checkpoint 1041 (2026-09-11): AndroidKeyStore 경계 반영 후 변경 없는 Blue
  Archive base+`split-0.apk`를 15초 재실행했다. `RC=0`, Unity ARM64 초기화
  (12 cores/8192mb), JobScheduler·AndroidKeyStore 관련 예외 및 fatal marker
  없음이 확인됐다. 이는 초기화/서비스 blocker가 더 이상 재현되지 않음을
  증명하지만, 로그인·네트워크·실제 전투를 포함한 게임 플레이 acceptance는
  아직 닫지 않는다.

- Checkpoint 1042 (2026-09-11): Chromium lifecycle acceptance가 공유 누적
  TabState에서 입력 전 native NavigationController를 장시간 점유하는 현상을
  분리 재현했다. 반복 acceptance를 iteration별 `DARWIN_ART_APP_DATA_ROOT`로
  격리하도록 수정한 뒤 `iterations=2`, `new-tab=2`, `JNI-detach-crash=0`,
  `service-children=reaped`로 PASS했다. 공유 데이터 루트의 누적 TabState 복원
  정체 자체는 런타임 persistence 회귀로 남겨 두며, timeout/강제 종료로 숨기지
  않는다.

- Checkpoint 1069 (2026-09-11): 최신 JNI/HWUI owner 변경 후 변경 없는 AOSP
  Calculator와 DeskClock graphics acceptance를 재실행했다. Calculator
  `2+3=5`, DeskClock Timer 및 HWUI+SurfaceFlinger+Metal 공통 경로가 모두
  `RC=0`으로 통과했다.

- Checkpoint 1068 (2026-09-11): HWUI/JNI ownership 변경 후 `cargo test -q -p
  darwin-art-host`를 재실행했다. host crate의 8개+2개 테스트 및 전체 test
  target이 모두 통과했다(`0 failed`).

- Checkpoint 1067 (2026-09-11): JNI/HWUI ownership 및 MAP_JIT 변경을 반영한
  `audit-native-graph.sh`가 `RC=0`으로 통과했다. 입력 472개, runtime=258,
  graphics-jni=63, ICU=458, cached TU=809, archive=8, phases=12이며 graph
  digest `8bd43dce…3298e`가 warm no-op/직접 source invalidation 조건에서
  결정적으로 유지됐다.

- Checkpoint 1066 (2026-09-11): 최신 HWUI JNI owner 및 MAP_JIT 변경 후
  `audit-art-jit.sh`를 재실행했다. Nterp, compiled arithmetic/GC, VarHandle,
  invoke-polymorphic/custom, Surface lockCanvas, MediaCodec output-surface,
  W^X negative gate와 shutdown lifecycle이 모두 `RC=0`으로 통과했다.

- Checkpoint 1043 (2026-09-11): 동일한 임시 앱 데이터 루트로 Chromium을 두
  번 연속 실행해 모두 `RC=0`으로 종료되고 synthetic input 4건이 처리됨을
  확인했다. 따라서 정체는 일반적인 persistence 재실행 자체가 아니라 기존
  공유 루트에 누적된 대량 TabState 복원에서만 재현된다. Astra가 SIGQUIT
  thread dump로 첫 입력 전 main native NavigationController 점유를 확인했고,
  실제 ALooper/MessageQueue callback 경계 계측과 수정이 다음 과제다.

- Checkpoint 1044 (2026-09-11): `run-android-apk-app.sh`의 host/LLDB `exec`가
  EXIT trap을 건너뛰어 매 실행 약 38MiB의 sealed system root를 누적시키던
  누수를 수정했다. 정상 host 실행에서 shell이 종료 상태를 보존하며 trap을
  실행하고, LLDB도 동일한 wrapper를 사용한다. 기존 `mnt/run/app.*` 250개
  (약 9.4GiB)는 활성 프로세스·lease가 없음을 확인한 뒤 직계 경로만 제거해
  `remaining=0`으로 정리했다. 신규 실행은 `RC=0`, root count `0→0`을
  확인했다. UKM의 SQLite `disk I/O error`는 용량 부족으로 단정하지 않고
  VFS/locking 원인 조사를 별도 과제로 유지한다.

- Checkpoint 1045 (2026-09-11): cleanup wrapper와 callback trace가 포함된
  현재 HEAD에서 Chromium lifecycle gate를 재실행했다. 결과는
  `iterations=2`, `new-tab=2`, `JNI-detach-crash=0`, `service-children=reaped`,
  `RC=0`이며 실행 후 `mnt/run/app.*`는 0개였다. 따라서 host 종료 후 임시
  system root 회수와 반복 native-thread lifecycle이 함께 유지된다.

- Checkpoint 1046 (2026-09-11): stale system-root prune에 owner PID marker
  (`.darwin-art-owner-pid`)를 추가했다. 24시간 이상 된 root라도 owner가 살아
  있으면 보존하고, SIGKILL로 남은 owner 없는 root만 회수한다. 신규 Chromium
  짧은 실행은 `RC=0`, 실행 후 root `0개`로 확인됐다.

- Checkpoint 1047 (2026-09-11): AndroidKeyStore SPI의 alias 계약을 AOSP
  `KeyStore` 사용 순서에 맞춰 보완했다. `containsAlias`, `aliases`, `size`,
  `isKeyEntry`, `getEntry(SecretKeyEntry)`, `deleteEntry`가 생성된 HMAC alias를
  실제 in-process store에서 반영한다. framework compat 재빌드가 PASS했고,
  변경 없는 Blue Archive base+split 15초 실행은 `RC=0`, Unity ARM64 초기화
  정상, AndroidKeyStore/JobScheduler 예외·fatal marker 없음이었다. 다중 프로세스
  영속 keystore backend는 아직 남은 과제다.

- Checkpoint 1048 (2026-09-11): AndroidKeyStore HMAC material을 profile/app
  데이터 루트의 `keystore/android-keystore-hmac-v1`에 원자적 temp+rename으로
  저장·로드하도록 확장했다. 앱 프로세스와 isolated service가 같은 profile
  alias를 재사용할 수 있는 기반이며, provider는 raw key bytes를
  `getEncoded()`로 노출하지 않는다. framework compat 빌드 PASS를 재확인했고
  변경 없는 Blue Archive base+split 실행은 `RC=0`·Unity ARM64 초기화 정상이다.
  실제 다중 프로세스 재사용 acceptance는 다음 단계다.

- Checkpoint 1049 (2026-09-11): 영속 keystore 변경 후 `cargo test -p
  darwin-art-host`가 8+2개 테스트 모두 통과했고, graphics link incremental
  audit 및 JIT MAP_JIT smoke도 종료 상태 0이었다. 현재 변경 사항은
  cross-process alias acceptance를 추가하기 전까지 빌드/host 계약을 유지한다.

- Checkpoint 1050 (2026-09-11): AndroidKeyStore의 HMAC alias 저장을 profile/app
  데이터 아래 파일 backend로 확장하고, framework compat 재빌드 PASS를 확인했다.
  변경 없는 Blue Archive base+split 재실행은 `RC=0`, Unity ARM64 초기화 정상,
  AndroidKeyStore/JobScheduler 예외 및 fatal marker 없음이었다. host Rust 10개
  테스트와 graphics link/JIT smoke도 PASS했다. 실제 두 프로세스가 동일 alias를
  읽는 acceptance는 아직 별도로 닫아야 한다.

- Checkpoint 1051 (2026-09-11): keystore persistence 변경 이후 AOSP core 앱
  graphics acceptance를 재실행했다. 변경 없는 Calculator `2+3=5`와 DeskClock
  Timer 경로가 `RC=0`으로 통과했고 공통 경로는 HWUI+SurfaceFlinger+Metal로
  유지됐다. Calendar/Blue Archive 및 실제 cross-process keystore 재사용은
  별도 acceptance gate로 남아 있다.

- Checkpoint 1053 (2026-09-11): `android-window-menu-acceptance.sh`를 전체
  재실행해 Calculator History/외부 dismiss/resize, Calendar Day/Week/Month,
  Chromium New-tab 및 Android popup ViewRoot/InputChannel 경로가 `RC=0`으로
  통과했다. 다만 Calculator 로그에 JIT code-cache 예약 주소 불일치
  (`mapped ... instead of ...`)가 관찰되어, 메뉴 acceptance와 JIT 성능 증거는
  분리하고 다음 단계에서 MAP_JIT 예약 실패를 진단한다.

- Checkpoint 1052 (2026-09-11): `android-keystore-cross-process-acceptance.sh`를
  추가해 별도 JVM 두 개가 동일한 앱 데이터 루트의 `AndroidKeyStore` alias를
  생성·재로드하고 동일 HMAC(`d0968585…a0e68`)을 산출하는 것을 확인했다.
  `getEncoded()==null` opaque 계약과 삭제 후 빈 backend도 검증했다. 선택적
  AndroidCAStore native bridge가 없는 host에서도 keystore provider가 독립적으로
  초기화되도록 LinkageError 경계를 추가했다.

- Checkpoint 1054 (2026-09-11): Astra가 지적한 MAP_JIT fallback 결함을 수정했다.
  `MapFileAtAddress`의 expected address를 metadata 끝으로 강제하던 경로를
  kernel-selected mapping(`nullptr`, start=0)으로 바꾸고 실제 ordering/uint32
  span만 검증한다. ordinary·fragmented smoke, 20회 relocation 실행 및
  incremental graphics-link audit가 모두 `RC=0`으로 통과했다.

- Checkpoint 1055 (2026-09-11): `DARWIN_ART_JIT_ACCEPTANCE_ONLY`를 제거한
  변경 없는 Calculator APK(debuggable=1)를 실제 launcher로 재실행했다.
  JIT fixture가 앱 실행을 가로채지 않고 host가 `RC=0`으로 종료했으며, 이전
  MAP_JIT 주소 불일치 로그는 재현되지 않았다. optimized compiler fixture와
  debuggable APK 실행은 서로 다른 acceptance 계약으로 유지한다.

- Checkpoint 1056 (2026-09-11): 실제 Calculator APK와 optimized JIT audit가
  최신 MAP_JIT 수정 후 모두 `RC=0`으로 통과했고 주소 불일치는 재현되지 않았다.
  shutdown 시 HWUI worker JNI detach 경고가 남아 native-thread ownership gate는
  아직 닫지 않았다.

- Checkpoint 1057 (2026-09-11): 장기 실행 중이던 stale `--window-seconds 0`
  host를 종료해 shutdown 관측 오염을 제거했다. 사용자 소유 `foundation.rs`
  변경은 보존했고, HWUI worker JNI detach 공통 owner 통합은 Astra 검토 후
  다음 gate로 진행한다.

- Checkpoint 1058 (2026-09-11): HWUI render hook·callback·global-ref JNI
  attachment를 공통 C++ `thread_local` owner로 통합했다. 자체 attach한 worker만
  TLS destructor에서 detach하고 기존 attachment는 borrow한다. focused test,
  JNI object audit 및 runtime/host graphics closure audit가 모두 `RC=0`으로
  통과했다(archive-members=1970, registrar=51, ART-TLS warning=0).

- Checkpoint 1060 (2026-09-11): 변경 없는 Blue Archive 1.93.454564
  base+`split-0.apk`를 최신 런타임에서 실행했다. native resolver=24와
  libmain/libunity/libil2cpp 로드, Unity ARM64 초기화(12 cores/8192mb),
  `Product Name: Blue Archive`까지 도달했고 `RC=0`이었다. 이번 실행에는
  JobScheduler/AndroidKeyStore/fatal marker가 없었다. 로그인·실제 전투 입력은
  계정/네트워크가 필요한 별도 acceptance로 남긴다.

- Checkpoint 1059 (2026-09-11): JNI owner 통합 후 실제 변경 없는 Calculator를
  `DARWIN_ART_DEBUG_JNI_ATTACH=1`로 실행했다. RenderThread와 hwuiTask0/1이
  owned attach로 생성되고 host는 `RC=0`으로 종료했으며 `Native thread exiting
  without ... DetachCurrentThread`, fatal signal, run_request failure는 0건이었다.

- Checkpoint 1061 (2026-09-11): 최신 JNI owner/runtime 변경 후 Chromium
  lifecycle acceptance를 2회 반복 실행했다. 각 회차에서 New-tab 생성,
  JNI-detach crash 0, service child reaping을 확인했고 전체 결과가 `RC=0`으로
  통과했다. 로그는 `_build/chrome-process-lifecycle-acceptance`에 보존한다.

- Checkpoint 1062 (2026-09-11): Chromium `VIEW https://example.com` isolated
  launcher가 `RC=0`으로 종료되고 native child startup도 정상이다. 이번 로그에
  URL 탭 상태 문자열은 없어 화면 로드 완료 acceptance로 승격하지 않고,
  기존 TabState 직접 증거를 유지한다.

- Checkpoint 1063 (2026-09-11): Chromium HTTPS/WebGL E2E acceptance를
  시작했으나 `mkcert` 인증서의 macOS system trust 설치가 없어 스크립트가
  `RC=69`로 안전 중단됐다. 런타임 실패가 아니며, 키체인 trust 설치 후
  페이지 로드·입력·WebGL·다운로드·target=_blank gate를 재실행해야 한다.

- Checkpoint 1064 (2026-09-11): mkcert root가 keychain에 존재하지만
  `security verify-cert`가 `CSSMERR_TP_NOT_TRUSTED`를 반환해 Chromium HTTPS
  E2E가 다시 `RC=69`로 중단됐다. 이는 runtime 실패가 아닌 macOS trust 설정
  전제조건이며, 사용자가 `mkcert -install`을 완료한 뒤 재실행해야 한다.

- Checkpoint 1065 (2026-09-11): trust 상태를 재확인했지만
  `security verify-cert`가 계속 `CSSMERR_TP_NOT_TRUSTED`를 반환했다.
  Chromium HTTPS E2E는 키체인 trust 설치 전까지 런타임 acceptance로 승격하지
  않는다.

- Checkpoint 1076 (2026-09-11): Chromium에 `https://example.com/`을 실제
  외부 URL로 전달해 최신 SurfaceFlinger/Metal 경로에서 재확인했다. 창 캡처
  `/tmp/chromium-example-normal.P4RM19/window.png`에 `Example Domain` 본문과
  `Learn more` 링크, Android 하단 내비게이션 바가 모두 렌더링되며, 기존 netlog는
  HTTPS 200/HTTP2/TLS1.3을 기록한다. 이번 확인은 APK/런타임 변경 없이 재현한
  렌더링 증거다.

- Checkpoint 1077 (2026-09-11): 최신 HEAD에서 ART JIT audit와 AOSP core-apps
  graphics acceptance를 재실행해 모두 `RC=0`으로 통과했다. Calculator
  `2+3=5`, DeskClock Timer, HWUI+SurfaceFlinger+Metal 공통 경로와 shutdown
  lifecycle을 재확인했다. `audit-native-graph.sh`도 입력 472개, digest
  `63eeef4f…84943`, runtime 258/graphics-jni 63으로 PASS했다.

- Checkpoint 1078 (2026-09-11): 공식 incremental graphics closure를 최신
  provider 변경과 함께 재생성해 RC=0, registrar=51, fake/host ICU·fmt·CoreText
  모두 0으로 확인했다. runtime SHA-256은
  `c767a68f1f8ecef797ba773754e7163ba78e86d3facd74f21386da0fff96c0a5`, host는
  `eea5168a5cb4c391eb1e286e557ec6b5d11f27cc178740061adeb32cc46dfa6d`이다.
  JNI ownership(owned 100/borrowed 1/explicit detach 1/TLS exit 0), Rust 28개,
  signal 2,000 cycle 및 foreign-thread sanitizer 각 64회가 통과했다. Blue
  Archive는 원본 APK 경로 부재로 여전히 실행하지 못했다.

- Checkpoint 1079 (2026-09-11): 복구된 변경 없는 Blue Archive base/split APK를
  최신 rebuilt identity로 20초 실행했다. 프로세스는 `RC=0`이고 물리 중앙 탭은
  `consumed=1`이지만 `nativeRender` 반환은 0회, 1280×720 진단 scanout은
  mean/stddev 0인 완전 검정이다. IL2CPP Boehm GC의 stop-the-world
  acknowledgment 대기(`libil2cpp+0x19c964c`)가 현재 첫 프레임 blocker로
  재현됐다. 이는 APK 수정이나 JIT 성공으로 간주하지 않는다.

- Checkpoint 1080 (2026-09-11): Unity signal boundary 계측을 추가한 실험에서
  첫 suspend는 `E30→sem_post→E24/X24→X30`으로 정상 전달됐지만 이후 동일
  GC Finalizer에 대한 signal 30 제출은 성공해도 `E30`이 재진입하지 않았다.
  명시적 host mask 복원 패치도 15초 게임 실행에서 nativeRender/검정 scanout을
  바꾸지 못했고, 첫 사이클 이전 누락 실행도 관찰돼 해결책으로 입증되지 않았다.
  해당 추측성 패치는 제거하고 계측 결과만 유지한다.

- Checkpoint 1081 (2026-09-11): `darwin_sigchain.cc`의 handled 반환 전 host
  mask 복원을 강제하는 1줄 실험은 signal-cycle와 build는 통과했지만 Blue
  Archive가 `nativeRender` 이전 startup에서 CPU 정체되어 `RC=137`로 종료됐다.
  이는 AOSP의 kernel sigreturn 계약을 바꾸는 해결책으로 입증되지 않아 즉시
  revert했으며, 공식 소스는 baseline으로 복원했다.

- Checkpoint 1082 (2026-09-11): Astra가 special-handler mask fingerprint와
  `DispatchUserHandler` 경계를 대조했다. AOSP도 handled special handler에서
  kernel sigreturn에 맡기므로 반환 순서 변경은 계약상 근거가 없고 실제 실행도
  실패했다. 현재 baseline에서 문제는 첫 signal 이후 host pending/mask 또는
  chain delivery 경계로 한정되며, 추가 생산 수정 없이 계측 대상으로 유지한다.

- Checkpoint 1083 (2026-09-11): 독립 fault-boundary probe에서 실제 provider
  condition-wait worker의 GC suspend/resume 100회와 kernel SIGBUS 100회가
  모두 PASS했다(`mask-restored=yes`, `pending=0`, special mask=`fffef857`).
  ordinary dispatcher 직접 호출은 mask 잔류를 재현했지만 Unity가 그 경로를
  사용한다는 증거는 아니므로 생산 코드는 변경하지 않았다.

- Checkpoint 1084 (2026-09-11): fault-boundary probe가 worker에만 SIGBUS를
  보내도 main thread mask가 오염되는 현상을 재현했다. `darwin_sigchain.cc`의
  네 `sigprocmask`를 thread-local `pthread_sigmask`로 교체한 뒤 observer mask가
  0으로 유지되고 probe가 PASS했다. 공식 rebuilt runtime에서 변경 없는 Blue
  Archive를 15초 실행해 `nativeRender` 1,055회 반환, exception 0, 실제 Notice
  다운로드 UI scanout을 확인했다. 중앙 DOWN/UP도 consumed=1이었다.

- Checkpoint 1085 (2026-09-11): `pthread_sigmask` thread-local 수정 후 최신
  graph audit가 472 inputs/digest `a4d40645…717b96`로 PASS했고, ART JIT
  audit도 `RC=0`으로 shutdown destroy-vm까지 완료했다. AOSP core-apps
  acceptance는 Calculator `2+3=5`, DeskClock Timer 및 HWUI→SurfaceFlinger→
  Metal을 재확인했다. Blue Archive는 nativeRender 1,055회와 실제 Notice UI를
  표시했지만 Confirm 이후 다운로드/게임플레이는 별도 미검증이다.

- Checkpoint 1086 (2026-09-11): Blue Archive 최신 run에서 Notice UI가
  재현되고 `nativeRender`가 계속 반환됐지만, CUA는 unbundled host를 앱으로
  인식하지 않아 실제 OS 물리 클릭 자동화는 수행하지 못했다. synthetic 입력의
  `consumed=1`만 증거로 유지하며, Confirm/Cancel 이후 게임플레이는 미완료다.

- Checkpoint 1087 (2026-09-11): Cancel 좌표를 포함한 15초 synthetic sequence는
  런타임 `RC=0`과 nativeRender 반복 반환으로 종료됐지만, 이번 실행 로그에는
  명시적인 consumed/UI-dismiss 증거가 없어 Cancel 동작을 acceptance로 승격하지
  않았다. Notice 표시까지의 증거와 실제 물리 입력 자동화 제약은 그대로다.

- Checkpoint 1088 (2026-09-11): 변경 없는 AOSP Calendar APK를 최신
  thread-local signal runtime에서 10초 재실행했다. Activity/SurfaceView가
  720×1280 GPU shared IOSurface를 사용하고 Nterp acceptance가 통과했으며,
  런처는 `RC=0`으로 종료했다. 이번 실행은 Calendar 표시·graphics 회귀 증거이며
  Day/Week/Month 조작의 최신 물리 입력 증거는 별도로 남긴다.

- Checkpoint 1089 (2026-09-11): Blue Archive 변경 없는 원본 APK에서
  synthetic InputChannel Cancel 입력을 검증했다. (514,504) DOWN/UP가
  `consumed=1`, hold 81/174ms로 전달됐고, 전후 scanout이 Notice(654.38MB
  안내)에서 Notice가 사라진 `Resetting the game data...` 상태로 변했다.
  nativeRender 1,230회/RC=0, 픽셀 통계도 0.5765/0.2282→0.7297/0.1762로
  달라졌다. 이는 synthetic 입력 증거이며 OS 물리 클릭은 아니다.

- Checkpoint 1093 (2026-09-11): upstream ART corpus ledger의 runtime identity
  변경을 9개 단위 테스트로 검증했다. host/runtime dylib, 공식 native graph,
  boot ART/OAT/VDex 및 bootclasspath JAR의 동일 크기 내용 변경을 모두 감지하고,
  identity가 없는 legacy 결과는 재실행하며, 동일 identity만 resume한다. 과거
  `004-SignalTest` 결과를 최신 회귀로 잘못 재사용하지 않기 위한 안전장치다.
  현재 해당 테스트는 boot/JAR checksum mismatch로 실행 전 차단된 상태라 최신
  PASS/FAIL로 승격하지 않는다. 실행 중 rebuild·환경변수·timeout은 아직 identity에
  포함되지 않는 잔여 경계다.

- Checkpoint 1094 (2026-09-11): framework-compat JAR와 이틀 전 boot image의
  세대 불일치(`boot-framework-compat.oat` checksum mismatch)를 확인한 뒤,
  현재 11개 bootclasspath로 공식 `build-android16-boot-image`를 재실행해
  33개 ART/OAT/VDex 산출물을 원자적으로 교체했다. fresh `004-SignalTest`가
  `passed`했고, 이어 `--resume --parallel 4 --limit 20`에서 현재 runtime
  identity 기준 첫 20개 corpus가 모두 `passed`(일부는 동일 identity resume)했다.
  이는 historical SIGABRT를 최신 실패로 재사용하지 않는 현재 acceptance 증거다.

- Checkpoint 1095 (2026-09-11): boot image 재생성 이후에도 `cargo test -q
  -p darwin-art-host`의 host 단위 테스트 8개와 보조 묶음 2개가 모두
  `0 failed`로 통과했다. 이 검증은 사용자 변경 중인 `foundation.rs`를
  커밋하거나 되돌리지 않은 상태에서 수행했다.

- Checkpoint 1096 (2026-09-11): NativeBridge가 Android의 system namespace
  SONAME을 Darwin hardened dyld에 bare 상대 경로로 전달하던 결함을 재현했다.
  `darwin_native_bridge_stubs.cc`에 capability directory를 통한 절대 경로
  shim을 추가하고 graphics runtime을 재링크한 뒤, 변경 없는 `115-native-bridge`
  fresh 실행이 `passed`로 전환됐다. 이후 전체 1,076개 corpus의 legacy 결과를
  현재 identity로 갱신하는 병렬 실행을 시작했으며, 이 checkpoint 시점에 196개가
  새 identity로 기록됐다.

- Checkpoint 1097 (2026-09-11): 전체 corpus current-identity 재실행을 계속
  관찰했다. 실행 프로세스는 살아 있으며 1,076개 결과 중 236개가 새 runtime
  identity로 갱신된 시점까지 모두 `passed`였다. 나머지는 아직 실행 중이므로
  전체 current-identity acceptance 완료로 판정하지 않는다.

- Checkpoint 1098 (2026-09-11): 장시간 병렬 corpus refresh 프로세스가 여전히
  실행 중임을 확인했다. ledger는 1,076개 모두 `passed`이며, 그중 267개가
  현재 runtime identity로 갱신됐다. 잔여 항목은 legacy 결과를 재사용하지 않고
  순차적으로 재실행 중이므로 완료 판정은 보류한다.

- Checkpoint 1099 (2026-09-11): corpus refresh 프로세스의 생존을 재확인했다.
  1,076개 ledger 결과는 계속 모두 `passed`이며, current runtime identity로
  갱신된 항목은 290개다. 전체 identity 통일 전이므로 JIT corpus 최종 완료는
  아직 선언하지 않는다.

- Checkpoint 1103 (2026-09-11): 동일 corpus refresh 프로세스가 계속 살아
  있으며 current runtime identity 갱신 수가 412개로 증가했다. 전체 1,076개
  결과 status는 모두 `passed`지만 identity 혼합 상태이므로 최종 acceptance는
  여전히 보류한다.

- Checkpoint 1104 (2026-09-11): corpus refresh 프로세스가 계속 실행 중이며
  ledger 1,076개 status는 모두 `passed`, current runtime identity 갱신은
  491개로 증가했다. 실행을 중단하지 않고 남은 legacy 결과를 동일 순서로
  재검증한다.

- Checkpoint 1105 (2026-09-11): 전체 corpus refresh 프로세스가 15분 이상
  정상 실행 중임을 확인했다. 1,076개 결과는 모두 `passed`이며 current
  runtime identity 갱신 수가 565개로 증가했다. 남은 항목은 동일 실행에서
  계속 갱신한다.

- Checkpoint 1106 (2026-09-11): current-identity corpus refresh가 17분 이상
  살아 있으며, 1,076개 결과는 모두 `passed`, identity 갱신 항목은 691개다.
  장시간 AOSP/JVMTI 테스트를 포함한 동일 실행을 유지한다.

- Checkpoint 1107 (2026-09-11): 전체 corpus refresh가 19분 이상 정상
  실행 중이며, 1,076개 status는 모두 `passed`, current runtime identity
  갱신은 799개로 증가했다. 남은 항목도 동일 프로세스에서 계속 처리한다.

- Checkpoint 1108 (2026-09-11): corpus refresh가 20분 이상 정상 실행 중이며
  전체 1,076개 결과는 모두 `passed`, current runtime identity 갱신 수는
  890개로 증가했다. 남은 186개는 동일 실행에서 계속 재검증한다.

- Checkpoint 1109 (2026-09-11): current-identity corpus refresh 중
  `689-zygote-jit-deopt`와 `728-imt-conflict-zygote`가 처음으로 `SIGABRT`
  (signal 6, 두 테스트 모두 runtime identity `20c39afc…`)로 실패했다.
  이는 전체 결과의 최신 실패이므로 historical 결과와 분리해 Astra 원인 검토를
  요청했다. refresh 프로세스는 아직 살아 있어 나머지 항목은 계속 실행 중이다.

- Checkpoint 1100 (2026-09-11): 현재 identity corpus refresh 프로세스가
  10분 이상 생존하고 CPU를 사용하며 계속 진행 중임을 확인했다. ledger는
  1,076개 모두 `passed`, current identity 갱신은 299개다. 프로세스를 재시작하거나
  병렬도를 바꾸지 않고 동일 실행을 유지한다.

- Checkpoint 1101 (2026-09-11): 동일 corpus refresh 프로세스를 계속 관찰했다.
  ledger 1,076개는 모두 `passed`이며 current runtime identity 갱신 항목이
  338개로 증가했다. 디스크 여유는 75GiB, refresh ledger는 36MiB로 확인되어
  저장공간 압박 없이 실행을 유지한다.

- Checkpoint 1102 (2026-09-11): corpus refresh가 12분 이상 생존한 것을
  재확인했다. 전체 1,076개 결과는 모두 `passed`이며 current runtime identity
  갱신 항목은 370개다. 장시간 테스트가 포함된 AOSP 순서를 유지하고 임의 중단이나
  재시작은 하지 않았다.

- Checkpoint 1110 (2026-09-11): Chromium 호환성 계층에서 `https://example.com/`
  을 실제 브라우저 탭으로 열어 확인했다. 제목/본문/`Learn more` 링크가 접근성
  트리에 노출되고 화면에도 정상 렌더링되었다. current-identity corpus refresh는
  별도 프로세스로 계속 실행 중이며, zygote 종료 경로의 SIGABRT 수정은 아직 남아 있다.

- Checkpoint 1111 (2026-09-11): Astra 진단에 따라 CommonPool shutdown이
  singleton을 lazy-create하지 않고 이미 생성된 pool만 stop하도록 수정했다. HWUI
  static foundation 재빌드와 graphics-link audit가 통과했으며, 새 runtime identity에서
  `689-zygote-jit-deopt`, `728-imt-conflict-zygote`, `980-redefine-object`를 포함한
  corpus 1,076/1,076이 모두 `passed`다.

- Checkpoint 1112 (2026-09-11): Astra가 지적한 종료 경합을 반영해 CommonPool
  marker를 mutex로 보호하고 shutdown owner를 `std::call_once`로 단일화했다. HWUI
  foundation 재빌드 및 graphics-link audit가 다시 통과했고, 세 zygote/JIT 회귀
  테스트가 새 링크 산출물에서 모두 `passed`했다.

- Checkpoint 1113 (2026-09-11): 전체 `tools/audit-art-jit.sh`를 새 링크 산출물로
  재실행해 `RC=0`을 확인했다. ARM64 intrinsics, compiled/JNI/GC/예외 경로,
  VarHandle 및 invoke-polymorphic/custom, 배열·문자열·CRC·Memory 경로와
  compiled/native exit hooks, VM shutdown이 모두 PASS 증거를 남겼다.

- Checkpoint 1114 (2026-09-11): 변경 없는 Calendar API29 APK를 20초 실행해
  설치·ClassLoader·ViewRoot/HWUI 초기화와 720x1280 GPU scanout을 확인했다.
  보존된 Chromium APK로 tab/grid graphics acceptance도 `PASS`했다
  (GLES/ANGLE/Graphite/Dawn/MoltenVK/AHB/SurfaceFlinger/Metal). HTTPS E2E는
  현재 macOS mkcert root 미신뢰로 중단되며, sudo 키체인 승인 후 재실행해야 한다.

- Checkpoint 1115 (2026-09-11): AndroidKeyStore cross-process acceptance가
  `PASS`했다(writer/reader HMAC 일치, key material 비노출). 변경 없는 Blue Archive
  base+split APK도 실제 설치·Unity/IL2CPP arm64 초기화·NativeBridge/네트워크 TLS
  경로까지 30초 실행 후 정상 종료했다. 로그인/다운로드/전투 입력은 계정·콘텐츠
  의존성이므로 별도 미완료 acceptance로 유지한다.

- Checkpoint 1116 (2026-09-11): `cargo test -p darwin-art-host`(8 tests)와
  `cargo test -p art-bootstrap`(14 tests)가 모두 통과했다. JNI attachment,
  host graphics/input, bootstrap build-contract 회귀를 현재 shutdown/identity
  산출물과 함께 재확인했다.

- Checkpoint 1117 (2026-09-11): `python3 -m unittest -q
  tools/test_art_upstream_corpus.py` 9개가 통과했고, `audit-native-graph.sh`도
  runtime=258, graphics-jni=63, ICU=458, cached TU=809, 472 inputs에서
  duplicate/누락 없이 `PASS`했다. graph digest는 `00b69efe…d8f5f`로 기록됐다.

- Checkpoint 1118 (2026-09-11): 보존된 변경 없는 Blue Archive base+split APK를
  실제 30초 실행해 Unity/IL2CPP arm64와 24개 native library 등록, TLS 요청,
  graphics present 및 정상 shutdown을 재확인했다. AndroidKeyStore cross-process
  HMAC acceptance도 `PASS`했다. 계정이 필요한 로그인·콘텐츠 다운로드·전투는
  여전히 별도 acceptance로 남아 있다.
- Checkpoint 1119 (2026-09-11): 현재 프로필의 원본 Blue Archive base+split APK를 다시 실행해 Unity/IL2CPP 진입과 graphics lifecycle을 확인했다. AndroidKeyStore cross-process writer/reader도 재실행해 HMAC 일치와 material 비노출을 확인했다. 계정·서버 콘텐츠에 종속된 이후 화면은 acceptance로 승격하지 않았다.

- Checkpoint 1120 (2026-09-11): Chromium lifecycle acceptance가 프로필 패키지
  mount의 변경 없는 APK를 자동 발견하도록 경로를 보강한 뒤 두 iteration을
  `PASS`했다(new-tab=2, JNI-detach-crash=0, service-children=reaped). 기존
  `_build/installed-apps`와 현재 profile store 양쪽을 지원해 재현 실행 경계를 고정했다.

- Checkpoint 1121 (2026-09-11): 현재 profile store의 Blue Archive base+split APK를
  런타임 변경 없이 30초 실행하고 Unity/IL2CPP native 경로와 GPU 진단 프레임을
  수집했다(exit=0, diagnostic PNG 15장). 포인터 테스트 훅은 실행 로그에 기록됐지만
  이 캡처만으로 게임 UI 입력 소비나 계정 이후 콘텐츠를 acceptance로 주장하지 않는다.

- Checkpoint 1122 (2026-09-11): Astra 재검토로 corpus identity를 정정했다.
  `_build/art-upstream-corpus/summary.json`은 1,076개가 모두 `passed`이지만
  runtime identity는 `20c39afc…` 1,073개와 `d698be27…` 3개로 혼합되어 있다.
  따라서 이를 단일 current identity 1,076/1,076으로 표현하지 않으며, 최신
  shutdown 수정의 focused 3건 PASS와 기존 identity 결과를 분리해 기록한다.
  `audit-art-jit.sh` 자체는 이번 실행에서 RC=0이었다.

- Checkpoint 1123 (2026-09-11): identity 혼합을 해소하기 위해
  `run-art-upstream-corpus.py --resume --parallel 8`을 현재 산출물 기준으로
  재실행했다. 프로세스와 8개 worker가 살아 있으며, 완료 전 결과를 PASS로
  승격하지 않는다.

- Checkpoint 1124 (2026-09-11): fresh corpus 실행 중 host/artifact가 변경되어
  identity가 다시 섞이는 현상을 Astra와 확인했다. 잘못된 귀속을 막기 위해 runner가
  각 완료 batch 전에 runtime identity를 재계산하고 시작값과 다르면 추가 ledger
  기록 없이 중단하도록 수정했다. `tools/test_art_upstream_corpus.py` 9개는 통과했다.

- Checkpoint 1125 (2026-09-11): 잔여 build/host 프로세스가 없는 상태에서
  runtime identity 안정성을 2초 간격으로 재확인했다(`26ba01e4…` 동일). drift
  guard가 적용된 전체 corpus fresh 재실행을 parallel=8로 시작했으며, 완료 전
  ledger를 최종 증거로 승격하지 않는다.

- Checkpoint 1126 (2026-09-11): controlled fresh corpus 프로세스가 동일
  invocation에서 8개 worker로 계속 실행 중임을 확인했다. 완료 또는 drift 감지
  전에는 결과를 해석하거나 identity PASS로 승격하지 않는다.

- Checkpoint 1127 (2026-09-11): 동일 fresh corpus 핸들을 재확인했으며 약 3분
  이상 8개 worker가 계속 진행 중이다. 완료 전 ledger의 기존 혼합 행은 유효한
  current-identity 증거로 사용하지 않는다.

- Checkpoint 1128 (2026-09-11): controlled fresh corpus가 약 5분째 동일
  프로세스에서 진행 중이다. 중간 ledger는 기존 행과 새 행이 함께 보이는
  부분 갱신 상태이므로, 프로세스 종료와 최종 identity 재계산 전에는 판정하지 않는다.

- Checkpoint 1129 (2026-09-11): corpus 핸들이 약 6분째 살아 있고 8개 worker가
  순차적으로 진행 중이다. 현재 summary는 1,075 passed/1 failed의 부분 상태이며,
  기존 identity 행이 남아 있으므로 완료 전 current-identity 판정을 하지 않는다.

- Checkpoint 1130 (2026-09-11): 동일 controlled corpus 핸들이 약 7분째 실행
  중이며 worker가 계속 교체되고 있다. 기존 summary의 실패/구 identity 행과
  새 결과가 공존하므로 최종 acceptance는 별도 fresh ledger에서 검증해야 한다.

- Checkpoint 1131 (2026-09-11): fresh corpus 핸들이 약 8분째 살아 있으며
  current identity 결과가 점진적으로 갱신되고 있다. 기존 ledger 행은 분리
  검증 대상이며, 프로세스 종료 후 별도 fresh ledger와 identity 재계산을 수행한다.

- Checkpoint 1132 (2026-09-11): 동일 corpus 세션이 약 10분째 계속 실행 중이며
  summary는 부분 갱신 상태다. 종료 전까지 drift guard와 기존 행 분리 원칙을
  유지하고, 최종 결과가 나오면 별도 identity-only ledger로 재검증한다.

- Checkpoint 1133 (2026-09-11): fresh corpus 세션이 약 11분째 살아 있고
  active test names가 2000번대까지 진행되었다. drift guard가 유지되는 동안
  중간 summary는 최종 결과로 해석하지 않는다.

- Checkpoint 1134 (2026-09-11): fresh corpus가 약 12분째 실행 중이며
  2041/2239 계열 항목까지 진행했다. 종료 전 summary는 부분 상태로 유지하고,
  최종 종료 코드와 identity 분포를 별도로 확인한다.

- Checkpoint 1135 (2026-09-11): fresh corpus가 약 14분째 실행 중이며
  2243/2259 계열 테스트까지 진행했다. 동일 세션을 유지하고 종료 시점의
  drift guard 결과와 실패 항목만 authoritative evidence로 수집한다.

- Checkpoint 1136 (2026-09-11): Astra가 최신 실패 두 건을 동일 원인으로
  분류했다. interpreter는 통과하고 JIT MAP_JIT allocator의 metadata/code
  uint32 거리 조건에서 status=123이 발생하며 shutdown은 정상이다. 0040 패치의
  임의주소 fallback은 AOSP 계약을 보장하지 않으므로 제거하지 않고, 실행 중인
  corpus를 중단하거나 새 빌드하지 않은 채 주소 계측과 bounded pair allocator를
  다음 수정 경계로 확정했다.

- Checkpoint 1137 (2026-09-11): 진행 중 corpus에서 `582-checker-bce-length`도
  interpreter PASS 후 동일한 `Darwin JIT data/code mappings exceed uint32 offset
  range`로 optimized compilation status=123을 기록했다. 이는 벡터/BCE 의미론
  실패가 아니라 공통 allocator 경계 재현 증거이며, corpus 세션은 계속 유지한다.

- Checkpoint 1138 (2026-09-11): controlled fresh corpus가 약 21분째 진행 중이며
  `642-fp-callees`까지 동일 identity로 재실행됐다. 현재 공통 MAP_JIT 범위 실패가
  4건으로 누적되었고, 세션 종료 전에는 allocator 수정이나 결과 승격을 하지 않는다.
