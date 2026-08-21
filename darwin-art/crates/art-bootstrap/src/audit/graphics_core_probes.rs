use super::*;

pub(super) struct CoreProbeObjects {
    pub elf: PathBuf,
    pub abi: PathBuf,
    pub process_state: PathBuf,
    pub process_options: PathBuf,
    pub shutdown: PathBuf,
    pub frame: PathBuf,
}

pub(super) fn compile_core_probe_objects(
    root: &Path,
    build_dir: &Path,
    include_refs: &[&Path],
    probe_cache: &Path,
    compiler_identity: &str,
) -> Result<CoreProbeObjects> {
    Ok(CoreProbeObjects {
        elf: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_elf_probe.cc",
            "darwin_art_runtime_elf_probe.cc.o",
        )?,
        abi: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_abi_probe.cc",
            "darwin_art_runtime_abi_probe.cc.o",
        )?,
        process_state: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_process_state.cc",
            "darwin_art_runtime_process_state.cc.o",
        )?,
        process_options: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_process_options.cc",
            "darwin_art_runtime_process_options.cc.o",
        )?,
        shutdown: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_shutdown_probe.cc",
            "darwin_art_runtime_shutdown_probe.cc.o",
        )?,
        frame: compile_probe(
            root,
            build_dir,
            include_refs,
            probe_cache,
            compiler_identity,
            "runtime_frame_probe.cc",
            "darwin_art_runtime_frame_probe.cc.o",
        )?,
    })
}

fn compile_probe(
    root: &Path,
    build_dir: &Path,
    include_refs: &[&Path],
    probe_cache: &Path,
    compiler_identity: &str,
    source: &str,
    object_name: &str,
) -> Result<PathBuf> {
    let object = build_dir.join(object_name);
    let mut command = runtime_cpp_command(include_refs);
    command
        .arg("-c")
        .arg(root.join("probes").join(source))
        .arg("-o")
        .arg(&object);
    let _ = compile_cached_probe_tu(&mut command, &object, probe_cache, compiler_identity)?;
    Ok(object)
}
