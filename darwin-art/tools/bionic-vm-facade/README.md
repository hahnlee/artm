# Bionic virtual-memory facade

This standalone Tier-1 provider owns a deliberately narrow Android arm64
virtual-memory namespace. The pinned NDK r28c/API35 `libc++_shared.so` imports
none of these functions, so this is an explicit general-JNI extension rather
than part of the 160-symbol libc++ closure. A normally linked API35 fixture
imports `mmap`, `mmap64`, `munmap`, `mprotect`, `madvise`, and `__errno`, all at
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

W^X is mandatory: simultaneous write and execute permission returns Android
`EACCES`; RW to RX and RX to RW transitions are supported. RX transitions
invalidate Darwin's instruction cache. The Android ELF fixture writes AArch64
instructions into RW memory, changes it to RX, and executes them.

Advice values are translated explicitly. Android `NORMAL/RANDOM/SEQUENTIAL/
WILLNEED` map to Darwin 0..3, Android `DONTNEED=4` maps to Darwin
`MADV_ZERO=11` to preserve Linux anonymous zero-fill semantics, and Android
`FREE=8` maps to Darwin `MADV_FREE=5`. Other known Android advice is
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
