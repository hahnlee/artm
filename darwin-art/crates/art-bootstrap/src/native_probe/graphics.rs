use super::*;

pub(crate) fn compile_runtime_graphics_phase(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
) -> Result<PathBuf> {
    if let Some(path) = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
    }
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

/// Compile the DEX/JNI ABI acceptance matrix independently from the launcher
/// and Android window/resource setup. The phase has no process-global state;
/// it receives the already-created ART/JNI handles at the call boundary.
pub(crate) fn compile_runtime_jni_acceptance_probe(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
) -> Result<PathBuf> {
    if let Some(path) = env::var_os("DARWIN_ART_NATIVE_JNI_ACCEPTANCE_OBJECT") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
    }
    let object = env::var_os("DARWIN_ART_NATIVE_JNI_ACCEPTANCE_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| build_dir.join("darwin_art_runtime_jni_acceptance_probe.cc.o"));
    if let Some(parent) = object.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_path = build_dir.join("runtime-probe-jni-acceptance-hashes.cache");
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .arg("-include")
        .arg("mirror/object_reference.h")
        .arg("-c")
        .arg(root.join("probes/runtime_jni_acceptance_probe.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

pub(crate) fn build_runtime_jni_acceptance_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_JNI_ACCEPTANCE_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_jni_acceptance_probe.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("JNI acceptance output has no parent: {}", output.display()))?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/runtime"),
        root.join("_aosp/art/runtime/base"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/libprofile"),
        root.join("_aosp/art/libnativebridge/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let object = compile_runtime_jni_acceptance_probe(root, build_dir, &include_refs)?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-jni-acceptance-probe: {}", output.display());
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
    if let Some(path) = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_INPUT_OBJECT") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
    }
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

pub(crate) fn compile_runtime_graphics_state_probe(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
) -> Result<PathBuf> {
    compile_runtime_graphics_state_probe_flavor(
        root,
        build_dir,
        includes,
        ndk_include,
        ndk_arch_include,
        true,
    )
}

/// Compile the state bridge for the direct-APK/headless flavor.  That flavor
/// deliberately does not pull HWUI/RenderThread types into its ABI; it still
/// needs the same opaque GraphicsState layout for the common lifecycle code.
pub(crate) fn compile_runtime_graphics_state_probe_cpu(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
) -> Result<PathBuf> {
    compile_runtime_graphics_state_probe_flavor(
        root,
        build_dir,
        includes,
        ndk_include,
        ndk_arch_include,
        false,
    )
}

fn compile_runtime_graphics_state_probe_flavor(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
    real_graphics: bool,
) -> Result<PathBuf> {
    let object = if real_graphics {
        env::var_os("DARWIN_ART_NATIVE_GRAPHICS_STATE_OBJECT")
            .map(PathBuf::from)
            .unwrap_or_else(|| build_dir.join("darwin_art_runtime_graphics_state.cc.o"))
    } else {
        build_dir.join("darwin_art_runtime_graphics_state_cpu.cc.o")
    };
    if let Some(parent) = object.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_path = if real_graphics {
        build_dir.join("runtime-probe-graphics-state-hashes.cache")
    } else {
        build_dir.join("runtime-probe-graphics-state-cpu-hashes.cache")
    };
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(ndk_arch_include)
        .arg("-idirafter")
        .arg(ndk_include)
        .args(["-Wno-macro-redefined", "-DDARWIN_ART_AOSP_COMPAT_LSEEK64"]);
    if real_graphics {
        command
            .args([
                "-DDARWIN_ART_REAL_GRAPHICS",
                "-DDARWIN_ART_HWUI_GPU",
                "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
            ])
            .args([
                "-I",
                root.join("_aosp/frameworks/base/libs/hwui")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/frameworks/base/libs/hwui/hwui")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia")
                    .to_str()
                    .unwrap(),
            ])
            .args(["-I", root.join("_aosp/external/skia").to_str().unwrap()])
            .args([
                "-I",
                root.join("_aosp/external/skia/include/core")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/system/core/libcutils/include")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/system/core/libutils/include")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/system/core/libsystem/include")
                    .to_str()
                    .unwrap(),
            ])
            .args([
                "-I",
                root.join("_aosp/system/logging/liblog/include")
                    .to_str()
                    .unwrap(),
            ]);
    }
    command
        .arg("-c")
        .arg(root.join("probes/runtime_graphics_state.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

pub(crate) fn build_runtime_graphics_state_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_STATE_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_graphics_state.cc.o")
        });
    let build_dir = output
        .parent()
        .ok_or_else(|| format!("graphics state output has no parent: {}", output.display()))?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/runtime"),
        root.join("_aosp/art/runtime/base"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("_aosp/external/skia"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    let object = compile_runtime_graphics_state_probe(
        root,
        build_dir,
        &include_refs,
        &ndk_include,
        &ndk_arch_include,
    )?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-graphics-state-probe: {}", output.display());
    Ok(())
}

pub(crate) fn compile_runtime_graphics_session_probe(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
) -> Result<PathBuf> {
    compile_runtime_graphics_session_probe_flavor(
        root,
        build_dir,
        includes,
        ndk_include,
        ndk_arch_include,
        true,
    )
}

pub(crate) fn compile_runtime_graphics_session_probe_cpu(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
) -> Result<PathBuf> {
    compile_runtime_graphics_session_probe_flavor(
        root,
        build_dir,
        includes,
        ndk_include,
        ndk_arch_include,
        false,
    )
}

fn compile_runtime_graphics_session_probe_flavor(
    root: &Path,
    build_dir: &Path,
    includes: &[&Path],
    ndk_include: &Path,
    ndk_arch_include: &Path,
    real_graphics: bool,
) -> Result<PathBuf> {
    if real_graphics && let Some(path) = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_SESSION_OBJECT") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
    }
    let object = if real_graphics {
        env::var_os("DARWIN_ART_NATIVE_GRAPHICS_SESSION_OBJECT")
            .map(PathBuf::from)
            .unwrap_or_else(|| build_dir.join("darwin_art_runtime_graphics_session.cc.o"))
    } else {
        build_dir.join("darwin_art_runtime_graphics_session_cpu.cc.o")
    };
    if let Some(parent) = object.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_path = if real_graphics {
        build_dir.join("runtime-probe-graphics-session-hashes.cache")
    } else {
        build_dir.join("runtime-probe-graphics-session-cpu-hashes.cache")
    };
    let compiler_identity = command_output(Command::new("clang++").arg("--version"))?;
    let mut command = runtime_cpp_command(includes);
    command
        .args(["-include", "mirror/object_reference.h"])
        .arg("-idirafter")
        .arg(ndk_arch_include)
        .arg("-idirafter")
        .arg(ndk_include)
        .args(["-Wno-macro-redefined", "-DDARWIN_ART_AOSP_COMPAT_LSEEK64"]);
    if real_graphics {
        command
            .args([
                "-DDARWIN_ART_REAL_GRAPHICS",
                "-DDARWIN_ART_HWUI_GPU",
                "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
            ])
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui/hwui"))
            .arg("-I")
            .arg(root.join("_aosp/frameworks/base/libs/hwui/pipeline/skia"));
    }
    command
        .arg("-I")
        .arg(root.join("_aosp/art/libdexfile"))
        .arg("-I")
        .arg(root.join("_aosp/art/libelffile"))
        .arg("-I")
        .arg(root.join("_aosp/art/libprofile"))
        .arg("-I")
        .arg(root.join("_aosp/art/libnativebridge/include"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia"))
        .arg("-I")
        .arg(root.join("_aosp/external/skia/include/core"))
        .arg("-I")
        .arg(root.join("_aosp/external/tinyxml2"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libcutils/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libutils/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/core/libsystem/include"))
        .arg("-I")
        .arg(root.join("_aosp/system/logging/liblog/include"))
        .arg("-c")
        .arg(root.join("probes/runtime_graphics_session.cc"))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, &cache_path, &compiler_identity)?;
    Ok(object)
}

/// Compile the small JavaVMExt network-loader boundary separately from the
/// large managed Activity entry probe.  Network fixture changes now
/// invalidate this TU only instead of recompiling the managed entry point.
pub(crate) fn build_runtime_graphics_session_probe(root: &Path) -> Result<()> {
    let output = env::var_os("DARWIN_ART_NATIVE_GRAPHICS_SESSION_OBJECT")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            root.join("_build/runtime-link-probe/darwin_art_runtime_graphics_session.cc.o")
        });
    let build_dir = output.parent().ok_or_else(|| {
        format!(
            "graphics session output has no parent: {}",
            output.display()
        )
    })?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/runtime"),
        root.join("_aosp/art/runtime/base"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("_aosp/external/skia"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    let object = compile_runtime_graphics_session_probe(
        root,
        build_dir,
        &include_refs,
        &ndk_include,
        &ndk_arch_include,
    )?;
    if object != output {
        fs::copy(&object, &output)?;
    }
    println!("build-runtime-graphics-session-probe: {}", output.display());
    Ok(())
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
