# Bionic binary stdio facade

This standalone provider implements the coherent fixed-register binary slice of
the pinned libc++ stdio imports. Android `FILE` is a 152-byte, 8-byte-aligned
opaque token and is never reinterpreted as Darwin `FILE*`. Dynamic tokens own no
host pointer; a mutex-protected side table owns all cursor, bytes, mode, pushback,
and virtual-fd state.

`__sF` is a guest-visible array of exactly three permanent Android FILE tokens.
During activation they represent an in-memory stdin source and captured
stdout/stderr sinks with virtual descriptors 0/1/2. Their storage has process
lifetime, while operations require an active provider. Dynamic `fopen` tokens
are freed by `fclose`; stale use is never dereferenced and returns `EBADF` until
allocator reuse, after which POSIX stale-FILE use remains undefined.

Supported imports are `fopen`, `fclose`, `fflush`, `fileno`, `fread`, `fwrite`,
`fseek`, `fseeko`, `ftello`, `fputc`, `getc`, and one-byte `ungetc`. Paths and
modes are byte strings. This vertical slice uses explicit in-memory file
snapshots and private writable streams, so it cannot expose host paths, FILEs,
or descriptors. `fileno` values starting at 20000 are provider-local only.
The accepted mode grammar is deliberately narrow: `r`/`rb`, all three `r+`
binary spellings, `w`/`wb`, and all three `w+` binary spellings. Append,
close-on-exec mode suffixes, persistence after close, and host filesystem access
are unsupported rather than silently approximated.
The standalone in-memory backing is capped at 16 MiB per stream; writes or
sparse extensions beyond that return Android `EFBIG` with checked arithmetic.

Long-term integration must replace the backing store and virtual-fd allocator
with the central filesystem facade namespace. A stdio token must retain one
reference to that central open-file description so `FILE` buffering, fd close,
and dup semantics have one owner; these local virtual fds must never be passed
to the current filesystem provider.

`fprintf`/`vfprintf` and all formatted varargs, wide, locale, `wchar_t`, and
long-double functions are deliberately absent from the resolver. Android and
Darwin PCS/`va_list`/wide layouts are not assumed compatible. The exact accepted
and rejected libc++ imports are in `manifests/imports.tsv`.

The global table lock serializes each operation including concurrent close/use.
Whichever acquires it before close completes; later calls return `EBADF` without
dereferencing the freed token. The gate runs this contract from real Android
AArch64 code. The C ABI shims and errno provider run the same E2E under Apple
Clang ASan and UBSan; the ownership core is safe Rust with checked arithmetic,
Clippy, formatting, exact imports, source-pinned ABI probes, and target-clean
checks. Rust's distributed ASan runtime is not claimed by this gate.
Allocation exhaustion inside the safe Rust backing store is not translated to
Android `ENOMEM`; the fixed 16 MiB cap bounds individual stream growth, but a
future central filesystem owner must provide fallible allocation/accounting.
