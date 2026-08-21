use super::*;

pub(crate) fn workspace_root() -> Result<PathBuf> {
    Ok(PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()?)
}

pub(crate) fn build_shell_gate(root: &Path, script: &str) -> Result<()> {
    build_shell_gate_with_args(root, script, &[])
}

pub(crate) fn build_shell_gate_with_args(root: &Path, script: &str, args: &[&str]) -> Result<()> {
    let script = root.join("tools").join(script);
    if !script.is_file() {
        return Err(format!("build gate script is missing: {}", script.display()).into());
    }
    run_command(
        Command::new("bash")
            .arg(script)
            .args(args)
            .current_dir(root),
    )
}

pub(crate) fn doctor() -> Result<()> {
    if env::consts::OS != "macos" || env::consts::ARCH != "aarch64" {
        return Err(format!(
            "requires arm64 macOS, found {}-{}",
            env::consts::ARCH,
            env::consts::OS
        )
        .into());
    }

    #[cfg(target_os = "macos")]
    let page_size = unsafe { getpagesize() };

    #[cfg(not(target_os = "macos"))]
    let page_size = 0;

    if page_size != 16 * 1024 {
        return Err(format!("expected a 16 KiB page size, found {page_size}").into());
    }
    for dependency in [
        "/opt/homebrew/opt/icu4c@78/include/unicode/ucnv.h",
        "/opt/homebrew/opt/icu4c@78/lib/libicui18n.dylib",
        "/opt/homebrew/opt/icu4c@78/lib/libicuuc.dylib",
    ] {
        if !Path::new(dependency).is_file() {
            return Err(format!("Homebrew icu4c@78 dependency is missing: {dependency}").into());
        }
    }

    let clang = command_output(Command::new("clang").arg("--version"))?;
    let first_line = clang.lines().next().unwrap_or("unknown clang");
    println!("doctor: arm64 macOS, page_size={page_size}, {first_line}");
    Ok(())
}
