use super::*;

pub(crate) struct RuntimeBootstrapStaging {
    pub(crate) root: PathBuf,
    pub(crate) build_dir: PathBuf,
    pub(crate) object_dir: PathBuf,
    pub(crate) patched_runtime: PathBuf,
    pub(crate) artbase: PathBuf,
    pub(crate) libprofile: PathBuf,
    pub(crate) runtime: PathBuf,
    pub(crate) android_jni_include: PathBuf,
    pub(crate) nativehelper_full_include: PathBuf,
    pub(crate) nativehelper_platform_headers: PathBuf,
    pub(crate) liblog_include: PathBuf,
    pub(crate) jni_proxy_include: PathBuf,
    pub(crate) jni_proxy_generated: PathBuf,
    pub(crate) android_icu_common: PathBuf,
    pub(crate) android_icu_i18n: PathBuf,
    pub(crate) libcutils_include: PathBuf,
    pub(crate) ndk_include: PathBuf,
    pub(crate) ndk_arch_include: PathBuf,
    pub(crate) includes: Vec<PathBuf>,
    pub(crate) operator_source: PathBuf,
}

pub(crate) fn prepare(root: &Path, real_graphics: bool) -> Result<RuntimeBootstrapStaging> {
    build_shell_gate(root, "build-android-elf-jni-fixture.sh")?;
    let build_paths = BuildPaths::from_root(root);
    run_command(
        Command::new("cargo")
            .args(["build", "--release", "--lib", "--manifest-path"])
            .arg(root.join("crates/darwin-art-elf-loader/Cargo.toml")),
    )?;

    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let cmdline = root.join("_aosp/art/cmdline");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let libelffile = root.join("_aosp/art/libelffile");
    let libprofile = root.join("_aosp/art/libprofile");
    let nativebridge_include = root.join("_aosp/art/libnativebridge/include");
    let nativeloader_include = root.join("_aosp/art/libnativeloader/include");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let runtime_gc = runtime.join("gc");
    let runtime_gc_collector = runtime.join("gc/collector");
    let runtime_gc_space = runtime.join("gc/space");
    let runtime_quick_entrypoints = runtime.join("entrypoints/quick");
    let runtime_mterp = runtime.join("interpreter/mterp");
    let runtime_oat = runtime.join("oat");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let generated_dir = root.join("_build/runtime-arm64/generated");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_headers = root.join("_aosp/libnativehelper/header_only_include");
    let nativehelper_platform_headers =
        root.join("_aosp/libnativehelper/platform_header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let odrefresh_include = root.join("_aosp/art/odrefresh/include");
    let sigchain = root.join("_aosp/art/sigchainlib");
    let unwindstack_include = root.join("_aosp/system/unwinding/libunwindstack/include");
    let compat = root.join("compat");
    let public_include = root.join("include");
    let graphics_registration_include = root.join("_build/android-graphics-jni/generated");
    let android_icu_include = root.join("_aosp/external/icu-graphics/libandroidicuinit/include");
    let android_icu_common = root.join("_aosp/external/icu-graphics/icu4c/source/common");
    let android_icu_i18n = root.join("_aosp/external/icu-graphics/icu4c/source/i18n");
    let android_graphics_apex_include = root.join("_aosp/frameworks/base/libs/hwui/apex/include");
    let aosp_fmt_include = root.join("_aosp/external/fmtlib/include");
    let libcutils_include = root.join("_aosp/system/core/libcutils/include");
    let liblog_include = root.join("_aosp/system/logging/liblog/include");
    let nativehelper_full_include = root.join("_aosp/libnativehelper-full/include");
    let elf_loader_include = root.join("crates/darwin-art-elf-loader/include");
    let jni_proxy_include = root.join("tools/android-jni-proxy/include");
    let jni_proxy_generated = root.join("tools/android-jni-proxy/generated");
    let elf_fixture_generated = root.join("_build/android-elf-jni-fixture/generated");
    let openjdk_math_source = root.join("_aosp/libcore/ojluni/src/main/native/Math.c");
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;

    if !generated_dir.join("asm_defines.h").exists() {
        return Err(
            "generated ART ABI constants are missing; run `build-runtime-arm64` first".into(),
        );
    }
    if !openjdk_math_source.is_file() {
        return Err(format!(
            "Android libcore Math source is missing: {}; run `art-bootstrap sync` first",
            openjdk_math_source.display()
        )
        .into());
    }

    let build_dir = build_paths.native_output(if real_graphics {
        "runtime-graphics-bootstrap"
    } else {
        "runtime-bootstrap"
    });
    let runtime_generated_dir = build_dir.join("generated");
    let patched_source_dir = build_dir.join("patched-source");
    let patched_runtime = patched_source_dir.join("runtime");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&patched_runtime)?;
    fs::create_dir_all(&object_dir)?;
    fs::create_dir_all(&runtime_generated_dir)?;
    copy_runtime_sources(&runtime, &patched_runtime)?;

    for patch in [
        "patches/art/0006-darwin-standard-signal-set.patch",
        "patches/art/0007-darwin-thread-cpu-time.patch",
        "patches/art/0008-darwin-nonfutex-suspend-barrier.patch",
        "patches/art/0009-darwin-locksupport-park.patch",
        "patches/art/0010-darwin-host-gc-release-policy.patch",
        "patches/art/0011-darwin-disable-userfaultfd-mark-compact.patch",
        "patches/art/0013-darwin-stat-mtime.patch",
        "patches/art/0014-darwin-exec-pidfd-fallback.patch",
        "patches/art/0017-darwin-disable-nterp.patch",
        "patches/art/0019-darwin-disable-nterp-catch-entry.patch",
        "patches/art/0022-darwin-base-relative-heap-references.patch",
        "patches/art/0023-darwin-enable-quick-allocation-entrypoints.patch",
        "patches/art/0024-darwin-arm64-ucontext-dump.patch",
        "patches/art/0025-darwin-morecore-diagnostics.patch",
        "patches/art/0027-darwin-string-abi-overlay.patch",
        "patches/art/0028-darwin-minimal-runtime-start.patch",
        "patches/art/0029-darwin-arm64-native-stack-pcs.patch",
        "patches/art/0030-darwin-large-object-bitmap-window.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(&patched_source_dir),
        )?;
    }

    let mut includes = vec![
        public_include,
        compat,
        elf_fixture_generated,
        elf_loader_include,
        jni_proxy_include.clone(),
        generated_dir,
        generator,
        patched_runtime.clone(),
        patched_artbase,
        artbase.clone(),
        cmdline,
        libdexfile,
        libelffile,
        libprofile.clone(),
        nativebridge_include,
        nativeloader_include,
        odrefresh_include,
        sigchain,
        runtime.clone(),
        runtime_base,
        runtime_arm64,
        runtime_gc,
        runtime_gc_collector,
        runtime_gc_space,
        runtime_quick_entrypoints,
        runtime_mterp,
        runtime_oat,
        palette_include,
        libbase_include,
        unwindstack_include,
        tinyxml2,
        android_jni_include.clone(),
        nativehelper_headers,
        nativehelper_platform_headers.clone(),
        dlmalloc,
        PathBuf::from("/opt/homebrew/opt/icu4c@78/include"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    if real_graphics {
        let generated_registration =
            graphics_registration_include.join("darwin_android_graphics_registration.h");
        if !generated_registration.is_file() {
            return Err(format!(
                "generated Android graphics registration header is missing: {}; run `build-android-graphics-jni` first",
                generated_registration.display()
            )
            .into());
        }
        if !android_icu_include
            .join("androidicuinit/android_icu_init.h")
            .is_file()
        {
            return Err("Android ICU init headers are missing; run `build-icu` first".into());
        }
        includes.insert(0, graphics_registration_include);
        includes.insert(1, android_icu_include);
        includes.insert(2, android_graphics_apex_include);
        includes.insert(3, aosp_fmt_include);
    }

    let operator_source = runtime_generated_dir.join("operator_out.cc");
    if !operator_source.is_file() {
        let mut generate = Command::new("python3");
        generate
            .arg(root.join("_aosp/art/tools/generate_operator_out.py"))
            .arg(&runtime);
        for header in [
            "base/callee_save_type.h",
            "base/locks.h",
            "class_status.h",
            "compilation_kind.h",
            "gc/allocator/rosalloc.h",
            "gc/allocator_type.h",
            "gc/collector/gc_type.h",
            "gc/collector/mark_compact.h",
            "gc/collector_type.h",
            "gc/space/region_space.h",
            "gc/space/space.h",
            "gc/weak_root_state.h",
            "gc_root.h",
            "indirect_reference_table.h",
            "instrumentation.h",
            "jdwp_provider.h",
            "jni_id_type.h",
            "linear_alloc.h",
            "lock_word.h",
            "oat/image.h",
            "oat/oat.h",
            "oat/oat_file.h",
            "process_state.h",
            "reflective_value_visitor.h",
            "stack.h",
            "suspend_reason.h",
            "thread.h",
            "thread_state.h",
            "trace.h",
            "trace_profile.h",
            "verifier/verifier_enums.h",
        ] {
            generate.arg(runtime.join(header));
        }
        fs::write(&operator_source, command_output(&mut generate)?)?;
    }

    Ok(RuntimeBootstrapStaging {
        root: root.to_owned(),
        build_dir,
        object_dir,
        patched_runtime,
        artbase,
        libprofile,
        runtime,
        android_jni_include,
        nativehelper_full_include,
        nativehelper_platform_headers,
        liblog_include,
        jni_proxy_include,
        jni_proxy_generated,
        android_icu_common,
        android_icu_i18n,
        libcutils_include,
        ndk_include,
        ndk_arch_include,
        includes,
        operator_source,
    })
}

fn copy_runtime_sources(runtime: &Path, patched_runtime: &Path) -> Result<()> {
    for source in [
        "runtime.cc",
        "signal_set.h",
        "class_linker.cc",
        "class_linker.h",
        "mirror/object_reference.h",
        "mirror/string-inl.h",
        "gc/heap.cc",
        "gc/space/malloc_space.cc",
        "gc/space/space.cc",
        "entrypoints/quick/quick_alloc_entrypoints.cc",
        "entrypoints/quick/callee_save_frame.h",
        "entrypoints/quick/quick_trampoline_entrypoints.cc",
        "arch/arm64/jni_frame_arm64.h",
        "runtime_common.cc",
        "runtime.h",
        "thread.cc",
        "thread.h",
        "thread_list.cc",
        "gc/collector/garbage_collector.cc",
        "gc/collector/mark_compact.cc",
        "oat/oat_file.cc",
        "exec_utils.cc",
        "signal_catcher.cc",
        "nterp_helpers.cc",
        "interpreter/mterp/nterp.cc",
    ] {
        let destination = patched_runtime.join(source);
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(runtime.join(source), destination)?;
    }
    Ok(())
}
