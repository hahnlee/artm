use std::fs;
use std::io;
use std::path::Path;

use darwin_art_build_contract::RUNTIME_CACHE_IDENTITY;
use darwin_art_build_contract::{GRAPHICS_ADAPTER_SOURCES, HEADLESS_ADAPTER_SOURCES};

use super::super::*;
use super::GRAPH_VERSION;
use super::atomic;
use super::cache::{
    cached_native_objects_from_dirs, emit_cached_native_graph, emit_cached_native_graph_with_inputs,
};
use super::foundation::{
    FoundationFamily, cached_foundation_objects, foundation_input_list, foundation_inputs,
};
use super::inputs::{collect_files, is_probe_only_input};
use super::manifest::prepare as prepare_manifest;
use super::probe_manifest;
use super::representative::emit_representative_edges;

pub(crate) fn emit_graph(out: &Path) -> io::Result<()> {
    let manifest = prepare_manifest(out)?;
    let root = manifest.root;
    let inputs = manifest.inputs;
    let bootstrap_inputs = manifest.bootstrap_inputs;
    let digest = manifest.digest;
    let toolchain = manifest.toolchain;
    let cache_dir = manifest.cache_dir;
    let digest_path = manifest.digest_path;

    let root_for_shell = root.to_string_lossy().into_owned();
    // Ninja edges run the already-built bootstrap CLI directly.  Cargo is
    // still the public entry point that emits this graph, but invoking Cargo
    // once per native TU edge defeats the persistent object cache and adds a
    // process/workspace resolution cost to every warm build.
    let bootstrap_cli = shell_quote(&root.join("target/debug/art-bootstrap").to_string_lossy());
    let bootstrap_cli_path = root.join("target/debug/art-bootstrap");
    let bootstrap_cli_target = ninja_path(&bootstrap_cli_path);
    let mut bootstrap_cli_inputs = vec![root.join("Cargo.toml"), root.join("Cargo.lock")];
    collect_files(
        &root.join("crates/art-bootstrap"),
        &root,
        &mut bootstrap_cli_inputs,
    );
    collect_files(
        &root.join("crates/darwin-art-build-contract"),
        &root,
        &mut bootstrap_cli_inputs,
    );
    bootstrap_cli_inputs.sort();
    bootstrap_cli_inputs.dedup();
    // Keep compiler outputs at a stable path.  The graph digest is a
    // manifest/invalidation identity, not an object-cache namespace: moving
    // objects into a new digest directory would turn every header edit into
    // a cold 200+ TU rebuild and defeat dependency-fingerprint caching.
    let native_output_root = root.join("_build");
    let runtime_owner_archive_path = root.join("target/release/libdarwin_art_runtime.a");
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
    let app_resources_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_app_resources.cc.o");
    let app_presentation_object_path =
        native_output_root.join("runtime-probes/darwin_art_runtime_app_presentation.cc.o");
    let stamp_path = cache_dir.join("graphics-bootstrap.stamp");
    let graphics_ready_path = cache_dir.join("graphics-bootstrap.ready");
    let runtime_stamp_path = cache_dir.join("runtime-bootstrap.stamp");
    let runtime_ready_path = cache_dir.join("runtime-bootstrap.ready");
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
    let runtime_owner_archive = ninja_path(&runtime_owner_archive_path);
    let filesystem_object = ninja_path(&filesystem_object_path);
    let network_object = ninja_path(&network_object_path);
    let hwui_object = ninja_path(&hwui_object_path);
    let graphics_object = ninja_path(&graphics_object_path);
    let graphics_phase_object = ninja_path(&graphics_phase_object_path);
    let graphics_input_object = ninja_path(&graphics_input_object_path);
    let graphics_state_object = ninja_path(&graphics_state_object_path);
    let jni_acceptance_object = ninja_path(&jni_acceptance_object_path);
    let app_bootstrap_object = ninja_path(&app_bootstrap_object_path);
    let app_resources_object = ninja_path(&app_resources_object_path);
    let app_presentation_object = ninja_path(&app_presentation_object_path);
    let stamp_for_shell = stamp_path.to_string_lossy().into_owned();
    let native_output_for_shell = native_output_root.to_string_lossy().into_owned();
    let runtime_common_objects = native_output_root.join("runtime-common/objects");
    let runtime_objects = native_output_root.join("runtime-bootstrap/objects");
    let graphics_objects = native_output_root.join("runtime-graphics-bootstrap/objects");
    let shared_runtime_cache_ready =
        fs::read_to_string(native_output_root.join("runtime-common/cache-identity"))
            .is_ok_and(|identity| identity.trim() == RUNTIME_CACHE_IDENTITY);
    let cached_runtime_objects = if shared_runtime_cache_ready {
        cached_native_objects_from_dirs(
            &[&runtime_common_objects, &runtime_objects],
            &runtime_archive_path,
            HEADLESS_ADAPTER_SOURCES,
        )?
    } else {
        None
    };
    let cached_graphics_objects = if shared_runtime_cache_ready {
        cached_native_objects_from_dirs(
            &[&runtime_common_objects, &graphics_objects],
            &archive_path,
            GRAPHICS_ADAPTER_SOURCES,
        )?
    } else {
        None
    };
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
    let probe_manifest::ProbeGraphInputs {
        filesystem_probe_inputs,
        network_probe_inputs,
        hwui_probe_inputs,
        graphics_probe_inputs,
        graphics_phase_inputs,
        graphics_input_inputs,
        graphics_state_inputs,
        graphics_session_inputs,
        jni_acceptance_inputs,
        app_bootstrap_inputs,
        app_resources_inputs,
        app_presentation_inputs,
        filesystem_probe_stamp,
        network_probe_stamp,
        hwui_probe_stamp,
        graphics_probe_stamp,
        graphics_phase_stamp,
        graphics_input_stamp,
        graphics_state_stamp,
        graphics_session_stamp,
        jni_acceptance_stamp,
        app_bootstrap_stamp,
        app_resources_stamp,
        app_presentation_stamp,
        runtime_entry_stamp,
    } = probe_manifest::collect(&root)?;

    // Rust runtime ownership is a first-class link input.  Keeping this as a
    // separate Ninja edge means a RuntimeSession/provider change rebuilds the
    // Rust archive and relinks the final dylib without invalidating any C++
    // translation unit in the ART/HWUI graph.
    let mut runtime_owner_inputs = vec![root.join("Cargo.toml"), root.join("Cargo.lock")];
    for directory in [
        "crates/darwin-art-runtime",
        "crates/darwin-art-engine-sys",
        "crates/darwin-art-abi",
    ] {
        collect_files(&root.join(directory), &root, &mut runtime_owner_inputs);
    }
    runtime_owner_inputs.sort();
    runtime_owner_inputs.dedup();

    let mut graph = String::new();
    graph.push_str("# Generated by darwin-art-xtask. Do not edit.\n");
    graph.push_str(&format!("# graph-version: {GRAPH_VERSION}\n"));
    graph.push_str(&format!("# input-digest: {digest}\n"));
    graph.push_str(&format!("# compiler: {} sdk: {SDK_NAME}\n", toolchain.cxx));
    graph.push_str("ninja_required_version = 1.10\n\n");
    graph.push_str("rule art_bootstrap_cli\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && cargo build -q -p art-bootstrap\n");
    graph.push_str("  description = Rust art-bootstrap CLI\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&bootstrap_cli_target);
    graph.push_str(": art_bootstrap_cli ");
    for input in &bootstrap_cli_inputs {
        graph.push_str(&ninja_path(input));
        graph.push(' ');
    }
    graph.push('\n');
    let mut cached_rules_emitted = false;
    if let Some(cached_objects) = cached_graphics_objects.as_deref() {
        let (shared_objects, flavor_objects): (Vec<_>, Vec<_>) = cached_objects
            .iter()
            .cloned()
            .partition(|object| object.object.starts_with(&runtime_common_objects));
        let shared_inputs = shared_objects
            .iter()
            .map(|object| object.object.clone())
            .collect::<Vec<_>>();
        emit_cached_native_graph_with_inputs(
            &mut graph,
            &flavor_objects,
            &archive,
            &mut cached_rules_emitted,
            &shared_inputs,
        );
        graph.push_str("build ");
        graph.push_str(&stamp);
        graph.push_str(": phony ");
        graph.push_str(&archive);
        graph.push('\n');
        graph.push_str("# graphics-bootstrap uses persisted per-object commands\n\n");
    } else {
        // A prior fallback may have produced an archive before a newly added
        // adapter source was promoted into the cache. The readiness marker is
        // intentionally a separate output: an existing archive alone must
        // not make Ninja accept an incomplete canonical fallback.
        graph.push_str("rule graphics_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
        graph.push_str(&shell_quote(&native_output_for_shell));
        graph.push(' ');
        graph.push_str(&bootstrap_cli);
        graph.push_str(" build-runtime-graphics-bootstrap-internal && touch ");
        graph.push_str(&shell_quote(&stamp_for_shell));
        graph.push(' ');
        graph.push_str(&shell_quote(&graphics_ready_path.to_string_lossy()));
        graph.push('\n');
        graph.push_str("  description = GRAPHICS bootstrap\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&stamp);
        graph.push(' ');
        graph.push_str(&archive);
        graph.push(' ');
        graph.push_str(&ninja_path(&graphics_ready_path));
        graph.push_str(": graphics_bootstrap ");
        graph.push_str(&bootstrap_input_list);
        graph.push('\n');
    }
    graph.push_str("build graphics-bootstrap: phony ");
    graph.push_str(&bootstrap_cli_target);
    graph.push(' ');
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
        // Keep the same completeness contract for the headless archive. This
        // marker is absent after an interrupted or source-incomplete fallback
        // and therefore forces the canonical builder to run again.
        graph.push_str("rule runtime_bootstrap\n");
        graph.push_str("  command = cd ");
        graph.push_str(&shell_quote(&root_for_shell));
        graph.push_str(" && DARWIN_ART_NATIVE_OUTPUT_ROOT=");
        graph.push_str(&shell_quote(&native_output_for_shell));
        graph.push(' ');
        graph.push_str(&bootstrap_cli);
        graph.push_str(" build-runtime-bootstrap-internal && touch ");
        graph.push_str(&shell_quote(&runtime_stamp_path.to_string_lossy()));
        graph.push(' ');
        graph.push_str(&shell_quote(&runtime_ready_path.to_string_lossy()));
        graph.push('\n');
        graph.push_str("  description = ART bootstrap\n");
        graph.push_str("  restat = 1\n\n");
        graph.push_str("build ");
        graph.push_str(&runtime_stamp);
        graph.push(' ');
        graph.push_str(&runtime_archive);
        graph.push(' ');
        graph.push_str(&ninja_path(&runtime_ready_path));
        graph.push_str(": runtime_bootstrap ");
        graph.push_str(&bootstrap_input_list);
        graph.push('\n');
    }
    graph.push_str("build runtime-bootstrap: phony ");
    graph.push_str(&bootstrap_cli_target);
    graph.push(' ');
    graph.push_str(&runtime_archive);
    graph.push('\n');

    graph.push_str("rule runtime_owner_archive\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(
        " && cargo build -q --release --manifest-path crates/darwin-art-runtime/Cargo.toml\n",
    );
    graph.push_str("  description = Rust runtime owner archive\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&runtime_owner_archive);
    graph.push_str(": runtime_owner_archive ");
    for input in &runtime_owner_inputs {
        graph.push_str(&ninja_path(input));
        graph.push(' ');
    }
    graph.push('\n');
    graph.push_str("build runtime-owner: phony ");
    graph.push_str(&runtime_owner_archive);
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-filesystem-probe\n");
    graph.push_str("  description = CXX runtime_filesystem_probe\n");
    // The Rust command owns the C++ dependency cache for this probe.  Ninja
    // only tracks the explicit phase stamp and source inputs; consuming a
    // depfile produced inside the bootstrap CLI makes the stored dependency mtime
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-network-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-hwui-probe\n");
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
    graph.push_str(" DARWIN_ART_NATIVE_APP_RESOURCES_OBJECT=");
    graph.push_str(&shell_quote(&app_resources_object_path.to_string_lossy()));
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" audit-runtime-graphics-link-fast\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-graphics-phase-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-graphics-input-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-graphics-state-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-graphics-session-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-jni-acceptance-probe\n");
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
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-app-bootstrap-probe\n");
    graph.push_str("  description = CXX runtime_app_bootstrap\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&app_bootstrap_object);
    graph.push_str(": runtime_app_bootstrap_probe ");
    graph.push_str(&app_bootstrap_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&app_bootstrap_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_app_resources_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_APP_RESOURCES_OBJECT=");
    graph.push_str(&shell_quote(&app_resources_object_path.to_string_lossy()));
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-app-resources-probe\n");
    graph.push_str("  description = CXX runtime_app_resources\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&app_resources_object);
    graph.push_str(": runtime_app_resources_probe ");
    graph.push_str(&app_resources_inputs);
    graph.push(' ');
    graph.push_str(&ninja_path(&app_resources_stamp));
    graph.push('\n');
    graph.push_str("rule runtime_app_presentation_probe\n");
    graph.push_str("  command = cd ");
    graph.push_str(&shell_quote(&root_for_shell));
    graph.push_str(" && DARWIN_ART_NATIVE_APP_PRESENTATION_OBJECT=");
    graph.push_str(&shell_quote(
        &app_presentation_object_path.to_string_lossy(),
    ));
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" build-runtime-app-presentation-probe\n");
    graph.push_str("  description = CXX runtime_app_presentation\n");
    graph.push_str("  restat = 1\n\n");
    graph.push_str("build ");
    graph.push_str(&app_presentation_object);
    graph.push_str(": runtime_app_presentation_probe ");
    graph.push(' ');
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
    graph.push_str(" DARWIN_ART_NATIVE_APP_RESOURCES_OBJECT=");
    graph.push_str(&shell_quote(&app_resources_object_path.to_string_lossy()));
    // The full upstream closure is a separate release/CI gate.  The Ninja
    // graph is the developer inner loop and must only relink/audit against
    // already materialized foundation artifacts.
    graph.push(' ');
    graph.push_str(&bootstrap_cli);
    graph.push_str(" audit-runtime-graphics-link-fast\n");
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
    graph.push_str(&app_resources_object);
    graph.push(' ');
    graph.push_str(&app_presentation_object);
    graph.push(' ');
    graph.push_str(&probe_only_input_list);
    graph.push(' ');
    graph.push_str(&runtime_owner_archive);
    graph.push(' ');
    graph.push_str(&ninja_path(&runtime_entry_stamp));
    graph.push('\n');
    graph.push_str("build graphics-audit: phony ");
    graph.push_str(&bootstrap_cli_target);
    graph.push(' ');
    graph.push_str(&runtime_library);
    graph.push('\n');
    graph.push_str("build graph-input-digest: phony ");
    graph.push_str(&ninja_path(&digest_path));
    graph.push('\n');

    emit_representative_edges(&mut graph, &root, &cache_dir, &toolchain)?;

    atomic::write(out, graph.as_bytes())?;
    println!(
        "darwin-art-xtask: wrote {} (inputs={} digest={digest})",
        out.display(),
        inputs.len()
    );
    Ok(())
}
