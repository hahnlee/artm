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
`GetEnv`, `FindClass`, and `RegisterNatives` path. Its backend `JNIEnv*` is valid
only during synchronous `JNI_OnLoad` and is cleared immediately afterward.

The fixture reaches its source-derived two-entry registration table, but the
backend intentionally returns `JNI_ERR`. It does not pass Android-PCS function
pointers to ART and installs no native method. Consequently ART reports the
failed `JNI_OnLoad` and keeps the `SharedLibrary`/ELF handle resident, as its
normal failure policy requires. Runtime shutdown destroys that ART owner and
calls the ELF close seam. A failed load is not advertised as unload-complete
JNI: the fixture has no finalizers, and the general `JNI_OnUnload`/ELF-finalizer
contract is still open.

Retaining `JavaVMExt::LoadNativeLibrary` also retains its legacy target-SDK
signal-chain repair call. `darwin_sigchain.cc` therefore provides the real
`EnsureFrontOfChain` behavior: if another action displaced ART's dispatcher,
it becomes the next action and the dispatcher is restored at the front.

## Acceptance stages

1. ARM64 ELF map/relocate/init and closed lookup: complete.
2. ART open with `needs_native_bridge=true` and atomic ELF-vs-Mach-O ownership:
   complete.
3. Proxy `JavaVM/JNIEnv` → real ELF `JNI_OnLoad` → `FindClass` → complete
   two-method `RegisterNatives` table reached, followed by explicit `JNI_ERR`:
   complete.
4. Per-shorty Android-to-Darwin function-pointer trampolines, actual ART native
   registration, `nativeAdd`, and the stack-spilling `nativeSpill` call:
   incomplete.

Stage 4 must repack the two calling conventions explicitly. The fixture gate
locks the Android spill layout to reference `sp+0`, `f4` `sp+8`, `f5` `sp+16`,
and `d4` `sp+24`; Darwin packs the corresponding tail at offsets 0, 8, 12, and
16. Raw pointer registration is forbidden.
