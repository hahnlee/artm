use super::*;

/// Build the source-pinned native closure consumed by the graphics link audit.
///
/// This phase is intentionally independent from probe compilation and final
/// symbol verification. The full audit invokes it, while the fast inner-loop
/// audit can reuse the already-materialized archives.
pub(super) fn run_graphics_upstream_gates(root: &Path) -> Result<()> {
    build_shell_gate(root, "build-bionic-runtime-provider-closure.sh")?;
    build_shell_gate_with_args(
        root,
        "audit-android16-graphics-closure.sh",
        &["--art-runtime"],
    )?;
    for script in [
        "build-android16-android-runtime-host.sh",
        "build-android16-libcore-darwin-linux.sh",
        "build-android16-os-constants-darwin.sh",
        "build-android16-unix-filesystem-darwin.sh",
        "build-android16-openjdkjvm-darwin.sh",
        "build-android16-file-input-stream-darwin.sh",
        "build-android16-file-descriptor-darwin.sh",
        "build-android16-system-natives-darwin.sh",
        "build-android16-unix-native-dispatcher-darwin.sh",
        "build-android16-openjdk-nio-mapping.sh",
        "build-android16-libcore-memory-darwin.sh",
        "build-android16-android-util-log.sh",
        "build-android16-virtual-ref-base-ptr.sh",
    ] {
        build_shell_gate(root, script)?;
    }
    Ok(())
}
