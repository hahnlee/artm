#![forbid(unsafe_code)]

use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

const GRAPH_VERSION: &str = "darwin-art-native-graph-v7";
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

#[derive(Clone)]
struct CachedNativeObject {
    object: PathBuf,
    source: PathBuf,
    command: String,
    shell_quoted: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FoundationFamily {
    Hwui,
    GraphicsJni,
    Icu,
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
    let hwui_foundation_archive_path = native_output_root.join(HWUI_STATIC_FOUNDATION_ARCHIVE);
    let hwui_apex_foundation_archive_path = native_output_root.join(HWUI_APEX_FOUNDATION_ARCHIVE);
    let graphics_jni_archive_path = native_output_root.join(ANDROID_GRAPHICS_JNI_ARCHIVE);
    let graphics_registrar_archive_path =
        native_output_root.join(ANDROID_GRAPHICS_REGISTRAR_ARCHIVE);
    let graphics_force_loaded_object_path =
        native_output_root.join(ANDROID_GRAPHICS_FORCE_LOADED_OBJECT);
    let icu_common_archive_path = native_output_root.join(ICU_COMMON_FOUNDATION_ARCHIVE);
    let icu_i18n_archive_path = native_output_root.join(ICU_I18N_FOUNDATION_ARCHIVE);
    let icu_stubdata_archive_path = native_output_root.join(ICU_STUBDATA_FOUNDATION_ARCHIVE);
    let icu_init_archive_path = native_output_root.join(ICU_INIT_FOUNDATION_ARCHIVE);
    let runtime_library_path = native_output_root.join(GRAPHICS_RUNTIME_LIBRARY);
    let filesystem_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_filesystem_probe.cc.o");
    let network_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_network_probe.cc.o");
    let hwui_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_hwui_probe.cc.o");
    let graphics_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_probe.cc.o");
    let graphics_phase_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_phase.cc.o");
    let graphics_input_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_input.cc.o");
    let graphics_state_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_state.cc.o");
    let graphics_session_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_graphics_session.cc.o");
    let jni_acceptance_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_jni_acceptance_probe.cc.o");
    let app_bootstrap_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_app_bootstrap.cc.o");
    let app_presentation_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_app_presentation.cc.o");
    let stamp_path = cache_dir.join("graphics-bootstrap.stamp");
    let runtime_stamp_path = cache_dir.join("runtime-bootstrap.stamp");
    let stamp = ninja_path(&stamp_path);
    let runtime_stamp = ninja_path(&runtime_stamp_path);
    let archive = ninja_path(&archive_path);
    let runtime_archive = ninja_path(&runtime_archive_path);
    let hwui_foundation_archive = ninja_path(&hwui_foundation_archive_path);
    let hwui_apex_foundation_archive = ninja_path(&hwui_apex_foundation_archive_path);
    let graphics_jni_archive = ninja_path(&graphics_jni_archive_path);
    let graphics_registrar_archive = ninja_path(&graphics_registrar_archive_path);
    let graphics_force_loaded_object = ninja_path(&graphics_force_loaded_object_path);
    let icu_common_archive = ninja_path(&icu_common_archive_path);
    let icu_i18n_archive = ninja_path(&icu_i18n_archive_path);
    let icu_stubdata_archive = ninja_path(&icu_stubdata_archive_path);
    let icu_init_archive = ninja_path(&icu_init_archive_path);
    let runtime_library = ninja_path(&runtime_library_path);
    let filesystem_object = ninja_path(&filesystem_object_path);
    let network_object = ninja_path(&network_object_path);
    let hwui_object = ninja_path(&hwui_object_path);
    let graphics_object = ninja_path(&graphics_object_path);
    let graphics_phase_object = ninja_path(&graphics_phase_object_path);
    let graphics_input_object = ninja_path(&graphics_input_object_path);
    let graphics_state_object = ninja_path(&graphics_state_object_path);
    let jni_acceptance_object = ninja_path(&jni_acceptance_object_path);
    let app_bootstrap_object = ninja_path(&app_bootstrap_object_path);
    let app_presentation_object = ninja_path(&app_presentation_object_path);
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
    let foundation_inputs = foundation_inputs(&root);
    let hwui_foundation_input_list =
        foundation_input_list(&root, &foundation_inputs, FoundationFamily::Hwui);
    let graphics_jni_foundation_input_list =
        foundation_input_list(&root, &foundation_inputs, FoundationFamily::GraphicsJni);
    let icu_foundation_input_list =
        foundation_input_list(&root, &foundation_inputs, FoundationFamily::Icu);
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
            "compat/darwin_surface_bridge.h",
            "compat/darwin_framework_natives.h",
            "compat/darwin_hwui_gpu_mode.h",
        ],
    );
    let graphics_phase_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_graphics_phase.cc",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_frame_probe.h",
        ],
    );
    let graphics_input_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_graphics_input.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_probe_internal.h",
            "probes/runtime_process_state.h",
        ],
    );
    let graphics_state_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_graphics_state.cc",
            "probes/runtime_graphics_state.h",
            "compat/darwin_surface_bridge.h",
        ],
    );
    let graphics_session_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_graphics_session.cc",
            "probes/runtime_graphics_session.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_state.h",
            "probes/runtime_process_state.h",
            "include/darwin_art/darwin_art.h",
        ],
    );
    let jni_acceptance_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_jni_acceptance_probe.cc",
            "probes/runtime_jni_acceptance_probe.h",
            "probes/runtime_abi_probe.h",
            "probes/runtime_jni_scope.h",
        ],
    );
    let app_bootstrap_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_app_bootstrap.cc",
            "probes/runtime_app_bootstrap.h",
            "probes/runtime_process_state.h",
        ],
    );
    let app_presentation_inputs = probe_inputs(
        &root,
        &[
            "probes/runtime_app_presentation.cc",
            "probes/runtime_app_presentation.h",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_state.h",
        ],
    );
    // Ninja normally invalidates by mtime.  These stable content stamps make
    // the probe boundary robust when a checkout/materializer preserves source
    // mtimes (and keep each phase's content identity independent of the broad
    // runtime graph digest).  The audit path regenerates the graph before
    // asking Ninja for its warm/no-op result.
    let filesystem_probe_stamp = probe_content_stamp(
        &root,
        "filesystem",
        &[
            "probes/runtime_filesystem_probe.cc",
            "probes/runtime_filesystem_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-ioctl-facade/include/darwin_art_bionic_ioctl.h",
        ],
    )?;
    let network_probe_stamp = probe_content_stamp(
        &root,
        "network",
        &[
            "probes/runtime_network_probe.cc",
            "probes/runtime_network_probe.h",
        ],
    )?;
    let hwui_probe_stamp = probe_content_stamp(
        &root,
        "hwui",
        &[
            "probes/runtime_hwui_probe.cc",
            "probes/runtime_hwui_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-socket-broker-adapter/include/darwin_art_bionic_socket_broker.h",
        ],
    )?;
    let graphics_probe_stamp = probe_content_stamp(
        &root,
        "graphics",
        &[
            "probes/runtime_graphics_probe.cc",
            "probes/runtime_graphics_probe.h",
            "compat/darwin_surface_bridge.h",
            "compat/darwin_framework_natives.h",
            "compat/darwin_hwui_gpu_mode.h",
        ],
    )?;
    let graphics_phase_stamp = probe_content_stamp(
        &root,
        "graphics-phase",
        &[
            "probes/runtime_graphics_phase.cc",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_frame_probe.h",
        ],
    )?;
    let graphics_input_stamp = probe_content_stamp(
        &root,
        "graphics-input",
        &[
            "probes/runtime_graphics_input.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_probe_internal.h",
            "probes/runtime_process_state.h",
        ],
    )?;
    let graphics_state_stamp = probe_content_stamp(
        &root,
        "graphics-state",
        &[
            "probes/runtime_graphics_state.cc",
            "probes/runtime_graphics_state.h",
            "compat/darwin_surface_bridge.h",
        ],
    )?;
    let graphics_session_stamp = probe_content_stamp(
        &root,
        "graphics-session",
        &[
            "probes/runtime_graphics_session.cc",
            "probes/runtime_graphics_session.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_state.h",
        ],
    )?;
    let jni_acceptance_stamp = probe_content_stamp(
        &root,
        "jni-acceptance",
        &[
            "probes/runtime_jni_acceptance_probe.cc",
            "probes/runtime_jni_acceptance_probe.h",
            "probes/runtime_abi_probe.h",
            "probes/runtime_jni_scope.h",
        ],
    )?;
    let app_bootstrap_stamp = probe_content_stamp(
        &root,
        "app-bootstrap",
        &[
            "probes/runtime_app_bootstrap.cc",
            "probes/runtime_app_bootstrap.h",
            "probes/runtime_process_state.h",
        ],
    )?;
    let app_presentation_stamp = probe_content_stamp(
        &root,
        "app-presentation",
        &[
            "probes/runtime_app_presentation.cc",
            "probes/runtime_app_presentation.h",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_state.h",
        ],
    )?;

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

    let hwui_cached =
        cached_foundation_objects(&native_output_root.join("hwui-static-foundation/objects"))?;
    let hwui_main = hwui_cached
        .iter()
        .filter(|object| {
            object
                .object
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| !name.starts_with("apex_"))
        })
        .cloned()
        .collect::<Vec<_>>();
    let hwui_apex = hwui_cached
        .iter()
        .filter(|object| {
            object
                .object
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.starts_with("apex_"))
        })
        .cloned()
        .collect::<Vec<_>>();
    let graphics_cached =
        cached_foundation_objects(&native_output_root.join("android-graphics-jni/objects"))?;
    let graphics_registrar = graphics_cached
        .iter()
        .filter(|object| {
            object.object.file_name().and_then(|name| name.to_str()) == Some("LayoutlibLoader.o")
        })
        .cloned()
        .collect::<Vec<_>>();
    let graphics_main = graphics_cached
        .iter()
        .filter(|object| {
            object.object.file_name().and_then(|name| name.to_str()) != Some("LayoutlibLoader.o")
        })
        .cloned()
        .collect::<Vec<_>>();
    let hwui_ready = hwui_main.len() == 81 && hwui_apex.len() == 5;
    let graphics_ready = graphics_main.len() == 61 && graphics_registrar.len() == 1;
    let icu_cached = cached_foundation_objects(&native_output_root.join("icu-foundation/objects"))?;
    let icu_common = icu_cached
        .iter()
        .filter(|object| object.object.to_string_lossy().contains("/common/"))
        .cloned()
        .collect::<Vec<_>>();
    let icu_i18n = icu_cached
        .iter()
        .filter(|object| object.object.to_string_lossy().contains("/i18n/"))
        .cloned()
        .collect::<Vec<_>>();
    let icu_stubdata = icu_cached
        .iter()
        .filter(|object| object.object.to_string_lossy().contains("/stubdata/"))
        .cloned()
        .collect::<Vec<_>>();
    let icu_init = icu_cached
        .iter()
        .filter(|object| object.object.to_string_lossy().contains("/androidicuinit/"))
        .cloned()
        .collect::<Vec<_>>();
    let icu_ready = icu_common.len() == 201
        && icu_i18n.len() == 254
        && icu_stubdata.len() == 1
        && icu_init.len() == 2;
    if hwui_ready {
        emit_cached_native_graph(
            &mut graph,
            &hwui_main,
            &hwui_foundation_archive,
            &mut cached_rules_emitted,
        );
        emit_cached_native_graph(
            &mut graph,
            &hwui_apex,
            &hwui_apex_foundation_archive,
            &mut cached_rules_emitted,
        );
    } else {
        graph.push_str("rule hwui_foundation_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && tools/build-android16-hwui-static-foundation.sh\n");
        graph.push_str("  description = HWUI foundation archives\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&hwui_foundation_archive);
        graph.push(' ');
        graph.push_str(&hwui_apex_foundation_archive);
        graph.push_str(": hwui_foundation_bootstrap ");
        graph.push_str(&hwui_foundation_input_list);
        graph.push('\n');
    }

    if icu_ready {
        emit_cached_native_graph(
            &mut graph,
            &icu_common,
            &icu_common_archive,
            &mut cached_rules_emitted,
        );
        emit_cached_native_graph(
            &mut graph,
            &icu_i18n,
            &icu_i18n_archive,
            &mut cached_rules_emitted,
        );
        emit_cached_native_graph(
            &mut graph,
            &icu_stubdata,
            &icu_stubdata_archive,
            &mut cached_rules_emitted,
        );
        emit_cached_native_graph(
            &mut graph,
            &icu_init,
            &icu_init_archive,
            &mut cached_rules_emitted,
        );
    } else {
        graph.push_str("rule icu_foundation_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && tools/build-android16-icu-foundation.sh\n");
        graph.push_str("  description = ICU foundation archives\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&icu_common_archive);
        graph.push(' ');
        graph.push_str(&icu_i18n_archive);
        graph.push(' ');
        graph.push_str(&icu_stubdata_archive);
        graph.push(' ');
        graph.push_str(&icu_init_archive);
        graph.push_str(": icu_foundation_bootstrap ");
        graph.push_str(&icu_foundation_input_list);
        graph.push('\n');
    }

    if graphics_ready {
        emit_cached_native_graph(
            &mut graph,
            &graphics_main,
            &graphics_jni_archive,
            &mut cached_rules_emitted,
        );
        emit_cached_native_graph(
            &mut graph,
            &graphics_registrar,
            &graphics_registrar_archive,
            &mut cached_rules_emitted,
        );
        graph.push_str("rule graphics_jni_force_load\n");
        graph.push_str("  command = ");
        graph.push_str(&shell_quote(&toolchain.cxx));
        graph.push_str(" -r -arch arm64 -Wl,-force_load,");
        graph.push_str(&shell_quote(&graphics_registrar_archive));
        graph.push_str(" -Wl,-force_load,");
        graph.push_str(&shell_quote(&graphics_jni_archive));
        graph.push_str(" -o $out\n");
        graph.push_str("  description = LINK GraphicsJNI force-load\n\n");
        graph.push_str("build ");
        graph.push_str(&graphics_force_loaded_object);
        graph.push_str(": graphics_jni_force_load ");
        graph.push_str(&graphics_jni_archive);
        graph.push(' ');
        graph.push_str(&graphics_registrar_archive);
        graph.push('\n');
    } else {
        graph.push_str("rule graphics_jni_foundation_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && tools/build-android16-android-graphics-jni.sh --object-audit\n");
        graph.push_str("  description = GraphicsJNI foundation archives\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&graphics_jni_archive);
        graph.push(' ');
        graph.push_str(&graphics_registrar_archive);
        graph.push(' ');
        graph.push_str(&graphics_force_loaded_object);
        graph.push_str(": graphics_jni_foundation_bootstrap ");
        graph.push_str(&graphics_jni_foundation_input_list);
        graph.push('\n');
    }
    graph.push_str("build graphics-foundation: phony ");
    graph.push_str(&hwui_foundation_archive);
    graph.push(' ');
    graph.push_str(&hwui_apex_foundation_archive);
    graph.push(' ');
    graph.push_str(&graphics_jni_archive);
    graph.push(' ');
    graph.push_str(&graphics_registrar_archive);
    graph.push(' ');
    graph.push_str(&graphics_force_loaded_object);
    graph.push(' ');
    graph.push_str(&icu_common_archive);
    graph.push(' ');
    graph.push_str(&icu_i18n_archive);
    graph.push(' ');
    graph.push_str(&icu_stubdata_archive);
    graph.push(' ');
    graph.push_str(&icu_init_archive);
    graph.push('\n');
    graph.push_str("build icu-foundation: phony ");
    graph.push_str(&icu_common_archive);
    graph.push(' ');
    graph.push_str(&icu_i18n_archive);
    graph.push(' ');
    graph.push_str(&icu_stubdata_archive);
    graph.push(' ');
    graph.push_str(&icu_init_archive);
    graph.push('\n');
    graph.push_str("build foundation: phony graphics-foundation icu-foundation\n\n");

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
    graph.push(' ');
    graph.push_str(&ninja_path(&filesystem_probe_stamp));
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
    graph.push(' ');
    graph.push_str(&ninja_path(&network_probe_stamp));
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
    graph.push(' ');
    graph.push_str(&ninja_path(&hwui_probe_stamp));
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
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_probe_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_graphics_phase_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT=");
    graph.push_str(&shell_quote(&graphics_phase_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-phase-probe\n");
    graph.push_str("  description = CXX runtime_graphics_phase\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&graphics_phase_object);
    graph.push_str(": runtime_graphics_phase_probe ");
    graph.push_str(&graphics_phase_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_phase_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_graphics_input_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_INPUT_OBJECT=");
    graph.push_str(&shell_quote(&graphics_input_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-input-probe\n");
    graph.push_str("  description = CXX runtime_graphics_input\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&graphics_input_object);
    graph.push_str(": runtime_graphics_input_probe ");
    graph.push_str(&graphics_input_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_input_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_graphics_state_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_STATE_OBJECT=");
    graph.push_str(&shell_quote(&graphics_state_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-state-probe\n");
    graph.push_str("  description = CXX runtime_graphics_state\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&graphics_state_object);
    graph.push_str(": runtime_graphics_state_probe ");
    graph.push_str(&graphics_state_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_state_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_graphics_session_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_SESSION_OBJECT=");
    graph.push_str(&shell_quote(
        &graphics_session_object_path.to_string_lossy(),
    ));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-session-probe\n");
    graph.push_str("  description = CXX runtime_graphics_session\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    let graphics_session_object = ninja_path(&graphics_session_object_path);
    graph.push_str("build ");
    graph.push_str(&graphics_session_object);
    graph.push_str(": runtime_graphics_session_probe ");
    graph.push_str(&graphics_session_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_session_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_jni_acceptance_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_JNI_ACCEPTANCE_OBJECT=");
    graph.push_str(&shell_quote(&jni_acceptance_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-jni-acceptance-probe\n");
    graph.push_str("  description = CXX runtime_jni_acceptance\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&jni_acceptance_object);
    graph.push_str(": runtime_jni_acceptance_probe ");
    graph.push_str(&jni_acceptance_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&jni_acceptance_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_app_bootstrap_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_APP_BOOTSTRAP_OBJECT=");
    graph.push_str(&shell_quote(&app_bootstrap_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-app-bootstrap-probe\n");
    graph.push_str("  description = CXX runtime_app_bootstrap\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&app_bootstrap_object);
    graph.push_str(": runtime_app_bootstrap_probe ");
    graph.push_str(&app_bootstrap_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&app_bootstrap_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_app_presentation_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_APP_PRESENTATION_OBJECT=");
    graph.push_str(&shell_quote(
        &app_presentation_object_path.to_string_lossy(),
    ));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-app-presentation-probe\n");
    graph.push_str("  description = CXX runtime_app_presentation\n");
    graph.push_str("  depfile = $out.d\n  deps = gcc\n  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&app_presentation_object);
    graph.push_str(": runtime_app_presentation_probe ");
    graph.push_str(&app_presentation_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&app_presentation_stamp));
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
    graph.push_str(" DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT=");
    graph.push_str(&shell_quote(&graphics_phase_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_GRAPHICS_INPUT_OBJECT=");
    graph.push_str(&shell_quote(&graphics_input_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_GRAPHICS_STATE_OBJECT=");
    graph.push_str(&shell_quote(&graphics_state_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_GRAPHICS_SESSION_OBJECT=");
    graph.push_str(&shell_quote(
        &graphics_session_object_path.to_string_lossy(),
    ));
    graph.push_str(" DARWIN_ART_NATIVE_JNI_ACCEPTANCE_OBJECT=");
    graph.push_str(&shell_quote(&jni_acceptance_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_HWUI_OBJECT=");
    graph.push_str(&shell_quote(&hwui_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_APP_BOOTSTRAP_OBJECT=");
    graph.push_str(&shell_quote(&app_bootstrap_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_APP_PRESENTATION_OBJECT=");
    graph.push_str(&shell_quote(
        &app_presentation_object_path.to_string_lossy(),
    ));
    // The full upstream closure is a separate release/CI gate.  The Ninja
    // graph is the developer inner loop and must only relink/audit against
    // already materialized foundation artifacts.
    graph.push_str(" cargo run -p art-bootstrap -- audit-runtime-graphics-link-fast\n");
    graph.push_str("  description = GRAPHICS link/audit\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&runtime_library);
    graph.push_str(": graphics_audit ");
    // The fast audit consumes these foundation artifacts directly.  Keep them
    // as real Ninja prerequisites so a missing or rebuilt foundation cannot
    // race the final dylib link/audit edge.
    graph.push_str(&hwui_foundation_archive);
    graph.push(' ');
    graph.push_str(&hwui_apex_foundation_archive);
    graph.push(' ');
    graph.push_str(&graphics_jni_archive);
    graph.push(' ');
    graph.push_str(&graphics_registrar_archive);
    graph.push(' ');
    graph.push_str(&graphics_force_loaded_object);
    graph.push(' ');
    graph.push_str(&icu_common_archive);
    graph.push(' ');
    graph.push_str(&icu_i18n_archive);
    graph.push(' ');
    graph.push_str(&icu_stubdata_archive);
    graph.push(' ');
    graph.push_str(&icu_init_archive);
    graph.push(' ');
    graph.push_str(&archive);
    graph.push(' ');
    graph.push_str(&filesystem_object);
    graph.push(' ');
    graph.push_str(&network_object);
    graph.push(' ');
    graph.push_str(&graphics_object);
    graph.push(' ');
    graph.push_str(&graphics_phase_object);
    graph.push(' ');
    graph.push_str(&graphics_input_object);
    graph.push(' ');
    graph.push_str(&graphics_state_object);
    graph.push(' ');
    graph.push_str(&graphics_session_object);
    graph.push(' ');
    graph.push_str(&jni_acceptance_object);
    graph.push(' ');
    graph.push_str(&hwui_object);
    graph.push(' ');
    graph.push_str(&app_bootstrap_object);
    graph.push(' ');
    graph.push_str(&app_presentation_object);
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
        PathBuf::from("probes/runtime_network_loader.cc"),
        PathBuf::from("probes/runtime_context_loader.cc"),
        PathBuf::from("probes/runtime_app_bootstrap.cc"),
        PathBuf::from("probes/runtime_app_bootstrap.h"),
        PathBuf::from("probes/runtime_app_presentation.cc"),
        PathBuf::from("probes/runtime_app_presentation.h"),
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
        PathBuf::from("probes/runtime_graphics_state.cc"),
        PathBuf::from("probes/runtime_graphics_state.h"),
        PathBuf::from("probes/runtime_graphics_session.cc"),
        PathBuf::from("probes/runtime_graphics_session.h"),
        PathBuf::from("probes/runtime_graphics_cpu_stubs.cc"),
        PathBuf::from("probes/runtime_jni_acceptance_probe.cc"),
        PathBuf::from("probes/runtime_jni_acceptance_probe.h"),
        PathBuf::from("probes/runtime_graphics_probe_internal.h"),
        PathBuf::from("probes/runtime_apk_graph.cc"),
        PathBuf::from("probes/runtime_apk_graph.h"),
        PathBuf::from("compat/darwin_surface_bridge.mm"),
        PathBuf::from("compat/darwin_surface_bridge.h"),
        PathBuf::from("compat/darwin_surface_internal.h"),
        PathBuf::from("compat/darwin_surface_gpu_bridge.mm"),
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

/// Inputs for the transitional graphics foundation edge.
///
/// The archive builders remain the authority for their generated manifests,
/// dependency checks, and per-TU command stamps.  Listing the source trees
/// here gives Ninja a conservative invalidation boundary so a changed HWUI or
/// GraphicsJNI source cannot leave an apparently up-to-date archive behind.
/// These inputs are intentionally kept out of `graph_inputs`: editing a
/// foundation source must not invalidate the ART runtime archive cache.
fn foundation_inputs(root: &Path) -> Vec<PathBuf> {
    let mut paths = vec![
        PathBuf::from("tools/build-android16-hwui-static-foundation.sh"),
        PathBuf::from("tools/build-android16-android-graphics-jni.sh"),
        PathBuf::from("upstream/android16-hwui-static-foundation.lock"),
        PathBuf::from("upstream/android16-android-graphics-jni.lock"),
        PathBuf::from("upstream/android16-hwui-gpu.lock"),
        PathBuf::from("tools/build-android16-icu-foundation.sh"),
        PathBuf::from("upstream/android16-icu-foundation.lock"),
        PathBuf::from("patches/frameworks-base/0001-darwin-android-critical-jni-abi.patch"),
        PathBuf::from("patches/frameworks-base/0002-darwin-lazy-native-window-jni.patch"),
        PathBuf::from("patches/frameworks-base/0003-darwin-hwui-gpu-layoutlib.patch"),
        PathBuf::from("patches/frameworks-base/0005-darwin-hwui-animation-pulse.patch"),
    ];
    for directory in [
        "_aosp/frameworks/base/libs/hwui",
        "_aosp/external/libjpeg-turbo",
        "_aosp/external/libultrahdr",
        "_aosp/frameworks/native/libs/gui",
        "_aosp/frameworks/av/media/ndk",
        "_aosp/hardware/libhardware/include_all",
        "_aosp/external/icu-graphics",
    ] {
        collect_files(&root.join(directory), root, &mut paths);
    }
    paths.sort();
    paths.dedup();
    paths.retain(|path| root.join(path).is_file());
    paths
}

fn foundation_input_list(root: &Path, inputs: &[PathBuf], family: FoundationFamily) -> String {
    inputs
        .iter()
        .filter(|path| is_foundation_family_input(path, family))
        .map(|path| ninja_path(&root.join(path)))
        .collect::<Vec<_>>()
        .join(" ")
}

/// Keep shell fallback edges independent even before their command-stamped
/// translation units are promoted.  A HWUI source edit must not rerun ICU or
/// GraphicsJNI's shell builder, and vice versa.
fn is_foundation_family_input(path: &Path, family: FoundationFamily) -> bool {
    let path = path.to_string_lossy();
    let shared = path.starts_with("patches/frameworks-base/");
    match family {
        FoundationFamily::Hwui => {
            shared
                || matches!(
                    path.as_ref(),
                    "tools/build-android16-hwui-static-foundation.sh"
                        | "upstream/android16-hwui-static-foundation.lock"
                        | "upstream/android16-hwui-gpu.lock"
                )
                || path.starts_with("_aosp/frameworks/base/libs/hwui/")
                || path.starts_with("_aosp/hwui-static-deps/")
        }
        FoundationFamily::GraphicsJni => {
            shared
                || matches!(
                    path.as_ref(),
                    "tools/build-android16-android-graphics-jni.sh"
                        | "upstream/android16-android-graphics-jni.lock"
                        | "upstream/android16-hwui-gpu.lock"
                )
                || path.starts_with("_aosp/frameworks/base/libs/hwui/")
                || path.starts_with("_aosp/external/libjpeg-turbo/")
                || path.starts_with("_aosp/external/libultrahdr/")
                || path.starts_with("_aosp/frameworks/native/libs/gui/")
                || path.starts_with("_aosp/frameworks/av/media/ndk/")
                || path.starts_with("_aosp/hardware/libhardware/include_all/")
        }
        FoundationFamily::Icu => {
            path == "tools/build-android16-icu-foundation.sh"
                || path == "upstream/android16-icu-foundation.lock"
                || path.starts_with("_aosp/external/icu-graphics/")
        }
    }
}

/// Read command stamps emitted by the foundation shell builders.  Once a
/// complete object set exists, the graph can promote that archive from a
/// shell edge to ordinary per-TU edges without duplicating the AOSP command
/// construction in Rust.  A partial/interrupted set deliberately returns an
/// empty vector so the canonical shell builder remains the safe fallback.
fn cached_foundation_objects(object_dir: &Path) -> io::Result<Vec<CachedNativeObject>> {
    let mut objects = Vec::new();
    let mut pending = vec![object_dir.to_path_buf()];
    while let Some(directory) = pending.pop() {
        let Ok(entries) = fs::read_dir(directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                pending.push(path);
                continue;
            }
            let command_file = path;
            if command_file.extension().and_then(|ext| ext.to_str()) != Some("command") {
                continue;
            }
            let object = command_file.with_extension("");
            if !object.is_file() {
                continue;
            }
            let command = fs::read_to_string(&command_file)?.trim().to_owned();
            let tokens = command.split_whitespace().collect::<Vec<_>>();
            let Some(source_index) = tokens.iter().position(|token| *token == "-c") else {
                continue;
            };
            let Some(source_token) = tokens.get(source_index + 1) else {
                continue;
            };
            let source = PathBuf::from(source_token.trim_matches('\''));
            if !source.is_file() {
                continue;
            }
            objects.push(CachedNativeObject {
                object,
                source,
                command,
                shell_quoted: true,
            });
        }
    }
    objects.sort_by(|left, right| left.object.cmp(&right.object));
    Ok(objects)
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
            | "probes/runtime_app_bootstrap.cc"
            | "probes/runtime_app_bootstrap.h"
            | "probes/runtime_app_presentation.cc"
            | "probes/runtime_app_presentation.h"
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
            | "probes/runtime_graphics_state.cc"
            | "probes/runtime_graphics_state.h"
            | "probes/runtime_graphics_session.cc"
            | "probes/runtime_graphics_session.h"
            | "probes/runtime_jni_acceptance_probe.cc"
            | "probes/runtime_jni_acceptance_probe.h"
            | "probes/runtime_graphics_probe_internal.h"
            | "probes/runtime_apk_graph.cc"
            | "probes/runtime_apk_graph.h"
            | "compat/darwin_surface_bridge.mm"
            | "compat/darwin_surface_bridge.h"
            | "compat/darwin_surface_internal.h"
            | "compat/darwin_surface_gpu_bridge.mm"
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
    probe_input_paths(root, paths)
        .into_iter()
        .map(|path| ninja_path(&path))
        .collect::<Vec<_>>()
        .join(" ")
}

fn probe_input_paths(root: &Path, paths: &[&str]) -> Vec<PathBuf> {
    paths
        .iter()
        .map(|path| root.join(path))
        .filter(|path| path.is_file())
        .collect()
}

/// Materialize a stable content identity for one narrow probe phase.
///
/// The stamp is updated only when the bytes or input path set changes, so a
/// repeated graph generation remains a true Ninja warm no-op.  Keeping one
/// stamp per phase is important: changing graphics state must not make the
/// graphics input/session objects dirty merely because the global graph
/// digest changed.
fn probe_content_stamp(root: &Path, name: &str, paths: &[&str]) -> io::Result<PathBuf> {
    let inputs = probe_input_paths(root, paths);
    let mut digest = Sha256::new();
    digest.update(b"darwin-art-probe-content-v1\0");
    for path in &inputs {
        let relative = path.strip_prefix(root).unwrap_or(path);
        digest.update(relative.to_string_lossy().as_bytes());
        digest.update([0]);
        digest.update(fs::read(path)?);
        digest.update([0]);
    }
    let content = format!("{:x}\n", digest.finalize());
    let stamp = root
        .join("_build/runtime-probes/content-stamps")
        .join(format!("{name}.sha256"));
    if fs::read_to_string(&stamp).ok().as_deref() != Some(&content) {
        if let Some(parent) = stamp.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&stamp, content)?;
    }
    Ok(stamp)
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
                shell_quoted: false,
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
        let quoted_command = if object.shell_quoted {
            object.command.clone()
        } else {
            object
                .command
                .split_whitespace()
                .map(shell_quote)
                .collect::<Vec<_>>()
                .join(" ")
        };
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
