use super::common::{build_runtime_native_owner, require_file};
use super::graphics_core_probes::{
    CoreProbeObjects, compile_core_probe_objects, core_probe_includes,
};
use super::graphics_link_checks::validate_graphics_runtime_link;
use super::graphics_link_inputs::GraphicsRuntimeInputs;
use super::graphics_phases::run_graphics_upstream_gates;
use super::graphics_surface::compile_surface_objects;
use super::*;
pub(crate) fn audit_runtime_graphics_link(root: &Path) -> Result<()> {
    audit_runtime_graphics_link_mode(root, true, false)
}

/// Validate/link against already-built graphics inputs without rerunning the
/// long upstream closure scripts. This is the inner-loop target after a
/// narrow TU change; the full command remains the release/CI gate.
pub(crate) fn audit_runtime_graphics_link_fast(root: &Path) -> Result<()> {
    audit_runtime_graphics_link_mode(root, false, false)
}

/// Reuse successful source-pinned foundation artifacts while still running
/// the graphics closure audit and final dylib/symbol checks. This is the
/// development path between the strict full gate and the artifact-only fast
/// link check.
pub(crate) fn audit_runtime_graphics_link_incremental(root: &Path) -> Result<()> {
    audit_runtime_graphics_link_mode(root, true, true)
}

