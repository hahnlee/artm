# Bionic virtual-memory facade

This standalone Tier-1 provider owns a deliberately narrow Android arm64
virtual-memory namespace. The pinned NDK r28c/API35 `libc++_shared.so` imports
none of these functions, so this is an explicit general-JNI extension rather
than part of the 160-symbol libc++ closure. A normally linked API35 fixture
imports `mmap`, `mmap64`, `mremap`, `munmap`, `mprotect`, `madvise`, and `__errno`, all at
the public `libc.so` `LIBC` symbol version.

`mmap` and `mmap64` accept only a null address, nonzero length no greater than
`PTRDIFF_MAX`, `MAP_PRIVATE|MAP_ANONYMOUS`, a zero offset, and `PROT_NONE/R/W/X`.
The anonymous fd is intentionally ignored and is never treated as a host fd.
Android `MAP_ANONYMOUS=0x20` is rebuilt as Darwin `MAP_ANON=0x1000`; guest flags
are never passed through. `MAP_FIXED`, shared, file-backed, ashmem, hints, and
nonzero offsets fail with explicit Android errno. Offset and range arithmetic
are checked against the Darwin page size exposed by the surrounding process.

Each successful host mapping is registered in a mutex-protected side table.
`mprotect`, `madvise`, and `munmap` accept only the complete owned mapping (the
original or host-rounded length); partial ranges are `EOPNOTSUPP`. Misaligned or
zero ranges fail before Darwin. Calls and retirement are serialized, so a
concurrent operation either sees the live mapping or is rejected from the table
without invoking a host syscall. The acceptance gate checks the table is empty
after unmap and never reads, writes, or executes a retired pointer. Provider
drop unmaps any remaining complete mappings.

Direct simultaneous write and execute mappings return Android `EACCES`; RW to
RX and RX to RW transitions are supported. An ownership-checked subset of an
anonymous private `PROT_NONE` reservation may later become RWX for Android V8
CodeRange compatibility. Such reservations carry Darwin `MAP_JIT`, and the host
executable carries macOS's narrow JIT and unsigned-executable-memory
entitlements. Other RWX transitions still fail closed. RX transitions
invalidate Darwin's instruction cache. Once a JIT reservation is host-RWX,
later Android `mprotect` write/execute phases translate to
`pthread_jit_write_protect_np` on the calling thread while the side table keeps
the guest-visible protection. Android's process-wide RWX contract does not map
directly onto Darwin's per-thread JIT mode: after a writing thread publishes
code, its first attempted entry into the MAP_JIT range raises Darwin `SIGBUS`.
The process signal bridge recognizes only an execute fault whose program
counter is inside a live, executable, provider-owned JIT range, switches that
thread back to execute mode, and retries the interrupted instruction. Other
`SIGBUS` faults remain ordinary Android signals. The Android ELF fixture writes
AArch64 instructions into RW memory, changes it to RX, and executes them.

Advice values are translated explicitly. Android `NORMAL/RANDOM/SEQUENTIAL/
WILLNEED` map to Darwin 0..3. Android `DONTNEED=4` uses an in-place fixed
anonymous remap to preserve Linux zero-fill semantics even
for protected PartitionAlloc ranges, and Android `FREE=8` maps to Darwin
`MADV_FREE=5`. File-backed `DONTNEED` remains file-backed and uses Darwin's
corresponding advice. Other known Android advice is
`EOPNOTSUPP`; unknown advice is `EINVAL`.

All guest failures publish Bionic pthread-local errno while C shims preserve
Darwin host errno. Unknown host errno translation sets a deterministic Android
`EIO` and a provider capability-failure bit. The resolver is closed and accepts
only the six exact versioned imports; it never uses dyld or `dlsym`.

File-backed mappings require a future central virtual-FD owner. That integration
must retain an open-file description independent of guest `close`, validate
mapping access mode and offset, and model EOF/SIGBUS without exposing a Darwin
fd. Partial unmap/protection needs an interval registry before it can be added.
Repeated `munmap` of a retired range is currently fail-closed `EINVAL`, stricter
than Linux's successful no-op.

Run `./audit.sh`. It pins NDK/Bionic/Darwin headers and libc exports, verifies
the zero pinned-libc++ demand, ABI constants/signatures, exact C dependencies,
exact versioned Android ELF imports, normal execution, Clippy/formatting,
C-boundary ASan and UBSan, concurrent owner retirement, and target cleanliness.
The distributed Rust ASan runtime is not claimed.
