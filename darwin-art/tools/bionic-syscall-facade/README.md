# Bionic syscall facade

This standalone module owns the one `syscall@LIBC` import in the SHA-locked
NDK r28c/API-35 arm64 `libc++_shared.so`. Ownership is deliberately limited to
all nine call sites actually present in that ELF: two `gettid` calls from
`__cxa_guard_acquire`, six private futex calls from libc++ atomic wait/notify,
and libunwind's one `rt_sigprocmask` readability probe. `callsites.tsv` fixes
every instruction address and argument form.

Android and Darwin variadic ABIs are not assumed interchangeable. An AArch64
assembly entry captures x0-x7 and the caller stack before a non-variadic core
dispatches Android syscall numbers. It never invokes Darwin `syscall`, dlsym,
dyld, or a host signal-mask API.

The supported behavior is:

- syscall 178 (`gettid`) returns a positive, stable, provider-owned ID unique
  to each live host thread;
- syscall 98 accepts only `FUTEX_WAIT_PRIVATE` with a readable aligned word and
  relative timeout, or `FUTEX_WAKE_PRIVATE` with count 1/`INT_MAX`;
- futex wait performs the compare and waiter admission under the same provider
  lock used by wake, then uses a fixed 257-address condition side table. It
  returns Android `EAGAIN` on compare mismatch or table exhaustion and
  `ETIMEDOUT` on timeout. A monotonic deadline is recomputed after every
  spurious wake, so wall-clock changes and spurious notifications cannot
  extend the caller's relative duration;
- syscall 135 accepts only libunwind's
  `rt_sigprocmask(-1, address, nullptr, 8)` probe. `mach_vm_region` checks that
  the eight-byte range is wholly readable without dereferencing it. The call
  always returns -1 with Android `EINVAL` for readable memory or `EFAULT` for
  an unreadable, unmapped, or overflowing range, matching the Linux probe's
  observed contract without changing a signal mask.

All other syscall numbers fail with Bionic `ENOSYS`; unowned futex operations
and argument forms fail with Bionic `EINVAL`/`EFAULT`. Host errno is preserved.
This is not a general Linux syscall translator, shared-process futex service,
robust-list implementation, PI futex implementation, or signal API. The exact
unsupported boundary is recorded in `manifests/unsupported.tsv`; no path fakes
success merely to satisfy eager relocation.

`audit.sh` pins the NDK ELF, Linux UAPI constants, LLVM sources, all nine
disassembly sites, and the exact libc++ import. Its real Android AArch64 ELF
gate exercises stable/unique thread IDs, compare mismatch, wake-one, wake-all,
contention, relative timeout, readable/guard/unmapped/overflow libunwind
probes, unreadable wake addresses, spurious wake deadline behavior, table
exhaustion, wake-token cleanup, unknown operations, Bionic errno, and host
errno under ASan/UBSan.
