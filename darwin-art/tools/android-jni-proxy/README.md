# Android 16 JNI proxy table gate

This standalone module supplies a small proxy `JavaVM`/`JNIEnv` surface for an
Android ARM64 JNI library without exposing an ART function table. The first
executable subset is `JavaVM::GetEnv` plus `JNIEnv::{GetVersion, FindClass,
ThrowNew, RegisterNatives, ExceptionCheck}`. All other table entries are null,
so an unsupported call fails visibly instead of reaching ART or an accidentally
similar Darwin symbol.

The table shape comes from the SHA-locked AOSP Android 16.0.0_r4
`libnativehelper/include_jni/jni.h`. `tools/generate_slots.py` parses every
declaration, locks the ordered-name digests, and generates the table sizes and
selected offsets. The gate verifies 233 `JNINativeInterface` slots and eight
`JNIInvokeInterface` slots, then also proves that the installed NDK compiler
header has the same ordered tables.

## Boundary and state

The public initializer accepts only three semantic backend callbacks. It never
accepts, stores, or returns an ART `JNIEnv`, `JavaVM`, `JNIEnvExt`, or raw ART
table. The handles visible to the Android ELF begin with this module's own
generated proxy table. The backend owns class handles and native registration;
the proxy owns the pending-exception bit and the attached test environment.

This first slice is deliberately single-environment and JNI 1.6-only. It has no
thread attachment/detachment, local/global reference lifecycle, exception
object, class-loader policy, or method invocation support. A registered native
function pointer is transferred to the backend but is not invoked by this gate.
That pointer remains Android-ABI-owned. A real backend must retain it as such
and use a signature-audited Android-to-ART function-pointer bridge for later
calls; registering it directly with ART is forbidden.

Null is an intentional fail-closed representation, but calling a null slot
would still crash the guest. Integration therefore requires a capability
preflight that proves all JNI operations a library can reach are within the
implemented manifest before loading constructors or executing `JNI_OnLoad`.
The proxy table alone is not that preflight and must not be advertised as a
general JNI implementation.

Only the successful `FindClass` and `RegisterNatives` paths are coherent in
this slice. Their failure-to-Java-exception behavior is not implemented.
`ThrowNew` records only a pending bit after backend success, not a throwable
object. A backend retaining `JNINativeMethod` metadata must copy it while the
callback is active and preserve the Android-code ownership of `function`.

## ARM64 procedure-call classification

Every implemented entry, including the `JNI_OnLoad(JavaVM*, void*)` fixture
entry, has a fixed non-variadic prototype containing only pointers and integer
scalars. No call exceeds four integer registers. Android AAPCS64 and Darwin
ARM64 PCS therefore agree for this exact set and no thunk is needed. In
particular, `RegisterNatives` passes a pointer to `JNINativeMethod`; it does not
pass the three-pointer aggregate by value, and compile-time layout checks cover
the pointee.

This conclusion does not extend to the remaining JNI table. Variadic `...`,
`va_list`, by-value aggregates, floating-point mixes, stack spills, and native
callbacks require their own bidirectional audit and possibly assembly thunks.
The per-entry decision is recorded in `manifests/abi.tsv`.

## Executable evidence

`audit.sh` fetches and hashes the pinned Android 16 header, regenerates the slot
constants, and compiles `probes/fixture.c` with the Android AArch64 clang target
as an ELF64 shared object. API 35 is the installed r28c compiler wrapper, while
the compilation uses the pinned Android 16 `jni.h`; the JNI 1.6 table ABI is
identical and independently digest-checked against the NDK header.

The ELF exports a real `JNI_OnLoad`. Its no-argument runner obtains the proxy
`JavaVM` through its sole resolved import, then Android-compiled instructions
indirectly call `GetEnv` and the five `JNIEnv` slots. The Darwin process maps and
executes that ELF through the isolated ELF loader, and a fake Darwin backend
verifies both class names, the `JNINativeMethod` tuple, and the thrown exception
message. Disassembly gating requires the expected indirect `blr` calls, so a
Darwin-only substitute fixture cannot satisfy the test.

Run `tools/android-jni-proxy/audit.sh`. It also checks exact exported/imported
symbols, absence of ART internals and `dlsym`, host and Android structure
offsets, unsupported-null slots, invalid `GetEnv` behavior, ASan/UBSan, Rust
formatting, and Clippy.

The referenced AOSP header is Apache-2.0 licensed. This repository stores only
source coordinates, hashes, derived slot numbers, and ordered-name digests; it
does not copy the upstream header.
