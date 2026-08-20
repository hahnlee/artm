use super::*;

pub(crate) fn build_runtime_host(root: &Path) -> Result<()> {
    let status = Command::new("cargo")
        .args(["build", "-p", "darwin-art-host"])
        .current_dir(root)
        .status()?;
    if !status.success() {
        return Err("failed to build the Rust darwin-art-host launcher".into());
    }
    let executable = root.join("target/debug/darwin-art-host");
    if !executable.is_file() {
        return Err(format!("Rust runtime host is missing: {}", executable.display()).into());
    }
    Ok(())
}

pub(crate) fn probe_runtime_dex(root: &Path, show_window: bool) -> Result<()> {
    probe_runtime_dex_flavor(root, show_window, false, false, false, false, false)
}

pub(crate) fn probe_runtime_elf_jni(root: &Path) -> Result<()> {
    build_elf_jni_dex_probe(root)?;
    probe_runtime_dex_flavor(root, false, false, false, true, false, false)
}

pub(crate) fn probe_runtime_network(root: &Path) -> Result<()> {
    build_network_dex_probe(root)?;
    // The host is intentionally GPU-only.  Network acceptance still drives
    // the same Activity/DecorView path as the other Android flavors, so use
    // the real graphics runtime rather than a CPU/minimal flavor that cannot
    // create an active Metal surface.
    probe_runtime_dex_flavor(root, false, true, false, false, true, false)
}

pub(crate) fn pinned_direct_apk_ndk_bin() -> Result<PathBuf> {
    let sdk_root = env::var_os("ANDROID_SDK_ROOT")
        .map(PathBuf::from)
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .map(|home| home.join("Library/Android/sdk"))
        })
        .ok_or("ANDROID_SDK_ROOT or HOME is required to locate pinned NDK r28c")?;
    let bin = sdk_root.join("ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin");
    for tool in ["aarch64-linux-android35-clang", "llvm-objcopy"] {
        if !bin.join(tool).is_file() {
            return Err(format!(
                "pinned NDK r28c tool is missing: {}",
                bin.join(tool).display()
            )
            .into());
        }
    }
    Ok(bin)
}

pub(crate) fn build_direct_apk_runtime_fixture(root: &Path) -> Result<PathBuf> {
    let tool_root = root.join("tools/android-apk-native-direct-load");
    run_command(Command::new("bash").arg(tool_root.join("audit.sh")))?;

    let build_dir = root.join("_build/android-apk-native-direct-runtime");
    let fixture_dir = build_dir.join("fixtures");
    fs::create_dir_all(&fixture_dir)?;
    let clang = pinned_direct_apk_ndk_bin()?.join("aarch64-linux-android35-clang");
    let grandchild = fixture_dir.join("libapk-direct-grandchild.so");
    let child = fixture_dir.join("libapk-direct-child.so");
    let root_library = fixture_dir.join("libapk-direct-root.so");
    let common = [
        "-shared",
        "-fPIC",
        "-O2",
        "-nostdlib",
        "-fuse-ld=lld",
        "-Wl,--hash-style=sysv",
        "-Wl,--build-id=none",
        "-Wl,-z,now",
        "-Wl,-z,norelro",
        "-Wl,-z,max-page-size=16384",
    ];
    run_command(
        Command::new(&clang)
            .args(common)
            .arg("-Wl,-soname,libapk-direct-grandchild.so")
            .arg(format!(
                "-Wl,--version-script,{}",
                tool_root.join("fixtures/grandchild.map").display()
            ))
            .arg(tool_root.join("fixtures/grandchild.c"))
            .arg("-o")
            .arg(&grandchild),
    )?;
    run_command(
        Command::new(&clang)
            .args(common)
            .arg("-Wl,-soname,libapk-direct-child.so")
            .arg(format!(
                "-Wl,--version-script,{}",
                tool_root.join("fixtures/child.map").display()
            ))
            .arg(tool_root.join("fixtures/child.c"))
            .arg(&grandchild)
            .arg("-o")
            .arg(&child),
    )?;
    run_command(
        Command::new(&clang)
            .args(common)
            .arg("-Wl,-soname,libapk-direct-root.so")
            .arg(format!(
                "-Wl,--version-script,{}",
                tool_root.join("fixtures/root.map").display()
            ))
            .arg(tool_root.join("fixtures/root.c"))
            .arg(&child)
            .arg("-o")
            .arg(&root_library),
    )?;
    for library in [&root_library, &child, &grandchild] {
        let kind = command_output(Command::new("file").arg(library))?;
        if !kind.contains("ELF 64-bit LSB shared object, ARM aarch64") {
            return Err(format!("unexpected direct APK fixture format: {kind}").into());
        }
    }

    let apk = build_dir.join("direct-runtime.apk");
    if apk.exists() {
        fs::set_permissions(&apk, fs::Permissions::from_mode(0o600))?;
        fs::remove_file(&apk)?;
    }
    run_command(
        Command::new("python3")
            .arg(tool_root.join("make_fixture.py"))
            .arg(&apk)
            .arg("valid")
            .arg(&root_library)
            .arg(&child)
            .arg(&grandchild),
    )?;
    if fs::metadata(&apk)?.mode() & 0o777 != 0o400 {
        return Err("direct APK runtime fixture is not immutable mode 0400".into());
    }
    Ok(apk)
}

