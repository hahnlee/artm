# Android 35 Bionic pthread/TLS provider — first coherent slice

This module is derived from the SHA-locked NDK r28c/API 35 arm64
`libc++_shared.so`. That ELF imports 24 `pthread_*` functions from
`libc.so@LIBC`; the first coherent slice owns 11 of them:

```text
pthread_self
pthread_key_create / pthread_key_delete
pthread_getspecific / pthread_setspecific
pthread_once
pthread_mutex_init / lock / trylock / unlock / destroy
```

The remaining 13 imports are retained in the manifest as unsupported. A
resolver can therefore never mistake this slice for complete pthread support.

## Object and state boundary

Android arm64 exposes `pthread_t` as an eight-byte `long`, key and once controls
as four-byte integers, and mutex storage as 40 opaque bytes. Darwin uses
different representations. None of these Android values is cast to a Darwin
`pthread_t`, `pthread_key_t`, `pthread_once_t`, or `pthread_mutex_t`.

- `pthread_self` returns a process-unique, thread-local 64-bit Android token. It
  is stable on one thread and never exposes Darwin's `pthread_t`.
- TLS keys use an Android high-bit/index key and one provider-global Darwin TLS
  key. Each participating host thread owns one fixed 128-slot value table;
  Android keys and values are never reinterpreted as Darwin pthread objects.
  The single host destructor performs Bionic's four passes, clearing each value
  before its callback so callback reinsertion is observed by the next pass. Key
  deletion does not invoke user destructors. Pinned Bionic exposes only
  `KEY_VALID_FLAG | slot`; its generation stays in internal per-thread data.
  Consequently, using an integer key after deletion is POSIX-undefined and can
  alias a newly allocated key in both Bionic and this provider. Adding guest-
  visible generation bits would break Bionic's key validation ABI.
  Deleting a key that still has values on foreign live threads never calls the
  Android destructor. Its stale value occupies only the existing fixed slot;
  repeated delete/recreate overwrites that slot, so storage is bounded by 128
  entries per participating thread rather than delete count. There is no
  per-key host-key deletion and therefore no late Darwin destructor UAF or
  owning cell cycle. Concurrent application-level stale-key use remains POSIX-
  undefined, while provider state access is serialized by its table lock.
- Once controls retain Android's visible states 0/1/2, while wait ownership and
  condition state live in a side table keyed by the Android object address.
- Normal process-private mutexes own an independently initialized Darwin mutex
  in a side table. The Android 40-byte storage remains Android-formatted and is
  marked `0xffff` only after successful destruction, matching Bionic's visible
  destroyed state.

The singleton provider owns these tables for the process and must outlive every
loaded Android image. Its root state is intentionally process-lifetime so late
Darwin TLS destructors cannot access a destroyed C++ static. Participating
thread tables remain registered until their one host destructor runs. A
quiescent reset succeeds only with no active Android keys and no foreign thread
table; it detaches and frees the caller's table. Mutex tombstones and completed-once entries remain bounded
by Android object addresses until the loader's provider-reset/process-exit
boundary. Key generation prevents deleted-slot stale TLS values;
object-address tombstones prevent use-after-destroy from silently constructing
a new mutex. A per-entry lifecycle lock serializes lookup operations with
destroy; destroy waits for in-flight provider calls and reopens the entry if the
host reports `EBUSY`. Pthread errors are returned as Android/Linux numbers rather than
Darwin errno numbers (`EAGAIN=11`, `EDEADLK=35`, `ENOTSUP=95`).

## Deliberate capability failures

Fork while once initialization is underway, robust mutexes, process-shared
objects, priority inheritance, recursive/error-check mutex attributes,
cancellation cleanup, condition variables, rwlocks, and thread create/join/
detach are not implemented. Non-null mutex attributes return Android `ENOTSUP`;
their resolver symbols are absent. There is no unprefixed interposition or
Darwin `dlsym` fallback.

The only Android-to-Darwin callback signatures exercised here are `void()` for
once and `void(void*)` for TLS destruction. Both are fixed, register-only ARM64
signatures already covered by the dual-PCS proof. This does not claim arbitrary
callback ABI compatibility.

## End-to-end gate

```sh
tools/build-android35-bionic-pthread-provider.sh
```

The gate:

1. re-derives all 24 pthread imports and their `LIBC` versions from the pinned
   `libc++_shared.so`;
2. sparsely materializes four exact Bionic implementation files without Git;
3. builds a Darwin arm64 provider archive and an NDK AArch64 ELF fixture;
4. loads that ELF with `darwin-art-elf-loader` and an exact `libc.so@LIBC`
   resolver, then actually executes all 11 imports;
5. runs eight Darwin threads through the same loaded Android image, checks
   stable/unique thread tokens, two-pass TLS destructors, once-only execution,
   16,000 contended mutex increments, and concurrent destroy-versus-lookup;
6. proves the pinned Bionic deleted-key/same-slot reuse alias without claiming
   that POSIX-undefined stale-key use is supported application behavior;
7. runs an ASan/UBSan eight-thread delete-versus-get/set stress plus 10,000
   same-thread create/set/delete cycles. The live-value count remains one—not
   10,000—and returns to zero at quiescent reset, proving bounded fixed-slot
   ownership without per-key host destructor races or ownership cycles.

Artifacts are written to `_build/bionic-pthread-provider`.
