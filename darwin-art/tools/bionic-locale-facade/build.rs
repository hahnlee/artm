use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn run(command: &mut Command, description: &str) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("failed to launch {description}: {error}"));
    assert!(status.success(), "{description} failed");
}

fn output(command: &mut Command, description: &str) -> String {
    let result = command
        .output()
        .unwrap_or_else(|error| panic!("failed to launch {description}: {error}"));
    assert!(
        result.status.success(),
        "{description} failed: {}",
        String::from_utf8_lossy(&result.stderr)
    );
    String::from_utf8(result.stdout)
        .expect("tool output is UTF-8")
        .trim()
        .to_owned()
}

fn main() {
    assert_eq!(env::var("CARGO_CFG_TARGET_OS").as_deref(), Ok("macos"));
    assert_eq!(env::var("CARGO_CFG_TARGET_ARCH").as_deref(), Ok("aarch64"));
    let output_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set"));
    let sdk = output(
        Command::new("xcrun").args(["--sdk", "macosx", "--show-sdk-path"]),
        "macOS SDK lookup",
    );
    let provider = output_dir.join("provider.o");
    let errno = output_dir.join("errno.o");
    let archive = output_dir.join("libdarwin_art_bionic_locale.a");
    run(
        Command::new("clang++")
            .args([
                "-arch",
                "arm64",
                "-isysroot",
                &sdk,
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-builtin",
                "-fvisibility=hidden",
                "-Iinclude",
                "-c",
                "src/provider.cc",
                "-o",
            ])
            .arg(&provider),
        "locale provider compile",
    );
    run(
        Command::new("clang")
            .args([
                "-arch",
                "arm64",
                "-isysroot",
                &sdk,
                "-std=c17",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I../bionic-errno-tls/include",
                "-I../bionic-errno-tls/generated",
                "-c",
                "../bionic-errno-tls/src/errno_tls.c",
                "-o",
            ])
            .arg(&errno),
        "Bionic errno compile",
    );
    run(
        Command::new("ar")
            .arg("rcs")
            .arg(&archive)
            .arg(&provider)
            .arg(&errno),
        "locale provider archive",
    );
    println!("cargo:rustc-link-search=native={}", output_dir.display());
    println!("cargo:rustc-link-lib=static=darwin_art_bionic_locale");
    println!("cargo:rustc-link-lib=c++");
    for source in [
        "src/provider.cc",
        "include/darwin_art_bionic_locale.h",
        "../bionic-errno-tls/src/errno_tls.c",
        "../bionic-errno-tls/include/darwin_art_bionic_errno.h",
        "../bionic-errno-tls/generated/darwin_to_android.inc",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }
}

#[allow(dead_code)]
fn _path_type(_: &Path) {}
