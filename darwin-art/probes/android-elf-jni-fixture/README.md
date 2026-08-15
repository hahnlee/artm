# Register-only Android ELF JNI fixture

This fixture is the first ART native-bridge acceptance target. It is compiled
as Android ARM64 ELF, has no `DT_NEEDED` dependency, and exports only
`JNI_OnLoad` and `JNI_OnUnload`.

`JNI_OnLoad` exercises the reverse ABI boundary through the supplied proxy
`JavaVM` and `JNIEnv` tables: `GetEnv`, `FindClass`, and `RegisterNatives`. It
registers `NativeFixture.nativeAdd(IJI)J` plus a deliberately spilled mixed
scalar/reference/FP method. A later ART acceptance invokes both through the
generated Darwin-to-Android shorty trampoline and verifies the results. The
fixture must never receive ART's Mach-O JNI tables directly.

The mixed method deliberately places two 32-bit floats before a 64-bit double
after exhausting the FP register bank. Its Darwin stack offsets are compact
while Android AAPCS64 assigns an eight-byte slot to each argument, so merely
calling the ELF address directly cannot pass the gate.

Run `tools/build-android-elf-jni-fixture.sh` to produce the locked-shape ELF in
`_build/android-elf-jni-fixture`. Building the ELF alone does not claim that
ART loading or either JNI ABI bridge is complete.
