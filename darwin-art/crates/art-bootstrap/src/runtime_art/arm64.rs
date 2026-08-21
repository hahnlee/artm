use super::*;

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
