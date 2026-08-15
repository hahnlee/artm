# Android 16 libcore.io.Linux Darwin boundary

This gate owns the complete 135-entry `libcore.io.Linux` JNI table from
`platform/libcore` at `080fac8bb8670bc7fbc895050caf4b13c4d6cd12`.
The upstream source and extracted `(kind, name, signature)` manifest are both
checksum verified at build time; no full libcore checkout or Git metadata is
created.

The initial Darwin capability slice implements real `open`, `fstat`,
`readBytes`, `close`, `mmap`, and `munmap` behavior plus all seven upstream
CriticalNative identity calls. Android/Linux open and mmap flags are translated
explicitly. Every remaining method is present in the registrar and fails with
`android.system.ErrnoException(ENOTSUP)` through a generated per-method wrapper;
it never reports fake success. Further POSIX methods should move from that
capability-failure set to real Darwin implementations by whole behavioral
families (filesystem, sockets, process, xattr), while Linux-only calls retain an
explicit `ENOTSUP` contract.

The generator decodes every upstream JNI descriptor into a fixed C++ parameter
list and the matching JNI return type. It emits no ellipsis wrappers. The
descriptor decoder handles object, primitive, array, float, and double types;
the pinned table itself has no float or double return shorty. The gate
checks representative generated `V`, `I`, `J`, object, and `Z` declarations,
then loads a test dylib in OpenJDK and invokes all five return classes to verify
that each call throws `android.system.ErrnoException(ENOTSUP)` across the real
Apple arm64 managed/native ABI.

Integration must atomically replace the existing eight-entry
`libcore/io/Linux` table in `compat/darwin_libcore_natives.cc`. Registering both
owners is forbidden. The full Darwin `android.system.OsConstants` mapping is a
separate required integration gate because Android numeric flags are not all
Darwin ABI values.
