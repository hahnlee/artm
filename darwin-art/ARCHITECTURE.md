# Darwin Android compatibility architecture

## Product rule

Darwin ART preserves every behavior that an Android application can observe and
uses macOS for the underlying mechanism. This is the same boundary Wine uses:
guest ABI and namespace above, host kernel and devices below.

```text
Android application behavior
  ART / Framework / Bionic ABI / Android resources and paths
                              ↓
Darwin compatibility boundary
  ELF loader / Bionic facade / path namespace / service bridges
                              ↓
macOS mechanisms
  Darwin VM and threads / APFS / sockets / Metal / CoreAudio / AppKit
```

An optimization is valid only if the application cannot distinguish it from
Android behavior. An Android `Button` remains a View rendered by HWUI/Skia; it
is not replaced by `NSButton`. Android `O_*`, errno, `stat`, pthread, and path
semantics remain Android/Linux values; Darwin values stay behind the boundary.

## Target process model

The current proof of concept loads a one-shot ART dynamic library into the Rust
host. The product design separates untrusted application code from privileged
macOS integration:

```text
darwin-androidd (Rust service broker)
  ├─ package and prefix ownership
  ├─ permissions and security-scoped bookmarks
  ├─ clipboard, notifications, URL and file-panel integration
  └─ shared Android service state

Android application process (sandboxed Darwin process)
  ├─ ART and Android Framework
  ├─ application DEX
  ├─ custom ARM64 ELF loader
  ├─ Bionic ABI facade
  └─ Android ARM64 .so code

macOS UI process/broker
  ├─ NSWindow and NSEvent
  ├─ IOSurface and Metal
  └─ CoreAudio and other user-facing frameworks
```

Android application processes should be separate Darwin processes, matching
Android's failure and security isolation. A shared broker owns global services;
it does not execute application JNI. VM execution remains an optional fallback
backend for binaries that require real Linux kernel behavior.

## Virtual Android prefix

Android code must never depend on host absolute paths. Each runtime profile has
a Wine-prefix-like Android root:

```text
~/Library/Application Support/DarwinART/prefixes/default/
  system/                       read-only runtime image
    etc/fonts.xml
    fonts/Roboto-Regular.ttf
  system_ext/                   read-only framework extension
  product/                      read-only/product overlays
    etc/fonts_customization.xml
  vendor/                       compatibility files when required
  apex/                         extracted APEX runtime assets
  data/
    app/                        installed APK state
    user/0/<package>/           private application data
    dalvik-cache/               generated runtime artifacts
    fonts/                      Android-managed/updatable fonts
  storage/emulated/0/           user-visible Android storage namespace
```

A mount table maps Android paths to providers. Resolution uses longest-prefix
matching after Android-compatible normalization:

| Android path | Provider |
|---|---|
| `/system`, `/product`, `/apex` | immutable runtime-prefix directories |
| `/data/user/0/<package>` | package-private case-sensitive storage |
| `/storage/emulated/0` | user-approved shared-folder provider |
| `/mnt/host/<name>` | explicit macOS share backed by a security-scoped bookmark |
| `/proc`, `/sys`, `/dev` | synthetic providers, never host directories |

The private prefix must be case-sensitive even when the user's main APFS volume
is not. The preferred product representation is a sparse, case-sensitive APFS
image or an equivalently isolated case-sensitive volume. Shared host folders
retain host semantics and are identified as such to Android callers. Symlink
escape, `..` traversal, and mount crossing are checked before a host file
descriptor is opened.

The namespace owns virtual uid/gid, modes, Android ownership tags, and any
metadata macOS cannot represent directly. File descriptors contain real Darwin
descriptors after resolution, but Android-visible flags, errno, and structures
are converted at every ABI boundary.

### Shared files

The macOS broker obtains user permission with security-scoped bookmarks and
publishes a mount inside `/storage` or `/mnt/host`. Android pickers and share
intents should return content URIs or Android paths, not `/Users/...` paths.
For operations that cannot safely expose a directory, the broker passes an
already-open file descriptor.

### Fonts

The deterministic baseline uses the Android release's Roboto/Noto files,
`fonts.xml`, and product customization XML inside the prefix. Minikin,
HarfBuzz, ICU, FreeType, and Skia retain Android fallback, shaping, and raster
behavior.

