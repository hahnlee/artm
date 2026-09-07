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

## Coverage index (2026-09-05)

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
| Native unwind metadata | Every ARM64 quick entrypoint now has truthful Mach-O CFI; nonlinear AOSP paths are split into adjacent FDEs without changing code bytes. memcmp16 and ordinary JNI/native assembly retain CFI; two dlsym lookup stubs and the Darwin local/remote unwindstack backend remain | Lower the two lookup stubs, implement Darwin unwindstack maps/register/memory backends, and pass untouched AOSP 137-cfi locally/remotely |
| Execution policy | Darwin bytecode/method-shape allowlist and duplicate compiler/inliner admission gates removed; AOSP background compilation now reaches boot JNI methods; Thread.currentThread JNI/Baker boundary survives repeated CC stress | Make normal app launch use the production JIT policy and validate broader background compilation |
| Original apps | Prior Blue Archive loading/input evidence predates this JIT closure | Repeat Blue Archive, Chrome and calculator with unrestricted JIT and no APK changes |

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

The local executable matrix passes end-to-end with Baker/ConcurrentCopying and
no Darwin bytecode or method-shape allowlist. The pinned AOSP intrinsic source
contract is mechanically classified. Specialized-HIR, Unsafe, all 25 String
entries, typed System.arraycopy paths, all 43 Math entries, CRC32, Memory,
Reference and boxing now pass interpreter, baseline and optimized execution.
The Android fixture compiler now
uses API 36 core-for-system-modules plus android.jar as its boot API while still
emitting Java 8 classfiles, preserving signature-polymorphic bytecode and modern
Android Math APIs. Next run the pinned upstream compiler corpus through a
generic unmodified-test entrypoint and record unsupported harness features.
After that is green, enable production app-launch JIT policy and repeat
unmodified Blue Archive, Chromium and calculator workloads. Native Mach-O
unwind metadata remains incomplete.

Read this index and the latest architecture-migration entry when resuming.
Append dated evidence as work advances; keep this status table current.

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
