//! Immutable source and patch manifest for the staged ART runtime shadow.
//!
//! Keeping this list separate from filesystem staging means changing the
//! compiler orchestration cannot silently change which upstream files are
//! copied or patched.

pub(super) const PATCHED_RUNTIME_SOURCES: &[&str] = &[
    "runtime.cc",
    "signal_set.h",
    "class_linker.cc",
    "class_linker.h",
    "mirror/object_reference.h",
    "mirror/string-inl.h",
    "gc/heap.cc",
    "gc/space/malloc_space.cc",
    "gc/space/space.cc",
    "entrypoints/quick/quick_alloc_entrypoints.cc",
    "entrypoints/quick/callee_save_frame.h",
    "entrypoints/quick/quick_trampoline_entrypoints.cc",
    "arch/arm64/jni_frame_arm64.h",
    "runtime_common.cc",
    "runtime.h",
    "thread.cc",
    "thread.h",
    "thread_list.cc",
    "gc/collector/garbage_collector.cc",
    "gc/collector/mark_compact.cc",
    "oat/oat_file.cc",
    "exec_utils.cc",
    "signal_catcher.cc",
    "nterp_helpers.cc",
    "interpreter/mterp/nterp.cc",
];

pub(super) const PATCHED_RUNTIME_PATCHES: &[&str] = &[
    "patches/art/0006-darwin-standard-signal-set.patch",
    "patches/art/0007-darwin-thread-cpu-time.patch",
    "patches/art/0008-darwin-nonfutex-suspend-barrier.patch",
    "patches/art/0009-darwin-locksupport-park.patch",
    "patches/art/0010-darwin-host-gc-release-policy.patch",
    "patches/art/0011-darwin-disable-userfaultfd-mark-compact.patch",
    "patches/art/0013-darwin-stat-mtime.patch",
    "patches/art/0014-darwin-exec-pidfd-fallback.patch",
    "patches/art/0017-darwin-disable-nterp.patch",
    "patches/art/0019-darwin-disable-nterp-catch-entry.patch",
    "patches/art/0022-darwin-base-relative-heap-references.patch",
    "patches/art/0023-darwin-enable-quick-allocation-entrypoints.patch",
    "patches/art/0024-darwin-arm64-ucontext-dump.patch",
    "patches/art/0025-darwin-morecore-diagnostics.patch",
    "patches/art/0027-darwin-string-abi-overlay.patch",
    "patches/art/0028-darwin-minimal-runtime-start.patch",
    "patches/art/0029-darwin-arm64-native-stack-pcs.patch",
    "patches/art/0030-darwin-large-object-bitmap-window.patch",
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_contains_the_runtime_shadow_and_patch_contract() {
        assert!(PATCHED_RUNTIME_SOURCES.contains(&"runtime.cc"));
        assert!(PATCHED_RUNTIME_SOURCES.contains(&"runtime.h"));
        assert!(
            PATCHED_RUNTIME_PATCHES
                .contains(&"patches/art/0030-darwin-large-object-bitmap-window.patch")
        );
    }
}
