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

`darwin-art-host` and every service child hold a versioned Unix-socket lease for
their complete process lifetime. Shutdown and idle detach are prohibited while
any lease is live. A single advisory lock makes one daemon authoritative for a
profile, and peer credentials restrict IPC to the profile owner.

`tools/run-android-apk-app.sh` uses the default profile automatically. Setting
`DARWIN_ART_APP_DATA_ROOT` is an explicit test override that retains the legacy
caller-owned storage paths and does not start the profile daemon.

## Operations

```sh
cargo build --release -p darwin-art-profile --bins
target/release/darwin-artctl ensure
target/release/darwin-artctl status
target/release/darwin-artctl shutdown
tools/audit-profile-daemon.sh
```

The IPC envelope is versioned (`DARTD001`, protocol version 1). Future shared
Android services should be added as daemon capabilities rather than recreated
inside each application process.