pub(crate) fn audit_runtime_graphics_link_mode(
    root: &Path,
    run_upstream_gates: bool,
    incremental: bool,
) -> Result<()> {
    run_command(
        Command::new("bash")
            .arg(root.join("tools/android-managed-native-load/audit.sh"))
            .arg("--build-only"),
    )?;
    if run_upstream_gates {
        run_graphics_upstream_gates(root, incremental)?;
    }
    let runtime_native_owner_archive = build_runtime_native_owner(root)?;

    let runtime = root.join("_aosp/art/runtime");
    let build_paths = BuildPaths::from_root(root);
    let build_dir = build_paths.native_output("runtime-graphics-link-probe");
    let object = build_dir.join("darwin_art_runtime.cc.o");
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
    let runtime_library = build_dir.join("libdarwin_art_runtime_graphics.dylib");
    let GraphicsRuntimeInputs {
        graphics_closure,
        bootstrap,
        icu_jni_archive,
        libcore_linux_archive,
        os_constants_archive,
        unix_filesystem_archive,
        openjdkjvm_archive,
        managed_load_archive,
        file_input_stream_archive,
        file_descriptor_archive,
        system_natives_archive,
        unix_native_dispatcher_archive,
        openjdk_nio_mapping_archive,
        openjdk_nio_support_archive,
        libcore_memory_archive,
        libcore_jni_constants_archive,
        asynchronous_close_registrar,
        asynchronous_close_backend,
        resource_jni_archive,
        android_util_log_archive,
        virtual_ref_base_ptr_archive,
        android_runtime_host,
    } = GraphicsRuntimeInputs::load(root, &build_paths)?;

    fs::create_dir_all(&build_dir)?;
    let includes = core_probe_includes(root, &build_paths, &runtime);
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    let hwui_object = if let Some(path) = env::var_os("DARWIN_ART_NATIVE_HWUI_OBJECT") {
        PathBuf::from(path)
    } else {
        compile_runtime_hwui_probe(
            root,
            &build_dir,
            &include_refs,
            &ndk_include,
            &ndk_arch_include,
        )?
    };
    // Keep the process probe flavor-neutral. The linked compatibility object is
    // the sole owner of DARWIN_ART_REAL_GRAPHICS and chooses the real backend.
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let probe_cache = build_dir.join("runtime-graphics-probe-hashes.cache");
    let core_build_dir = build_paths.native_output("native-probes/core");
    fs::create_dir_all(&core_build_dir)?;
    let core_probe_cache = core_build_dir.join("core-probe-hashes.cache");
    let CoreProbeObjects {
        elf: elf_probe_object,
        abi: abi_probe_object,
        process_state: process_state_object,
        process_options: process_options_object,
        shutdown: shutdown_probe_object,
        frame: frame_probe_object,
    } = compile_core_probe_objects(
        root,
        &core_build_dir,
        &include_refs,
        &core_probe_cache,
        &compiler_identity,
    )?;
    let graphics_probe_object = if let Some(path) = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        let graphics_probe_object = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_OBJECT")
            .map(PathBuf::from)
            .unwrap_or_else(|| build_dir.join("darwin_art_runtime_graphics_probe.cc.o"));
        if let Some(parent) = graphics_probe_object.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut graphics_probe_command = runtime_cpp_command(&include_refs);
        graphics_probe_command
            .args(["-include", "mirror/object_reference.h"])
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-DLOG_TAG=\"DarwinArtHWUI\"")
            .arg("-DSK_BUILD_FOR_ANDROID_FRAMEWORK")
            .arg("-include")
            .arg("log/log_main.h")
            .arg("-DDARWIN_ART_REAL_GRAPHICS")
            .arg("-DDARWIN_ART_HWUI_GPU")
            .arg("-DDARWIN_ART_AOSP_COMPAT_LSEEK64")
            .arg("-c")
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui/hwui"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/androidfw/include"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/include"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/native/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/incremental_delivery/incfs/util/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/core/libutils/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/core/libsystem/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/core/libcutils/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/core/libutils/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/incremental_delivery/incfs/util/include"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/native/libs/ui/include"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/native/libs/ui/include_types"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/native/libs/nativewindow/include"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/native/libs/arect/include"))
            .arg("-I")
            .arg(root.join("_aosp/system/logging/liblog/include"))
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
        graphics_probe_object
    };
    let graphics_phase_object = compile_runtime_graphics_phase(root, &build_dir, &include_refs)?;
    let graphics_gpu_object = if let Some(path) =
        env::var_os("DARWIN_ART_NATIVE_GRAPHICS_GPU_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        compile_runtime_graphics_gpu_probe(
            root,
            &build_dir,
            &include_refs,
            &ndk_include,
            &ndk_arch_include,
        )?
    };
    let graphics_input_object =
        compile_runtime_graphics_input_probe(root, &build_dir, &include_refs)?;
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
    let graphics_session_object_real = compile_runtime_graphics_session_probe(
        root,
        &build_dir,
        &include_refs,
        &ndk_include,
        &ndk_arch_include,
    )?;
    let jni_acceptance_object =
        compile_runtime_jni_acceptance_probe(root, &build_dir, &include_refs)?;
    let mut probe_command = runtime_cpp_command(&include_refs);
    probe_command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(ndk_arch_include)
        .arg("-idirafter")
        .arg(ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-DDARWIN_ART_REAL_GRAPHICS")
        .arg("-DDARWIN_ART_HWUI_GPU")
        .arg("-DDARWIN_ART_AOSP_COMPAT_LSEEK64")
        .arg("-DLOG_TAG=\"DarwinArtHWUI\"")
        .arg("-DSK_BUILD_FOR_ANDROID_FRAMEWORK")
        .arg("-include")
        .arg("log/log_main.h")
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
        .arg(root.join("_aosp/system/logging/liblog/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libcutils/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libutils/include"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/hwui"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/base/libs/androidfw/include"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/native/libs/ui/include"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/native/libs/ui/include_types"))
        .arg("-I")
        .arg(root.join("_aosp/frameworks/minikin/include"))
        .arg("-I")
        .arg(root.join("_aosp/external/googletest/googletest/include"))
        .arg("-I")
        .arg(root.join("_aosp/external/harfbuzz_ng/src"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libutils/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/incremental_delivery/incfs/util/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libsystem/include"))
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
    let registration_object = build_dir.join("darwin_art_runtime_registration_phase.cc.o");
    let mut registration_command = runtime_cpp_command(&include_refs);
    registration_command
        .arg("-DDARWIN_ART_REAL_GRAPHICS")
        .arg("-DDARWIN_ART_HWUI_GPU")
        .arg("-c")
        .arg(root.join("probes/runtime_registration_phase.cc"))
        .arg("-o")
        .arg(&registration_object);
    let _ = compile_cached_probe_tu(
        &mut registration_command,
        &registration_object,
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
    let _compiled_app_resources =
        compile_runtime_app_resources_probe(root, &build_dir, &include_refs)?;
    let _compiled_app_activity =
        compile_runtime_app_activity_probe(root, &build_dir, &include_refs)?;
    let app_presentation_object = if let Some(path) =
        env::var_os("DARWIN_ART_NATIVE_APP_PRESENTATION_OBJECT")
        && Path::new(&path).is_file()
    {
        PathBuf::from(path)
    } else {
        compile_runtime_app_presentation_probe(root, &build_dir, &include_refs)?
    };
    let app_resources_object = app_resources_object_path(&build_dir);
    require_file(
        &app_resources_object,
        "graphics app resources object is missing",
    )?;
    let app_activity_object = app_activity_object_path(&build_dir);
    require_file(
        &app_activity_object,
        "graphics app activity object is missing",
    )?;
    let (surface_object, surface_gpu_object) =
        compile_surface_objects(root, &build_dir, &probe_cache, &compiler_identity)?;

    let link_map = build_dir.join("runtime-graphics-link.map");
    let mut linker = Command::new("clang++");
    linker
        .arg("-dynamiclib")
        .arg("-Wl,-install_name,@rpath/libdarwin_art_runtime_graphics.dylib")
        .arg("-Wl,-exported_symbol,_darwin_art_run_process")
        .arg("-Wl,-exported_symbol,_darwin_art_shutdown_process")
        .arg("-Wl,-exported_symbol,_darwin_art_dispatch_pointer")
        .arg("-Wl,-exported_symbol,_darwin_art_pump_framework_frame")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_create")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_close")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_destroy")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_dispatch_pointer")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_dispatch_pointer_v2")
        .arg("-Wl,-exported_symbol,_darwin_art_graphics_session_pump_frame")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_create")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_update")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_map_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_unmap_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_present")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_pump_events")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_next_pointer_event")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_next_pointer_event_v2")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_destroy")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_active_gpu")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_gpu_active_canvas")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_install_hooks")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_clear_hooks")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_native_acquire")
        .arg("-Wl,-exported_symbol,_darwin_art_provider_native_release")
        .arg("-Wl,-exported_symbol,_darwin_art_runtime_native_owner_create")
        .arg("-Wl,-exported_symbol,_darwin_art_runtime_native_owner_attach")
        .arg("-Wl,-exported_symbol,_darwin_art_runtime_native_owner_lookup")
        .arg("-Wl,-exported_symbol,_darwin_art_runtime_native_owner_destroy")
        .arg("-Wl,-dead_strip")
        .arg(format!("-Wl,-map,{}", link_map.display()))
        .arg(&object)
        .arg(&registration_object)
        .arg(&elf_probe_object)
        .arg(&abi_probe_object)
        .arg(&process_state_object)
        .arg(&process_options_object)
        .arg(&shutdown_probe_object)
        .arg(&frame_probe_object)
        .arg(&graphics_probe_object)
        .arg(&graphics_gpu_object)
        .arg(&graphics_state_object)
        .arg(&network_loader_object)
        .arg(&context_loader_object)
        .arg(&app_bootstrap_object)
        .arg(&app_resources_object)
        .arg(&app_activity_object)
        .arg(&app_presentation_object)
        .arg(&jni_acceptance_object)
        .arg(&graphics_session_object_real)
        .arg(&graphics_phase_object)
        .arg(&graphics_input_object)
        .arg(&filesystem_object)
        .arg(&network_object)
        .arg(&hwui_object)
        .arg(&surface_object)
        .arg(&surface_gpu_object)
        .arg(root.join("_build/skia-metal-gpu/libskia.a"))
        .arg(root.join("_build/skia-metal-gpu/libskcms.a"))
        // RenderNode/RecordingCanvas are the real HWUI display-list path. Keep
        // these AOSP objects in the same GPU link so RenderNodeDrawable replay
        // cannot silently fall back to the bitmap/CPU bridge.
        .arg(root.join("_build/hwui-static-foundation/libhwui-static-darwin.a"))
        .arg(root.join("_build/android-graphics-jni/libandroid-graphics-jni-darwin.a"))
        // This is the already-audited force/normal composition of all 32
        // graphics archives. Place its fixed definitions before ART's normal
        // archives so the latter extract only additional runtime providers.
        .arg(&graphics_closure)
        .arg(&bootstrap)
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
        .arg(root.join("_build/icu-foundation/libandroidicuinit-darwin.a"))
        .arg(root.join("crates/darwin-art-elf-loader/target/release/libdarwin_art_elf_loader.a"))
        .arg(format!(
            "-Wl,-force_load,{}",
            runtime_native_owner_archive.display()
        ))
        .arg(&system_natives_archive)
        .arg(&file_descriptor_archive)
        .arg(&unix_native_dispatcher_archive)
        .arg(&file_input_stream_archive)
        .arg(&openjdk_nio_mapping_archive)
        .arg(&openjdk_nio_support_archive)
        .arg(&libcore_memory_archive)
        .arg(&libcore_jni_constants_archive)
        .arg(&unix_filesystem_archive)
        // Complete java.lang.Runtime owner must precede openjdkjvm, which
        // supplies its JVM_* support symbols.
        .arg(&managed_load_archive)
        .arg(&openjdkjvm_archive)
        .arg(&os_constants_archive)
        .arg(&android_util_log_archive)
        .arg(&virtual_ref_base_ptr_archive)
        .arg(format!(
            "-Wl,-force_load,{}",
            resource_jni_archive.display()
        ))
        .arg(&android_runtime_host)
        .arg(&libcore_linux_archive)
        .arg(&asynchronous_close_registrar)
        .arg(&asynchronous_close_backend)
        .arg(root.join("_build/interpreter-core/libart-interpreter-darwin.a"))
        .arg(root.join("_build/runtime-arm64/libart-arm64-darwin.a"))
        .arg(root.join("_build/runtime-core/libart-core-darwin.a"))
        .arg(root.join("_build/runtime-platform/libart-platform-darwin.a"))
        .arg(root.join("_build/dex-probe/libdexfile-darwin.a"))
        .arg(
            build_paths
                .native_output("runtime-graphics-bootstrap/objects/artbase_os_linux_aosp_fmt.cc.o"),
        )
        .arg(root.join("_build/foundation/libartbase-darwin.a"))
        .arg(root.join("_build/ziparchive-incfs/libziparchive-for-incfs-darwin.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .arg(format!("-Wl,-force_load,{}", icu_jni_archive.display()))
        // ld64 does not rescan archives that appeared before the force-loaded
        // resource/ICU roots. Re-supply their complete Android.bp providers in
        // dependency order; normal archive extraction prevents duplicates with
        // the already-composed graphics closure.
        .arg(root.join("_build/androidfw-foundation/libandroidfw-darwin.a"))
        .arg(root.join("_build/ui-types-foundation/libui-types.a"))
        .arg(root.join("_build/nativehelper-device-foundation/libnativehelper-device-darwin.a"))
        .arg(root.join("_build/graphics-foundations/libutils-darwin.a"))
        .arg(root.join("_build/graphics-foundations/libutils-binder-darwin.a"))
        .arg(root.join("_build/graphics-foundations/libcutils-darwin.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .arg(root.join("_build/libbase-foundation/libandroid-base-darwin.a"))
        .arg(root.join("_build/ziparchive-incfs/libziparchive-for-incfs-darwin.a"))
        .arg(root.join("_build/foundation/libziparchive-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicui18n-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicuuc-common-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicuuc-stubdata-darwin.a"))
        .arg(root.join("_build/graphics-codecs/libpng-darwin.a"))
        .arg(root.join("_build/graphics-codecs/libz-darwin.a"))
        .args([
            "-L/opt/homebrew/lib",
            "-llz4",
            "-lz",
            "-framework",
            "CoreFoundation",
            "-framework",
            "CoreGraphics",
            "-framework",
            "ImageIO",
            "-framework",
            "Foundation",
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
    let link_stamp = build_dir.join("runtime-graphics-link.fingerprint");
    let output = link_with_cache(&mut linker, &runtime_library, &link_stamp)?;
    if !output.status.success() {
        let stderr = String::from_utf8(output.stderr)?;
        fs::write(build_dir.join("link.err"), &stderr)?;
        return Err(format!("real-graphics Runtime link failed: {description}\n{stderr}").into());
    }

    validate_graphics_runtime_link(root, &runtime_library, &link_map)?;
    build_runtime_host(root)?;
    println!(
        "audit-runtime-graphics-link: closure complete registrar=51 fake-symbols=0 host-icu=0 host-fmt=0 CoreText=0"
    );
    Ok(())
}
