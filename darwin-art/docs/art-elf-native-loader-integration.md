# ART Android ELF JNI integration boundary

This gate is a deliberately narrow ART integration proof. It is not general
Android `.so` compatibility.

## Accepted image

`tools/build-android-elf-jni-fixture.sh` builds a root, child, and grandchild
AArch64 `ET_DYN` graph and generates an identity header from all three exact byte
lengths and SHA-256 values. The adapter delegates the caller-selected parent
directory as trusted host authority, then the filesystem broker opens the root
and recursively named siblings as byte components with `O_NOFOLLOW`,
`O_NONBLOCK`, regular-file checks, and fixed file/count/total-size caps. The
same authorized descriptor supplies metadata and bytes. Provider SONAMEs are
never opened from disk; RPATH/RUNPATH, dyld lookup, and alternate paths are not
consulted for an Android ELF graph.

Selecting and opening the parent directory is host policy, not an ancestor
symlink sandbox: only its final component is opened with `O_NOFOLLOW`. The host
must also prevent concurrent writes to authorized inodes. Filename walking is
byte-preserving, including a non-UTF-8 root basename, but the current
`ClosedElfNamespace` uses Rust `String` keys. Embedded non-UTF-8 SONAME or
DT_NEEDED values therefore fail with an explicit namespace-capability error
after byte-component validation.

The discovery gate also uses the pinned NDK r28c `libc++_shared.so`: a sibling
root discovers it while `libc.so`, `libdl.so`, and `libm.so` are poisoned disk
entries and must be skipped as providers. Metadata inspection deliberately
accepts loader-unsupported tags such as GNU RELRO for discovery; the later load
stage still applies its own explicit mapping/relocation capability checks.

The root needs the real child plus explicit virtual host/Bionic providers. The
child needs the real grandchild plus those providers; the grandchild is a pure
leaf. The host provider
exports one reviewed fixed-register `void(int)` lifecycle recorder; unknown
SONAMEs and symbols fail closed. Both ELF objects have initializer/finalizer
arrays and no GNU RELRO or TLS. The root's constructor and `NativeAdd` reach a
real child export through eager `R_AARCH64_JUMP_SLOT` relocation.

## ART lifecycle

`JavaVMExt::LoadNativeLibrary` calls the Darwin `OpenNativeLibrary` seam. Mach-O
libraries keep their existing `dlopen`/`dlclose` ownership. A broker-discovered
ELF receives a private Rust graph handle. The C ABI stages the
complete closure, resolves it only against graph scope plus the explicit
provider, and runs every constructor before returning the still-private handle.
The adapter creates no lifecycle/provider/graph handle until discovery has
completed, and publishes only after graph construction and JNI-proxy preflight. It is tagged
`needs_native_bridge=true`; close dispatch is therefore atomic between the ELF
handle and the raw dyld handle.

ART looks up `JNI_OnLoad` through `NativeBridgeGetTrampoline2`. The fixed
lifecycle trampoline substitutes the closed proxy `JavaVM*`; the Android image
never sees ART's real function tables. The proxy implements only the fixture's
`GetEnv`, `FindClass`, `RegisterNatives`, and `ThrowNew` path. Backend calls use
the current ART thread's `JNIEnv`; no synchronous-load pointer is retained.

The RegisterNatives backend accepts exactly the fixture's eight reviewed
signatures. Generic sibling discovery does not yet imply arbitrary registered-
JNI support; named JNI can only proceed through the separately supported shorty
trampoline path. The fixture backend creates
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
graph together. ART shutdown provides the required external quiescence, then
the close seam unpublishes/unmaps the thunks before finalizing the root and then
the child, each before its mapping is released.
The shutdown acceptance also requires the global live-page count to return to
zero after `DestroyJavaVM`. The host recorder observes the exact sequence child
constructor → root constructor → `JNI_OnLoad` → root C++ callback →
root ELF finalizer → child C++ callback → child ELF finalizer. Thus
constructor order, child relocation, actual JNI registration, and reverse graph
finalization are one real ART process gate.
The fixture's `JNI_OnUnload` is a reviewed no-op. Both root and child define a
hidden/local `__dso_handle`, register an observable callback through the Bionic
`__cxa_atexit` provider, and keep that handle out of `.dynsym`. The graph
publishes each live range before constructors. Unload observes root callback →
root ELF finalizer → child callback → child ELF finalizer, then unmaps. The
provider rejects null-handle registrations for graph owners and routes
simultaneously live owners by disjoint image ranges.

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
   complete for the graph root.
5. Recursive sibling `DT_NEEDED` discovery, including the reviewed
   root→child→grandchild graph, explicit providers,
   transactional constructors, and reverse finalizers: complete.
   A second actual-ART graph has the same two-level dependency topology but no
   Bionic imports and exports `JNI_OnLoad` without `JNI_OnUnload`; it proves the
   generic path is not coupled to fixture route masks or optional lifecycle
   exports.
6. Class-loader namespace caching, ZIP/APK extraction, arbitrary
   RegisterNatives/JNI proxy expansion, CriticalNative, and complete provider
   tables: incomplete.

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
