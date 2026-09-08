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
