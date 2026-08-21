use super::*;
use crate::native_build::PendingNativeCompile;

pub(super) fn adapter_jobs(
    staged: &RuntimeBootstrapStaging,
    real_graphics: bool,
    includes: &[&Path],
    runtime_includes: &[&Path],
) -> Vec<PendingNativeCompile> {
    let mut jobs = Vec::new();
    for adapter_source in [
        "darwin_art_abi_layout.cc",
        "darwin_android_jni_trampoline.cc",
        "darwin_android_elf_image_registry.cc",
        "darwin_provider_owners.cc",
        "darwin_framework_natives.cc",
        "darwin_framework_resource_registration.cc",
        "darwin_framework_system_natives.cc",
        "darwin_framework_animation_natives.cc",
        "darwin_icu_natives.cc",
        "darwin_icu_jni_bridge.cc",
        "darwin_libcore_natives.cc",
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
    ] {
        if (real_graphics && adapter_source == "darwin_icu_natives.cc")
            || (!real_graphics && adapter_source == "darwin_icu_jni_bridge.cc")
        {
            continue;
        }
        let common = is_common_adapter_source(adapter_source);
        let adapter_object_dir = if common {
            &staged.runtime_core_object_dir
        } else {
            &staged.object_dir
        };
        let adapter_object = adapter_object_dir.join(format!("{adapter_source}.o"));
        let compile_includes = if common { runtime_includes } else { includes };
        let mut adapter_command = if real_graphics && adapter_source == "darwin_libcore_natives.cc"
        {
            let mut libcore_includes = includes.to_vec();
            libcore_includes
                .retain(|path| *path != Path::new("/opt/homebrew/opt/icu4c@78/include"));
            libcore_includes.insert(0, staged.android_icu_i18n.as_path());
            libcore_includes.insert(0, staged.android_icu_common.as_path());
            runtime_bootstrap_cpp_command(&libcore_includes)
        } else {
            runtime_bootstrap_cpp_command(compile_includes)
        };
        if real_graphics
            && matches!(
                adapter_source,
                "darwin_framework_natives.cc" | "darwin_framework_resource_registration.cc"
            )
        {
            adapter_command
                .arg("-DDARWIN_ART_REAL_GRAPHICS")
                .arg("-I")
                .arg(&staged.libcutils_include);
        }
        if real_graphics && adapter_source == "darwin_icu_jni_bridge.cc" {
            adapter_command.arg("-I").arg(staged.root.join("include"));
        }
        if real_graphics && adapter_source == "darwin_libcore_natives.cc" {
            adapter_command.arg("-DDARWIN_ART_FULL_LIBCORE_LINUX");
        }
        if matches!(
            adapter_source,
            "darwin_runtime_adapters.cc"
                | "darwin_runtime_elf_lifecycle.cc"
                | "darwin_runtime_elf_resolver.cc"
                | "darwin_runtime_native_loader.cc"
                | "darwin_runtime_jni_registration.cc"
                | "darwin_provider_owners.cc"
                | "darwin_jni_proxy_lookup.cc"
                | "darwin_jni_proxy_registration.cc"
        ) {
            for include in [
                "tools/bionic-provider-namespace/include",
                "tools/bionic-dso-lifecycle-facade/include",
                "tools/bionic-fs-facade/include",
                "tools/bionic-dns-facade/include",
                "tools/bionic-socket-broker-adapter/include",
                "tools/bionic-sendfile-facade/include",
                "tools/bionic-stdio-facade/include",
                "tools/bionic-ioctl-facade/include",
                "tools/bionic-strftime-facade/include",
            ] {
                adapter_command.arg("-I").arg(staged.root.join(include));
            }
        }
        if adapter_source == "darwin_android_elf_image_registry.cc" {
            adapter_command.arg("-I").arg(
                staged
                    .root
                    .join("tools/android-dl-iterate-phdr-provider/include"),
            );
        }
        adapter_command
            .arg("-idirafter")
            .arg(&staged.ndk_arch_include)
            .arg("-idirafter")
            .arg(&staged.ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(staged.root.join("compat").join(adapter_source))
            .arg("-o")
            .arg(&adapter_object);
        jobs.push(PendingNativeCompile {
            command: adapter_command,
            object: adapter_object,
        });
    }
    jobs
}

fn is_common_adapter_source(source: &str) -> bool {
    matches!(
        source,
        "darwin_art_abi_layout.cc"
            | "darwin_android_jni_trampoline.cc"
            | "darwin_android_elf_image_registry.cc"
            | "darwin_provider_owners.cc"
            | "darwin_framework_animation_natives.cc"
            | "darwin_runtime_adapters.cc"
            | "darwin_runtime_platform_stubs.cc"
            | "darwin_native_bridge_stubs.cc"
            | "darwin_jni_shorty.cc"
            | "darwin_jni_proxy_lookup.cc"
            | "darwin_jni_proxy_registration.cc"
            | "darwin_runtime_elf_lifecycle.cc"
            | "darwin_runtime_elf_resolver.cc"
            | "darwin_runtime_native_loader.cc"
            | "darwin_runtime_jni_registration.cc"
            | "darwin_sigchain.cc"
            | "fault_handler_arm64_darwin.cc"
    )
}
