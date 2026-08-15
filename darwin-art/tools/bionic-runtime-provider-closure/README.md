# Bionic runtime provider closure

This module is the concrete link closure behind the generated closed Bionic
namespace. It does not add another implementation of a libc symbol. Instead,
it combines the existing C, C++, and Rust provider archives so
`darwin_art_bionic_namespace_bind_builtins` can bind every manifest owner to
its actual resolver.

The current closure contains fourteen providers and resolves 147 exact routes:
129 of the pinned Android 35 arm64 `libc++_shared.so` libc-family imports plus
all 18 public `liblog.so` exports. The remaining 31 libc imports stay explicit
capability failures. Unknown SONAMEs, symbols, or GNU versions never fall back
to dyld or host `dlsym`.

Run:

```sh
tools/build-bionic-runtime-provider-closure.sh
```

The gate builds one Rust static archive for the stateful filesystem,
process-state, stdio, and DSO-lifecycle owners; one native archive for the
remaining providers and namespace; and the pinned AOSP gdtoa float-conversion
archive. It then links a real arm64 executable with Android ICU 76.1 and AOSP
liblog, seals the namespace, and resolves every generated route through the
actual provider callbacks. Host or dynamic ICU linkage is rejected.

The embedding lifetime is strict: create, bind all providers, seal, load and
run guest DSOs, run guest static and `__cxa_finalize` teardown, unload the ELF
graph, then quiescently tear down the namespace. The closure currently keeps
the filesystem and stdio virtual descriptor owners separate; sharing one
Android open-file-description table is a later compatibility boundary.
