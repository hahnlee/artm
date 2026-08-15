# Porting gates

## G0 — ARM64 Darwin ABI

- [x] Pin ART without storing Git object history.
- [x] Detect the real 16 KiB Darwin page size.
- [x] Validate Mach-O symbol naming and CFI with an ART-derived entrypoint.
- [x] Convert the upstream ARM64 assembly macros behind `__APPLE__` guards.
- [x] Compile quick/JNI/native ARM64 entrypoints with generated ART constants.
- [ ] Port ART's full DWARF CFI state machine to Apple's assembler; the PoC
      entrypoint artifacts currently omit CFI.

## G1 — ART foundations

- [x] Materialize only `libartbase`, `libdexfile`, and their current dependency closure.
- [x] Make `GetPageSizeSlow()` use `sysconf(_SC_PAGE_SIZE)` on Darwin.
- [x] Build the first `libartbase` Mach-O ARM64 foundation archive.
- [x] Parse a generated `classes.dex` through AOSP `StandardDexFile` on Darwin.
- [x] Verify generated and Android 16 boot-class-path DEX with AOSP `DexFileVerifier`.
- [x] Preserve macOS's mandatory 4 GiB `__PAGEZERO` and move ART's compressed
      managed-reference heap into a base-relative 4 GiB virtual window.

## G2 — Interpreter-only runtime

- [x] Compile ART host platform hooks (`runtime/thread/monitor`) as Mach-O ARM64.
- [x] Compile patched `Mutex` and `Monitor` core implementations on Darwin.
- [ ] Split explicit `*_darwin.cc` files when behavior diverges from host Linux hooks.
- [x] Generate upstream ARM64 ABI constants and compile `Arm64Context`, thread,
      and entrypoint initialization as Mach-O.
- [x] Compile ART's C++ switch interpreter as a Mach-O archive.
- [x] Compile `Runtime::Create`, `ClassLinker`, `Thread`, `ThreadList`, `Heap`,
      instrumentation, callbacks, intern table, OAT manager, and `JavaVMExt`.
- [x] Compile GC accounting, collectors, spaces, and quick/JNI C++ entrypoints;
      use CMS by default and keep CMC in non-userfaultfd fallback form.
- [x] Add a real `Runtime::Create()` link-closure audit; all native symbols,
      including quick/JNI/context assembly families, are resolved.
- [x] Implement Darwin ARM64 `ucontext_t` register access for crash diagnostics.
- [ ] Complete signal-driven implicit-check handling; the first `-Xint` gate
      can keep implicit checks disabled.
- [x] Compile ART's existing pthread mutex/condition-variable fallback on Darwin.
- [ ] Implement pthread wakeups for ART empty-checkpoint requests.
- [x] Replace the non-futex `LockSupport.park/unpark` no-op with a tested
      pthread mutex/condition-variable implementation on Darwin.
- [ ] Support cross-thread monitor inflation; current bootstrap enforces current-owner only.
- [x] Enter `Runtime::Create()` with `-Xint`, no zygote, JIT, AOT, or Nterp.
- [x] Complete boot-class initialization and return successfully from
      `Runtime::Create()` with the pinned Android 16 core JARs.
- [x] Execute `Hello.answer()` from DEX through the C++ switch interpreter.
- [x] Keep application DEX off the boot class path and load it through an ART
      `PathClassLoader` created by the native bootstrap probe.
- [x] Execute interpreted Java through ART's generic JNI trampoline into a
      registered Darwin function and return the result to Java.
- [x] Compile and register ART's complete runtime-native source table.
- [x] Run early root class initialization and initialize ART intrinsics in the
      Darwin minimal-start path.
- [x] Initialize ART's main `ThreadGroup`/`Thread` peer without starting daemon
      services, enabling `ThreadLocal` and Android `BlockGuard` paths.

## G3 — Android-facing runtime

- [x] Extract and verify matching Android 16 core-oj/core-libart DEX inputs.
- [x] Fetch the locked Android 16 `core-icu4j` class JAR without Git history,
      convert it to DEX locally, and verify it with AOSP `DexFileVerifier`.
- [x] Load the verified core-oj/core-libart/core-icu4j DEX files into
      `ClassLinker`.
- [x] Load a generated app DEX into its own `PathClassLoader` and execute it.
- [x] Add the first explicit Java/JNI native method (`hostPageSize`).
- [x] Initialize `java.lang.System` with a Darwin subset of libcore natives for
      primitive bit conversion, `sysconf`, user/environment/`uname`, ICU
      metadata, special properties, and standard file descriptors.
- [x] Execute AOSP's native `System.arraycopy()` from interpreted app DEX.
- [x] Invoke an app's conventional `public static void main(String[])` via JNI.
- [x] Route Android `System.out.println` through `PrintStream`, Android
      `CharsetICU`, host ICU4C conversion, `IoBridge`, and Darwin `write(2)`.
- [ ] Register the minimal libcore native method set needed by ordinary Java
      startup without loading Android `.so` libraries.
- [ ] Launch a minimal `Activity` compatibility class.
- [ ] Connect the View backend to ProjectGPU and an `NSWindow`.

## Deferred performance work

- Darwin `MAP_JIT` and write-protect transitions.
- ARM64 quick compiler/JIT and signal-driven implicit checks.
- APK ELF loader, Bionic ABI surface, and JNI `.so` support.
