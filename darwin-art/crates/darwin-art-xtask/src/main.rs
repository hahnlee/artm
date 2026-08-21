#![forbid(unsafe_code)]

use sha2::{Digest, Sha256};
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

mod graph;

use darwin_art_build_contract::RUNTIME_CACHE_IDENTITY;
use graph::emit::emit_graph;

const GRAPH_VERSION: &str = "darwin-art-native-graph-v13-cache-stamp-input";
const GRAPHICS_BOOTSTRAP_ARCHIVE: &str =
    "runtime-graphics-bootstrap/libart-runtime-graphics-bootstrap-darwin.a";
const RUNTIME_BOOTSTRAP_ARCHIVE: &str = "runtime-bootstrap/libart-runtime-bootstrap-darwin.a";
const HWUI_STATIC_FOUNDATION_ARCHIVE: &str = "hwui-static-foundation/libhwui-static-darwin.a";
const HWUI_APEX_FOUNDATION_ARCHIVE: &str =
    "hwui-static-foundation/libandroid-graphics-apex-common-darwin.a";
const ANDROID_GRAPHICS_JNI_ARCHIVE: &str = "android-graphics-jni/libandroid-graphics-jni-darwin.a";
const ANDROID_GRAPHICS_REGISTRAR_ARCHIVE: &str =
    "android-graphics-jni/libandroid-graphics-layoutlib-registrar-darwin.a";
const ANDROID_GRAPHICS_FORCE_LOADED_OBJECT: &str =
    "android-graphics-jni/android-graphics-jni-force-loaded.o";
const ICU_COMMON_FOUNDATION_ARCHIVE: &str = "icu-foundation/libicuuc-common-darwin.a";
const ICU_I18N_FOUNDATION_ARCHIVE: &str = "icu-foundation/libicui18n-darwin.a";
const ICU_STUBDATA_FOUNDATION_ARCHIVE: &str = "icu-foundation/libicuuc-stubdata-darwin.a";
const ICU_INIT_FOUNDATION_ARCHIVE: &str = "icu-foundation/libandroidicuinit-darwin.a";
const GRAPHICS_RUNTIME_LIBRARY: &str =
    "runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib";

const SDK_NAME: &str = "macosx";

const CXX_FLAGS: &[&str] = &[
    "-std=c++20",
    "-fPIC",
    "-Wall",
    "-Wextra",
    "-DDARWIN_ART_REAL_GRAPHICS",
    "-DDARWIN_ART_HWUI_GPU",
    "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
];

fn main() {
    if let Err(error) = run() {
        eprintln!("darwin-art-xtask: {error}");
        std::process::exit(2);
    }
}

fn run() -> Result<(), String> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        Some("native-graph") => {
            let mut out = None;
            while let Some(arg) = args.next() {
                match arg.as_str() {
                    "--out" => out = args.next().map(PathBuf::from),
                    value => return Err(format!("unknown native-graph option: {value}")),
                }
            }
            let out = out.ok_or_else(|| "native-graph requires --out <path>".to_owned())?;
            emit_graph(&out).map_err(|error| error.to_string())
        }
        _ => Err("usage: cargo run -p darwin-art-xtask -- native-graph --out <path>".to_owned()),
    }
}

fn repository_root(out: &Path) -> PathBuf {
    let start = if out.is_absolute() {
        out.parent().unwrap_or(out).to_path_buf()
    } else {
        env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
    };

    if let Some(root) = start
        .ancestors()
        .find(|candidate| candidate.join("Cargo.toml").is_file())
    {
        return root.to_path_buf();
    }
    // An absolute output path outside the checkout is useful for CI scratch
    // graphs. Fall back to the invocation directory before accepting an
    // unrelated path as the repository root.
    env::current_dir()
        .ok()
        .and_then(|current| {
            current
                .ancestors()
                .find(|candidate| candidate.join("Cargo.toml").is_file())
                .map(Path::to_path_buf)
        })
        .unwrap_or(start)
}

