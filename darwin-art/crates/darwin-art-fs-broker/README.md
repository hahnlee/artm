# Darwin ART filesystem broker gate

This standalone crate is the first read-only filesystem authorization gate for
a previously selected mount. `ReadOnlyBroker` owns a directory file descriptor;
guest-relative byte paths are resolved one component at a time with Darwin
`openat(2)`. Intermediate components use `O_DIRECTORY | O_NOFOLLOW`, and the
leaf uses `O_NOFOLLOW`. A trailing slash additionally requires the leaf to be a
directory. Metadata is obtained from the opened descriptor, never by looking up
the path a second time.

## Deliberately unsupported

Symlinks are denied at every path component, including the final component.
The broker does not emulate Linux symlink resolution, does not return a
best-effort target, and does not turn unsupported cases into success. Callers
that need symlinks must add a separately reviewed resolver with an explicit
containment proof.

Paths are relative to the broker's mount-root descriptor. Empty bytes name the
mount root; absolute paths, NUL, `.`, `..`, repeated separators, and a lone
trailing separator are rejected before any lookup. Non-UTF-8 bytes are passed
unchanged to Darwin; the mounted filesystem may itself reject them (APFS
commonly returns `EILSEQ`) and that remains a hard error.

Holding every opened directory descriptor prevents a concurrent rename from
redirecting subsequent components to a replacement at the old pathname. As on
other systems without Linux `openat2(RESOLVE_BENEATH)`, the mount root and
namespace mutation authority remain part of the trust boundary: a party that
can relocate an already-open directory and then mutate that directory can
change what the descriptor itself contains.

Only regular files and directories are admitted. Other node types are rejected
after a nonblocking read-only open. This crate currently targets macOS/Darwin
only and fails to compile elsewhere rather than providing a weaker fallback.