CoreText may later enumerate host fonts, but it is only a provider. Approved
font files are published through synthetic `/data/fonts` entries and an
Android font configuration. Android code must not depend directly on
`/System/Library/Fonts` or CoreText layout behavior.

## Native ARM64 ELF execution

Apple Silicon can execute Android ARM64 instructions directly, but dyld cannot
load Android ELF and Darwin cannot service Linux syscalls. Native support is
therefore two separate components.

### ELF loader

The loader owns:

1. ELF64/AArch64 validation and executable-segment policy;
2. `PT_LOAD`, `PT_DYNAMIC`, RELRO, and memory protection;
3. AArch64 RELA relocations, GNU/SysV hash, symbol versions, and weak symbols;
4. dependency namespaces and Android linker search rules;
5. constructors/destructors and `dlopen`/`dlsym`/`dlclose`;
6. Bionic TLS, thread-local destructors, and module IDs;
7. ELF unwind metadata and Android `libc++_shared.so` exception behavior;
8. JNI discovery and `JNI_OnLoad`/`JNI_OnUnload` lifetime.

The loader maps ELF directly; it does not convert it permanently to Mach-O.
Rust is suitable for parsing, validation, dependency policy, and the mount
namespace. C/C++ and small ARM64 assembly thunks own varargs, TLS, unwind, and
ABI entrypoints where exact Bionic layout is required.

### Virtual Android DSOs

Normal NDK libraries import Bionic and Android libraries rather than issuing a
kernel instruction themselves. The loader resolves those imports to facades:

```text
libapplication.so (Android ELF)
  ├─ libc.so          Bionic-compatible libc and syscall facade
  ├─ libm.so          Android math ABI
  ├─ libdl.so         custom ELF linker API
  ├─ liblog.so        Android logging API
  ├─ libandroid.so    lifecycle, asset, window, and input bridge
  ├─ libc++_shared.so Android C++ runtime as ELF
  └─ libEGL/GLES.so   later ANGLE/Metal graphics backend
```

The facades are analogous to Wine's `ntdll` and system DLLs. They preserve the
guest ABI and call Darwin mechanisms internally.

## Prior art and the boundary we adopt

This design reuses lessons from existing foreign-ABI runtimes rather than
assuming that Linux-on-Darwin is unexplored:

