# API 36 super-image i18n APEX extractor

This standalone, std-only Rust tool reads the known Android API 36 ARM64
`system.img` layout without mounting or modifying it:

1. validate the primary GPT and select the `super` partition;
2. validate Android LP geometry, metadata SHA-256 checksums, and the `system`
   logical-partition extents;
3. expose those physical extents as a bounded read-only logical view;
4. walk the EROFS path `/system/apex/com.android.i18n.apex`;
5. decode only that inode's compact LZ4 big-pcluster extents into a new output.

The system logical partition is never copied. Unsupported GPT, LP target types,
EROFS layouts, compression algorithms, holes, overlaps, or malformed bounds are
hard errors. The input is opened read-only and the output uses `create_new`.

```sh
cargo run --release --manifest-path tools/super-i18n-apex-extract/Cargo.toml -- \
  "$HOME/Library/Android/sdk/system-images/android-36/google_apis_playstore/arm64-v8a/system.img" \
  /tmp/com.android.i18n.apex
```

The report includes GPT, LP and EROFS versions, logical/physical read counts,
the selected inode and a built-in SHA-256 of the extracted APEX.