pub(crate) fn build_runtime_direct_apk_link(root: &Path) -> Result<PathBuf> {
    audit_runtime_link(root)?;
    let runtime = root.join("_aosp/art/runtime");
    let build_dir = root.join("_build/runtime-direct-apk-link-probe");
    let original_member_dir = build_dir.join("original-member");
    let patched_member_dir = build_dir.join("patched-member");
    fs::create_dir_all(&original_member_dir)?;
    fs::create_dir_all(&patched_member_dir)?;
    let source_archive = root.join("_build/runtime-bootstrap/libart-runtime-bootstrap-darwin.a");
    let bootstrap = build_dir.join("libart-runtime-direct-apk-bootstrap-darwin.a");
    fs::copy(&source_archive, &bootstrap)?;
    let member_name = "darwin_runtime_adapters.cc.o";
    let original_member = original_member_dir.join(member_name);
    let patched_member = patched_member_dir.join(member_name);
    if original_member.exists() {
        fs::remove_file(&original_member)?;
    }
    if patched_member.exists() {
        fs::remove_file(&patched_member)?;
    }
    run_command(
        Command::new("ar")
            .args(["-x"])
            .arg(&source_archive)
            .arg(member_name)
            .current_dir(&original_member_dir),
    )?;
    let objcopy = pinned_direct_apk_ndk_bin()?.join("llvm-objcopy");
    let renames = [
        (
            "_darwin_art_elf_discover_sibling_graph",
            "_darwin_art_direct_discover_sibling_graph",
        ),
        (
            "_darwin_art_elf_discovered_graph_root_soname",
            "_darwin_art_direct_discovered_graph_root_soname",
        ),
        (
            "_darwin_art_elf_discovered_graph_sources",
            "_darwin_art_direct_discovered_graph_sources",
        ),
        (
            "_darwin_art_elf_discovered_graph_destroy",
            "_darwin_art_direct_discovered_graph_destroy",
        ),
    ];
    let mut objcopy_command = Command::new(objcopy);
    for (from, to) in renames {
        objcopy_command.args(["--redefine-sym", &format!("{from}={to}")]);
    }
    run_command(objcopy_command.arg(&original_member).arg(&patched_member))?;
    let patched_undefined = command_output(Command::new("nm").arg("-u").arg(&patched_member))?;
    for (_, direct) in renames {
        if !patched_undefined.contains(direct) {
            return Err(format!("patched runtime adapter does not reference {direct}").into());
        }
    }
    run_command(
        Command::new("ar")
            .arg("-d")
            .arg(&bootstrap)
            .arg(member_name),
    )?;
    run_command(
        Command::new("ar")
            .arg("-r")
            .arg(&bootstrap)
            .arg(&patched_member),
    )?;
    run_command(Command::new("ar").arg("-s").arg(&bootstrap))?;

    let object = build_dir.join("darwin_art_runtime_direct_apk.cc.o");
    let graph_object = build_dir.join("darwin_art_runtime_apk_graph.cc.o");
    let filesystem_object = compile_runtime_filesystem_probe(root, &build_dir)?;
    let network_object = if let Some(path) = env::var_os("DARWIN_ART_NATIVE_NETWORK_OBJECT") {
        PathBuf::from(path)
    } else {
        compile_runtime_network_probe(root, &build_dir)?
    };
    if !network_object.is_file() {
        return Err(format!(
            "runtime network object is missing: {}",
            network_object.display()
        )
        .into());
    }
    let surface_object = root.join("_build/runtime-link-probe/darwin_surface_bridge.mm.o");
    let runtime_library = build_dir.join("libdarwin_art_runtime_direct_apk.dylib");
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("crates/darwin-art-elf-loader/include"),
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
    let probe_cache = build_dir.join("runtime-direct-apk-probe-hashes.cache");
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
        .arg("-DDARWIN_ART_REAL_GRAPHICS")
        .arg("-DDARWIN_ART_HWUI_GPU")
        .arg("-I")
        .arg(root.join("_aosp/frameworks/minikin/include"))
        .arg("-I")
        .arg(root.join("_aosp/external/harfbuzz_ng/src"))
        .arg("-DDARWIN_ART_DIRECT_APK_RUNTIME")
        .arg("-DDARWIN_ART_AOSP_COMPAT_LSEEK64")
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-DLOG_TAG=\"DarwinArtHWUI\"")
        .arg("-DSK_BUILD_FOR_ANDROID_FRAMEWORK")
        .arg("-include")
        .arg("log/log_main.h")
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
        .arg(root.join("probes/runtime_graphics_probe.cc"))
        .arg("-o")
        .arg(&graphics_probe_object);
    let _ = compile_cached_probe_tu(
        &mut graphics_probe_command,
        &graphics_probe_object,
        &probe_cache,
        &compiler_identity,
    )?;
    let mut probe_command = runtime_cpp_command(&include_refs);
    probe_command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-DDARWIN_ART_REAL_GRAPHICS")
        .arg("-DDARWIN_ART_HWUI_GPU")
        .arg("-I")
        .arg(root.join("_aosp/external/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-DDARWIN_ART_DIRECT_APK_RUNTIME")
        .arg("-c")
        .arg(root.join("probes/runtime_link_probe.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(
        &mut probe_command,
        &object,
        &probe_cache,
        &compiler_identity,
    )?;
    let mut graph_command = runtime_cpp_command(&include_refs);
    graph_command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-DDARWIN_ART_DIRECT_APK_RUNTIME")
        .arg("-c")
        .arg(root.join("probes/runtime_apk_graph.cc"))
        .arg("-o")
        .arg(&graph_object);
    let _ = compile_cached_probe_tu(
        &mut graph_command,
        &graph_object,
        &probe_cache,
        &compiler_identity,
    )?;

    let mut linker = Command::new("clang++");
    linker
        .arg("-dynamiclib")
        .arg("-Wl,-install_name,@rpath/libdarwin_art_runtime_direct_apk.dylib")
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
        .arg("-Wl,-dead_strip")
        .arg(&object)
        .arg(&elf_probe_object)
        .arg(&abi_probe_object)
        .arg(&process_state_object)
        .arg(&frame_probe_object)
        .arg(&graphics_probe_object)
        .arg(&filesystem_object)
        .arg(&graph_object)
        .arg(&network_object)
        .arg(&surface_object)
        .arg(root.join("_build/skia-metal-gpu/libskia.a"))
        .arg(root.join("_build/skia-metal-gpu/libskcms.a"))
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
    run_command(&mut linker)?;
    let undefined = command_output(Command::new("nm").arg("-u").arg(&runtime_library))?;
    if undefined.contains("darwin_art_direct_") {
        return Err("direct APK runtime dylib has unresolved discovery shims".into());
    }
    let all_symbols = command_output(Command::new("nm").args(["-aC"]).arg(&runtime_library))?;
    for required in [
        "darwin_art_direct_discover_sibling_graph",
        "darwin_art_direct_discovered_graph_sources",
        "darwin_art_elf_graph_load",
        "NativeBridgeGetTrampoline2",
    ] {
        if !all_symbols.contains(required) {
            return Err(format!("direct APK runtime lacks required symbol {required}").into());
        }
    }
    Ok(runtime_library)
}

pub(crate) fn probe_runtime_apk_direct(root: &Path) -> Result<()> {
    let apk = build_direct_apk_runtime_fixture(root)?;
    let runtime_library = build_runtime_direct_apk_link(root)?;
    prepare_icu_bootclasspath(root)?;
    build_runtime_host(root)?;
    let executable = root.join("target/debug/darwin-art-host");
    let core_oj = root.join("_prebuilt/android-16/bootclasspath/core-oj.jar");
    let core_libart = root.join("_prebuilt/android-16/bootclasspath/core-libart.jar");
    let framework = root.join("_prebuilt/android-16/bootclasspath/framework.jar");
    let core_icu4j = root.join("_build/bootclasspath/core-icu4j.jar");
    let classes_dex = root.join("_build/dex-probe/dex/classes.dex");
    for input in [
        &executable,
        &runtime_library,
        &core_oj,
        &core_libart,
        &framework,
        &core_icu4j,
        &classes_dex,
        &apk,
    ] {
        if !input.is_file() {
            return Err(format!("direct APK runtime input is missing: {}", input.display()).into());
        }
    }
    let output = command_output(
        Command::new(&executable)
            .arg(&runtime_library)
            .arg(&core_oj)
            .arg(&core_libart)
            .arg(&framework)
            .arg(&core_icu4j)
            .arg(&classes_dex)
            .env("DARWIN_ART_DIRECT_APK_FIXTURE", &apk)
            .env("DARWIN_ART_DIRECT_APK_ROOT", "libapk-direct-root.so"),
    )?;
    let expected = "Hello from Darwin ART main: 안녕\n\
                    ART Android direct APK ELF: source=readonly-fd-slices copy=0 extract=0 alignment=16384 graph=root+child+grandchild load=JavaVMExt+NativeBridge JNI_OnLoad=0x00010006 unload=shutdown-trampolines-zero authority=isolated-process\n\
                    ART Darwin Runtime::Create: ok\n\
                    ART Darwin app ClassLoader: PathClassLoader\n\
                    ART Darwin DEX interpreter: Hello.answer()=42\n\
                    ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
                    ART runtime native: System.arraycopy()=42\n\
                    ART Android framework: ProbeActivity().probeValue()=42\n\
                    ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
                    ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=360x640\n\
                    ART Android lifecycle: Activity.onCreate()=43\n\
                    ART Darwin launcher: main(String[])=ok";
    if output.trim() != expected {
        return Err(format!("unexpected direct APK runtime output: {output:?}").into());
    }
    println!(
        "probe-runtime-apk-direct: ART JavaVMExt + NativeBridge + readonly APK fd slices PASS"
    );
    Ok(())
}

pub(crate) struct PrivateApkNativeFixture {
    pub(crate) temporary_root: PathBuf,
    pub(crate) extracted_root: PathBuf,
    pub(crate) apk_sha256: String,
    pub(crate) root_sha256: String,
}

impl Drop for PrivateApkNativeFixture {
    fn drop(&mut self) {
        let _ = fs::set_permissions(&self.extracted_root, fs::Permissions::from_mode(0o700));
        let _ = fs::remove_dir_all(&self.temporary_root);
    }
}

pub(crate) struct PrivateNetworkFixture {
    pub(crate) root: PathBuf,
    pub(crate) library: PathBuf,
}

impl Drop for PrivateNetworkFixture {
    fn drop(&mut self) {
        let _ = fs::set_permissions(&self.root, fs::Permissions::from_mode(0o700));
        let _ = fs::remove_dir_all(&self.root);
    }
}

pub(crate) fn prepare_private_network_fixture(root: &Path) -> Result<PrivateNetworkFixture> {
    let source = root.join("_build/network-runtime-probe/libdarwin_art_network_runtime.so");
    if !source.is_file() {
        return Err(format!("network runtime fixture is missing: {}", source.display()).into());
    }
    let temporary_base = env::temp_dir();
    let mut private_root = None;
    for attempt in 0..128_u32 {
        let candidate = temporary_base.join(format!(
            "darwin-art-network-runtime.{}.{}",
            std::process::id(),
            attempt
        ));
        match fs::create_dir(&candidate) {
            Ok(()) => {
                private_root = Some(candidate);
                break;
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.into()),
        }
    }
    let private_root = private_root.ok_or("could not allocate private network fixture")?;
    let library = private_root.join("libdarwin_art_network_runtime.so");
    fs::copy(&source, &library)?;
    fs::set_permissions(&library, fs::Permissions::from_mode(0o400))?;
    fs::set_permissions(&private_root, fs::Permissions::from_mode(0o500))?;
    Ok(PrivateNetworkFixture {
        root: private_root,
        library,
    })
}

pub(crate) fn sha256_file(path: &Path) -> Result<String> {
    let mut file = fs::File::open(path)?;
    let mut digest = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

pub(crate) fn prepare_private_apk_native_fixture(root: &Path) -> Result<PrivateApkNativeFixture> {
    let fixture_root = root.join("_build/android-elf-jni-fixture");
    let root_soname = "libdarwin-art-generic-root.so";
    let sonames = [
        root_soname,
        "libdarwin-art-generic-child.so",
        "libdarwin-art-generic-grandchild.so",
        "libdarwin-art-jni-fixture.so",
        "libdarwin-art-jni-child.so",
        "libdarwin-art-jni-grandchild.so",
    ];
    let temporary_base = env::temp_dir();
    let mut temporary_root = None;
    for attempt in 0..128_u32 {
        let candidate = temporary_base.join(format!(
            "darwin-art-apk-native-runtime.{}.{}",
            std::process::id(),
            attempt
        ));
        match fs::create_dir(&candidate) {
            Ok(()) => {
                fs::set_permissions(&candidate, fs::Permissions::from_mode(0o700))?;
                temporary_root = Some(candidate);
                break;
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.into()),
        }
    }
    let temporary_root =
        temporary_root.ok_or("could not allocate private APK native fixture directory")?;
    let mut cleanup = PrivateApkNativeFixture {
        extracted_root: temporary_root.join("extracted"),
        temporary_root,
        apk_sha256: String::new(),
        root_sha256: String::new(),
    };
    let apk_source = cleanup.temporary_root.join("apk/lib/arm64-v8a");
    fs::create_dir_all(&apk_source)?;
    for soname in sonames {
        let source = fixture_root.join(soname);
        if !source.is_file() {
            return Err(format!("APK native fixture is missing: {}", source.display()).into());
        }
        fs::copy(source, apk_source.join(soname))?;
    }
    let apk = cleanup.temporary_root.join("fixture.apk");
    run_command(
        Command::new("zip")
            .current_dir(cleanup.temporary_root.join("apk"))
            .args(["-q", "-9", "-X", "-r"])
            .arg(&apk)
            .arg("."),
    )?;
    cleanup.apk_sha256 = sha256_file(&apk)?;

    let extractor = root.join("tools/android-apk-native-extract/Cargo.toml");
    if !extractor.is_file() {
        return Err(format!("APK native extractor is missing: {}", extractor.display()).into());
    }
    let extraction_output = command_output(
        Command::new("cargo")
            .args(["run", "--quiet", "--release", "--manifest-path"])
            .arg(&extractor)
            .arg("--")
            .arg(&apk)
            .arg(&cleanup.extracted_root)
            .arg(root_soname),
    )?;
    if !extraction_output.starts_with("apk-native-extract: PASS files=6 stored=0 deflated=6 ")
        || !extraction_output.contains(
            "crc=verified mode=dir0500+file0400 publish=atomic root=libdarwin-art-generic-root.so",
        )
    {
        return Err(
            format!("unexpected APK native extraction output: {extraction_output:?}").into(),
        );
    }
    if fs::metadata(&cleanup.extracted_root)?.mode() & 0o777 != 0o500 {
        return Err("APK native extraction directory is not sealed to mode 0500".into());
    }
    for soname in sonames {
        let original = fixture_root.join(soname);
        let extracted = cleanup.extracted_root.join(soname);
        if fs::metadata(&extracted)?.mode() & 0o777 != 0o400 {
            return Err(format!("extracted native fixture is not mode 0400: {soname}").into());
        }
        let original_sha256 = sha256_file(&original)?;
        let extracted_sha256 = sha256_file(&extracted)?;
        if original_sha256 != extracted_sha256 {
            return Err(format!("APK extraction changed native fixture bytes: {soname}").into());
        }
        if soname == root_soname {
            cleanup.root_sha256 = extracted_sha256;
        }
    }
    Ok(cleanup)
}

pub(crate) fn probe_runtime_graphics(root: &Path) -> Result<()> {
    probe_runtime_dex_flavor(root, false, true, false, false, false, false)
}

pub(crate) fn probe_runtime_graphics_window(root: &Path) -> Result<()> {
    probe_runtime_dex_flavor(root, true, true, false, false, false, false)
}

pub(crate) fn prepare_probe_android_system_root(root: &Path) -> Result<PathBuf> {
    let base = env::temp_dir();
    let mut directory = None;
    for attempt in 0..128_u32 {
        let candidate = base.join(format!(
            "darwin-art-android-system-root.{}.{}",
            std::process::id(),
            attempt
        ));
        match fs::create_dir(&candidate) {
            Ok(()) => {
                directory = Some(candidate);
                break;
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.into()),
        }
    }
    let directory = directory.ok_or("could not allocate Android system root")?;
    let etc = directory.join("etc");
    let fonts = directory.join("fonts");
    fs::create_dir_all(&etc)?;
    fs::create_dir_all(&fonts)?;
    fs::copy(root.join("probes/button/fonts.xml"), etc.join("fonts.xml"))?;
    fs::copy(
        root.join("_aosp/external/skia/resources/fonts/Roboto-Regular.ttf"),
        fonts.join("Roboto-Regular.ttf"),
    )?;
    fs::set_permissions(etc.join("fonts.xml"), fs::Permissions::from_mode(0o400))?;
    fs::set_permissions(
        fonts.join("Roboto-Regular.ttf"),
        fs::Permissions::from_mode(0o400),
    )?;
    fs::set_permissions(&etc, fs::Permissions::from_mode(0o500))?;
    fs::set_permissions(&fonts, fs::Permissions::from_mode(0o500))?;
    fs::set_permissions(&directory, fs::Permissions::from_mode(0o500))?;
    Ok(directory)
}

pub(crate) fn probe_runtime_button(root: &Path, show_window: bool) -> Result<()> {
    probe_runtime_dex_flavor(root, show_window, true, true, false, false, false)
}

pub(crate) fn probe_runtime_apk_app(root: &Path, show_window: bool) -> Result<()> {
    build_shell_gate(root, "android-apk-app-runtime/audit.sh")?;
    probe_runtime_dex_flavor(root, show_window, true, false, false, false, true)
}
