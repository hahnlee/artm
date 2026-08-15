use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn output(command: &mut Command, description: &str) -> String {
    let result = command
        .output()
        .unwrap_or_else(|error| panic!("failed to launch {description}: {error}"));
    assert!(result.status.success(), "{description} failed");
    String::from_utf8(result.stdout).unwrap().trim().to_owned()
}

fn compile(source: &str, object: &Path, sdk: &str, includes: &[&str]) {
    let mut command = Command::new("clang");
    command.args([
        "-arch",
        "arm64",
        "-isysroot",
        sdk,
        "-std=c17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
    ]);
    for include in includes {
        command.arg(format!("-I{include}"));
    }
    let status = command
        .args(["-c", source, "-o"])
        .arg(object)
        .status()
        .unwrap();
    assert!(status.success(), "clang failed for {source}");
}

fn main() {
    assert_eq!(env::var("CARGO_CFG_TARGET_OS").as_deref(), Ok("macos"));
    assert_eq!(env::var("CARGO_CFG_TARGET_ARCH").as_deref(), Ok("aarch64"));
    let output_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let sdk = output(
        Command::new("xcrun").args(["--sdk", "macosx", "--show-sdk-path"]),
        "SDK lookup",
    );
    let shims = output_dir.join("shims.o");
    let errno = output_dir.join("errno.o");
    let archive = output_dir.join("libdarwin_art_bionic_process_state.a");
    compile("src/shims.c", &shims, &sdk, &["include"]);
    compile(
        "../bionic-errno-tls/src/errno_tls.c",
        &errno,
        &sdk,
        &[
            "../bionic-errno-tls/include",
            "../bionic-errno-tls/generated",
        ],
    );
    assert!(
        Command::new("ar")
            .arg("rcs")
            .arg(&archive)
            .arg(&shims)
            .arg(&errno)
            .status()
            .unwrap()
            .success()
    );
    println!("cargo:rustc-link-search=native={}", output_dir.display());
    println!("cargo:rustc-link-lib=static=darwin_art_bionic_process_state");
    for source in [
        "src/shims.c",
        "include/darwin_art_bionic_process_state.h",
        "../bionic-errno-tls/src/errno_tls.c",
        "../bionic-errno-tls/include/darwin_art_bionic_errno.h",
        "../bionic-errno-tls/generated/darwin_to_android.inc",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }
}
