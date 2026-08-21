use super::common::require_file;
use super::*;

pub(crate) fn audit_runtime_link(root: &Path) -> Result<()> {
    const MAX_EXPECTED_UNDEFINED: usize = 365;

    let runtime = root.join("_aosp/art/runtime");
    let build_dir = root.join("_build/runtime-link-probe");
    let object = build_dir.join("darwin_art_runtime.cc.o");
    let surface_object = build_dir.join("darwin_surface_bridge.mm.o");
    // The CPU/runtime link owns only the IOSurface/AppKit core. The Ganesh
    // Metal implementation is compiled and linked exclusively by the
    // graphics runtime below, where the GPU Skia archive is explicit.
    let runtime_library = build_dir.join("libdarwin_art_runtime.dylib");
    fs::create_dir_all(&build_dir)?;
    let filesystem_object = if let Some(path) = env::var_os("DARWIN_ART_NATIVE_FILESYSTEM_OBJECT") {
        PathBuf::from(path)
    } else {
        compile_runtime_filesystem_probe(root, &build_dir)?
    };
    require_file(&filesystem_object, "runtime filesystem object is missing")?;
    let network_object = if let Some(path) = env::var_os("DARWIN_ART_NATIVE_NETWORK_OBJECT") {
        PathBuf::from(path)
    } else {
        compile_runtime_network_probe(root, &build_dir)?
    };
    require_file(&network_object, "runtime network object is missing")?;
    build_shell_gate(root, "build-bionic-runtime-provider-closure.sh")?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-bootstrap/patched-source/runtime"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/cmdline"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/libprofile"),
        root.join("_aosp/art/libnativebridge/include"),
        runtime.clone(),
        runtime.join("base"),
        runtime.join("arch/arm64"),
        root.join("_aosp/art/libartpalette/include"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("_aosp/external/dlmalloc"),
        root.join("tools/bionic-dns-facade/include"),
        root.join("tools/bionic-fs-facade/include"),
        root.join("tools/bionic-ioctl-facade/include"),
        root.join("tools/bionic-socket-broker-adapter/include"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let probe_cache = build_dir.join("runtime-link-probe-hashes.cache");
    let elf_probe_object = build_dir.join("darwin_art_runtime_elf_probe.cc.o");
    let mut elf_probe_command = runtime_cpp_command(&include_refs);
    elf_probe_command
        .arg("-c")
        .arg(root.join("probes/runtime_elf_probe.cc"))
        .arg("-o")
        .arg(&elf_probe_object);
    let _ = compile_cached_probe_tu(
        &mut elf_probe_command,
        &elf_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let abi_probe_object = build_dir.join("darwin_art_runtime_abi_probe.cc.o");
    let mut abi_probe_command = runtime_cpp_command(&include_refs);
    abi_probe_command
        .arg("-c")
        .arg(root.join("probes/runtime_abi_probe.cc"))
        .arg("-o")
        .arg(&abi_probe_object);
    let _ = compile_cached_probe_tu(
        &mut abi_probe_command,
        &abi_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let process_state_object = build_dir.join("darwin_art_runtime_process_state.cc.o");
    let mut process_state_command = runtime_cpp_command(&include_refs);
    process_state_command
        .arg("-c")
        .arg(root.join("probes/runtime_process_state.cc"))
        .arg("-o")
        .arg(&process_state_object);
    let _ = compile_cached_probe_tu(
        &mut process_state_command,
        &process_state_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let process_options_object = build_dir.join("darwin_art_runtime_process_options.cc.o");
    let mut process_options_command = runtime_cpp_command(&include_refs);
    process_options_command
        .arg("-c")
        .arg(root.join("probes/runtime_process_options.cc"))
        .arg("-o")
        .arg(&process_options_object);
    let _ = compile_cached_probe_tu(
        &mut process_options_command,
        &process_options_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let shutdown_probe_object = build_dir.join("darwin_art_runtime_shutdown_probe.cc.o");
    let mut shutdown_probe_command = runtime_cpp_command(&include_refs);
    shutdown_probe_command
        .arg("-c")
        .arg(root.join("probes/runtime_shutdown_probe.cc"))
        .arg("-o")
        .arg(&shutdown_probe_object);
    let _ = compile_cached_probe_tu(
        &mut shutdown_probe_command,
        &shutdown_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let frame_probe_object = build_dir.join("darwin_art_runtime_frame_probe.cc.o");
    let mut frame_probe_command = runtime_cpp_command(&include_refs);
    frame_probe_command
        .arg("-c")
        .arg(root.join("probes/runtime_frame_probe.cc"))
        .arg("-o")
        .arg(&frame_probe_object);
    let _ = compile_cached_probe_tu(
        &mut frame_probe_command,
        &frame_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let graphics_probe_object = build_dir.join("darwin_art_runtime_graphics_probe.cc.o");
    let mut graphics_probe_command = runtime_cpp_command(&include_refs);
    graphics_probe_command
        .arg("-c")
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/effects"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/private"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/android"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/utils"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/codec"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/minikin/include"))
        .arg("-I")
        .arg(root.join("_aosp/external/harfbuzz_ng/src"))
        .arg("-I")
        .arg(root.join("_aosp/external/googletest/googletest/include"))
        .arg(root.join("probes/runtime_graphics_probe.cc"))
        .arg("-o")
        .arg(&graphics_probe_object);
    let _ = compile_cached_probe_tu(
        &mut graphics_probe_command,
        &graphics_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let graphics_phase_object = compile_runtime_graphics_phase(root, &build_dir, &include_refs)?;
    let graphics_input_object =
        compile_runtime_graphics_input_probe(root, &build_dir, &include_refs)?;
    let graphics_cpu_stubs_object = build_dir.join("darwin_art_runtime_graphics_cpu_stubs.cc.o");
    let mut graphics_cpu_stubs_command = runtime_cpp_command(&include_refs);
    graphics_cpu_stubs_command
        .arg("-c")
        .arg(root.join("probes/runtime_graphics_cpu_stubs.cc"))
        .arg("-o")
        .arg(&graphics_cpu_stubs_object);
    let _ = compile_cached_probe_tu(
        &mut graphics_cpu_stubs_command,
        &graphics_cpu_stubs_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let graphics_state_object = if let Some(path) =
        env::var_os("DARWIN_ART_NATIVE_GRAPHICS_STATE_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        compile_runtime_graphics_state_probe(
            root,
            &build_dir,
            &include_refs,
            &ndk_include,
            &ndk_arch_include,
        )?
    };
    let jni_acceptance_object =
        compile_runtime_jni_acceptance_probe(root, &build_dir, &include_refs)?;
    let mut probe_command = runtime_cpp_command(&include_refs);
    probe_command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-c")
        .arg(root.join("probes/runtime_entry_probe.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(
        &mut probe_command,
        &object,
        &probe_cache,
        &compiler_identity,
    )?;
    let network_loader_object =
        compile_runtime_network_loader_probe(root, &build_dir, &include_refs)?;
    let context_loader_object =
        compile_runtime_context_loader_probe(root, &build_dir, &include_refs)?;
    let app_bootstrap_object = if let Some(path) =
        env::var_os("DARWIN_ART_NATIVE_APP_BOOTSTRAP_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        compile_runtime_app_bootstrap_probe(root, &build_dir, &include_refs)?
    };
    let app_presentation_object = if let Some(path) =
        env::var_os("DARWIN_ART_NATIVE_APP_PRESENTATION_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        compile_runtime_app_presentation_probe(root, &build_dir, &include_refs)?
    };
    let mut surface_command = Command::new("clang++");
    surface_command
        .args(["-std=c++20", "-fobjc-arc", "-Wall", "-Wextra", "-c"])
        .arg(root.join("compat/darwin_surface_bridge.mm"))
        .arg("-I")
        .arg(root.join("compat"))
        .arg("-I")
        .arg(root.join("include"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/effects"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/utils"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/private"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/android"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/codec"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/effects"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia"))
        .arg("-I")
        .arg(root.join("_aosp/system/logging/liblog/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libcutils/include"))
        .arg("-DSK_BUILD_FOR_ANDROID_FRAMEWORK")
        .arg("-DSK_USER_CONFIG_HEADER=\"include/config/SkUserConfigManual.h\"")
        .arg("-o")
        .arg(&surface_object);
    let _ = compile_cached_probe_tu(
        &mut surface_command,
        &surface_object,
        &probe_cache,
        &compiler_identity,
    )?;

    let mut linker = Command::new("clang++");
    linker
        .arg("-dynamiclib")
        .arg("-Wl,-install_name,@rpath/libdarwin_art_runtime.dylib")
        .arg("-Wl,-exported_symbol,_darwin_art_run_process")
        .arg("-Wl,-exported_symbol,_darwin_art_shutdown_process")
        .arg("-Wl,-exported_symbol,_darwin_art_dispatch_pointer")
        .arg("-Wl,-exported_symbol,_darwin_art_pump_framework_frame")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_create")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_update")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_map_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_unmap_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_present")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_pump_events")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_next_pointer_event")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_destroy")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_active_gpu")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_install_hooks")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_clear_hooks")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_native_acquire")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_native_release")
        .arg("-Wl,-dead_strip")
        .arg(&object)
        .arg(&elf_probe_object)
        .arg(&abi_probe_object)
        .arg(&process_state_object)
        .arg(&process_options_object)
        .arg(&shutdown_probe_object)
        .arg(&frame_probe_object)
        .arg(&graphics_probe_object)
        .arg(&graphics_state_object)
        .arg(&graphics_cpu_stubs_object)
        .arg(&network_loader_object)
        .arg(&context_loader_object)
        .arg(&app_bootstrap_object)
        .arg(&app_presentation_object)
        .arg(&jni_acceptance_object)
        .arg(&graphics_phase_object)
        .arg(&graphics_input_object)
        .arg(&filesystem_object)
        .arg(&network_object)
        .arg(&surface_object)
        .arg(root.join("_build/runtime-bootstrap/libart-runtime-bootstrap-darwin.a"))
        .arg(format!(
            "-Wl,-force_load,{}",
            root.join(
                "_build/bionic-runtime-provider-closure/libdarwin-art-bionic-binary128-conversion.a"
            )
            .display()
        ))
        .arg(
            root.join(
                "_build/bionic-runtime-provider-closure/libdarwin-art-bionic-native-providers.a",
            ),
        )
        .arg(
            root.join(
                "_build/bionic-runtime-provider-closure/libdarwin-art-bionic-float-conversion.a",
            ),
        )
        .arg(
            root.join(
                "_build/bionic-runtime-provider-closure/libdarwin-art-bionic-rust-providers.a",
            ),
        )
        .arg(format!(
            "-Wl,-force_load,{}",
            root.join("_build/icu-foundation/libandroidicuinit-darwin.a")
                .display()
        ))
        .arg(root.join("_build/icu-foundation/libicuuc-common-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicuuc-stubdata-darwin.a"))
        .arg(root.join("crates/darwin-art-elf-loader/target/release/libdarwin_art_elf_loader.a"))
        .arg(root.join("_build/interpreter-core/libart-interpreter-darwin.a"))
        .arg(root.join("_build/runtime-arm64/libart-arm64-darwin.a"))
        .arg(root.join("_build/runtime-core/libart-core-darwin.a"))
        .arg(root.join("_build/runtime-platform/libart-platform-darwin.a"))
        .arg(root.join("_build/dex-probe/libdexfile-darwin.a"))
        .arg(root.join("_build/foundation/libartbase-darwin.a"))
        .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
        .arg(root.join("_build/foundation/libziparchive-darwin.a"))
        .arg(root.join("_build/nativehelper-foundation/libnativehelper_jvm.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .args([
            "-L/opt/homebrew/lib",
            "-L/opt/homebrew/opt/icu4c@78/lib",
            "-lfmt",
            "-llz4",
            "-licui18n",
            "-licuuc",
            "-licudata",
            "-lz",
            "-framework",
            "AppKit",
            "-framework",
            "IOSurface",
            "-framework",
            "Metal",
            "-framework",
            "QuartzCore",
            "-framework",
            "Security",
            "-o",
        ])
        .arg(&runtime_library);
    let description = describe_command(&linker);
    let link_stamp = build_dir.join("runtime-link.fingerprint");
    let output = link_with_cache(&mut linker, &runtime_library, &link_stamp)?;
    if output.status.success() {
        let symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_library))?;
        for required in [
            "_darwin_art_run_process",
            "_darwin_art_shutdown_process",
            "_darwin_art_dispatch_pointer",
            "_darwin_art_surface_create",
            "_darwin_art_surface_update",
            "_darwin_art_surface_map_producer",
            "_darwin_art_surface_unmap_producer",
            "_darwin_art_surface_present",
            "_darwin_art_surface_pump_events",
            "_darwin_art_surface_next_pointer_event",
            "_darwin_art_surface_destroy",
            "_darwin_art_provider_install_hooks",
            "_darwin_art_provider_clear_hooks",
            "_darwin_art_provider_native_acquire",
            "_darwin_art_provider_native_release",
        ] {
            if !symbols.contains(required) {
                return Err(format!(
                    "runtime C ABI library does not export required symbol {required}"
                )
                .into());
            }
        }
        let all_symbols = command_output(Command::new("nm").args(["-aC"]).arg(&runtime_library))?;
        for required in [
            "darwin_art_elf_graph_load",
            "darwin_art_elf_graph_lookup_root",
            "darwin_art_elf_graph_unload",
            "darwin_art_jni_proxy_init",
            "darwin_art_jni_proxy_java_vm",
            "darwin_art_elf_jni_fixture_registration_status",
            "darwin_art_elf_jni_fixture_lifecycle_status",
            "darwin_art_elf_jni_fixture_namespace_lifecycle_status",
            "darwin_art_bionic_namespace_bind_builtins",
            "darwin_art_bionic_binary128_conversion_resolve",
            "darwin_art_bionic_strtold",
            "darwin_art_bionic_strtold_l",
            "darwin_art_bionic_wcstold",
            "darwin_art_bionic_syslog_resolve",
            "darwin_art_bionic_syscall_resolve",
            "darwin_art_bionic_fs_process_install",
            "darwin_art_bionic_fs_process_uninstall",
            "darwin_art_bionic_socket_broker_activate",
            "darwin_art_bionic_socket_broker_deactivate",
            "darwin_art_bionic_socket_broker_is_active",
            "darwin_art_bionic_socket_broker_resolve",
            "darwin_art_bionic_socket_broker_dns_resolve",
            "darwin_art_bionic_dns_reset_for_test",
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
            "darwin_art_bionic_rust_provider_closure_anchor",
            "ElfJniOnLoadTrampoline",
            "CreateRegularTrampolines",
            "TrampolineEntryMask",
            "IsTrampolineEntry",
            "TrampolineLiveCount",
        ] {
            if !all_symbols.contains(required) {
                return Err(
                    format!("runtime ELF JNI bridge lacks required symbol {required}").into(),
                );
            }
        }
        build_runtime_host(root)?;
        println!("audit-runtime-link: C ABI dylib closure complete undefined=0 exports=15");
        return Ok(());
    }

    let stderr = String::from_utf8(output.stderr)?;
    fs::write(build_dir.join("link.err"), &stderr)?;
    if !stderr.contains("Undefined symbols for architecture arm64") {
        return Err(format!("unexpected Runtime link failure: {description}\n{stderr}").into());
    }
    let undefined = stderr
        .lines()
        .filter_map(|line| {
            line.trim_start()
                .strip_prefix('"')?
                .split_once("\", referenced from:")
                .map(|(symbol, _)| symbol.to_owned())
        })
        .collect::<BTreeSet<_>>();
    let symbol_list = undefined.iter().cloned().collect::<Vec<_>>().join("\n") + "\n";
    fs::write(build_dir.join("link.undefined"), symbol_list)?;
    let quick = undefined
        .iter()
        .filter(|symbol| symbol.starts_with("_art_quick_"))
        .count();
    let jni = undefined
        .iter()
        .filter(|symbol| symbol.starts_with("_art_jni_"))
        .count();
    let context = usize::from(undefined.contains("_artContextCopyForLongJump"));
    if quick != 0 || jni != 0 || context != 0 {
        return Err(format!(
            "ARM64 entrypoint link regression: quick={quick} jni={jni} context={context}"
        )
        .into());
    }
    if undefined.len() > MAX_EXPECTED_UNDEFINED {
        return Err(format!(
            "Runtime link closure regressed: undefined={} maximum={MAX_EXPECTED_UNDEFINED}",
            undefined.len()
        )
        .into());
    }
    println!(
        "audit-runtime-link: closure incomplete undefined={} quick=0 jni=0 context=0",
        undefined.len()
    );
    Ok(())
}
