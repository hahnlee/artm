# Bionic read-only filesystem facade gate

This standalone gate connects the Android arm64 libc imports `open`, `openat`,
`read`, `close`, and `fstat` to the Darwin component-walking filesystem broker.
It is an inspection/proof module and is not wired into the runtime.

## Boundary and ownership

The facade is activated with an already-open Darwin directory, mounted at the
guest byte path `/system`, with guest cwd `/system`. `darwin-art-prefix`
normalizes guest paths and rejects mount escape; `darwin-art-fs-broker` walks
each component relative to that directory with no-follow semantics. Symlinks,
`..` escape, writable operations, special nodes, and paths outside the one
immutable mount fail closed.

Guest descriptors are facade-local virtual integers beginning at 10000. A
Darwin descriptor is retained in the private Rust table and is never returned
or resolver-exposed. This range is not globally collision-free: there is no
coordination with another descriptor provider or with an application that
manufactures integers. Runtime integration must use one central descriptor
namespace (or non-overlapping tagged ranges) and reject cross-provider use.

The descriptor mutex is intentionally simple. `read` holds the global table
lock for the entire host read, so a concurrent `close` or another descriptor
operation is serialized until that read completes. This is the current
contract, not Linux close/read concurrency emulation.

## Supported calls

- `open(path, flags, ...)`: read-only access under the mount root.
- `openat(AT_FDCWD, path, flags, ...)`: equivalent to `open`; an absolute path
  also ignores `dirfd`, matching the relevant Linux rule.
- `read`, `close`, and `fstat` for facade-owned virtual descriptors.
- Android arm64 `struct stat` is materialized as the pinned 128-byte Bionic
  layout; it never exposes a Darwin `struct stat`.

Relative `openat` against a valid facade directory descriptor is explicitly
unsupported and returns Android `EOPNOTSUPP`; an unknown descriptor returns
`EBADF`. `lseek`, writable/create/truncate/append modes, rename, unlink, socket,
and every symbol absent from the closed resolver are unsupported capabilities.
`O_DIRECT`, `O_PATH`, and unknown flags return `EOPNOTSUPP`; write intent returns
`EROFS`. The variadic shims never consume a mode argument because every flag
combination requiring one is rejected first.

## Errno and activation contracts

Expected Darwin failures are translated through the standalone Bionic errno
TLS provider. Darwin errno is saved/restored by every C shim and its address is
never guest-visible. An unknown Darwin errno or internal capability failure
sets the sticky `capability_failure` marker and deterministically publishes
Android `EIO` instead of leaving stale Bionic errno. The embedding boundary
must stop guest execution when that marker is observed; `EIO` is a fail-closed
diagnostic, not a claimed semantic translation.

Activation is pthread-local. TLS owns an `Arc<Facade>`, so forgetting the guard
leaks a reference rather than leaving a dangling pointer. `Activation` is
deliberately `!Send`/`!Sync`; a compile-fail doctest locks the rule that a guard
cannot move to a different pthread. Nested activation restores the previous
`Arc` on the same pthread.

## Deterministic audit

Run `./audit.sh`. It pins and hashes the NDK r28c API 35 headers, the prefix and
broker sources, the Bionic errno translator, the OS-constant manifest, and the
real libc++ import classification. It then:

1. cross-compiles a real Android AArch64 ELF fixture whose only undefined
   symbols are `__errno`, `open`, `openat`, `read`, `close`, and `fstat`;
2. loads it with a closed resolver and exercises content, Android stat layout,
   `ENOENT`, unsupported flags, write rejection, traversal and symlink escape,
   virtual-dirfd rejection, and virtual descriptors at or above 10000;
3. proves Darwin host errno is preserved and no unknown translation occurred;
4. runs broker/prefix tests, the activation `!Send` compile-fail test, Clippy,
   and formatting checks using a temporary target directory.

The gate performs no filesystem writes through the facade. Its test setup is
temporary host data created by the audit harness.
