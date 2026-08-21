use super::*;

#[path = "graphics_hwui.rs"]
mod graphics_hwui;
#[path = "graphics_session.rs"]
mod graphics_session;
#[path = "graphics_state.rs"]
mod graphics_state;

pub(crate) use graphics_hwui::*;
pub(crate) use graphics_session::*;
pub(crate) use graphics_state::*;

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