| Project | Mechanism | What we adopt | What we do not adopt |
|---|---|---|---|
| [Noah](https://github.com/linux-noah/noah) | macOS Hypervisor.framework traps Linux syscalls, translates them to Darwin, and loads ELF | proof that a Darwin-hosted ELF/syscall personality is possible; a possible fallback execution domain for raw `svc` | translating the complete Linux userspace ABI as the Tier 1 fast path; the project is archived and reports broad incompatibility from missing syscalls |
| [gVisor](https://gvisor.dev/docs/architecture_guide/intro/) | a userspace application kernel implements Linux-visible processes, filesystems, signals, memory, and networking | guest-visible state must be owned explicitly; host calls are implementation details rather than direct syscall passthrough | its complete Linux process/kernel model and Linux-host interception mechanisms |
| [FreeBSD Linuxulator](https://docs.freebsd.org/en/articles/linux-emulation/) | the host kernel selects a Linux ABI personality with syscall, errno, signal, and structure translation tables | source-derived translation tables and coherent ABI ownership | a kernel-resident implementation, which macOS does not expose as a product API |
| [Blink](https://github.com/jart/blink) | CPU emulation plus a portable subset of the Linux syscall ABI | explicit compatibility manifests and precise unsupported results | CPU instruction emulation on same-ISA Apple Silicon |

Noah is the closest direct precedent, but its result also explains why the
mainstream path starts one level above raw syscalls. Android NDK libraries
normally reach the kernel through Bionic imports. Resolving those imports to
our virtual DSOs keeps ordinary ARM64 instructions native and limits semantic
translation to named ABI entrypoints.

Raw `svc #0` is a separate capability. A future Noah-like Hypervisor.framework
domain may trap it without running a full Linux kernel, but that execution
model cannot be silently mixed with host ART/JNI pointers. It requires an
explicit shared-memory/RPC ABI or moving the relevant runtime into the same
guest address space. Until that boundary is proven, raw-syscall libraries use
the VM fallback rather than weakening the Tier 1 process.

## Syscall policy

The runtime does not try to translate every `svc` instruction dynamically.
The normal fast path intercepts calls at the Bionic API boundary:

```text
Android .so -> Bionic open() -> path resolver -> flag/stat conversion -> Darwin open()
```

Initial syscall families are grouped by semantics:

| Family | Darwin implementation strategy |
|---|---|
| file I/O and VM | direct Darwin calls after namespace/constant conversion |
| sockets | Darwin sockets with Android structures, options, and errno conversion |
| pthread/TLS | Bionic memory layout backed by a Darwin parking-lot/side table |
| futex | user-space wait/wake parking lot; never reinterpret Bionic mutex memory |
| epoll/eventfd/timerfd | kqueue-backed Android objects with edge/oneshot fidelity |
| signals | explicit Android signal model and reserved runtime signals |
| properties/Binder/ashmem | runtime broker objects rather than host syscalls |
| `/proc`, `/sys`, devices, ioctl | synthetic or allow-listed providers |

An Android binary may contain a raw Linux sequence such as `svc #0`. Executing
it against XNU is unsafe because the syscall ABI and numbers are unrelated.
Install-time inspection scans executable code and dependency provenance. The
first native backend rejects raw syscall binaries with a precise diagnostic or
selects the Linux VM fallback. Rewriting raw syscall instructions is a later
experiment, not the compatibility foundation. Anti-cheat, DRM, self-modifying
code, and kernel-specific ioctl use remain VM-fallback candidates.

## Capability tiers

The presence of `.so` does not define a high tier. Most ordinary Android apps
include native analytics, database, compression, crypto, image, or vendor SDKs.
Tier 1 must support those libraries.

| Tier | Required capability |
|---|---|
| 0 — bootstrap | framework/ART bring-up probes; Java-only is acceptable only here |
| 1 — mainstream apps | DEX plus ordinary ARM64 JNI `.so`; ELF relocation, Bionic file/memory/thread/TLS/socket basics, libc++, no raw syscall requirement |
| 2 — native-heavy apps | broader pthread/futex/epoll, SQLite/crypto/media CPU paths, complex `dlopen`, multiple native SDKs |
| 3 — games | EGL/GLES/Vulkan translation, native audio, frame pacing, controllers, high-throughput shared surfaces |
| 4 — kernel-coupled | direct syscalls, anti-cheat, DRM, proprietary drivers and kernel ioctls; VM fallback or explicit unsupported result |

An APK classifier records ABI, `DT_NEEDED`, imports, TLS, relocations, executable
memory requirements, raw `svc` instructions, graphics APIs, and known restricted
SDKs. Backend selection is capability-based: native Darwin when the required
surface is implemented, Linux VM fallback otherwise.

## Current state

Completed foundations include the Darwin ARM64 ART interpreter/runtime,
Android framework boot, real `Activity`/`PhoneWindow`/`DecorView`, Android 16
GraphicsJNI/HWUI/Skia software rendering, IOSurface/Metal presentation, ICU,
Minikin/HarfBuzz/FreeType text foundations, resource JNI, AndroidRuntime
ownership, and a complete registered `libcore.io.Linux` method table with an
incrementally implemented Darwin backend.

The current vertical slice is replacing partial platform owners required by a
real `android.widget.Button`: Android constants, filesystem, NIO mapping,
system-font configuration, and framework resources. Native application ELF is
not loaded yet.

## Ordered implementation after the Button gate

1. Implement the prefix mount table and path resolver as an independently
   tested Rust library with a stable C ABI.
2. Route `libcore.io.Linux` file/path methods through it; populate a real
   `/system`, `/product`, `/apex`, and package-private `/data` prefix.
3. Add an inspection-only ARM64 ELF parser and APK native-capability report.
4. Map a trivial Android NDK ELF library, apply relocations, run constructors,
   and call `JNI_OnLoad` without Linux or a VM.
5. Provide the first virtual DSOs: `libdl`, `liblog`, and a narrow but coherent
   Bionic `libc` file/memory/string surface.
6. Execute a JNI library that opens an Android-prefix file, starts a pthread,
   allocates TLS, and returns a value to interpreted Java.
7. Expand the Bionic facade by real application import manifests, preserving a
   strict unsupported-symbol report instead of adding per-symbol success stubs.
8. Add the VM fallback selector for raw-syscall and kernel-coupled binaries.

This order makes Tier 1 include realistic third-party native libraries before
the graphics stack expands to games.
