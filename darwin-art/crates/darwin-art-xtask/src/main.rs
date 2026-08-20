#![forbid(unsafe_code)]

use sha2::{Digest, Sha256};
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

const GRAPH_VERSION: &str = "darwin-art-native-graph-v4";
const GRAPHICS_BOOTSTRAP_ARCHIVE: &str =
    "runtime-graphics-bootstrap/libart-runtime-graphics-bootstrap-darwin.a";
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RepresentativeEdge {
    name: &'static str,
    source: &'static str,
    headers: &'static [&'static str],
    objc: bool,
}

const REPRESENTATIVE_EDGES: &[RepresentativeEdge] = &[
    RepresentativeEdge {
        name: "elf-image-registry",
        source: "compat/darwin_android_elf_image_registry.cc",
        headers: &[
            "compat/darwin_android_elf_image_registry.h",
            "crates/darwin-art-elf-loader/include/darwin_art_elf_loader.h",
            "tools/android-dl-iterate-phdr-provider/include/darwin_art_dl_iterate_phdr.h",
        ],
        objc: false,
    },
    RepresentativeEdge {
        name: "asynchronous-close-monitor",
        source: "compat/darwin_asynchronous_close_monitor.cc",
        headers: &["compat/AsynchronousCloseMonitor.h"],
        objc: false,
    },
    RepresentativeEdge {
        name: "android-base-logging",
        source: "compat/android_base_logging.cc",
        headers: &[
            "compat/log/log.h",
            "_aosp/system/libbase/include/android-base/logging.h",
        ],
        objc: false,
    },
];

#[derive(Clone, Debug, Eq, PartialEq)]
struct ToolchainInputs {
    cxx: String,
    sdkroot: String,
}

fn toolchain_inputs() -> ToolchainInputs {
    ToolchainInputs {
        cxx: env::var("DARWIN_ART_CXX")
            .or_else(|_| env::var("CXX"))
            .unwrap_or_else(|_| "clang++".to_owned()),
        sdkroot: env::var("DARWIN_ART_SDKROOT")
            .or_else(|_| env::var("SDKROOT"))
            .unwrap_or_default(),
    }
}

fn json_escape(value: &str) -> String {
    value
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}

