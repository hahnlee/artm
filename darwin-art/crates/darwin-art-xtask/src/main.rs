#![forbid(unsafe_code)]

use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

const GRAPH_VERSION: &str = "darwin-art-native-graph-v6";
const GRAPHICS_BOOTSTRAP_ARCHIVE: &str =
    "runtime-graphics-bootstrap/libart-runtime-graphics-bootstrap-darwin.a";
const RUNTIME_BOOTSTRAP_ARCHIVE: &str = "runtime-bootstrap/libart-runtime-bootstrap-darwin.a";
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

struct CachedNativeObject {
    object: PathBuf,
    source: PathBuf,
    command: String,
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
    // These edges are deliberately separate from the ART bootstrap archive:
    // they are small, independently cacheable production-side TUs and make
    // the graph's object ownership observable without duplicating the 200+
    // upstream ART objects in the transitional archive builder.
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
    // The bootstrap archive must not be invalidated by probe-only sources.
    // Those sources are linked by the final dylib edge and therefore have a
    // narrower invalidation boundary than the provider/runtime archive.
    let bootstrap_inputs = inputs
        .iter()
        .filter(|path| !is_probe_only_input(path))
        .cloned()
        .collect::<Vec<_>>();
    let digest = digest_inputs(&root, &bootstrap_inputs)?;
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
            bootstrap_inputs.len(),
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
    // Keep compiler outputs at a stable path.  The graph digest is a
    // manifest/invalidation identity, not an object-cache namespace: moving
    // objects into a new digest directory would turn every header edit into
    // a cold 200+ TU rebuild and defeat dependency-fingerprint caching.
    let native_output_root = root.join("_build");
    let archive_path = native_output_root.join(GRAPHICS_BOOTSTRAP_ARCHIVE);
    let runtime_archive_path = native_output_root.join(RUNTIME_BOOTSTRAP_ARCHIVE);
    let runtime_library_path = native_output_root.join(GRAPHICS_RUNTIME_LIBRARY);
    let filesystem_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_filesystem_probe.cc.o");
    let network_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_network_probe.cc.o");
    let hwui_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_hwui_probe.cc.o");
    let graphics_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_probe.cc.o");
    let stamp_path = cache_dir.join("graphics-bootstrap.stamp");
    let runtime_stamp_path = cache_dir.join("runtime-bootstrap.stamp");
    let stamp = ninja_path(&stamp_path);
    let runtime_stamp = ninja_path(&runtime_stamp_path);
    let archive = ninja_path(&archive_path);
    let runtime_archive = ninja_path(&runtime_archive_path);
    let runtime_library = ninja_path(&runtime_library_path);
    let filesystem_object = ninja_path(&filesystem_object_path);
    let network_object = ninja_path(&network_object_path);
    let hwui_object = ninja_path(&hwui_object_path);
    let graphics_object = ninja_path(&graphics_object_path);
    let stamp_for_shell = stamp_path.to_string_lossy().into_owned();
    let native_output_for_shell = native_output_root.to_string_lossy().into_owned();
    let cached_runtime_objects = cached_native_objects(
        &native_output_root.join("runtime-bootstrap/objects"),
        &runtime_archive_path,
    )?;
    let cached_graphics_objects = cached_native_objects(
        &native_output_root.join("runtime-graphics-bootstrap/objects"),
        &archive_path,
    )?;
    let filesystem_object_for_shell = filesystem_object_path.to_string_lossy().into_owned();
    let network_object_for_shell = network_object_path.to_string_lossy().into_owned();
    let bootstrap_input_list = bootstrap_inputs
        .iter()
        .map(|path| ninja_path(&root.join(path)))
        .collect::<Vec<_>>()
        .join(" ");
    let probe_only_input_list = inputs
        .iter()
        .filter(|path| is_probe_only_input(path))
        .map(|path| ninja_path(&root.join(path)))
        .collect::<Vec<_>>()
        .join(" ");
    // Probe objects are separate graph products.  Do not attach the complete
    // bootstrap input closure to each one: that turns an edit to an unrelated
    // probe/provider into a rebuild of every probe.  The compiler writes the
    // real transitive dependency list to `$out.d`; the explicit inputs below
    // seed Ninja's first build and keep the ownership boundary readable.
    let filesystem_probe_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_filesystem_probe.cc",
            "probes/runtime_filesystem_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-ioctl-facade/include/darwin_art_bionic_ioctl.h",
        ],
    );
    let network_probe_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_network_probe.cc",
            "probes/runtime_network_probe.h",
        ],
    );
    let hwui_probe_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_hwui_probe.cc",
            "probes/runtime_hwui_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-socket-broker-adapter/include/darwin_art_bionic_socket_broker.h",
        ],
    );
    let graphics_probe_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_graphics_probe.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_phase.cc",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_input.cc",
            "probes/runtime_graphics_probe_internal.h",
            "compat/darwin_surface_bridge.h",
            "compat/darwin_framework_natives.h",
            "compat/darwin_hwui_gpu_mode.h",
        ],
    );

    let mut graph = String::new();
    graph.push_str("# Generated by darwin-art-xtask. Do not edit.\n");
    graph.push_str(&format!("# graph-version: {GRAPH_VERSION}\n"));
    graph.push_str(&format!("# input-digest: {digest}\n"));
    graph.push_str(&format!("# compiler: {} sdk: {SDK_NAME}\n", toolchain.cxx));
    graph.push_str("ninja_required_version = 1.10\n\n");
    let mut cached_rules_emitted = false;
    if let Some(cached_objects) = cached_graphics_objects.as_deref() {
        emit_cached_native_graph(
            &mut graph,
            cached_objects,
            &archive,
            &mut cached_rules_emitted,
        );
        graph.push_str("build ");
        graph.push_str(&stamp);
        graph.push_str(": phony ");
        graph.push_str(&archive);
        graph.push('\n');
        graph.push_str("# graphics-bootstrap uses persisted per-object commands\n\n");
    } else {
        graph.push_str("rule graphics_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
        graph.push_str(&shell_quote(&native_output_for_shell));
        graph.push_str(
            " cargo run -p art-bootstrap -- build-runtime-graphics-bootstrap-internal && touch ",
        );
        graph.push_str(&shell_quote(&stamp_for_shell));
        graph.push('\n');
        graph.push_str("  description = GRAPHICS bootstrap\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&stamp);
        graph.push(' ');
        graph.push_str(&archive);
        graph.push_str(": graphics_bootstrap ");
        graph.push_str(&bootstrap_input_list);
        graph.push('\n');
    }
    graph.push_str("build graphics-bootstrap: phony ");
    graph.push_str(&archive);
    graph.push('\n');
    if let Some(cached_objects) = cached_runtime_objects.as_deref() {
        emit_cached_native_graph(
            &mut graph,
            cached_objects,
            &runtime_archive,
            &mut cached_rules_emitted,
        );
        graph.push_str("build ");
        graph.push_str(&runtime_stamp);
        graph.push_str(": phony ");
        graph.push_str(&runtime_archive);
        graph.push('\n');
        graph.push_str("# runtime-bootstrap uses persisted per-object commands\n\n");
    } else {
        graph.push_str("rule runtime_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
        graph.push_str(&shell_quote(&native_output_for_shell));
        graph.push_str(" cargo run -p art-bootstrap -- build-runtime-bootstrap-internal && touch ");
        graph.push_str(&shell_quote(&runtime_stamp_path.to_string_lossy()));
        graph.push('\n');
        graph.push_str("  description = ART bootstrap\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&runtime_stamp);
        graph.push(' ');
        graph.push_str(&runtime_archive);
        graph.push_str(": runtime_bootstrap ");
        graph.push_str(&bootstrap_input_list);
        graph.push('\n');
    }
    graph.push_str("build runtime-bootstrap: phony ");
    graph.push_str(&runtime_archive);
    graph.push('\n');
    graph.push_str("rule runtime_filesystem_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT=");
    graph.push_str(&shell_quote(&filesystem_object_for_shell));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-filesystem-probe\n");
    graph.push_str("  description = CXX runtime_filesystem_probe\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&filesystem_object);
    graph.push_str(": runtime_filesystem_probe ");
    graph.push_str(&filesystem_probe_inputs);
    graph.push('\n');
    graph.push_str("rule runtime_network_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT=");
    graph.push_str(&shell_quote(&network_object_for_shell));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-network-probe\n");
    graph.push_str("  description = CXX runtime_network_probe\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&network_object);
    graph.push_str(": runtime_network_probe ");
    graph.push_str(&network_probe_inputs);
    graph.push('\n');
    graph.push_str("rule runtime_hwui_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT=");
    graph.push_str(&shell_quote(&hwui_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-hwui-probe\n");
    graph.push_str("  description = CXX runtime_hwui_probe\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&hwui_object);
    graph.push_str(": runtime_hwui_probe ");
    graph.push_str(&hwui_probe_inputs);
    graph.push('\n');
    graph.push_str("rule runtime_graphics_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_OBJECT=");
    graph.push_str(&shell_quote(&graphics_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- audit-runtime-graphics-link-fast\n");
    graph.push_str("  description = CXX runtime_graphics_probe\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&graphics_object);
    graph.push_str(": runtime_graphics_probe ");
    graph.push_str(&graphics_probe_inputs);
    graph.push('\n');
    graph.push_str("rule graphics_audit\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
    graph.push_str(&shell_quote(&native_output_for_shell));
    graph.push_str(" DARWIN_ART_NATIVE_FILESYSTEM_OBJECT=");
    graph.push_str(&shell_quote(&filesystem_object_for_shell));
    graph.push_str(" DARWIN_ART_NATIVE_NETWORK_OBJECT=");
    graph.push_str(&shell_quote(&network_object_for_shell));
    graph.push_str(" DARWIN_ART_NATIVE_GRAPHICS_OBJECT=");
    graph.push_str(&shell_quote(&graphics_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_HWUI_OBJECT=");
    graph.push_str(&shell_quote(&hwui_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- audit-runtime-graphics-link\n");
    graph.push_str("  description = GRAPHICS link/audit\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&runtime_library);
    graph.push_str(": graphics_audit ");
    graph.push_str(&archive);
    graph.push(' ');
    graph.push_str(&filesystem_object);
    graph.push(' ');
    graph.push_str(&network_object);
    graph.push(' ');
    graph.push_str(&graphics_object);
    graph.push(' ');
    graph.push_str(&hwui_object);
    graph.push(' ');
    graph.push_str(&probe_only_input_list);
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
    // This digest is the invalidation boundary for native objects, not for
    // the Rust command that happens to emit the Ninja file.  Rust orchestration
    // changes are intentionally excluded here: changing an xtask, bootstrap
    // CLI, or Cargo manifest must not force hundreds of unchanged C++/ObjC++
    // translation units to rebuild.  Bump GRAPH_VERSION when the graph
    // policy/command generation itself changes.
    let mut paths = vec![
        PathBuf::from("sources.lock"),
        PathBuf::from("bootclasspath.lock"),
        PathBuf::from("tools/build-android-elf-jni-fixture.sh"),
        PathBuf::from("tools/audit-android16-graphics-closure.sh"),
        PathBuf::from("probes/runtime_filesystem_probe.cc"),
        PathBuf::from("probes/runtime_filesystem_probe.h"),
        PathBuf::from("probes/runtime_network_probe.cc"),
        PathBuf::from("probes/runtime_network_probe.h"),
        PathBuf::from("probes/runtime_acceptance_phases.cc"),
        PathBuf::from("probes/runtime_acceptance_phases.h"),
        PathBuf::from("probes/runtime_hwui_probe.cc"),
        PathBuf::from("probes/runtime_hwui_probe.h"),
        PathBuf::from("probes/runtime_entry_probe.cc"),
        PathBuf::from("probes/runtime_link_probe.cc"),
        PathBuf::from("probes/runtime_elf_probe.cc"),
        PathBuf::from("probes/runtime_elf_probe.h"),
        PathBuf::from("probes/runtime_abi_probe.cc"),
        PathBuf::from("probes/runtime_abi_probe.h"),
        PathBuf::from("probes/runtime_process_state.cc"),
        PathBuf::from("probes/runtime_process_state.h"),
        PathBuf::from("probes/runtime_process_options.cc"),
        PathBuf::from("probes/runtime_process_options.h"),
        PathBuf::from("probes/runtime_jni_scope.h"),
        PathBuf::from("probes/runtime_shutdown_probe.cc"),
        PathBuf::from("probes/runtime_shutdown_probe.h"),
        PathBuf::from("probes/runtime_frame_probe.cc"),
        PathBuf::from("probes/runtime_frame_probe.h"),
        PathBuf::from("probes/runtime_graphics_probe.cc"),
        PathBuf::from("probes/runtime_graphics_probe.h"),
        PathBuf::from("probes/runtime_graphics_phase.cc"),
        PathBuf::from("probes/runtime_graphics_phase.h"),
        PathBuf::from("probes/runtime_graphics_input.cc"),
        PathBuf::from("probes/runtime_graphics_probe_internal.h"),
        PathBuf::from("probes/runtime_apk_graph.cc"),
        PathBuf::from("probes/runtime_apk_graph.h"),
        PathBuf::from("compat/darwin_surface_bridge.mm"),
        PathBuf::from("compat/darwin_surface_bridge.h"),
        PathBuf::from("compat/darwin_provider_owners.cc"),
        PathBuf::from("compat/darwin_provider_owners.h"),
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

fn is_probe_only_input(path: &Path) -> bool {
    matches!(
        path.to_string_lossy().as_ref(),
        "probes/runtime_filesystem_probe.cc"
            | "probes/runtime_filesystem_probe.h"
            | "probes/runtime_network_probe.cc"
            | "probes/runtime_network_probe.h"
            | "probes/runtime_acceptance_phases.cc"
            | "probes/runtime_acceptance_phases.h"
            | "probes/runtime_hwui_probe.cc"
            | "probes/runtime_hwui_probe.h"
            | "probes/runtime_entry_probe.cc"
            | "probes/runtime_link_probe.cc"
            | "probes/runtime_elf_probe.cc"
            | "probes/runtime_elf_probe.h"
            | "probes/runtime_abi_probe.cc"
            | "probes/runtime_abi_probe.h"
            | "probes/runtime_process_state.cc"
            | "probes/runtime_process_state.h"
            | "probes/runtime_process_options.cc"
            | "probes/runtime_process_options.h"
            | "probes/runtime_jni_scope.h"
            | "probes/runtime_shutdown_probe.cc"
            | "probes/runtime_shutdown_probe.h"
            | "probes/runtime_frame_probe.cc"
            | "probes/runtime_frame_probe.h"
            | "probes/runtime_graphics_probe.cc"
            | "probes/runtime_graphics_probe.h"
            | "probes/runtime_graphics_phase.cc"
            | "probes/runtime_graphics_phase.h"
            | "probes/runtime_graphics_input.cc"
            | "probes/runtime_graphics_probe_internal.h"
            | "probes/runtime_apk_graph.cc"
            | "probes/runtime_apk_graph.h"
            | "compat/darwin_surface_bridge.mm"
            | "compat/darwin_surface_bridge.h"
    )
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

fn probe_inputs(root: &Path, paths: &[&str]) -> String {
    paths
        .iter()
        .map(|path| root.join(path))
        .filter(|path| path.is_file())
        .map(|path| ninja_path(&path))
        .collect::<Vec<_>>()
        .join(" ")
}

/// Recover the compiler command and source identity persisted by
/// `compile_with_dependency_cache`. This lets a later graph generation turn
/// an already materialized bootstrap into real per-object Ninja edges without
/// duplicating ART's long include/define command construction in xtask.
fn cached_native_objects(
    object_dir: &Path,
    archive: &Path,
) -> io::Result<Option<Vec<CachedNativeObject>>> {
    let mut by_name = BTreeMap::new();
    let Ok(entries) = fs::read_dir(object_dir) else {
        return Ok(None);
    };
    for entry in entries.flatten() {
        let fingerprint = entry.path();
        if fingerprint
            .extension()
            .and_then(|extension| extension.to_str())
            != Some("fingerprint")
        {
            continue;
        }
        let object = fingerprint.with_extension("");
        // A Ninja-produced depfile may not exist yet (Ninja can be starting
        // from a persisted Rust fingerprint), but the object and its command
        // fingerprint are still sufficient to seed the cached graph.  The
        // first cached edge will regenerate the depfile; requiring it here
        // would incorrectly fall back to the monolithic Rust builder.
        if !object.is_file() {
            continue;
        }
        let contents = fs::read_to_string(&fingerprint)?;
        let Some(command_line) = contents
            .lines()
            .find_map(|line| line.strip_prefix("command="))
        else {
            continue;
        };
        let tokens = command_line.split_whitespace().collect::<Vec<_>>();
        let Some(source_index) = tokens.iter().position(|token| *token == "-c") else {
            continue;
        };
        let Some(source) = tokens.get(source_index + 1).map(PathBuf::from) else {
            continue;
        };
        if !source.is_file() {
            continue;
        }
        let Some(name) = object.file_name().and_then(|name| name.to_str()) else {
            continue;
        };
        by_name.insert(
            name.to_owned(),
            CachedNativeObject {
                object,
                source,
                command: command_line.to_owned(),
            },
        );
    }
    // The bootstrap archives are deliberately large (200+ objects).  A
    // partially interrupted Rust builder can leave a handful of fingerprints
    // and a seemingly valid archive; do not promote that into a native graph.
    // Falling back to the canonical builder below is safer than silently
    // linking an incomplete ART runtime.
    if by_name.len() < 128 || !archive.is_file() {
        return Ok(None);
    }
    let archive_members = Command::new("ar").arg("-t").arg(archive).output()?;
    if !archive_members.status.success() {
        return Ok(None);
    }
    let mut ordered = Vec::new();
    for member in String::from_utf8_lossy(&archive_members.stdout).lines() {
        if member == "__.SYMDEF" || member == "__.SYMDEF SORTED" {
            continue;
        }
        if let Some(object) = by_name.remove(member.trim()) {
            ordered.push(object);
        }
    }
    // An interrupted archive command can leave a valid, but incomplete,
    // archive behind.  Never let that partial member order become the cache
    // manifest: append every remaining fingerprint object in stable name
    // order so the next graph rebuilds a complete archive.
    ordered.extend(by_name.into_values());
    if ordered.len() < 128 {
        return Ok(None);
    }
    Ok(Some(ordered))
}

fn emit_cached_native_graph(
    graph: &mut String,
    objects: &[CachedNativeObject],
    archive: &str,
    rules_emitted: &mut bool,
) {
    if !*rules_emitted {
        graph.push_str("rule native_cached_cpp\n");
        graph.push_str("  command = $compile_command\n");
        graph.push_str("  depfile = $out.d\n");
        graph.push_str("  deps = gcc\n");
        graph.push_str("  description = CXX $out\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("rule native_cached_archive\n");
        graph.push_str("  command = rm -f $out && ar rcs $out $in\n");
        graph.push_str("  description = AR $out\n");
        graph.push_str("  restat = 1\n\n");
        *rules_emitted = true;
    }
    let mut object_paths = Vec::with_capacity(objects.len());
    for object in objects {
        let output = ninja_path(&object.object);
        let source = ninja_path(&object.source);
        graph.push_str("build ");
        graph.push_str(&output);
        graph.push_str(": native_cached_cpp ");
        graph.push_str(&source);
        graph.push('\n');
        graph.push_str("  compile_command = ");
        // Fingerprints store a diagnostic, whitespace-separated command.  It
        // is sufficient to recover the persisted argv here because the
        // bootstrap paths contain no spaces; every token still needs shell
        // quoting (notably ART defines containing parentheses or quotes).
        let quoted_command = object
            .command
            .split_whitespace()
            .map(shell_quote)
            .collect::<Vec<_>>()
            .join(" ");
        graph.push_str(&quoted_command.replace('$', "$$"));
        graph.push('\n');
        object_paths.push(output);
    }
    graph.push_str("build ");
    graph.push_str(archive);
    graph.push_str(": native_cached_archive ");
    graph.push_str(&object_paths.join(" "));
    graph.push('\n');
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
            "probes/runtime_process_options.cc"
        )));
        assert!(!is_probe_only_input(Path::new(
            "compat/darwin_runtime_adapters.cc"
        )));
    }
}
