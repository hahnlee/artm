# Bionic network runtime integration freeze

This new-only preflight freezes the smallest safe path from a real ART native
load to the already accepted loopback network providers. It does not bind the
standalone socket provider into the shared runtime yet.

The Android NDK r28c/API-35 AArch64 fixture exports `JNI_OnLoad`, obtains the
live ART `JNIEnv`, and registers one static `(I)I` native method. The method
performs a numeric-loopback HTTP/1.0 request through these exact imports:

```text
__errno -> getaddrinfo -> socket -> connect -> send/recv -> close
        -> freeaddrinfo
```

It accepts only a Java-supplied TCP port and hard-codes `127.0.0.1`; it cannot
request external DNS or connect to a non-loopback address. `JNI_OnLoad` itself
does no I/O. This allows the host probe to create the listener before ART loads
the DSO and to execute the request only after native registration succeeds.

`manifests/routes.tsv` is the future process namespace contract. Every symbol
is owned once under exact `libc.so`/`LIBC`; notably, public `close` belongs to
the central descriptor broker rather than the filesystem or socket provider.
`manifests/lifecycle.tsv` fixes admission and teardown order. Guest DSO
finalizers run while providers are still callable; namespace admission then
closes, in-flight operations drain, DNS quarantine is reclaimed, the socket
owner is uninstalled, and the empty broker is destroyed.
`manifests/required-broker-abi.tsv` freezes the missing lease primitive, while
`manifests/shared-scope.tsv` lists every shared integration surface that must
change atomically once that primitive passes its sanitizer gate.

## Current hard blocker

The central broker safely pins leases only for its built-in
`read`/`write`/`poll`/`ioctl`/`close` paths. Its public ABI cannot currently
lend a generation-checked socket object to `connect`, `send(flags)`,
`recv(flags)`, `bind`, `listen`, or `accept4`. The standalone socket facade's
private token table cannot remain beside the broker: both use marked positive
integers and process-wide libc `close` would have ambiguous ownership.

Runtime activation therefore requires one narrow broker extension: a
versioned, typed socket-control surface for connect/bind/listen/accept and
send/recv flags. It must acquire the same generation/kind/owner lease used by
close, invoke only the matching installed owner outside the broker mutex, and
release before returning. Accept must publish its child atomically or close it
on publication failure. The ABI must never expose a host descriptor or object
key to guest code. A generic object unwrap or private `ioctl` request is not an
acceptable substitute.

Run the structural preflight with:

```sh
./tools/bionic-network-runtime-integration/audit.sh
```

The gate compiles the real Android JNI DSO, checks its sole export and exact
eight-symbol import surface, verifies that the current broker ABI still lacks
the required dispatch primitive, and checks the pinned route/lifecycle freeze.
It intentionally reports `activation=blocked-safe` until the broker extension
and actual ART loopback execution are landed together.
