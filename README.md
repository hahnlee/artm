# Darwin ART

This repository contains a Darwin-native Android compatibility runtime. The
active implementation lives in [`darwin-art`](darwin-art/README.md) and runs
Android 16 ART, framework code, DEX, framework widgets, and bounded Android
ARM64 native libraries directly on Apple Silicon without a Linux VM.

## Start here

```sh
cd darwin-art
cargo run -q -p art-bootstrap -- probe-runtime-apk-app-window
```

To launch a compatible no-native APK in the interactive host:

```sh
cd darwin-art
./tools/run-android-apk-app.sh path/to/app.apk 30
```

See [`darwin-art/README.md`](darwin-art/README.md) for supported Android APIs,
build inputs, reproducible gates, architecture, and current compatibility
boundaries.
