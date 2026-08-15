# ART Android ELF JNI integration boundary

This gate is a deliberately narrow ART integration proof. It is not general
Android `.so` compatibility.

## Accepted image

`tools/build-android-elf-jni-fixture.sh` builds one import-free AArch64
`ET_DYN` fixture and generates an identity header from its exact byte length
and SHA-256. The runtime reads the file once, verifies that identity, and gives
those same bytes to `darwin-art-elf-loader`. No Darwin global symbol lookup is
available.

The accepted image has no `DT_NEEDED`, `DT_FINI`, `DT_FINI_ARRAY`, GNU RELRO,
TLS, or unsupported relocations. The loader owns one image only; it does not
recursively load a SONAME dependency graph. ELF finalizers are not implemented,
so images that require them remain a hard capability error.

## ART lifecycle

`JavaVMExt::LoadNativeLibrary` calls the Darwin `OpenNativeLibrary` seam. Mach-O
libraries keep their existing `dlopen`/`dlclose` ownership. The hash-locked ELF
fixture instead receives a private Rust loader handle, runs its init array, and
is published only after lifecycle-symbol and JNI-proxy preflight. It is tagged
`needs_native_bridge=true`; close dispatch is therefore atomic between the ELF
handle and the raw dyld handle.

ART looks up `JNI_OnLoad` through `NativeBridgeGetTrampoline2`. The fixed
lifecycle trampoline substitutes the closed proxy `JavaVM*`; the Android image
never sees ART's real function tables. The proxy implements only the fixture's
`GetEnv`, `FindClass`, `RegisterNatives`, and `ThrowNew` path. Backend calls use
the current ART thread's `JNIEnv`; no synchronous-load pointer is retained.

The backend accepts exactly the fixture's eight reviewed signatures. It creates
one cache-owned executable page while writable, emits the cached thunks, flushes
the instruction cache, changes the page to read/execute, and publishes its
generation/range. `NativeBridgeIsNativeBridgeFunctionPointer` recognizes only
the callable entries, never a literal or arbitrary address in that range.
Its source-derived entry mask distinguishes all eight methods, so duplicate
classification of one address cannot satisfy the complete-table acceptance gate.
Only those Darwin-entry thunks are passed to ART `RegisterNatives`; raw ELF
function pointers are never installed.

No global `ART_TARGET_ANDROID` change is needed. ART's registration path still
asks `NativeBridgeIsNativeBridgeFunctionPointer` about every installed pointer;
the non-Android Darwin branch then retains each already-repacked Darwin entry
as the method target. The observed complete-table mask is part of runtime acceptance.

Registration is transactional at this boundary. Every thunk must exist and
ART must register the complete table before the backend returns `JNI_OK`. A
failure unregisters the exact fixture class, unpublishes the generation, and
unmaps the page. On success the `ElfLibrary` owns the executable page and ELF
image together. ART shutdown provides the required external quiescence, then
the close seam unpublishes/unmaps the thunks before unmapping the ELF image.
The shutdown acceptance also requires the global live-page count to return to
zero after `DestroyJavaVM`.
The fixture's `JNI_OnUnload` is a reviewed no-op; general ELF finalizers remain
unsupported.

Retaining `JavaVMExt::LoadNativeLibrary` also retains its legacy target-SDK
signal-chain repair call. `darwin_sigchain.cc` therefore provides the real
`EnsureFrontOfChain` behavior: if another action displaced ART's dispatcher,
it becomes the next action and the dispatcher is restored at the front.

## Acceptance stages

1. ARM64 ELF map/relocate/init and closed lookup: complete.
2. ART open with `needs_native_bridge=true` and atomic ELF-vs-Mach-O ownership:
   complete.
3. Proxy `JavaVM/JNIEnv` → real ELF `JNI_OnLoad` → `FindClass` → eight-method
   `RegisterNatives` table reached: complete.
4. Regular-JNI scalar/reference shorty generation, actual ART registration,
   narrow and FP stack repacking, and Z/B/C/S/I/J/F/D/L/V return paths:
   complete for the dependency-free fixture.
5. Arbitrary libraries, CriticalNative, recursive `DT_NEEDED` namespaces, and
   complete JNI proxy tables: incomplete.

Stage 4 explicitly repacks the two calling conventions. The fixture gate
compiles and disassembles the same source for both targets: Android uses
reference `sp+0`, `f4` `sp+8`, `f5` `sp+16`, and `d4` `sp+24`; Darwin uses
offsets 0, 8, 12, and 16. The generated thunk preserves x1-x7/v0-v7,
substitutes x0 with the proxy `JNIEnv`, and moves only the stack tail into
Android eight-byte slots. A second method forces 1/2/4/8-byte integer-like
values onto the stack. The planner tracks GP and FP banks independently,
caches identical target+shorty thunks, and rejects V arguments and unknown
aggregate/HFA/varargs spellings. CriticalNative is outside this regular-JNI API.

`nativeUsesEnv` runs after `JNI_OnLoad`, calls proxy `GetVersion` and
`FindClass`, and succeeds only because each backend call obtains
`Thread::Current()->GetJniEnv()`. No load-thread `JNIEnv` is retained. This is
still a deliberately small proxy table and must not be described as general
`.so` support.

The executable page currently uses Darwin's anonymous RW mapping followed by
an irreversible transition to RX. This is W^X and passes the development
binary gate on Apple Silicon, but hardened-runtime deployment still needs an
explicit `MAP_JIT`/code-signing entitlement policy and acceptance test.
