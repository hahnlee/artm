# Darwin ART profile daemon

`darwin-artd` is the per-profile owner of state shared by Android application
processes. It is deliberately closer to Wine's `wineserver` boundary than to
an app-launch helper: applications request resources and hold leases, while the
daemon alone owns their lifecycle.

## Filesystem ownership

Each profile has one case-sensitive APFS sparse bundle at:

```text
~/Library/Application Support/DarwinART/profiles/<profile>/
  control.sock
  darwin-artd.lock
  android-data.sparsebundle
  mnt/
```

The daemon creates the image as `APFSX`, mounts it without Finder browsing,
verifies that differently-cased names coexist, and initializes Android data,
package, emulated-storage, and run directories. The socket and lock stay on the
host volume so they remain reachable while the guest volume is detached.

The Rust launcher opens a versioned Unix-socket lease and carries its descriptor
through `exec(2)` into each `darwin-art-host`, publishing the real process PID
and package name through `darwin-artctl ps`. The host restores close-on-exec
before it launches children; each Android Service child receives a distinct
lease through the same exec bridge. Shutdown and idle detach are prohibited
while any lease is live. Keeping profile IPC out of the latency-sensitive ART
host link also avoids coupling runtime rebuilds to daemon implementation
changes. A single advisory lock makes one daemon authoritative for a profile,
and peer credentials restrict IPC to the profile owner.

## Install and launch

The normal interface separates one-time APK inspection/installation from
package launch:

```sh
cargo xtask build
tools/darwin-art install path/to/app.apk
tools/darwin-art list
tools/darwin-art run com.example.app
tools/darwin-art ps
```

Installation copies the unchanged APK into an immutable, content-addressed
package directory and atomically registers a versioned launch record under the
profile's case-sensitive guest volume. A deoptimized sidecar DEX, when needed,
is copied into the same persistent package-code area. Later `run PACKAGE`
operations resolve that record through `darwin-artd`; they do not inspect the
APK, invoke Cargo, rebuild native code, or reinstall the package. Records and
application data survive daemon detach/restart.

`tools/run-android-apk-app.sh` remains the low-level installer/launcher used by
this interface. Setting `DARWIN_ART_APP_DATA_ROOT` is an explicit test override
that retains the legacy caller-owned storage paths and does not start the
profile daemon.

## Operations

```sh
cargo build --release -p darwin-art-profile --bins
target/release/darwin-artctl ensure
target/release/darwin-artctl status
target/release/darwin-artctl list
target/release/darwin-artctl ps
target/release/darwin-artctl shutdown
tools/audit-profile-daemon.sh
```

The IPC envelope is versioned (`DARTD001`, protocol version 1). Future shared
Android services should be added as daemon capabilities rather than recreated
inside each application process.