fn edge_digest(
    root: &Path,
    edge: &RepresentativeEdge,
    toolchain: &ToolchainInputs,
) -> io::Result<String> {
    let mut digest = Sha256::new();
    digest.update(GRAPH_VERSION.as_bytes());
    digest.update(edge.name.as_bytes());
    digest.update(edge.source.as_bytes());
    digest.update(toolchain.cxx.as_bytes());
    digest.update(toolchain.sdkroot.as_bytes());
    for flag in CXX_FLAGS {
        digest.update(flag.as_bytes());
        digest.update([0]);
    }
    for path in std::iter::once(edge.source).chain(edge.headers.iter().copied()) {
        digest.update(path.as_bytes());
        digest.update([0]);
        digest.update(fs::read(root.join(path))?);
        digest.update([0]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn emit_representative_edges(
    graph: &mut String,
    root: &Path,
    cache_dir: &Path,
    toolchain: &ToolchainInputs,
) -> io::Result<()> {
    // These edges are an opt-in preflight target until the bootstrap command
    // consumes their objects directly. Keeping them off the default audit
    // prevents the transitional graph from doing duplicate compilation.
    let object_dir = cache_dir.join("native-tu");
    graph.push_str("rule native_representative_cpp\n");
    graph.push_str("  command = mkdir -p ");
    graph.push_str(&shell_quote(&object_dir.to_string_lossy()));
    graph.push_str(" && ");
    graph.push_str(&shell_quote(&toolchain.cxx));
    for flag in CXX_FLAGS {
        graph.push(' ');
        graph.push_str(flag);
    }
    for include in [
        "compat",
        "include",
        "crates/darwin-art-elf-loader/include",
        "_aosp/art/runtime",
        "_aosp/art/libartbase",
        "_build/foundation/patched-source/libartbase",
        "tools/android-dl-iterate-phdr-provider/include",
        "_aosp/system/libbase/include",
        "_aosp/libnativehelper/include_jni",
    ] {
        let include_path = root.join(include);
        if include_path.is_dir() {
            graph.push_str(" -I");
            graph.push_str(&shell_quote(&include_path.to_string_lossy()));
        }
    }
    if !toolchain.sdkroot.is_empty() {
        graph.push_str(" -isysroot ");
        graph.push_str(&shell_quote(&toolchain.sdkroot));
    }
    graph.push_str(" -MMD -MF $depfile -c $in -o $out\n");
    graph.push_str("  depfile = $out.d\n");
    graph.push_str("  deps = gcc\n");
    graph.push_str("  description = CXX $out\n\n");

    let mut outputs = Vec::new();
    for edge in REPRESENTATIVE_EDGES {
        let digest = edge_digest(root, edge, toolchain)?;
        let object = object_dir.join(format!("{}-{digest}.o", edge.name));
        let output = ninja_path(&object);
        let source = ninja_path(&root.join(edge.source));
        graph.push_str("build ");
        graph.push_str(&output);
        graph.push_str(": native_representative_cpp ");
        graph.push_str(&source);
        for header in edge.headers {
            graph.push(' ');
            graph.push_str(&ninja_path(&root.join(header)));
        }
        graph.push('\n');
        outputs.push(output);
    }
    graph.push_str("build native-tu-preflight: phony ");
    graph.push_str(&outputs.join(" "));
    graph.push('\n');
    Ok(())
}

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

fn emit_graph(out: &Path) -> io::Result<()> {
    let root = repository_root(out);
    let inputs = graph_inputs(&root);
    let digest = digest_inputs(&root, &inputs)?;
    let toolchain = toolchain_inputs();
    if let Some(parent) = out.parent() {
        fs::create_dir_all(parent)?;
    }
    let cache_dir = root.join("_build/native-cache").join(&digest);
    fs::create_dir_all(&cache_dir)?;
    let digest_path = cache_dir.join("inputs.sha256");
    fs::write(&digest_path, format!("{digest}  {GRAPH_VERSION}\n"))?;
    fs::write(
        cache_dir.join("manifest.json"),
        format!(
            "{{\"graph_version\":\"{GRAPH_VERSION}\",\"digest\":\"{digest}\",\"input_count\":{},\"compiler\":\"{}\",\"sdk\":\"{}\",\"edges\":[{}]}}\n",
            inputs.len(),
            json_escape(&toolchain.cxx),
            SDK_NAME,
            REPRESENTATIVE_EDGES
                .iter()
                .map(|edge| {
                    format!(
                        "{{\"name\":\"{}\",\"digest\":\"{}\",\"source\":\"{}\",\"language\":\"{}\"}}",
                        edge.name,
                        edge_digest(&root, edge, &toolchain).unwrap_or_else(|_| "missing".into()),
                        edge.source,
                        if edge.objc { "objc++" } else { "c++" }
                    )
                })
                .collect::<Vec<_>>()
                .join(",")
        ),
    )?;

    let root_for_shell = root.to_string_lossy().into_owned();
    let native_output_root = cache_dir.join("outputs");
    let archive_path = native_output_root.join(GRAPHICS_BOOTSTRAP_ARCHIVE);
    let runtime_library_path = native_output_root.join(GRAPHICS_RUNTIME_LIBRARY);
    let filesystem_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_filesystem_probe.cc.o");
    let stamp_path = cache_dir.join("graphics-bootstrap.stamp");
    let stamp = ninja_path(&stamp_path);
    let archive = ninja_path(&archive_path);
    let runtime_library = ninja_path(&runtime_library_path);
    let filesystem_object = ninja_path(&filesystem_object_path);
    let stamp_for_shell = stamp_path.to_string_lossy().into_owned();
    let native_output_for_shell = native_output_root.to_string_lossy().into_owned();
    let filesystem_object_for_shell = filesystem_object_path.to_string_lossy().into_owned();
    let input_list = inputs
        .iter()
        .map(|path| ninja_path(&root.join(path)))
        .collect::<Vec<_>>()
        .join(" ");

    let mut graph = String::new();
    graph.push_str("# Generated by darwin-art-xtask. Do not edit.\n");
    graph.push_str(&format!("# graph-version: {GRAPH_VERSION}\n"));
    graph.push_str(&format!("# input-digest: {digest}\n"));
    graph.push_str(&format!("# compiler: {} sdk: {SDK_NAME}\n", toolchain.cxx));
    graph.push_str("ninja_required_version = 1.10\n\n");
    graph.push_str("rule graphics_bootstrap\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
    graph.push_str(&shell_quote(&native_output_for_shell));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-bootstrap && touch ");
    graph.push_str(&shell_quote(&stamp_for_shell));
    graph.push('\n');
    graph.push_str("  description = GRAPHICS bootstrap\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&stamp);
    graph.push(' ');
    graph.push_str(&archive);
    graph.push_str(": graphics_bootstrap ");
    graph.push_str(&input_list);
    graph.push('\n');
    graph.push_str("build graphics-bootstrap: phony ");
    graph.push_str(&stamp);
    graph.push('\n');
    graph.push_str("rule runtime_filesystem_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT=");
    graph.push_str(&shell_quote(&filesystem_object_for_shell));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-filesystem-probe\n");
    graph.push_str("  description = CXX runtime_filesystem_probe\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&filesystem_object);
    graph.push_str(": runtime_filesystem_probe ");
    graph.push_str(&input_list);
    graph.push('\n');
    graph.push_str("rule graphics_audit\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
    graph.push_str(&shell_quote(&native_output_for_shell));
    graph.push_str(" DARWIN_ART_NATIVE_FILESYSTEM_OBJECT=");
    graph.push_str(&shell_quote(&filesystem_object_for_shell));
    graph.push_str(" cargo run -p art-bootstrap -- audit-runtime-graphics-link\n");
    graph.push_str("  description = GRAPHICS link/audit\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&runtime_library);
    graph.push_str(": graphics_audit ");
    graph.push_str(&archive);
    graph.push(' ');
    graph.push_str(&filesystem_object);
    graph.push('\n');
    graph.push_str("build graphics-audit: phony ");
    graph.push_str(&runtime_library);
    graph.push('\n');
    graph.push_str("build graph-input-digest: phony ");
    graph.push_str(&ninja_path(&digest_path));
    graph.push('\n');

    emit_representative_edges(&mut graph, &root, &cache_dir, &toolchain)?;

    let mut file = fs::File::create(out)?;
    file.write_all(graph.as_bytes())?;
    println!(
        "darwin-art-xtask: wrote {} (inputs={} digest={digest})",
        out.display(),
        inputs.len()
    );
    Ok(())
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

fn graph_inputs(root: &Path) -> Vec<PathBuf> {
    let mut paths = vec![
        PathBuf::from("Cargo.toml"),
        PathBuf::from("Cargo.lock"),
        PathBuf::from("sources.lock"),
        PathBuf::from("bootclasspath.lock"),
        PathBuf::from("crates/art-bootstrap/Cargo.toml"),
        PathBuf::from("crates/art-bootstrap/src/main.rs"),
        PathBuf::from("crates/art-bootstrap/src/build_context.rs"),
        PathBuf::from("crates/art-bootstrap/src/help.rs"),
        PathBuf::from("crates/darwin-art-elf-loader/Cargo.toml"),
        PathBuf::from("crates/darwin-art-xtask/Cargo.toml"),
        PathBuf::from("crates/darwin-art-xtask/src/main.rs"),
        PathBuf::from("tools/build-android-elf-jni-fixture.sh"),
        PathBuf::from("tools/audit-android16-graphics-closure.sh"),
        PathBuf::from("probes/runtime_filesystem_probe.cc"),
        PathBuf::from("probes/runtime_filesystem_probe.h"),
        PathBuf::from("probes/runtime_network_probe.cc"),
        PathBuf::from("probes/runtime_network_probe.h"),
        PathBuf::from("probes/runtime_apk_graph.cc"),
        PathBuf::from("probes/runtime_apk_graph.h"),
    ];
    // Keep this graph tied to the production bootstrap closure. In
    // particular, acceptance probes and unrelated graphics gates should not
    // invalidate the runtime archive cache.
    for directory in [
        "compat",
        "include",
        "patches/art",
        "crates/darwin-art-elf-loader/src",
        "tools/android-jni-proxy/include",
        "tools/android-jni-proxy/generated",
        "tools/android-dl-iterate-phdr-provider/include",
        "tools/bionic-dns-facade/include",
        "tools/bionic-dso-lifecycle-facade/include",
        "tools/bionic-fs-facade/include",
        "tools/bionic-ioctl-facade/include",
        "tools/bionic-provider-namespace/include",
        "tools/bionic-sendfile-facade/include",
        "tools/bionic-socket-broker-adapter/include",
        "tools/bionic-stdio-facade/include",
        "tools/bionic-strftime-facade/include",
    ] {
        collect_files(&root.join(directory), root, &mut paths);
    }
    for script in [
        "build-bionic-runtime-provider-closure.sh",
        "build-android16-android-runtime-host.sh",
        "build-android16-libcore-darwin-linux.sh",
        "build-android16-os-constants-darwin.sh",
        "build-android16-unix-filesystem-darwin.sh",
        "build-android16-openjdkjvm-darwin.sh",
        "build-android16-file-input-stream-darwin.sh",
        "build-android16-file-descriptor-darwin.sh",
        "build-android16-system-natives-darwin.sh",
        "build-android16-unix-native-dispatcher-darwin.sh",
        "build-android16-openjdk-nio-mapping.sh",
        "build-android16-libcore-memory-darwin.sh",
        "build-android16-android-util-log.sh",
        "build-android16-virtual-ref-base-ptr.sh",
    ] {
        paths.push(PathBuf::from("tools").join(script));
    }
    paths.extend([
        PathBuf::from("probes/android-elf-jni-fixture/child.c"),
        PathBuf::from("probes/android-elf-jni-fixture/child.exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_child.c"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_child.exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_grandchild.c"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_grandchild.exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_root.c"),
        PathBuf::from("probes/android-elf-jni-fixture/generic_root.exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/grandchild.c"),
        PathBuf::from("probes/android-elf-jni-fixture/grandchild.exports.map"),
        PathBuf::from("probes/android-elf-jni-fixture/host_provider.c"),
        PathBuf::from("probes/android-elf-jni-fixture/native_fixture.c"),
        PathBuf::from("tools/android-jni-proxy/src/proxy.c"),
        PathBuf::from("tools/android-jni-proxy/sources.lock"),
    ]);
    paths.sort();
    paths.dedup();
    paths.retain(|path| root.join(path).is_file());
    paths
}

fn collect_files(directory: &Path, root: &Path, output: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_files(&path, root, output);
        } else if path.is_file()
            && let Ok(relative) = path.strip_prefix(root)
        {
            output.push(relative.to_path_buf());
        }
    }
}

fn digest_inputs(root: &Path, inputs: &[PathBuf]) -> io::Result<String> {
    let mut digest = Sha256::new();
    digest.update(GRAPH_VERSION.as_bytes());
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
    fn graphics_bootstrap_archive_is_declared_under_cache_outputs() {
        let output_root = Path::new("_build/native-cache/digest/outputs");
        assert_eq!(
            output_root.join(GRAPHICS_BOOTSTRAP_ARCHIVE),
            PathBuf::from(
                "_build/native-cache/digest/outputs/runtime-graphics-bootstrap/\
                 libart-runtime-graphics-bootstrap-darwin.a"
            )
        );
    }
}
