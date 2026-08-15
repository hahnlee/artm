# Bionic read-only filesystem facade gate

This standalone gate connects 13 Class-B Android arm64 libc filesystem imports
to the Darwin component-walking filesystem broker: `open`, `openat`, `read`,
`close`, `fstat`, `stat`, `lstat`, `readlink`, `getcwd`, `chdir`, `opendir`,
`readdir`, and `closedir`. It is not wired into the runtime.

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

Directory streams use a second facade-owned side table. The guest `DIR*` is a
stable opaque token allocation containing only a numeric identity; Darwin
`DIR*`, its fd, and stream state remain exclusively in the table. The returned
`struct dirent*` points to a separate translated Android-only buffer. `closedir`
removes the side-table record and frees both guest allocations because POSIX
defines every later use of that `DIR*` or entry pointer as undefined. Thus
memory is proportional to concurrently live streams rather than lifetime open
count. A 512-cycle stress test requires the active table to return to zero.
As with virtual fds, another provider must not invent or accept these tokens.

## Supported calls

- `open(path, flags, ...)`: read-only access under the mount root.
- `openat(AT_FDCWD, path, flags, ...)`: equivalent to `open`; an absolute path
  also ignores `dirfd`, matching the relevant Linux rule.
- `read`, `close`, and `fstat` for facade-owned virtual descriptors.
- `stat` for admitted regular files/directories; the no-follow authorization
  policy means it deliberately does not implement Linux symlink following.
- `lstat` for admitted non-symlink nodes. A final symlink rejected with host
  `ELOOP` becomes `EOPNOTSUPP`, because the broker cannot safely return symlink
  metadata. `readlink` returns `EINVAL` for securely opened regular files and
  directories, matching Linux/Bionic, and the same final-symlink capability
  rejection. Intermediate-component failures retain their broker-translated
  errno (`ENOTDIR` for the pinned Darwin no-follow walk), rather than being
  mislabeled as final-link support. Link bytes are never fabricated.
- `getcwd` copies the guest byte path into caller-owned storage. Null-buffer
  allocation is `EOPNOTSUPP` because this module does not own the Bionic
  allocator. `chdir` verifies a directory through the broker and updates one
  process-wide facade cwd shared by all activated pthreads.
- `opendir`, `readdir`, and `closedir` translate a private Darwin stream into
  the pinned Android arm64 280-byte `dirent` layout. Directory names remain
  uninterpreted bytes. Known Darwin `d_type` values are translated explicitly;
  unknown values become Android `DT_UNKNOWN`.
- Android arm64 `struct stat` is materialized as the pinned 128-byte Bionic
  layout; it never exposes a Darwin `struct stat`.

The cwd is a verified lexical guest path. Every later relative lookup performs
a fresh broker walk from the immutable mount root, so containment survives host
renames, but exact POSIX directory-identity behavior across an external host
rename is not claimed.

`readdir` and `closedir` hold one global directory-table lock across each host
operation. They are serialized across all facade streams; a close cannot race
the corresponding host call. End-of-directory returns null without changing
Bionic errno. The translated entry buffer remains allocated but, as on Bionic,
its contents may be replaced by the next `readdir` on that stream. `closedir`
frees the entry and token after the serialized host close; concurrent or later
use is the POSIX-undefined caller case and is not made into a tombstone API.

Relative `openat` against a valid facade file descriptor is explicitly
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

Run `./audit.sh`. It pins and hashes the NDK r28c API 35 `fcntl.h`, `stat.h`,
`dirent.h`, and `unistd.h` inputs, the prefix and broker sources, the Bionic
errno translator, the OS-constant manifest, and the real libc++ Class-B import
classification. Compile-time probes lock every function signature plus the
Android `stat` and `dirent` field layouts. It then:

1. cross-compiles a real Android AArch64 ELF fixture whose only undefined
   symbols are `__errno` and the 13 listed facade imports;
2. loads it with a closed resolver and exercises content, Android stat/dirent
   layouts, cwd transitions, EOF errno, `ENOENT`, unsupported flags, write
   rejection, regular/final/intermediate `readlink` distinctions,
   traversal/symlink policy, virtual-dirfd rejection, and virtual descriptors
   at or above 10000;
3. proves Darwin host errno is preserved and no unknown translation occurred;
4. runs broker/prefix tests, the activation `!Send` compile-fail test, Clippy,
   and formatting checks using a temporary target directory.

The gate performs no filesystem writes through the facade. Its test setup is
temporary host data created by the audit harness.
