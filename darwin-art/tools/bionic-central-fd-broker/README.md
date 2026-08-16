# Bionic central descriptor broker

This standalone gate specifies the one process-wide guest descriptor namespace
that the Darwin ART Bionic providers must eventually share. It does not modify
or integrate the current filesystem, stdio, socket, sendfile, or ioctl providers.

Each published descriptor is a positive 31-bit token with a broker marker, a
20-bit generation, and a 10-bit slot. Closing increments the generation before
the slot is reused; generation exhaustion permanently retires the slot. A stale
token therefore cannot name a later object in the same slot. These tokens never
contain or expose a Darwin descriptor.

Run the standalone acceptance gate with:

```sh
./tools/bionic-central-fd-broker/audit.sh
```

## C ABI and ownership contract

- A provider installs an ABI-v2 callback table and receives an opaque owner
  handle. The prefix-sized ABI-v1 table remains accepted without reading or
  invoking its absent offset callbacks. Multiple owners, including multiple
  owners of the same descriptor kind, share the central slot allocator without
  overlapping token ranges.
- The provider kinds are filesystem file, filesystem random device, stdio, and
  socket; epoll is an internal broker-owned kind. A published slot records both
  the exact owner instance and kind; typed operations reject a foreign owner or
  wrong kind before invoking a callback.
- A slot points to a refcounted open-file description. `dup`,
  `duplicate_with_flags`, and `F_DUPFD_CLOEXEC` allocate distinct
  generation-tagged slots referencing that same description. File status flags
  and offset are shared and serialized across all references; `FD_CLOEXEC` is
  per descriptor. Plain dup clears it, while `duplicate_with_flags(CLOEXEC)`
  and F_DUPFD_CLOEXEC set it. F_DUPFD_CLOEXEC selects the numerically lowest
  available encoded token at or above the requested nonnegative minimum. The
  owner close callback runs exactly once after the last descriptor and active
  lease.
- Read, write, poll, ioctl, and sendfile dispatch borrow a generation-checked
  lease. Callbacks execute without the broker mutex. Close atomically marks the
  slot closing, rejects new leases, waits for every active lease, invokes the
  owner close callback, and only then advances the generation and makes the slot
  reusable.
- Sendfile acquires input and output leases atomically. Its input must be a
  filesystem file and its output must be a filesystem file or socket. The
  standalone copy loop demonstrates cross-owner dispatch; production offset,
  partial-write, and signal behavior remains owned by the existing sendfile/FS
  semantic boundary.
- Poll ignores negative descriptors and reports Android `POLLNVAL` (`0x20`) for
  invalid or stale positive tokens. A mixed poll set is routed entry-by-entry to
  the recorded owners; no provider probes another provider's private range.
- `epoll_create1` creates a broker-owned descriptor. `epoll_ctl` supports
  ADD/MOD/DEL for socket descriptions, rejects a duplicate registration key,
  and stores a weak OFD reference plus event data. Distinct dup tokens may be
  registered separately, and readiness remains attached to the OFD after one
  token closes. `epoll_wait(..., timeout=0)` fans out to socket owner poll
  callbacks while holding active OFD leases; blocking timeouts fail closed.
- Owner uninstall first enters draining state, which rejects publication. It
  returns busy while any live descriptor, active lease, or close callback
  remains. Only a later quiescent call removes the owner. Broker destruction is
  likewise rejected until all owners are uninstalled.

The host probe covers four provider kinds and six simultaneous owners, including
three file owners and a prefix-sized ABI-v1 owner; namespace collision freedom;
foreign-owner and wrong-kind rejection;
double close; stale access; same-slot generation-safe reuse; close racing a
blocked read; uninstall racing a blocked close callback; typed ioctl; mixed
poll; shared dup offset/status versus independent descriptor flags; last-close
refcounting; duplicate epoll registration; and file-to-socket sendfile. It runs
the same state machine under ASan, UBSan, and TSan and rejects host descriptor
or dynamic-loader imports. A real NDK r28c API-35 Android AArch64 ELF separately
locks the `dup`, `dup3`, `fcntl`, `epoll_create1`, `epoll_ctl`, `epoll_wait`, and
`close` LIBC import surface and Android constant/layout ABI.

## Integration blockers

- The current filesystem and socket facades each allocate their own guest
  integer tokens, while stdio owns separate stream state. Their allocation,
  lookup, close, and test-only reset paths must be replaced together; switching
  only one provider would leave ambiguous descriptors at the libc entry points.
- Process-wide libc `close`, `read`, `write`, `poll`/`ppoll`, `ioctl`, and
  `sendfile` must enter the broker first. Provider-specific close functions may
  use `close_owned` during migration, but the final public `close` route must be
  generic and owner-dispatched.
- Guest descriptors `0`, `1`, and `2` need an explicit bootstrap policy. This
  prototype intentionally emits only marked high tokens; it does not decide
  whether standard streams are fixed broker slots, inherited capabilities, or
  unavailable in an app sandbox.
- Exact-target `dup2`/`dup3` remains unresolved. A generation-safe encoded token
  cannot simultaneously equal an arbitrary caller-selected integer and reject
  stale copies after replacement. The broker deliberately exposes the
  non-Linux-named `duplicate_with_flags` allocation primitive instead. The
  Android fixture pins the real `dup3` import as a required translation seam;
  a libc shim must reject it until a separate guest-fd indirection policy exists
  and must never silently route it to `duplicate_with_flags`.
- `fcntl(F_DUPFD)` without CLOEXEC, `SCM_RIGHTS`, directory handles, and epoll
  nesting are still fail-closed. SCM_RIGHTS in particular must transfer a new
  OFD reference, never copy a token integer across broker instances.
- Real blocking poll/epoll requires a central wakeup/fan-in mechanism across
  virtual files, sockets, close events, timers, and signals. The current epoll
  callback demonstrates generation-safe level readiness only; it does not claim
  timeout, signal-mask, EPOLLET, EPOLLONESHOT, or fairness parity.
- Socket lifecycle/data operations with non-read/write arguments are not yet a
  public broker capability. There is intentionally no API that unwraps a guest
  token into an owner object, and `ioctl` must not be used to tunnel `connect`,
  `bind`, `listen`, `accept`, `send(flags)`, or `recv(flags)`. Integration needs
  an ABI-versioned, typed socket-control callback surface that validates the
  live generation, socket kind, and owner, holds the descriptor/description/
  owner lease for the complete callback, and publishes or rolls back an
  accepted child atomically. Until that exists these calls must fail closed.
- Provider callbacks must treat the broker object key as opaque and maintain
  their own object lifetime until close returns. They must never unwrap another
  owner's key or publish a host descriptor as a guest token.
