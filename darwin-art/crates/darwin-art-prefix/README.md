# Darwin ART virtual prefix

This crate routes Android-visible byte paths into an immutable mount identifier
and a normalized mount-relative byte path. It deliberately does not require
UTF-8 and does not store or return host paths.

The caller owns the host-side mount table and must open the returned relative
path beneath a pre-opened directory descriptor. A later filesystem broker must
walk every component with `openat`-style operations, preserve trailing-slash
directory intent, and reject symlink escapes and rename races. String
concatenation with a macOS path is outside this API's security contract. Until
that broker exists, this crate must not authorize host filesystem operations.

Absolute `..` components clamp at the virtual Android root, matching Linux
pathname lookup. Mount selection happens after lexical normalization and uses
the longest whole-component prefix. Since lexical `..` is not equivalent to
kernel namei when a preceding component is a symlink, the eventual FD broker is
the authority for actual traversal.

Run the Rust and C ABI acceptance gate with:

```sh
bash tools/build-prefix-resolver.sh
```
