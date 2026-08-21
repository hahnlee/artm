#![forbid(unsafe_code)]

use sha2::{Digest, Sha256};
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

mod graph;

use graph::cache::{cached_native_objects, emit_cached_native_graph};
use graph::foundation::{
    FoundationFamily, cached_foundation_objects, foundation_input_list, foundation_inputs,
};
use graph::inputs::{
    collect_files, graph_inputs, is_probe_only_input, probe_content_stamp, probe_inputs,
};
use graph::representative::{
    REPRESENTATIVE_EDGES, edge_digest, emit_representative_edges, json_escape, toolchain_inputs,
};

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
    // The Rust command owns the C++ dependency cache for this probe.  Ninja
    // only tracks the explicit phase stamp and source inputs; consuming a
    // depfile produced inside `cargo run` makes the stored dependency mtime
    // race the copied object and dirties every warm graph invocation.
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
    // Dependency fingerprints are maintained by art-bootstrap.
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
    // Dependency fingerprints are maintained by art-bootstrap.
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
    // The graphics translation unit is materialized by the same audit
    // command that links the probe dylib.  Pass every split probe output
    // through here; otherwise this edge silently falls back to the legacy
    // runtime-graphics-link-probe directory and can link a mixed stale/new
    // object set before the final audit edge runs.
    graph.push_str(" && rm -f ");
    graph.push_str(&shell_quote(&graphics_object_path.to_string_lossy()));
    graph.push(' ');
    graph.push_str(&shell_quote(
        &graphics_object_path.with_extension("o.d").to_string_lossy(),
    ));
    graph.push(' ');
    graph.push_str(&shell_quote(
        &graphics_object_path
            .with_extension("o.fingerprint")
            .to_string_lossy(),
    ));
    graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
    graph.push_str(&shell_quote(&native_output_for_shell));
    graph.push_str(" DARWIN_ART_NATIVE_FILESYSTEM_OBJECT=");
    graph.push_str(&shell_quote(&filesystem_object_path.to_string_lossy()));
    graph.push_str(" DARWIN_ART_NATIVE_NETWORK_OBJECT=");
    graph.push_str(&shell_quote(&network_object_path.to_string_lossy()));
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
    graph.push_str(" cargo run -p art-bootstrap -- audit-runtime-graphics-link-fast\n");
    graph.push_str("  description = CXX runtime_graphics_probe\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&graphics_object);
    graph.push_str(": runtime_graphics_probe ");
    graph.push_str(&graphics_probe_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_probe_stamp));
    graph.push(' ');
    graph.push_str(&graphics_phase_object);
    graph.push(' ');
    graph.push_str(&graphics_input_object);
    graph.push(' ');
    graph.push_str(&graphics_state_object);
    graph.push(' ');
    graph.push_str(&ninja_path(&graphics_session_object_path));
    graph.push(' ');
    graph.push_str(&jni_acceptance_object);
    graph.push(' ');
    graph.push_str(&hwui_object);
    graph.push(' ');
    graph.push_str(&app_bootstrap_object);
    graph.push(' ');
    graph.push_str(&app_presentation_object);
    graph.push('\n');
    graph.push_str("rule runtime_graphics_phase_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_GRAPHICS_PHASE_OBJECT=");
    graph.push_str(&shell_quote(&graphics_phase_object_path.to_string_lossy()));
    graph.push_str(" cargo run -p art-bootstrap -- build-runtime-graphics-phase-probe\n");
    graph.push_str("  description = CXX runtime_graphics_phase\n");
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    graph.push_str("  restat = 1\n\n");
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
    use crate::graph::foundation::is_foundation_family_input;

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
