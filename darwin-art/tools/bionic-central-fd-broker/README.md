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

- A provider installs an ABI-v1 callback table and receives an opaque owner
  handle. Multiple owners, including multiple owners of the same descriptor
  kind, share the central slot allocator without overlapping token ranges.
- The four initial kinds are filesystem file, filesystem random device, stdio,
  and socket. A published slot records both the exact owner instance and kind;
  typed operations reject a foreign owner or wrong kind before invoking a
  callback.
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
- Owner uninstall first enters draining state, which rejects publication. It
  returns busy while any live descriptor, active lease, or close callback
  remains. Only a later quiescent call removes the owner. Broker destruction is
  likewise rejected until all owners are uninstalled.

The probe covers four kinds and five simultaneous owners, including two file
owners; namespace collision freedom; foreign-owner and wrong-kind rejection;
double close; stale access; same-slot generation-safe reuse; close racing a
blocked read; uninstall racing a blocked close callback; typed ioctl; mixed
poll; and file-to-socket sendfile. It runs the same state machine under ASan,
UBSan, and TSan and rejects host descriptor or dynamic-loader imports.

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
- `dup`/`dup2`/`dup3`, `fcntl(F_DUPFD*)`, `SCM_RIGHTS`, directory handles, and
  epoll registrations need explicit shared-object reference semantics before
  they can enter this ABI. A copied integer token is not a new lease.
- Real blocking poll requires a central wakeup/fan-in mechanism across virtual
  files, sockets, and close events. The current callback demonstrates safe typed
  dispatch only; it does not claim Linux timeout, signal-mask, or readiness-edge
  parity.
- Provider callbacks must treat the broker object key as opaque and maintain
  their own object lifetime until close returns. They must never unwrap another
  owner's key or publish a host descriptor as a guest token.