fn digest_inputs(root: &Path, inputs: &[PathBuf]) -> io::Result<String> {
    let mut digest = Sha256::new();
    digest.update(GRAPH_VERSION.as_bytes());
    digest.update([0]);
    digest.update(RUNTIME_CACHE_IDENTITY.as_bytes());
    for path in inputs {
        digest.update(path.to_string_lossy().as_bytes());
        digest.update([0]);
        digest.update(fs::read(root.join(path))?);
        digest.update([0]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn ninja_path(path: &Path) -> String {
    path.to_string_lossy().replace('$', "$$").replace(' ', "$ ")
}

fn shell_quote(path: &str) -> String {
    if path
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || b"_./-".contains(&byte))
    {
        path.to_owned()
    } else {
        format!("'{}'", path.replace('\'', "'\\''"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::graph::foundation::{FoundationFamily, is_foundation_family_input};
    use crate::graph::inputs::{graph_inputs, is_probe_only_input, probe_content_stamp};

    #[test]
    fn shell_quote_is_stable_for_normal_repository_paths() {
        assert_eq!(shell_quote("/tmp/darwin-art"), "/tmp/darwin-art");
        assert_eq!(shell_quote("/tmp/with space"), "'/tmp/with space'");
    }

    #[test]
    fn ninja_path_escapes_ninja_metacharacters() {
        assert_eq!(ninja_path(Path::new("a b/$c")), "a$ b/$$c");
    }

    #[test]
    fn graphics_bootstrap_archive_is_declared_at_stable_output_path() {
        let output_root = Path::new("_build");
        assert_eq!(
            output_root.join(GRAPHICS_BOOTSTRAP_ARCHIVE),
            PathBuf::from(
                "_build/runtime-graphics-bootstrap/\
                 libart-runtime-graphics-bootstrap-darwin.a"
            )
        );
    }

    #[test]
    fn native_graph_digest_excludes_rust_orchestration() {
        let paths = graph_inputs(Path::new("."));
        for excluded in [
            "Cargo.toml",
            "Cargo.lock",
            "crates/art-bootstrap/Cargo.toml",
            "crates/art-bootstrap/src/main.rs",
            "crates/art-bootstrap/src/build_context.rs",
            "crates/art-bootstrap/src/help.rs",
            "crates/darwin-art-elf-loader/Cargo.toml",
            "crates/darwin-art-xtask/Cargo.toml",
            "crates/darwin-art-xtask/src/main.rs",
        ] {
            assert!(
                !paths.iter().any(|path| path == Path::new(excluded)),
                "Rust orchestration path leaked into native graph digest: {excluded}"
            );
        }
    }

    #[test]
    fn probe_sources_do_not_invalidate_the_bootstrap_archive() {
        assert!(is_probe_only_input(Path::new(
            "probes/runtime_link_probe.cc"
        )));
        assert!(is_probe_only_input(Path::new(
            "compat/darwin_surface_bridge.mm"
        )));
        assert!(is_probe_only_input(Path::new(
            "compat/darwin_surface_gpu_bridge.mm"
        )));
        assert!(is_probe_only_input(Path::new(
            "probes/runtime_process_options.cc"
        )));
        assert!(!is_probe_only_input(Path::new(
            "compat/darwin_runtime_adapters.cc"
        )));
    }

    #[test]
    fn foundation_fallback_inputs_are_partitioned_by_owner() {
        let hwui_script = Path::new("tools/build-android16-hwui-static-foundation.sh");
        let graphics_jni_script = Path::new("tools/build-android16-android-graphics-jni.sh");
        let icu_script = Path::new("tools/build-android16-icu-foundation.sh");
        assert!(is_foundation_family_input(
            hwui_script,
            FoundationFamily::Hwui
        ));
        assert!(!is_foundation_family_input(
            hwui_script,
            FoundationFamily::Icu
        ));
        let hwui_animation_patch =
            Path::new("patches/frameworks-base/0005-darwin-hwui-animation-pulse.patch");
        assert!(is_foundation_family_input(
            hwui_animation_patch,
            FoundationFamily::Hwui
        ));
        assert!(!is_foundation_family_input(
            hwui_animation_patch,
            FoundationFamily::Icu
        ));
        assert!(is_foundation_family_input(
            graphics_jni_script,
            FoundationFamily::GraphicsJni
        ));
        assert!(!is_foundation_family_input(
            graphics_jni_script,
            FoundationFamily::Hwui
        ));
        assert!(is_foundation_family_input(
            icu_script,
            FoundationFamily::Icu
        ));
        assert!(!is_foundation_family_input(
            icu_script,
            FoundationFamily::GraphicsJni
        ));
        let icu_source = Path::new("_aosp/external/icu-graphics/icu4c/source/common/foo.cpp");
        let hwui_source = Path::new("_aosp/frameworks/base/libs/hwui/RenderNode.cpp");
        assert!(is_foundation_family_input(
            icu_source,
            FoundationFamily::Icu
        ));
        assert!(!is_foundation_family_input(
            icu_source,
            FoundationFamily::Hwui
        ));
        assert!(is_foundation_family_input(
            hwui_source,
            FoundationFamily::Hwui
        ));
        assert!(is_foundation_family_input(
            hwui_source,
            FoundationFamily::GraphicsJni
        ));
    }

    #[test]
    fn probe_content_stamp_changes_only_for_content_changes() {
        let root = std::env::temp_dir().join(format!(
            "darwin-art-probe-stamp-test-{}",
            std::process::id()
        ));
        let source = root.join("probes/state.cc");
        fs::create_dir_all(source.parent().expect("probe parent")).expect("probe directory");
        fs::write(&source, "state-v1\n").expect("initial probe source");
        let first =
            probe_content_stamp(&root, "state", &["probes/state.cc"]).expect("first content stamp");
        let first_content = fs::read_to_string(&first).expect("first stamp content");
        let second = probe_content_stamp(&root, "state", &["probes/state.cc"])
            .expect("stable content stamp");
        assert_eq!(first, second);
        assert_eq!(
            first_content,
            fs::read_to_string(&second).expect("stable stamp content")
        );
        fs::write(&source, "state-v2\n").expect("changed probe source");
        let third = probe_content_stamp(&root, "state", &["probes/state.cc"])
            .expect("changed content stamp");
        assert_ne!(
            first_content,
            fs::read_to_string(third).expect("changed stamp content")
        );
        fs::remove_dir_all(root).expect("probe stamp test cleanup");
    }
}
