use super::*;
use crate::native_build::{PendingNativeCompile, compile_pending_native};

pub(crate) use crate::native_graph::build_native_graph;

pub(crate) fn build_runtime_platform(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    if !runtime.join("Android.bp").exists() || !tinyxml2.join("tinyxml2.h").exists() {
        return Err("runtime platform sources are missing; run `art-bootstrap sync` first".into());
    }

    let includes = [
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let build_dir = root.join("_build/runtime-platform");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&object_dir)?;

    let sources = [
        runtime.join("runtime_linux.cc"),
        runtime.join("thread_linux.cc"),
        runtime.join("monitor_linux.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected runtime object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-platform-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-platform: Mach-O arm64 objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

pub(crate) fn build_runtime_core(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");

    let build_dir = root.join("_build/runtime-core");
    let patched_runtime = build_dir.join("patched-source/runtime");
    let patched_base = patched_runtime.join("base");
    let patched_mirror = patched_runtime.join("mirror");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&patched_base)?;
    fs::create_dir_all(&patched_mirror)?;
    fs::create_dir_all(&object_dir)?;
    fs::copy(
        runtime.join("monitor.cc"),
        patched_runtime.join("monitor.cc"),
    )?;
    fs::copy(runtime.join("base/mutex.h"), patched_base.join("mutex.h"))?;
    fs::copy(runtime.join("base/mutex.cc"), patched_base.join("mutex.cc"))?;
    fs::copy(
        runtime.join("mirror/object_reference.h"),
        patched_mirror.join("object_reference.h"),
    )?;

    for patch in [
        "patches/art/0003-darwin-allow-pthread-monitors.patch",
        "patches/art/0004-darwin-uncontended-monitor-lock.patch",
        "patches/art/0026-darwin-base-relative-object-references-only.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(build_dir.join("patched-source")),
        )?;
    }

    let includes = [
        patched_runtime.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let sources = [
        patched_base.join("mutex.cc"),
        patched_runtime.join("monitor.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected runtime core object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-core-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-core: pthread monitor bootstrap objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

pub(crate) fn probe_park(root: &Path) -> Result<()> {
    let mutex_object = root.join("_build/runtime-core/objects/mutex.cc.o");
    let patched_runtime = root.join("_build/runtime-bootstrap/patched-source/runtime");
    if !mutex_object.exists() || !patched_runtime.join("thread.h").exists() {
        return Err(
            "Darwin runtime objects are missing; run `build-runtime-bootstrap` first".into(),
        );
    }

    let build_dir = root.join("_build/park-probe");
    fs::create_dir_all(&build_dir)?;
    let object = build_dir.join("park_probe.cc.o");
    let executable = build_dir.join("park-probe");
    let generated = root.join("_build/runtime-arm64/generated");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_include = root.join("_aosp/libnativehelper/header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let includes = [
        patched_runtime.as_path(),
        generated.as_path(),
        generator.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        libbase_include.as_path(),
        palette_include.as_path(),
        jni_include.as_path(),
        nativehelper_include.as_path(),
        dlmalloc.as_path(),
        tinyxml2.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    run_command(
        common_cpp_command(&includes)
            .arg("-Wno-invalid-offsetof")
            .arg("-c")
            .arg(root.join("probes/park_probe.cc"))
            .arg("-o")
            .arg(&object),
    )?;
    run_command(
        Command::new("clang++")
            .arg("-Wl,-dead_strip")
            .arg(&object)
            .arg(&mutex_object)
            .arg(root.join("_build/foundation/libartbase-darwin.a"))
            .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
            .args(["-L/opt/homebrew/lib", "-lfmt"])
            .arg("-o")
            .arg(&executable),
    )?;
    let output = command_output(&mut Command::new(&executable))?;
    let expected = "ART Darwin park: pre-permit=yes wakeups=200 timeout=yes";
    if output.trim() != expected {
        return Err(format!("unexpected Darwin park probe output: {output:?}").into());
    }
    println!("probe-park: {}", output.trim());
    Ok(())
}

pub(crate) fn build_runtime_arm64(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_abi = root.join("_build/runtime-core/patched-source/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let compat = root.join("compat");
    if !generator.join("asm_defines.cc").exists() {
        return Err("ART ABI generator is missing; run `art-bootstrap sync` first".into());
    }

    let build_dir = root.join("_build/runtime-arm64");
    let generated_dir = build_dir.join("generated");
    let patched_runtime = build_dir.join("patched-source/runtime");
    let patched_arm64 = patched_runtime.join("arch/arm64");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&generated_dir)?;
    fs::create_dir_all(&patched_arm64)?;
    fs::create_dir_all(&object_dir)?;
    fs::copy(
        runtime_arm64.join("context_arm64.h"),
        patched_arm64.join("context_arm64.h"),
    )?;
    fs::copy(
        runtime_arm64.join("context_arm64.cc"),
        patched_arm64.join("context_arm64.cc"),
    )?;
    fs::copy(
        runtime_arm64.join("instruction_set_features_arm64.cc"),
        patched_arm64.join("instruction_set_features_arm64.cc"),
    )?;
    for assembly in [
        "asm_support_arm64.S",
        "jni_entrypoints_arm64.S",
        "memcmp16_arm64.S",
        "quick_entrypoints_arm64.S",
        "native_entrypoints_arm64.S",
    ] {
        fs::copy(runtime_arm64.join(assembly), patched_arm64.join(assembly))?;
    }
    for patch in [
        "patches/art/0001-arm64-mach-o-assembly.patch",
        "patches/art/0005-darwin-arm64-context-word-type.patch",
        "patches/art/0012-darwin-arm64-quick-symbols.patch",
        "patches/art/0015-darwin-arm64-feature-fallback.patch",
        "patches/art/0016-darwin-arm64-direct-c-symbols.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(build_dir.join("patched-source")),
        )?;
    }

    let includes = [
        generated_dir.as_path(),
        compat.as_path(),
        generator.as_path(),
        patched_runtime.as_path(),
        runtime_abi.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];

    let generated_assembly = generated_dir.join("asm_defines.s");
    run_command(
        runtime_cpp_command(&includes)
            .args(["-include", "mirror/object_reference.h"])
            .arg("-S")
            .arg(generator.join("asm_defines.cc"))
            .arg("-o")
            .arg(&generated_assembly),
    )?;
    let generated_header = command_output(
        Command::new("python3")
            .arg(generator.join("make_header.py"))
            .arg(&generated_assembly),
    )?;
    for required in [
        "#define THREAD_FLAGS_OFFSET",
        "#define THREAD_CARD_TABLE_OFFSET",
        "#define THREAD_EXCEPTION_OFFSET",
        "#define THREAD_ID_OFFSET",
    ] {
        if !generated_header.contains(required) {
            return Err(format!("generated asm_defines.h is missing {required}").into());
        }
    }
    fs::write(generated_dir.join("asm_defines.h"), generated_header)?;

    let sources = [
        patched_arm64.join("context_arm64.cc"),
        runtime.join("arch/context.cc"),
        runtime.join("arch/instruction_set_features.cc"),
        runtime_arm64.join("thread_arm64.cc"),
        runtime_arm64.join("entrypoints_init_arm64.cc"),
        patched_arm64.join("instruction_set_features_arm64.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-Wno-deprecated-anon-enum-enum-conversion")
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected ARM64 runtime object format: {kind}").into());
        }
        objects.push(object);
    }

    // Apple's integrated assembler rejects a small subset of ART's otherwise
    // valid DWARF CFI state machine. The entrypoints themselves are usable for
    // this bootstrap, so emit a clearly named no-CFI PoC artifact. Restoring
    // correct Darwin unwind metadata is required before production use.
    for assembly in [
        "jni_entrypoints_arm64.S",
        "memcmp16_arm64.S",
        "quick_entrypoints_arm64.S",
        "native_entrypoints_arm64.S",
    ] {
        let source = patched_arm64.join(assembly);
        let preprocessed = command_output(
            Command::new("clang")
                .args(["-E", "-x", "assembler-with-cpp", "-DART_PAGE_SIZE_AGNOSTIC"])
                .arg(format!("-I{}", generated_dir.display()))
                .arg(format!("-I{}", patched_arm64.display()))
                .arg(format!("-I{}", runtime_arm64.display()))
                .arg(format!("-I{}", runtime.display()))
                .arg(&source),
        )?;
        let mut stripped_cfi = 0usize;
        let mut darwin_assembly = String::with_capacity(preprocessed.len());
        for line in preprocessed.lines() {
            if line.trim_start().starts_with(".cfi_") {
                stripped_cfi += 1;
            } else {
                darwin_assembly.push_str(line);
                darwin_assembly.push('\n');
            }
        }
        if stripped_cfi == 0 {
            return Err(format!("expected CFI directives in {assembly}").into());
        }
        let generated_source = generated_dir.join(format!("{assembly}.darwin-no-cfi.S"));
        fs::write(&generated_source, darwin_assembly)?;
        let object = object_dir.join(format!("{assembly}.o"));
        run_command(
            Command::new("clang")
                .args(["-arch", "arm64", "-x", "assembler", "-c"])
                .arg(&generated_source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected ARM64 assembly object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-arm64-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-arm64: generated ABI constants, Mach-O objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

pub(crate) fn build_interpreter_core(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_abi = root.join("_build/runtime-core/patched-source/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let generated_dir = root.join("_build/runtime-arm64/generated");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_headers = root.join("_aosp/libnativehelper/header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    if !generated_dir.join("asm_defines.h").exists() {
        return Err(
            "generated ART ABI constants are missing; run `build-runtime-arm64` first".into(),
        );
    }

    let includes = [
        generated_dir.as_path(),
        generator.as_path(),
        runtime_abi.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        nativehelper_headers.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let build_dir = root.join("_build/interpreter-core");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&object_dir)?;
    let sources = [
        runtime.join("interpreter/interpreter.cc"),
        runtime.join("interpreter/interpreter_cache.cc"),
        runtime.join("interpreter/interpreter_common.cc"),
        runtime.join("interpreter/interpreter_switch_impl0.cc"),
        runtime.join("interpreter/lock_count_data.cc"),
        runtime.join("interpreter/shadow_frame.cc"),
        runtime.join("interpreter/unstarted_runtime.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected interpreter object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-interpreter-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-interpreter-core: AOSP C++ interpreter Mach-O objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

pub(crate) fn build_runtime_bootstrap(root: &Path) -> Result<()> {
    build_native_graph(root, "runtime-bootstrap")
}

pub(crate) fn build_runtime_graphics_bootstrap(root: &Path) -> Result<()> {
    build_native_graph(root, "graphics-bootstrap")
}

pub(crate) fn build_runtime_graphics_bootstrap_inner(root: &Path) -> Result<()> {
    build_runtime_bootstrap_flavor(root, true)
}

pub(crate) fn build_runtime_bootstrap_inner(root: &Path) -> Result<()> {
    build_runtime_bootstrap_flavor(root, false)
}

pub(crate) fn build_runtime_bootstrap_flavor(root: &Path, real_graphics: bool) -> Result<()> {
    build_shell_gate(root, "build-android-elf-jni-fixture.sh")?;
    let build_paths = BuildPaths::from_root(root);
    run_command(
        Command::new("cargo")
            .args(["build", "--release", "--lib", "--manifest-path"])
            .arg(root.join("crates/darwin-art-elf-loader/Cargo.toml")),
    )?;
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let cmdline = root.join("_aosp/art/cmdline");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let libelffile = root.join("_aosp/art/libelffile");
    let libprofile = root.join("_aosp/art/libprofile");
    let nativebridge_include = root.join("_aosp/art/libnativebridge/include");
    let nativeloader_include = root.join("_aosp/art/libnativeloader/include");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let runtime_gc = runtime.join("gc");
    let runtime_gc_collector = runtime.join("gc/collector");
    let runtime_gc_space = runtime.join("gc/space");
    let runtime_quick_entrypoints = runtime.join("entrypoints/quick");
    let runtime_mterp = runtime.join("interpreter/mterp");
    let runtime_oat = runtime.join("oat");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let generated_dir = root.join("_build/runtime-arm64/generated");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_headers = root.join("_aosp/libnativehelper/header_only_include");
    let nativehelper_platform_headers =
        root.join("_aosp/libnativehelper/platform_header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let odrefresh_include = root.join("_aosp/art/odrefresh/include");
    let sigchain = root.join("_aosp/art/sigchainlib");
    let unwindstack_include = root.join("_aosp/system/unwinding/libunwindstack/include");
    let compat = root.join("compat");
    let public_include = root.join("include");
    let graphics_registration_include = root.join("_build/android-graphics-jni/generated");
    let android_icu_include = root.join("_aosp/external/icu-graphics/libandroidicuinit/include");
    let android_icu_common = root.join("_aosp/external/icu-graphics/icu4c/source/common");
    let android_icu_i18n = root.join("_aosp/external/icu-graphics/icu4c/source/i18n");
    let android_graphics_apex_include = root.join("_aosp/frameworks/base/libs/hwui/apex/include");
    let aosp_fmt_include = root.join("_aosp/external/fmtlib/include");
    let libcutils_include = root.join("_aosp/system/core/libcutils/include");
    let liblog_include = root.join("_aosp/system/logging/liblog/include");
    let nativehelper_full_include = root.join("_aosp/libnativehelper-full/include");
    let elf_loader_include = root.join("crates/darwin-art-elf-loader/include");
    let jni_proxy_include = root.join("tools/android-jni-proxy/include");
    let jni_proxy_generated = root.join("tools/android-jni-proxy/generated");
    let elf_fixture_generated = root.join("_build/android-elf-jni-fixture/generated");
    let openjdk_math_source = root.join("_aosp/libcore/ojluni/src/main/native/Math.c");
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;

    if !generated_dir.join("asm_defines.h").exists() {
        return Err(
            "generated ART ABI constants are missing; run `build-runtime-arm64` first".into(),
        );
    }
    if !openjdk_math_source.is_file() {
        return Err(format!(
            "Android libcore Math source is missing: {}; run `art-bootstrap sync` first",
            openjdk_math_source.display()
        )
        .into());
    }

    let build_dir = build_paths.native_output(if real_graphics {
        "runtime-graphics-bootstrap"
    } else {
        "runtime-bootstrap"
    });
    let runtime_generated_dir = build_dir.join("generated");
    let patched_source_dir = build_dir.join("patched-source");
    let patched_runtime = patched_source_dir.join("runtime");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&patched_runtime)?;
    fs::create_dir_all(&object_dir)?;
    fs::create_dir_all(&runtime_generated_dir)?;
    fs::copy(
        runtime.join("runtime.cc"),
        patched_runtime.join("runtime.cc"),
    )?;
    fs::copy(
        runtime.join("signal_set.h"),
        patched_runtime.join("signal_set.h"),
    )?;
    fs::copy(
        runtime.join("class_linker.cc"),
        patched_runtime.join("class_linker.cc"),
    )?;
    fs::copy(
        runtime.join("class_linker.h"),
        patched_runtime.join("class_linker.h"),
    )?;
    fs::create_dir_all(patched_runtime.join("mirror"))?;
    fs::copy(
        runtime.join("mirror/object_reference.h"),
        patched_runtime.join("mirror/object_reference.h"),
    )?;
    fs::copy(
        runtime.join("mirror/string-inl.h"),
        patched_runtime.join("mirror/string-inl.h"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc"))?;
    fs::copy(
        runtime.join("gc/heap.cc"),
        patched_runtime.join("gc/heap.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc/space"))?;
    fs::copy(
        runtime.join("gc/space/malloc_space.cc"),
        patched_runtime.join("gc/space/malloc_space.cc"),
    )?;
    fs::copy(
        runtime.join("gc/space/space.cc"),
        patched_runtime.join("gc/space/space.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("entrypoints/quick"))?;
    fs::copy(
        runtime.join("entrypoints/quick/quick_alloc_entrypoints.cc"),
        patched_runtime.join("entrypoints/quick/quick_alloc_entrypoints.cc"),
    )?;
    fs::copy(
        runtime.join("entrypoints/quick/callee_save_frame.h"),
        patched_runtime.join("entrypoints/quick/callee_save_frame.h"),
    )?;
    fs::copy(
        runtime.join("entrypoints/quick/quick_trampoline_entrypoints.cc"),
        patched_runtime.join("entrypoints/quick/quick_trampoline_entrypoints.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("arch/arm64"))?;
    fs::copy(
        runtime.join("arch/arm64/jni_frame_arm64.h"),
        patched_runtime.join("arch/arm64/jni_frame_arm64.h"),
    )?;
    fs::copy(
        runtime.join("runtime_common.cc"),
        patched_runtime.join("runtime_common.cc"),
    )?;
    fs::copy(runtime.join("runtime.h"), patched_runtime.join("runtime.h"))?;
    fs::copy(runtime.join("thread.cc"), patched_runtime.join("thread.cc"))?;
    fs::copy(runtime.join("thread.h"), patched_runtime.join("thread.h"))?;
    fs::copy(
        runtime.join("thread_list.cc"),
        patched_runtime.join("thread_list.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc/collector"))?;
    fs::copy(
        runtime.join("gc/collector/garbage_collector.cc"),
        patched_runtime.join("gc/collector/garbage_collector.cc"),
    )?;
    fs::copy(
        runtime.join("gc/collector/mark_compact.cc"),
        patched_runtime.join("gc/collector/mark_compact.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("oat"))?;
    fs::copy(
        runtime.join("oat/oat_file.cc"),
        patched_runtime.join("oat/oat_file.cc"),
    )?;
    fs::copy(
        runtime.join("exec_utils.cc"),
        patched_runtime.join("exec_utils.cc"),
    )?;
    fs::copy(
        runtime.join("signal_catcher.cc"),
        patched_runtime.join("signal_catcher.cc"),
    )?;
    fs::copy(
        runtime.join("nterp_helpers.cc"),
        patched_runtime.join("nterp_helpers.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("interpreter/mterp"))?;
    fs::copy(
        runtime.join("interpreter/mterp/nterp.cc"),
        patched_runtime.join("interpreter/mterp/nterp.cc"),
    )?;
    for patch in [
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
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(&patched_source_dir),
        )?;
    }

    let mut includes = vec![
        public_include.as_path(),
        compat.as_path(),
        elf_fixture_generated.as_path(),
        elf_loader_include.as_path(),
        jni_proxy_include.as_path(),
        generated_dir.as_path(),
        generator.as_path(),
        patched_runtime.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        cmdline.as_path(),
        libdexfile.as_path(),
        libelffile.as_path(),
        libprofile.as_path(),
        nativebridge_include.as_path(),
        nativeloader_include.as_path(),
        odrefresh_include.as_path(),
        sigchain.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        runtime_gc.as_path(),
        runtime_gc_collector.as_path(),
        runtime_gc_space.as_path(),
        runtime_quick_entrypoints.as_path(),
        runtime_mterp.as_path(),
        runtime_oat.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        unwindstack_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        nativehelper_headers.as_path(),
        nativehelper_platform_headers.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/opt/icu4c@78/include"),
        Path::new("/opt/homebrew/include"),
    ];
    if real_graphics {
        let generated_registration =
            graphics_registration_include.join("darwin_android_graphics_registration.h");
        if !generated_registration.is_file() {
            return Err(format!(
                "generated Android graphics registration header is missing: {}; run `build-android-graphics-jni` first",
                generated_registration.display()
            )
            .into());
        }
        if !android_icu_include
            .join("androidicuinit/android_icu_init.h")
            .is_file()
        {
            return Err("Android ICU init headers are missing; run `build-icu` first".into());
        }
        includes.insert(0, graphics_registration_include.as_path());
        includes.insert(1, android_icu_include.as_path());
        includes.insert(2, android_graphics_apex_include.as_path());
        includes.insert(3, aosp_fmt_include.as_path());
    }
    let sources = [
        "fault_handler.cc",
        "interpreter/mterp/nterp.cc",
        "dex_register_location.cc",
        "handle.cc",
        "java_frame_root_info.cc",
        "jit/jit_memory_region.cc",
        "jit/profile_saver.cc",
        "jit/small_pattern_matcher.cc",
        "offsets.cc",
        "reflective_value_visitor.cc",
        "jit/jit.cc",
        "jit/jit_code_cache.cc",
        "jit/jit_options.cc",
        "jit/profiling_info.cc",
        "mirror/emulated_stack_frame.cc",
        "mirror/executable.cc",
        "monitor_objects_stack_visitor.cc",
        "signal_catcher.cc",
        "debug_print.cc",
        "debugger.cc",
        "dex/dex_file_annotations.cc",
        "exec_utils.cc",
        "hidden_api.cc",
        "jni/check_jni.cc",
        "jni/jni_internal.cc",
        "method_handles.cc",
        "mirror/class_ext.cc",
        "mirror/field.cc",
        "mirror/method.cc",
        "mirror/method_handle_impl.cc",
        "mirror/method_handles_lookup.cc",
        "mirror/method_type.cc",
        "mirror/var_handle.cc",
        "native_bridge_art_interface.cc",
        "native_stack_dump.cc",
        "oat/elf_file.cc",
        "oat/index_bss_mapping.cc",
        "oat/jni_stub_hash_map.cc",
        "plugin.cc",
        "scoped_thread_state_change.cc",
        "startup_completed_task.cc",
        "string_builder_append.cc",
        "ti/agent.cc",
        "trace.cc",
        "trace_profile.cc",
        "var_handles.cc",
        "verifier/class_verifier.cc",
        "verifier/instruction_flags.cc",
        "verifier/method_verifier.cc",
        "verifier/reg_type.cc",
        "verifier/reg_type_cache.cc",
        "verifier/register_line.cc",
        "verifier/verifier_deps.cc",
        "backtrace_helper.cc",
        "jit/debugger_interface.cc",
        "metrics/reporter.cc",
        "monitor_pool.cc",
        "non_debuggable_classes.cc",
        "nterp_helpers.cc",
        "oat/image.cc",
        "oat/oat.cc",
        "oat/oat_file.cc",
        "oat/oat_file_assistant.cc",
        "oat/oat_file_assistant_context.cc",
        "oat/oat_quick_method_header.cc",
        "oat/stack_map.cc",
        "oat/sdc_file.cc",
        "object_lock.cc",
        "quick_exception_handler.cc",
        "reference_table.cc",
        "reflection.cc",
        "reflective_handle_scope.cc",
        "stack.cc",
        "thread_pool.cc",
        "vdex_file.cc",
        "entrypoints/entrypoint_utils.cc",
        "entrypoints/jni/jni_entrypoints.cc",
        "entrypoints/math_entrypoints.cc",
        "entrypoints/quick/quick_alloc_entrypoints.cc",
        "entrypoints/quick/quick_cast_entrypoints.cc",
        "entrypoints/quick/quick_deoptimization_entrypoints.cc",
        "entrypoints/quick/quick_dexcache_entrypoints.cc",
        "entrypoints/quick/quick_entrypoints_enum.cc",
        "entrypoints/quick/quick_field_entrypoints.cc",
        "entrypoints/quick/quick_fillarray_entrypoints.cc",
        "entrypoints/quick/quick_jni_entrypoints.cc",
        "entrypoints/quick/quick_lock_entrypoints.cc",
        "entrypoints/quick/quick_math_entrypoints.cc",
        "entrypoints/quick/quick_string_builder_append_entrypoints.cc",
        "entrypoints/quick/quick_thread_entrypoints.cc",
        "entrypoints/quick/quick_throw_entrypoints.cc",
        "entrypoints/quick/quick_trampoline_entrypoints.cc",
        "runtime.cc",
        "class_linker.cc",
        "thread.cc",
        "thread_list.cc",
        "gc/heap.cc",
        "intern_table.cc",
        "instrumentation.cc",
        "runtime_callbacks.cc",
        "oat/oat_file_manager.cc",
        "jni/java_vm_ext.cc",
        "runtime_options.cc",
        "base/locks.cc",
        "base/gc_visited_arena_pool.cc",
        "base/mem_map_arena_pool.cc",
        "base/quasi_atomic.cc",
        "base/timing_logger.cc",
        "gc/accounting/bitmap.cc",
        "gc/accounting/card_table.cc",
        "gc/accounting/heap_bitmap.cc",
        "gc/accounting/mod_union_table.cc",
        "gc/accounting/remembered_set.cc",
        "gc/accounting/space_bitmap.cc",
        "gc/allocation_record.cc",
        "gc/allocator/art-dlmalloc.cc",
        "gc/allocator/rosalloc.cc",
        "gc/collector/concurrent_copying.cc",
        "gc/collector/garbage_collector.cc",
        "gc/collector/immune_region.cc",
        "gc/collector/immune_spaces.cc",
        "gc/collector/mark_compact.cc",
        "gc/collector/mark_sweep.cc",
        "gc/collector/partial_mark_sweep.cc",
        "gc/collector/semi_space.cc",
        "gc/collector/sticky_mark_sweep.cc",
        "gc/gc_cause.cc",
        "gc/reference_processor.cc",
        "gc/reference_queue.cc",
        "gc/scoped_gc_critical_section.cc",
        "gc/space/bump_pointer_space.cc",
        "gc/space/dlmalloc_space.cc",
        "gc/space/image_space.cc",
        "gc/space/large_object_space.cc",
        "gc/space/malloc_space.cc",
        "gc/space/region_space.cc",
        "gc/space/rosalloc_space.cc",
        "gc/space/space.cc",
        "gc/space/zygote_space.cc",
        "gc/task_processor.cc",
        "gc/verification.cc",
        "javaheapprof/javaheapsampler.cc",
        "app_info.cc",
        "art_field.cc",
        "art_method.cc",
        "barrier.cc",
        "cha.cc",
        "class_loader_context.cc",
        "class_root.cc",
        "class_table.cc",
        "common_throws.cc",
        "compat_framework.cc",
        "indirect_reference_table.cc",
        "jni/jni_env_ext.cc",
        "jni/local_reference_table.cc",
        "jni/jni_id_manager.cc",
        "mirror/array.cc",
        "mirror/class.cc",
        "mirror/dex_cache.cc",
        "mirror/object.cc",
        "mirror/stack_frame_info.cc",
        "mirror/stack_trace_element.cc",
        "mirror/string.cc",
        "mirror/throwable.cc",
        "native/dalvik_system_BaseDexClassLoader.cc",
        "native/dalvik_system_DexFile.cc",
        "native/dalvik_system_VMDebug.cc",
        "native/dalvik_system_VMRuntime.cc",
        "native/dalvik_system_VMStack.cc",
        "native/dalvik_system_ZygoteHooks.cc",
        "native/java_lang_Class.cc",
        "native/java_lang_Object.cc",
        "native/java_lang_StackStreamFactory.cc",
        "native/java_lang_String.cc",
        "native/java_lang_StringFactory.cc",
        "native/java_lang_System.cc",
        "native/java_lang_Thread.cc",
        "native/java_lang_Throwable.cc",
        "native/java_lang_VMClassLoader.cc",
        "native/java_lang_invoke_MethodHandle.cc",
        "native/java_lang_invoke_MethodHandleImpl.cc",
        "native/java_lang_ref_FinalizerReference.cc",
        "native/java_lang_ref_Reference.cc",
        "native/java_lang_reflect_Array.cc",
        "native/java_lang_reflect_Constructor.cc",
        "native/java_lang_reflect_Executable.cc",
        "native/java_lang_reflect_Field.cc",
        "native/java_lang_reflect_Method.cc",
        "native/java_lang_reflect_Parameter.cc",
        "native/java_lang_reflect_Proxy.cc",
        "native/java_util_concurrent_atomic_AtomicLong.cc",
        "native/jdk_internal_misc_Unsafe.cc",
        "native/libcore_io_Memory.cc",
        "native/libcore_util_CharsetUtils.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmServer.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc",
        "native/sun_misc_Unsafe.cc",
        "runtime_common.cc",
        "runtime_intrinsics.cc",
        "well_known_classes.cc",
    ];
    // Soong generates this translation unit from ART's enum declarations. Use
    // the pinned upstream generator instead of maintaining Darwin-only copies
    // of dozens of operator<< implementations.
    let operator_headers = [
        "base/callee_save_type.h",
        "base/locks.h",
        "class_status.h",
        "compilation_kind.h",
        "gc/allocator/rosalloc.h",
        "gc/allocator_type.h",
        "gc/collector/gc_type.h",
        "gc/collector/mark_compact.h",
        "gc/collector_type.h",
        "gc/space/region_space.h",
        "gc/space/space.h",
        "gc/weak_root_state.h",
        "gc_root.h",
        "indirect_reference_table.h",
        "instrumentation.h",
        "jdwp_provider.h",
        "jni_id_type.h",
        "linear_alloc.h",
        "lock_word.h",
        "oat/image.h",
        "oat/oat.h",
        "oat/oat_file.h",
        "process_state.h",
        "reflective_value_visitor.h",
        "stack.h",
        "suspend_reason.h",
        "thread.h",
        "thread_state.h",
        "trace.h",
        "trace_profile.h",
        "verifier/verifier_enums.h",
    ];
    let operator_source = runtime_generated_dir.join("operator_out.cc");
    if !operator_source.is_file() {
        let mut generate = Command::new("python3");
        generate
            .arg(root.join("_aosp/art/tools/generate_operator_out.py"))
            .arg(&runtime);
        for header in operator_headers {
            generate.arg(runtime.join(header));
        }
        fs::write(&operator_source, command_output(&mut generate)?)?;
    }

    let compiler_identity = format!(
        "{}macOS {} ({})",
        command_output(Command::new("clang++").arg("--version"))?,
        command_output(Command::new("sw_vers").arg("-productVersion"))?.trim(),
        command_output(Command::new("sw_vers").arg("-buildVersion"))?.trim()
    );
    let file_hash_cache_path = build_dir.join("file-hashes.cache");
    let mut file_hash_cache = FileHashCache::load(&file_hash_cache_path)?;
    let mut objects = Vec::new();
    let mut compiled_objects = 0usize;
    let mut cached_objects = 0usize;
    let operator_object = object_dir.join("generated_operator_out.cc.o");
    let mut operator_command = runtime_bootstrap_cpp_command(&includes);
    operator_command
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-c")
        .arg(&operator_source)
        .arg("-o")
        .arg(&operator_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut operator_command,
            &operator_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(operator_object);

    for profile_source in [
        "profile/profile_boot_info.cc",
        "profile/profile_compilation_info.cc",
    ] {
        let profile_object =
            object_dir.join(format!("libprofile_{}.o", profile_source.replace('/', "_")));
        let mut profile_command = runtime_bootstrap_cpp_command(&includes);
        profile_command
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(libprofile.join(profile_source))
            .arg("-o")
            .arg(&profile_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut profile_command,
                &profile_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
        objects.push(profile_object);
    }
    if real_graphics {
        // The default foundation archive was compiled against the developer
        // machine's fmt headers. Recompile the only fmt-using libartbase TU in
        // this isolated flavor so ART and module-complete AOSP libbase share
        // the pinned fmt ABI without a Homebrew fmt provider.
        let os_linux_object = object_dir.join("artbase_os_linux_aosp_fmt.cc.o");
        let mut os_linux_command = runtime_cpp_command(&includes);
        os_linux_command
            .arg("-c")
            .arg(artbase.join("base/os_linux.cc"))
            .arg("-o")
            .arg(&os_linux_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut os_linux_command,
                &os_linux_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
    }
    // Compile Android 16 libcore's complete java.lang.Math native table as C.
    // Keeping this TU unchanged preserves AOSP's FastNative ABI and avoids a
    // Darwin-specific reimplementation of its 23 libm entry points.
    let openjdk_math_object = object_dir.join("libcore_openjdk_Math.c.o");
    let mut openjdk_math_command = Command::new("clang");
    openjdk_math_command
        .args([
            "-std=gnu11",
            "-O2",
            "-DNDEBUG",
            "-ftrivial-auto-var-init=zero",
            "-ffunction-sections",
            "-fdata-sections",
        ])
        .arg(format!("-I{}", android_jni_include.display()))
        .arg(format!("-I{}", nativehelper_full_include.display()))
        .arg(format!("-I{}", nativehelper_platform_headers.display()))
        .arg(format!("-I{}", liblog_include.display()))
        .arg("-c")
        .arg(&openjdk_math_source)
        .arg("-o")
        .arg(&openjdk_math_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut openjdk_math_command,
            &openjdk_math_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(openjdk_math_object);
    let jni_proxy_object = object_dir.join("darwin_art_jni_proxy.c.o");
    let mut jni_proxy_command = Command::new("clang");
    jni_proxy_command
        .args([
            "-std=c17",
            "-O2",
            "-DNDEBUG",
            "-fvisibility=hidden",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wall",
            "-Wextra",
            "-Werror",
        ])
        .arg(format!("-I{}", jni_proxy_include.display()))
        .arg(format!("-I{}", jni_proxy_generated.display()))
        .arg("-c")
        .arg(root.join("tools/android-jni-proxy/src/proxy.c"))
        .arg("-o")
        .arg(&jni_proxy_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut jni_proxy_command,
            &jni_proxy_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(jni_proxy_object);
    let mut adapter_jobs = Vec::new();
    for adapter_source in [
        "darwin_android_jni_trampoline.cc",
        "darwin_android_elf_image_registry.cc",
        "darwin_provider_owners.cc",
        "darwin_framework_natives.cc",
        "darwin_icu_natives.cc",
        "darwin_icu_jni_bridge.cc",
        "darwin_libcore_natives.cc",
        "darwin_runtime_adapters.cc",
        "darwin_sigchain.cc",
        "fault_handler_arm64_darwin.cc",
    ] {
        if (real_graphics && adapter_source == "darwin_icu_natives.cc")
            || (!real_graphics && adapter_source == "darwin_icu_jni_bridge.cc")
        {
            continue;
        }
        let adapter_object = object_dir.join(format!("{adapter_source}.o"));
        let mut adapter_command = if real_graphics && adapter_source == "darwin_libcore_natives.cc"
        {
            let mut libcore_includes = includes.clone();
            libcore_includes
                .retain(|path| *path != Path::new("/opt/homebrew/opt/icu4c@78/include"));
            libcore_includes.insert(0, android_icu_i18n.as_path());
            libcore_includes.insert(0, android_icu_common.as_path());
            runtime_bootstrap_cpp_command(&libcore_includes)
        } else {
            runtime_bootstrap_cpp_command(&includes)
        };
        if real_graphics && adapter_source == "darwin_framework_natives.cc" {
            adapter_command
                .arg("-DDARWIN_ART_REAL_GRAPHICS")
                .arg("-I")
                .arg(&libcutils_include);
        }
        if real_graphics && adapter_source == "darwin_icu_jni_bridge.cc" {
            adapter_command.arg("-I").arg(root.join("include"));
        }
        if real_graphics && adapter_source == "darwin_libcore_natives.cc" {
            adapter_command.arg("-DDARWIN_ART_FULL_LIBCORE_LINUX");
        }
        if matches!(
            adapter_source,
            "darwin_runtime_adapters.cc" | "darwin_provider_owners.cc"
        ) {
            adapter_command
                .arg("-I")
                .arg(root.join("tools/bionic-provider-namespace/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-dso-lifecycle-facade/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-fs-facade/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-dns-facade/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-socket-broker-adapter/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-sendfile-facade/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-stdio-facade/include"))
                .arg("-I")
                .arg(root.join("tools/bionic-ioctl-facade/include"));
            adapter_command
                .arg("-I")
                .arg(root.join("tools/bionic-strftime-facade/include"));
        }
        if adapter_source == "darwin_android_elf_image_registry.cc" {
            adapter_command
                .arg("-I")
                .arg(root.join("tools/android-dl-iterate-phdr-provider/include"));
        }
        adapter_command
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(root.join("compat").join(adapter_source))
            .arg("-o")
            .arg(&adapter_object);
        adapter_jobs.push(PendingNativeCompile {
            command: adapter_command,
            object: adapter_object,
        });
    }
    let (adapter_objects, adapter_compiled, adapter_cached) =
        compile_pending_native(adapter_jobs, &compiler_identity)?;
    compiled_objects += adapter_compiled;
    cached_objects += adapter_cached;
    objects.extend(adapter_objects);
    let mut runtime_jobs = Vec::new();
    for source in sources {
        let object_name = source.replace('/', "_");
        let object = object_dir.join(format!("{object_name}.o"));
        let source_path = if matches!(
            source,
            "runtime.cc"
                | "class_linker.cc"
                | "thread.cc"
                | "thread_list.cc"
                | "gc/heap.cc"
                | "gc/collector/garbage_collector.cc"
                | "gc/collector/mark_compact.cc"
                | "gc/space/malloc_space.cc"
                | "gc/space/space.cc"
                | "entrypoints/quick/quick_alloc_entrypoints.cc"
                | "entrypoints/quick/quick_trampoline_entrypoints.cc"
                | "runtime_common.cc"
                | "oat/oat_file.cc"
                | "exec_utils.cc"
                | "signal_catcher.cc"
                | "nterp_helpers.cc"
                | "interpreter/mterp/nterp.cc"
        ) {
            patched_runtime.join(source)
        } else {
            runtime.join(source)
        };
        let mut compile_command = runtime_bootstrap_cpp_command(&includes);
        if matches!(
            source,
            "entrypoints/jni/jni_entrypoints.cc" | "oat/jni_stub_hash_map.cc"
        ) {
            // These two upstream TUs include the arm64 frame header through a
            // source-relative path. Select the Darwin PCS overlay before that
            // include guard can lock in Android's AAPCS64 stack layout.
            compile_command.args(["-include", "arch/arm64/jni_frame_arm64.h"]);
        }
        compile_command
            // Darwin has no system <elf.h>. The NDK copy is used as a
            // lowest-priority, headers-only definition of the Android ELF ABI.
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(source_path)
            .arg("-o")
            .arg(&object);
        runtime_jobs.push(PendingNativeCompile {
            command: compile_command,
            object,
        });
    }
    let (runtime_objects, runtime_compiled, runtime_cached) =
        compile_pending_native(runtime_jobs, &compiler_identity)?;
    compiled_objects += runtime_compiled;
    cached_objects += runtime_cached;
    for object in runtime_objects {
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected Runtime object format: {kind}").into());
        }
        objects.push(object);
    }
    let runtime_object = object_dir.join("runtime.cc.o");
    let symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_object))?;
    if !symbols.contains("_ZN3art7Runtime6Create") {
        return Err("compiled Runtime object does not export Runtime::Create".into());
    }
    file_hash_cache.save(&file_hash_cache_path)?;
    let archive = build_dir.join(if real_graphics {
        "libart-runtime-graphics-bootstrap-darwin.a"
    } else {
        "libart-runtime-bootstrap-darwin.a"
    });
    create_archive(&archive, &objects)?;
    println!(
        "{}: ART runtime initialization spine Mach-O objects={} compiled={} cached={} archive={}",
        if real_graphics {
            "build-runtime-graphics-bootstrap"
        } else {
            "build-runtime-bootstrap"
        },
        objects.len(),
        compiled_objects,
        cached_objects,
        archive.display()
    );
    Ok(())
}

pub(crate) fn compile_runtime_filesystem_probe(root: &Path, build_dir: &Path) -> Result<PathBuf> {
    let object = build_dir.join("darwin_art_runtime_filesystem_probe.cc.o");
    fs::create_dir_all(build_dir)?;
    let cache_path = build_dir.join("runtime-probe-file-hashes.cache");
    let mut cache = FileHashCache::load(&cache_path)?;
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = Command::new("clang++");
    command
        .args(["-std=c++20", "-fPIC", "-Wall", "-Wextra", "-c"])
        .arg(root.join("probes/runtime_filesystem_probe.cc"))
        .arg("-I")
        .arg(root.join("probes"))
        .arg("-I")
        .arg(root.join("tools/bionic-fs-facade/include"))
        .arg("-I")
        .arg(root.join("tools/bionic-ioctl-facade/include"))
        .arg("-o")
        .arg(&object);
    let _ = compile_with_dependency_cache(&mut command, &object, &compiler_identity, &mut cache)?;
    cache.save(&cache_path)?;
    Ok(object)
}

pub(crate) fn compile_runtime_network_probe(root: &Path, build_dir: &Path) -> Result<PathBuf> {
    let object = build_dir.join("darwin_art_runtime_network_probe.cc.o");
    let server_object = build_dir.join("darwin_art_runtime_network_server.cc.o");
    let phase_object = build_dir.join("darwin_art_runtime_network_phase.cc.o");
    fs::create_dir_all(build_dir)?;
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    for (source, output, cache_name) in [
        (
            "runtime_network_probe.cc",
            &server_object,
            "runtime-probe-network-server-hashes.cache",
        ),
        (
            "runtime_acceptance_phases.cc",
            &phase_object,
            "runtime-probe-network-phase-hashes.cache",
        ),
    ] {
        let cache_path = build_dir.join(cache_name);
        let mut cache = FileHashCache::load(&cache_path)?;
        let mut command = Command::new("clang++");
        command
            .args(["-std=c++20", "-fPIC", "-Wall", "-Wextra", "-c"])
            .arg(root.join("probes").join(source))
            .arg("-I")
            .arg(root.join("probes"))
            .arg("-I")
            .arg(root.join("_aosp/libnativehelper/include_jni"))
            .arg("-o")
            .arg(output);
        let _ =
            compile_with_dependency_cache(&mut command, output, &compiler_identity, &mut cache)?;
        cache.save(&cache_path)?;
    }
    run_command(
        Command::new("clang++")
            .args(["-r"])
            .arg(&server_object)
            .arg(&phase_object)
            .arg("-o")
            .arg(&object),
    )?;
    Ok(object)
}

/// Compile the small JNI orchestration phase independently from the heavy
/// HWUI/Skia implementation.  This keeps Activity/content validation changes
/// from invalidating the graphics RenderNode translation unit.
pub(crate) fn compile_runtime_graphics_phase(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
) -> Result<PathBuf> {
    let object = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| build_dir.join("darwin_art_runtime_graphics_phase.cc.o"));
    if let Some(parent) = object.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_path = build_dir.join("runtime-probe-graphics-phase-hashes.cache");
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .arg("-c")
        .arg(root.join("probes/runtime_graphics_phase.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

pub(crate) fn build_runtime_graphics_phase_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_graphics_phase.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("graphics phase output has no parent: {}", output.display()))?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/cmdline"),
        root.join("_aosp/art/libnativebridge/include"),
        root.join("_aosp/art/runtime"),
        root.join("_aosp/art/runtime/base"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let object = compile_runtime_graphics_phase(root, build_dir, &include_refs)?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-graphics-phase-probe: {}", output.display());
    Ok(())
}

/// Compile the pointer-input half of the graphics probe independently from
/// the HWUI/Skia-heavy presentation translation unit. Its dependency cache is
/// intentionally separate so input changes do not rebuild GPU replay code.
pub(crate) fn compile_runtime_graphics_input_probe(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
) -> Result<PathBuf> {
    let object = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_INPUT_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| build_dir.join("darwin_art_runtime_graphics_input.cc.o"));
    if let Some(parent) = object.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_path = build_dir.join("runtime-probe-graphics-input-hashes.cache");
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .arg("-c")
        .arg(root.join("probes/runtime_graphics_input.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

pub(crate) fn compile_runtime_hwui_probe(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
) -> Result<PathBuf> {
    let object = build_dir.join("darwin_art_runtime_hwui_probe.cc.o");
    fs::create_dir_all(build_dir)?;
    let cache_path = build_dir.join("runtime-probe-hwui-hashes.cache");
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .args([
            "-include",
            "mirror/object_reference.h",
            "-idirafter",
            ndk_arch_include
                .to_str()
                .ok_or("invalid NDK arch include path")?,
            "-idirafter",
            ndk_include.to_str().ok_or("invalid NDK include path")?,
            "-Wno-macro-redefined",
            "-DDARWIN_ART_REAL_GRAPHICS",
            "-DDARWIN_ART_HWUI_GPU",
            "-DDARWIN_ART_AOSP_COMPAT_LSEEK64",
            "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
            "-DLOG_TAG=\"DarwinArtHWUI\"",
            "-include",
            "log/log_main.h",
        ])
        .arg("-I")
        .arg(root.join("_aosp/external/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/effects"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/private"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/utils"))
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
        .arg(root.join("_aosp/system/core/libsystem/include"))
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
        .arg(root.join("_aosp/system/incremental_delivery/incfs/util/include"))
        .arg("-c")
        .arg(root.join("probes/runtime_hwui_probe.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

pub(crate) fn build_runtime_network_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_OUTPUT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_network_probe.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("native probe output has no parent: {}", output.display()))?;
    let object = compile_runtime_network_probe(root, build_dir)?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-network-probe: {}", output.display());
    Ok(())
}

pub(crate) fn build_runtime_graphics_input_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_INPUT_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_graphics_input.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("native probe output has no parent: {}", output.display()))?;
    let runtime = root.join("_aosp/art/runtime");
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/cmdline"),
        root.join("_aosp/art/libnativebridge/include"),
        root.join("_aosp/art/runtime"),
        runtime.join("base"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("tools/bionic-provider-namespace/include"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let object = compile_runtime_graphics_input_probe(root, build_dir, &include_refs)?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-graphics-input-probe: {}", output.display());
    Ok(())
}

pub(crate) fn build_runtime_filesystem_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_OUTPUT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_filesystem_probe.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("native probe output has no parent: {}", output.display()))?;
    let object = compile_runtime_filesystem_probe(root, build_dir)?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-filesystem-probe: {}", output.display());
    Ok(())
}

pub(crate) fn build_runtime_hwui_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_OUTPUT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_hwui_probe.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("native probe output has no parent: {}", output.display()))?;
    let runtime = root.join("_aosp/art/runtime");
    let build_paths = BuildPaths::from_root(root);
    let includes = [
        root.join("include"),
        root.join("compat"),
        build_paths.native_output("runtime-arm64/generated"),
        build_paths.native_output("runtime-graphics-bootstrap/patched-source/runtime"),
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
    let object = compile_runtime_hwui_probe(
        root,
        build_dir,
        &include_refs,
        &ndk_include,
        &ndk_arch_include,
    )?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-hwui-probe: {}", output.display());
    Ok(())
}

pub(crate) fn compile_cached_probe_tu(
    command: &mut Command,
    object: &Path,
    cache_path: &Path,
    compiler_identity: &str,
) -> Result<bool> {
    let mut cache = FileHashCache::load(cache_path)?;
    let compiled = compile_with_dependency_cache(command, object, compiler_identity, &mut cache)?;
    cache.save(cache_path)?;
    Ok(compiled)
}
