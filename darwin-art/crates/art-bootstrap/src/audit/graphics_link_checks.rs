use std::fs;
use std::path::Path;
use std::process::Command;

use crate::Result;
use crate::support::command_output;

pub(super) fn validate_graphics_runtime_link(
    root: &Path,
    runtime_library: &Path,
    link_map: &Path,
) -> Result<()> {
    let global_symbols = command_output(Command::new("nm").args(["-gU"]).arg(runtime_library))?;
    for required in [
        "_darwin_art_run_process",
        "_darwin_art_shutdown_process",
        "_darwin_art_dispatch_pointer",
        "_darwin_art_pump_framework_frame",
        "_darwin_art_graphics_session_create",
        "_darwin_art_graphics_session_close",
        "_darwin_art_graphics_session_destroy",
        "_darwin_art_graphics_session_dispatch_pointer",
        "_darwin_art_graphics_session_dispatch_pointer_v2",
        "_darwin_art_graphics_session_dispatch_key_v1",
        "_darwin_art_graphics_session_pump_frame",
        "_darwin_art_surface_create",
        "_darwin_art_surface_resize",
        "_darwin_art_surface_get_size",
        "_darwin_art_surface_update",
        "_darwin_art_surface_map_producer",
        "_darwin_art_surface_unmap_producer",
        "_darwin_art_surface_present",
        "_darwin_art_surface_pump_events",
        "_darwin_art_surface_next_pointer_event",
        "_darwin_art_surface_next_pointer_event_v2",
        "_darwin_art_surface_next_key_event_v1",
        "_darwin_art_surface_destroy",
        "_darwin_art_provider_install_hooks",
        "_darwin_art_provider_clear_hooks",
        "_darwin_art_provider_native_acquire",
        "_darwin_art_provider_native_release",
        "_darwin_art_runtime_native_owner_create",
        "_darwin_art_runtime_native_owner_attach",
        "_darwin_art_runtime_native_owner_lookup",
        "_darwin_art_runtime_native_owner_destroy",
    ] {
        if !global_symbols.contains(required) {
            return Err(format!("real-graphics Runtime lacks required symbol {required}").into());
        }
    }
    let all_symbols = command_output(Command::new("nm").args(["-aC"]).arg(runtime_library))?;
    for registrar in [
        "_init_android_graphics",
        "_register_android_graphics_classes",
        "register_android_content_AssetManager",
        "register_android_content_StringBlock",
        "register_android_content_XmlBlock",
        "register_android_content_res_ApkAssets",
        "register_com_android_internal_util_VirtualRefBasePtr",
        "register_android_util_Log",
        "darwin_art_android_runtime_install",
        "darwin_art_android_runtime_uninstall",
        "darwin_art::libcore_darwin::RegisterLinuxNatives",
        "register_android_system_OsConstants",
        "register_java_io_UnixFileSystem",
        "register_java_io_FileInputStream",
        "register_java_io_FileDescriptor",
        "register_java_lang_System",
        "register_java_sun_nio_fs_UnixNativeDispatcher",
        "register_sun_nio_ch_IOUtil",
        "register_sun_nio_ch_FileChannelImpl",
        "register_sun_nio_ch_FileDispatcherImpl",
        "register_sun_nio_ch_NativeThread",
        "darwin_art_restore_sun_nio_ch_NativeThread_signal",
        "register_libcore_io_Memory",
        "DarwinArtLibcoreJniConstants::GetPrimitiveByteArrayClass",
        "JVM_GetLastErrorString",
        "register_libcore_io_AsynchronousCloseMonitor",
        "async_close_monitor_signal_blocked_threads",
        "JniConstants_FileDescriptor_descriptor",
        "darwin_art_elf_graph_load",
        "darwin_art_elf_graph_lookup_root",
        "darwin_art_elf_graph_unload",
        "darwin_art_jni_proxy_init",
        "darwin_art_jni_proxy_java_vm",
        "darwin_art_elf_jni_fixture_registration_status",
        "darwin_art_elf_jni_fixture_lifecycle_status",
        "darwin_art_bionic_namespace_bind_builtins",
        "darwin_art_bionic_binary128_conversion_resolve",
        "darwin_art_bionic_strtold",
        "darwin_art_bionic_strtold_l",
        "darwin_art_bionic_wcstold",
        "darwin_art_bionic_syslog_resolve",
        "darwin_art_bionic_syscall_resolve",
        "darwin_art_bionic_stdio_process_install",
        "darwin_art_bionic_stdio_process_uninstall",
        "darwin_art_bionic_formatted_stdio_resolve",
        "darwin_art_bionic_scanf_resolve",
        "darwin_art_bionic_sscanf",
        "darwin_art_bionic_vsscanf",
        "darwin_art_bionic_swprintf_resolve",
        "darwin_art_bionic_swprintf",
        "darwin_art_bionic_ioctl_resolve",
        "darwin_art_bionic_ioctl_activate",
        "darwin_art_bionic_ioctl_deactivate",
        "darwin_art_bionic_sendfile_resolve",
        "darwin_art_bionic_sendfile_activate",
        "darwin_art_bionic_sendfile_deactivate",
        "darwin_art_bionic_sendfile",
        "darwin_art_bionic_strftime_resolve",
        "darwin_art_bionic_strftime_activate",
        "darwin_art_bionic_strftime_deactivate",
        "darwin_art_bionic_wide_stdio_resolve",
        "darwin_art_bionic_fputwc",
        "darwin_art_bionic_getwc",
        "darwin_art_bionic_ungetwc",
        "darwin_art_bionic_wide_float_resolve",
        "ElfJniOnLoadTrampoline",
        "CreateRegularTrampolines",
        "TrampolineEntryMask",
        "IsTrampolineEntry",
        "TrampolineLiveCount",
    ] {
        if !all_symbols.contains(registrar) {
            return Err(format!("real-graphics Runtime lacks registrar symbol {registrar}").into());
        }
    }
    for forbidden in [
        "DarwinPaint",
        "DarwinRenderNode",
        "DarwinAssetManager",
        "LogIsLoggable",
        "LogPrintln",
        "ProbeCanvas",
        "JniConstants_FileDescriptor_fd",
        "UnixFileSystemInitIds",
        "UnixFileSystemGetBooleanAttributes",
        "FileDescriptorGetAppend",
        "FileDescriptorIsSocket",
    ] {
        if all_symbols.contains(forbidden) {
            return Err(format!("real-graphics Runtime contains fake symbol {forbidden}").into());
        }
    }
    let has_icu78 = all_symbols.lines().any(|line| {
        line.split_whitespace().last().is_some_and(|symbol| {
            !symbol.starts_with("_OUTLINED_FUNCTION_") && symbol.ends_with("_78")
        })
    });
    let has_icu76 = all_symbols.lines().any(|line| {
        line.split_whitespace().last().is_some_and(|symbol| {
            !symbol.starts_with("_OUTLINED_FUNCTION_") && symbol.ends_with("_76")
        })
    });
    if has_icu78 || !has_icu76 {
        return Err("real-graphics Runtime did not retain a pure AOSP ICU76 ABI".into());
    }
    let registration_header = fs::read_to_string(
        root.join("_build/android-graphics-jni/generated/darwin_android_graphics_registration.h"),
    )?;
    if !registration_header.contains("kNativeClassCount = 51") {
        return Err("real-graphics registrar is not the verified 51-class set".into());
    }
    let dependencies = command_output(Command::new("otool").arg("-L").arg(runtime_library))?;
    for forbidden in ["CoreText", "libicu", "libfmt", "libfreetype"] {
        if dependencies.contains(forbidden) {
            return Err(format!("forbidden real-graphics host dependency: {forbidden}").into());
        }
    }
    let link_map_contents = fs::read_to_string(link_map)?;
    if link_map_contents.contains("/opt/homebrew/opt/icu")
        || link_map_contents.contains("/opt/homebrew/Cellar/icu")
        || link_map_contents.contains("/opt/homebrew/opt/fmt")
        || link_map_contents.contains("/opt/homebrew/Cellar/fmt")
    {
        return Err("real-graphics link map consumed a Homebrew ICU/fmt provider".into());
    }
    Ok(())
}
