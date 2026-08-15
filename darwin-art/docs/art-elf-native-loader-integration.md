# ART integration for Android ELF libraries

ART already owns Java class-loader identity, the loaded-library table,
`JNI_OnLoad` state, recursive-load detection, and `JNI_OnUnload` ordering in
`JavaVMExt::LoadNativeLibrary`. Darwin ART must preserve that ownership. The
custom ELF loader replaces the host `dlopen` mechanism; it does not create a
second native-library lifecycle beside ART.

## Existing seam

`compat/darwin_runtime_adapters.cc` currently implements
`android::OpenNativeLibrary` with Darwin `dlopen`. The return value becomes the
opaque handle stored by ART's `SharedLibrary`. Symbol lookup then takes one of
two paths:

- ordinary handles use Darwin `dlsym`;
- handles marked `needs_native_bridge` use
  `NativeBridgeGetTrampoline2(handle, symbol, shorty, call_type)`.

The Android ELF path deliberately uses the second branch even on the same CPU
ISA. Here “bridge” means a foreign object format and calling convention, not
instruction emulation.

## Required flow

```text
System.load / Runtime.nativeLoad
  -> JavaVMExt::LoadNativeLibrary
  -> OpenNativeLibrary
       -> resolve Android-prefix path through the dirfd broker
       -> verify ELF64/AArch64 capability report
       -> create or reuse the class-loader namespace
       -> map dependencies, relocate, protect RELRO, run constructors
       -> return a tagged loader-owned handle
       -> needs_native_bridge = true
  -> SharedLibrary records class-loader ownership
  -> NativeBridgeGetTrampoline2("JNI_OnLoad", shorty=null)
       -> loader export lookup
  -> ART calls JNI_OnLoad and records its result
```

`CloseNativeLibrary(handle, needs_native_bridge=true)` decrements the loader
reference count. ART calls `JNI_OnUnload` first where its existing lifecycle
requires it. The loader then runs finalizers in dependency-safe order and
unmaps only when no namespace or handle owns the module. Runtime shutdown must
quiesce native threads and unload these handles before destroying ART, the
proxy JNI tables, the filesystem broker, or virtual DSO providers.

## Handle and namespace rules

- Detect ELF by checked magic/class/machine, not by a `.so` suffix.
- A handle is an opaque registry token, never a host dyld handle or an
  unvalidated mapped address.
- Key module identity by authorized file descriptor/inode plus namespace, not
  by an untrusted path string.
- Preserve ART's prohibition on loading one library into two Java class
  loaders unless Android linker namespace policy explicitly supplies distinct
  instances.
- Resolve `DT_NEEDED` only through the requesting namespace and allowlisted
  virtual SONAME providers. Never fall back to `RTLD_DEFAULT`.
- Propagate exact GNU version requirements and weak-symbol semantics into the
  resolver.
- Return owned error strings compatible with `NativeLoaderFreeErrorMessage`;
  no fatal logging for an ordinary unsupported library.

## Calling-convention boundary

`JNI_OnLoad(JavaVM*, void*)` and `JNI_OnUnload(JavaVM*, void*)` are useful first
gates because their arguments stay in registers. They do not prove general JNI
compatibility. Android ELF native methods receive Android AAPCS64 stack
arguments, while the Darwin ART runtime currently prepares its Mach-O native
calls using Apple ARM64 packing.

For a resolved JNI method, `NativeBridgeGetTrampoline2` must use `shorty` and
`JNICallType` to return a cached Darwin-to-Android repacking thunk. In the
reverse direction Android code must receive proxy `JNIEnv` and `JavaVM` invoke
tables whose Android-ABI entry thunks substitute the real ART pointer and
repack into Darwin ABI calls. Directly returning an Android function for a
spilled method, or directly exposing ART's Mach-O JNI table, is forbidden.

Virtual DSO providers follow the same policy. Their internal host addresses
are implementation targets. The ELF namespace publishes Android-ABI entry
thunks, especially for variadic APIs and structures such as `va_list` whose
layout is ABI-specific.

## Acceptance order

1. Import-free ELF: `PT_LOAD`, relative relocations, constructors, export call.
2. Closed fixed-symbol import with GNU-version checking and RELRO.
3. Register-only `JNI_OnLoad` through ART, including `GetEnv` on the first
   reviewed proxy path.
4. A native method whose arguments remain in registers.
5. Deliberately spilled narrow/integer/reference and floating-point arguments
   through the generated ART-to-ELF thunk.
6. Android-to-ART proxy-JNI calls that also force stack arguments.
7. Reentrant dependency load, class-loader isolation, `JNI_OnUnload`, and clean
   runtime teardown.

No stage may silently choose dyld or a direct function pointer when its ABI
contract is not proven.
