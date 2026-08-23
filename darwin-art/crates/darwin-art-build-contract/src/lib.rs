//! Small, dependency-free contracts shared by native build orchestration.
//!
//! This crate deliberately contains no filesystem or process code.  It is the
//! stable Rust source of truth for identities that must agree between the
//! canonical ART builder and the incremental Ninja graph generator.

/// Bump when the common runtime/adapters include or command contract changes.
/// A mismatch disables cache promotion until the canonical builder repopulates
/// `_build/runtime-common`.
pub const RUNTIME_CACHE_IDENTITY: &str =
    "darwin-art-runtime-core-cache-v2-common-includes-fmt-adapters";

/// Identity of the generated native Ninja graph. Keeping this beside the
/// runtime cache contract prevents the canonical builder and graph emitter
/// from silently disagreeing about graph format or edge ownership.
pub const NATIVE_GRAPH_VERSION: &str = "darwin-art-native-graph-v14-shared-adapter-manifest";

/// Canonical adapter translation units for the two runtime flavors.  Keeping
/// this list in the dependency-free contract crate prevents the Cargo
/// bootstrap and Ninja graph emitter from drifting when a native boundary is
/// split or added.
pub const HEADLESS_ADAPTER_SOURCES: &[&str] = &[
    "darwin_art_abi_layout.cc",
    "darwin_android_jni_trampoline.cc",
    "darwin_android_elf_image_registry.cc",
    "darwin_provider_owners.cc",
    "darwin_framework_natives.cc",
    "darwin_motion_event_natives.cc",
    "darwin_framework_binder_natives.cc",
    "darwin_framework_system_property_natives.cc",
    "darwin_framework_asset_manager_natives.cc",
    "darwin_framework_render_node_natives.cc",
    "darwin_framework_graphics_runtime.cc",
    "darwin_framework_resource_registration.cc",
    "darwin_framework_system_natives.cc",
    "darwin_framework_animation_natives.cc",
    "darwin_icu_natives.cc",
    "darwin_libcore_natives.cc",
    "darwin_libcore_unicode_natives.cc",
    "darwin_runtime_adapters.cc",
    "darwin_runtime_platform_stubs.cc",
    "darwin_native_bridge_stubs.cc",
    "darwin_jni_shorty.cc",
    "darwin_jni_proxy_lookup.cc",
    "darwin_jni_proxy_registration.cc",
    "darwin_runtime_elf_lifecycle.cc",
    "darwin_runtime_elf_resolver.cc",
    "darwin_runtime_native_loader.cc",
    "darwin_runtime_jni_registration.cc",
    "darwin_sigchain.cc",
    "fault_handler_arm64_darwin.cc",
];

pub const GRAPHICS_ADAPTER_SOURCES: &[&str] = &[
    "darwin_art_abi_layout.cc",
    "darwin_android_jni_trampoline.cc",
    "darwin_android_elf_image_registry.cc",
    "darwin_provider_owners.cc",
    "darwin_framework_natives.cc",
    "darwin_motion_event_natives.cc",
    "darwin_framework_binder_natives.cc",
    "darwin_framework_system_property_natives.cc",
    "darwin_framework_asset_manager_natives.cc",
    "darwin_framework_render_node_natives.cc",
    "darwin_framework_graphics_runtime.cc",
    "darwin_framework_resource_registration.cc",
    "darwin_framework_system_natives.cc",
    "darwin_framework_animation_natives.cc",
    "darwin_icu_jni_bridge.cc",
    "darwin_libcore_natives.cc",
    "darwin_libcore_unicode_natives.cc",
    "darwin_runtime_adapters.cc",
    "darwin_runtime_platform_stubs.cc",
    "darwin_native_bridge_stubs.cc",
    "darwin_jni_shorty.cc",
    "darwin_jni_proxy_lookup.cc",
    "darwin_jni_proxy_registration.cc",
    "darwin_runtime_elf_lifecycle.cc",
    "darwin_runtime_elf_resolver.cc",
    "darwin_runtime_native_loader.cc",
    "darwin_runtime_jni_registration.cc",
    "darwin_sigchain.cc",
    "fault_handler_arm64_darwin.cc",
];

pub const COMMON_ADAPTER_SOURCES: &[&str] = &[
    "darwin_art_abi_layout.cc",
    "darwin_android_jni_trampoline.cc",
    "darwin_android_elf_image_registry.cc",
    "darwin_provider_owners.cc",
    "darwin_framework_animation_natives.cc",
    "darwin_runtime_adapters.cc",
    "darwin_runtime_platform_stubs.cc",
    "darwin_native_bridge_stubs.cc",
    "darwin_jni_shorty.cc",
    "darwin_jni_proxy_lookup.cc",
    "darwin_jni_proxy_registration.cc",
    "darwin_runtime_elf_lifecycle.cc",
    "darwin_runtime_elf_resolver.cc",
    "darwin_runtime_native_loader.cc",
    "darwin_runtime_jni_registration.cc",
    "darwin_sigchain.cc",
    "fault_handler_arm64_darwin.cc",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimeFlavor {
    Headless,
    Graphics,
}

impl RuntimeFlavor {
    pub const fn real_graphics(self) -> bool {
        matches!(self, Self::Graphics)
    }

    pub const fn output_dir(self) -> &'static str {
        match self {
            Self::Headless => "runtime-bootstrap",
            Self::Graphics => "runtime-graphics-bootstrap",
        }
    }

    pub const fn archive_name(self) -> &'static str {
        match self {
            Self::Headless => "libart-runtime-bootstrap-darwin.a",
            Self::Graphics => "libart-runtime-graphics-bootstrap-darwin.a",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flavor_contracts_are_distinct_and_stable() {
        assert!(!RuntimeFlavor::Headless.real_graphics());
        assert!(RuntimeFlavor::Graphics.real_graphics());
        assert_ne!(
            RuntimeFlavor::Headless.output_dir(),
            RuntimeFlavor::Graphics.output_dir()
        );
        assert_ne!(
            RuntimeFlavor::Headless.archive_name(),
            RuntimeFlavor::Graphics.archive_name()
        );
    }
}
