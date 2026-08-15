# Bionic DSO lifecycle facade

This standalone provider owns the exact libc++ lifecycle imports
`__cxa_atexit` and `__cxa_finalize`. It stores the function, argument, and DSO
triple without reinterpretation, extracts an entry before invoking it, executes
matching callbacks in reverse registration order exactly once, and releases its
mutex around each callback. A callback may therefore register another callback
or recursively finalize without deadlocking; the next scan starts from the
newest live entry.

The behavior is pinned to AOSP Bionic `android-15.0.0_r1` commit
`361ba86734fb2821a6adcfdf775db8abd04e0de0`. Non-null finalize invokes a
provider-owned `__unregister_atfork` composition hook after draining. Global
`finalize(NULL)` drains every DSO plus null-handle registrations and invokes a
provider-owned stdio-cleanup hook, matching the source-pinned Bionic boundary.
No call is forwarded to Darwin `__cxa_*`, dyld, `dlopen`, or `dlsym`.

## Loader boundary

The loader must keep a mapped image executable through this sequence:

1. Load and relocate the image, obtain its image-local `__dso_handle`, and call
   `Lifecycle::publish(handle)` before initializers can register destructors.
2. On unload, call the resolved Android `__cxa_finalize(handle)` entry. It
   drains new registrations made by destructor callbacks as well.
3. Call `Lifecycle::try_unpublish(handle)`. A busy result means a registration
   or callback raced with unload, so the loader must finalize again and retry.
4. Only after successful unpublish may the loader unmap executable segments.

Registration for a non-null, unpublished handle fails with `-1`; null-handle
process-exit registration remains valid. This fail-closed publication rule is
the facade's loader-safety extension. The loader must serialize the final
successful unpublish against admission of new guest work.

The Android-to-Darwin registration call contains only three pointer-class
arguments (`x0`-`x2`), and the reverse destructor call contains one (`x0`).
Those fixed-register slices are identical in Android AAPCS64 and Darwin arm64,
so no PCS thunk is required. This conclusion does not extend to variadic,
aggregate, HFA, or stack-spilled calls.

`audit.sh` builds a real Android AArch64 ELF with exactly the two imports. It
verifies exact triples, per-DSO and global LIFO order, duplicate suppression,
recursive finalization, callback-created registrations, concurrent
register/finalize races, cleanup hooks, and rejection of unmap while an Android
callback is executing. A separate host C boundary harness runs the prefixed shim
under ASan and UBSan. The sanitizer claim covers that C ABI boundary only; the
exact-two-import Android ELF is executed directly and is not sanitizer-built.
