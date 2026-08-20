use super::*;

pub(crate) fn build_dex_probe(root: &Path) -> Result<()> {
    build_foundation(root)?;

    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let libziparchive_include = root.join("_aosp/system/libziparchive/include");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    if !libdexfile.join("Android.bp").exists() {
        return Err("libdexfile sources are missing; run `art-bootstrap sync` first".into());
    }

    let java_home = PathBuf::from("/opt/homebrew/opt/openjdk@17");
    let jni_include = java_home.join("include");
    let jni_darwin_include = jni_include.join("darwin");
    if !jni_include.join("jni.h").exists() {
        return Err("OpenJDK 17 JNI headers were not found under /opt/homebrew".into());
    }

    let build_dir = root.join("_build/dex-probe");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;
    fs::create_dir_all(&object_dir)?;

    let android_platform_jar = find_android_platform_jar()?;
    let android_mock_jar = android_platform_jar
        .parent()
        .ok_or("Android platform jar has no parent")?
        .join("optional/android.test.mock.jar");
    if !android_mock_jar.exists() {
        return Err(format!(
            "Android mock library is missing: {}",
            android_mock_jar.display()
        )
        .into());
    }
    let javac_classpath = env::join_paths([&android_platform_jar, &android_mock_jar])?;

    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg("-classpath")
            .arg(&javac_classpath)
            .arg(root.join("probes/Hello.java"))
            .arg(root.join("probes/ProbeActivity.java"))
            .arg(root.join("probes/ProbeContext.java"))
            .arg(root.join("probes/ProbeContentResolver.java"))
            .arg(root.join("probes/ProbeResources.java"))
            .arg(root.join("probes/ProbePackageManager.java"))
            .arg(root.join("probes/ProbeXmlResourceParser.java"))
            .arg(root.join("probes/ProbeCanvas.java"))
            .arg(root.join("probes/ProbeView.java"))
            .arg(root.join("probes/ProbeContentRoot.java"))
            .arg(root.join("probes/compile-stubs/android/content/IContentProvider.java"))
            .arg(root.join("probes/compile-stubs/android/content/ContentCaptureOptions.java"))
            .arg(root.join("probes/compile-stubs/android/view/autofill/AutofillManager.java")),
    )?;

    run_command(
        Command::new("unzip")
            .args(["-qq", "-o"])
            .arg(&android_mock_jar)
            .arg("android/test/mock/MockPackageManager.class")
            .arg("-d")
            .arg(&class_dir),
    )?;

    let package_manager_class = class_dir.join("dev/darwinart/probe/ProbePackageManager.class");
    let mock_package_manager_class = class_dir.join("android/test/mock/MockPackageManager.class");

    let hello_class = class_dir.join("dev/darwinart/probe/Hello.class");
    let activity_class = class_dir.join("dev/darwinart/probe/ProbeActivity.class");
    let context_class = class_dir.join("dev/darwinart/probe/ProbeContext.class");
    let resolver_class = class_dir.join("dev/darwinart/probe/ProbeContentResolver.class");
    let resources_class = class_dir.join("dev/darwinart/probe/ProbeResources.class");
    let xml_parser_class = class_dir.join("dev/darwinart/probe/ProbeXmlResourceParser.class");
    let canvas_class = class_dir.join("dev/darwinart/probe/ProbeCanvas.class");
    let view_class = class_dir.join("dev/darwinart/probe/ProbeView.class");
    let content_root_class = class_dir.join("dev/darwinart/probe/ProbeContentRoot.class");
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(&android_platform_jar)
            .arg("--classpath")
            .arg(&class_dir)
            .arg("--classpath")
            .arg(&android_mock_jar)
            .arg("--output")
            .arg(&dex_dir)
            .arg(&hello_class)
            .arg(&activity_class)
            .arg(&context_class)
            .arg(&resolver_class)
            .arg(&resources_class)
            .arg(&package_manager_class)
            .arg(&mock_package_manager_class)
            .arg(&xml_parser_class)
            .arg(&canvas_class)
            .arg(&view_class)
            .arg(&content_root_class),
    )?;

    let includes = [
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        libbase_include.as_path(),
        libziparchive_include.as_path(),
        palette_include.as_path(),
        Path::new("/opt/homebrew/include"),
        jni_include.as_path(),
        jni_darwin_include.as_path(),
    ];
    let dex_operator_source = build_dir.join("generated/dexfile_operator_out.cc");
    generate_operator_source(
        root,
        &libdexfile,
        &[
            "dex/dex_file.h",
            "dex/dex_file_layout.h",
            "dex/dex_instruction.h",
            "dex/dex_instruction_utils.h",
            "dex/invoke_type.h",
        ],
        &dex_operator_source,
    )?;
    let dex_sources = [
        dex_operator_source,
        libdexfile.join("dex/dex_file.cc"),
        libdexfile.join("dex/dex_file_loader.cc"),
        libdexfile.join("dex/standard_dex_file.cc"),
        libdexfile.join("dex/compact_dex_file.cc"),
        libdexfile.join("dex/compact_offset_table.cc"),
        libdexfile.join("dex/dex_file_verifier.cc"),
        libdexfile.join("dex/dex_file_exception_helpers.cc"),
        libdexfile.join("dex/dex_file_layout.cc"),
        libdexfile.join("dex/dex_file_tracking_registrar.cc"),
        libdexfile.join("dex/dex_instruction.cc"),
        libdexfile.join("dex/descriptors_names.cc"),
        libdexfile.join("dex/modifiers.cc"),
        libdexfile.join("dex/primitive.cc"),
        libdexfile.join("dex/signature.cc"),
        libdexfile.join("dex/type_lookup_table.cc"),
        libdexfile.join("dex/utf.cc"),
    ];
    let mut dex_objects = Vec::new();
    for source in dex_sources {
        dex_objects.push(compile_cpp(&source, &object_dir, &includes)?);
    }

    let dex_archive = build_dir.join("libdexfile-darwin.a");
    create_archive(&dex_archive, &dex_objects)?;

    let probe = build_dir.join("dex-probe");
    run_command(
        common_cpp_command(&includes)
            .arg(root.join("probes/dex_probe.cc"))
            .arg(&dex_archive)
            .arg(root.join("_build/foundation/libartbase-darwin.a"))
            .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
            .arg(root.join("_build/foundation/libziparchive-darwin.a"))
            .args(["-Wl,-dead_strip", "-lz", "-o"])
            .arg(&probe),
    )?;

    let classes_dex = dex_dir.join("classes.dex");
    let output = command_output(Command::new(&probe).arg(&classes_dex))?;
    let expected = "AOSP DEX: verified=yes version=35 classes=12 methods=318 \
                    class[0]=Landroid/test/mock/MockPackageManager; \
                    class[1]=Ldev/darwinart/probe/Hello; \
                    class[2]=Ldev/darwinart/probe/ProbeActivity; \
                    class[3]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[4]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[5]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[6]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[7]=Ldev/darwinart/probe/ProbeContext; \
                    class[8]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[9]=Ldev/darwinart/probe/ProbeResources; \
                    class[10]=Ldev/darwinart/probe/ProbeView; \
                    class[11]=Ldev/darwinart/probe/ProbeXmlResourceParser;";
    if output.trim() != expected {
        return Err(format!("unexpected DEX probe output: {output:?}").into());
    }

    let corrupt_dex = dex_dir.join("classes-corrupt.dex");
    let mut corrupt_bytes = fs::read(&classes_dex)?;
    let last = corrupt_bytes
        .last_mut()
        .ok_or("generated classes.dex was unexpectedly empty")?;
    *last ^= 0x01;
    fs::write(&corrupt_dex, corrupt_bytes)?;
    let rejected = Command::new(&probe).arg(&corrupt_dex).output()?;
    let rejected_stderr = String::from_utf8_lossy(&rejected.stderr);
    if rejected.status.success() || !rejected_stderr.contains("DEX verification failed") {
        return Err(format!(
            "corrupted DEX was not rejected as expected: status={} stderr={rejected_stderr:?}",
            rejected.status
        )
        .into());
    }

    println!("build-dex: {} corrupt=rejected", output.trim());
    Ok(())
}

pub(crate) fn build_elf_jni_dex_probe(root: &Path) -> Result<()> {
    let baseline_dex = root.join("_build/dex-probe/dex/classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    for input in [&baseline_dex, &dex_probe] {
        if !input.is_file() {
            return Err(format!(
                "ELF JNI DEX baseline is missing: {}; run `build-dex` first",
                input.display()
            )
            .into());
        }
    }
    let build_dir = root.join("_build/elf-jni-dex");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;
    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg(root.join("probes/android-elf-jni-fixture/NativeFixture.java")),
    )?;
    let classes_dex = dex_dir.join("classes.dex");
    if classes_dex.is_file() {
        fs::remove_file(&classes_dex)?;
    }
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(find_android_platform_jar()?)
            .arg("--output")
            .arg(&dex_dir)
            .arg(&baseline_dex)
            .arg(class_dir.join("darwin/art/nativefixture/NativeFixture.class")),
    )?;
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    let expected = "AOSP DEX: verified=yes version=35 classes=13 methods=323 \
                    class[0]=Landroid/test/mock/MockPackageManager; \
                    class[1]=Ldarwin/art/nativefixture/NativeFixture; \
                    class[2]=Ldev/darwinart/probe/Hello; \
                    class[3]=Ldev/darwinart/probe/ProbeActivity; \
                    class[4]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[5]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[6]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[7]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[8]=Ldev/darwinart/probe/ProbeContext; \
                    class[9]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[10]=Ldev/darwinart/probe/ProbeResources; \
                    class[11]=Ldev/darwinart/probe/ProbeView; \
                    class[12]=Ldev/darwinart/probe/ProbeXmlResourceParser;";
    if output.trim() != expected {
        return Err(format!("unexpected ELF JNI DEX probe output: {output:?}").into());
    }
    println!("build-elf-jni-dex: {}", output.trim());
    Ok(())
}

pub(crate) fn build_network_dex_probe(root: &Path) -> Result<()> {
    let baseline_dex = root.join("_build/dex-probe/dex/classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    for input in [&baseline_dex, &dex_probe] {
        if !input.is_file() {
            return Err(format!(
                "network DEX baseline is missing: {}; run `build-dex` first",
                input.display()
            )
            .into());
        }
    }
    let tool = root.join("tools/bionic-network-runtime-integration");
    let build_dir = root.join("_build/network-runtime-probe");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;
    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg(tool.join("probes/NetworkRuntimeFixture.java"))
            .arg(root.join("probes/button/ProbeAnimationHost.java")),
    )?;
    let classes_dex = dex_dir.join("classes.dex");
    if classes_dex.is_file() {
        fs::remove_file(&classes_dex)?;
    }
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(find_android_platform_jar()?)
            .arg("--min-api")
            .arg("35")
            .arg("--output")
            .arg(&dex_dir)
            .arg(&baseline_dex)
            .arg(class_dir.join("dev/darwinart/probe/NetworkRuntimeFixture.class"))
            .arg(class_dir.join("dev/darwinart/probe/ProbeAnimationHost.class"))
            .arg(class_dir.join("dev/darwinart/probe/ProbeAnimationHost$1.class")),
    )?;
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    if !output.contains("classes=15")
        || !output.contains("Ldev/darwinart/probe/NetworkRuntimeFixture;")
        || !output.contains("Ldev/darwinart/probe/ProbeAnimationHost;")
    {
        return Err(format!("unexpected network DEX probe output: {output:?}").into());
    }

    let fixture = build_dir.join("libdarwin_art_network_runtime.so");
    let clang = pinned_direct_apk_ndk_bin()?.join("aarch64-linux-android35-clang");
    run_command(
        Command::new(clang)
            .args([
                "-std=c17",
                "-O2",
                "-fno-builtin",
                "-fPIC",
                "-fno-stack-protector",
                "-U_FORTIFY_SOURCE",
                "-D_FORTIFY_SOURCE=0",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-shared",
                "-nostdlib",
                "-fuse-ld=lld",
                "-Wl,--build-id=none",
                "-Wl,--hash-style=sysv",
                "-Wl,-z,now",
                "-Wl,-z,norelro",
                "-Wl,-z,max-page-size=16384",
                "-Wl,-soname,libdarwin_art_network_runtime.so",
            ])
            .arg(format!(
                "-Wl,--version-script,{}",
                tool.join("probes/exports.map").display()
            ))
            .arg(tool.join("probes/network_jni.c"))
            .arg("-lc")
            .arg("-o")
            .arg(&fixture),
    )?;
    let kind = command_output(Command::new("file").arg(&fixture))?;
    if !kind.contains("ELF 64-bit LSB shared object, ARM aarch64") {
        return Err(format!("unexpected network fixture format: {kind}").into());
    }
    println!("build-network-dex: classes=15 methods=328 ELF=arm64 imports=8 loopback=only");
    Ok(())
}

pub(crate) fn build_button_dex_probe(root: &Path) -> Result<()> {
    // Rebuild the baseline first: the Button flavor intentionally reuses the
    // launcher/context/resource test classes, but replaces only Activity/View
    // and adds the real SystemFonts bootstrap. Keeping a separate DEX prevents
    // widget dependencies from weakening the small baseline regression gate.
    build_dex_probe(root)?;

    let android_platform_jar = find_android_platform_jar()?;
    let android_mock_jar = android_platform_jar
        .parent()
        .ok_or("Android platform jar has no parent")?
        .join("optional/android.test.mock.jar");
    if !android_mock_jar.is_file() {
        return Err(format!(
            "Android mock library is missing: {}",
            android_mock_jar.display()
        )
        .into());
    }

    let baseline_classes = root.join("_build/dex-probe/classes");
    let build_dir = root.join("_build/button-dex");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;

    let javac_classpath = env::join_paths([&android_platform_jar, &android_mock_jar])?;
    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg("-classpath")
            .arg(&javac_classpath)
            .arg(root.join("probes/button/FontBootstrap.java"))
            .arg(root.join("probes/button/ProbeAnimationHost.java"))
            .arg(root.join("probes/button/ProbeActivity.java"))
            .arg(root.join("probes/button/ProbeView.java"))
            .arg(root.join("tools/android-apk-app-runtime/fixture/DarwinServiceBridge.java")),
    )?;

    let baseline = |relative: &str| baseline_classes.join(relative);
    let button = |relative: &str| class_dir.join(relative);
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(&android_platform_jar)
            .arg("--classpath")
            .arg(&baseline_classes)
            .arg("--classpath")
            .arg(&android_mock_jar)
            .arg("--output")
            .arg(&dex_dir)
            .arg(baseline("android/test/mock/MockPackageManager.class"))
            .arg(baseline("dev/darwinart/probe/Hello.class"))
            .arg(baseline("dev/darwinart/probe/ProbeCanvas.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContentResolver.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContentRoot.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContext.class"))
            .arg(baseline("dev/darwinart/probe/ProbePackageManager.class"))
            .arg(baseline("dev/darwinart/probe/ProbeResources.class"))
            .arg(baseline("dev/darwinart/probe/ProbeXmlResourceParser.class"))
            .arg(button("dev/darwinart/probe/FontBootstrap.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost$1.class"))
            .arg(button("dev/darwinart/probe/ProbeActivity.class"))
            .arg(button("dev/darwinart/probe/ProbeView.class"))
            .arg(button("dev/darwinart/simple/DarwinServiceBridge.class"))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$DisplayHandler.class",
            )),
    )?;

    let classes_dex = dex_dir.join("classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    let expected = "AOSP DEX: verified=yes version=35 classes=18 methods=369 \
                    class[0]=Landroid/test/mock/MockPackageManager; \
                    class[1]=Ldev/darwinart/probe/FontBootstrap; \
                    class[2]=Ldev/darwinart/probe/Hello; \
                    class[3]=Ldev/darwinart/probe/ProbeActivity; \
                    class[4]=Ldev/darwinart/probe/ProbeAnimationHost$1; \
                    class[5]=Ldev/darwinart/probe/ProbeAnimationHost; \
                    class[6]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[7]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[8]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[9]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[10]=Ldev/darwinart/probe/ProbeContext; \
                    class[11]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[12]=Ldev/darwinart/probe/ProbeResources; \
                    class[13]=Ldev/darwinart/probe/ProbeView; \
                    class[14]=Ldev/darwinart/probe/ProbeXmlResourceParser; \
                    class[15]=Ldev/darwinart/simple/DarwinServiceBridge$DisplayHandler; \
                    class[16]=Ldev/darwinart/simple/DarwinServiceBridge$ManagerHandler; \
                    class[17]=Ldev/darwinart/simple/DarwinServiceBridge;";
    if output.trim() != expected {
        return Err(format!("unexpected Button DEX probe output: {output:?}").into());
    }

    println!("build-button-dex: {}", output.trim());
    Ok(())
}

pub(crate) fn find_d8() -> Result<PathBuf> {
    let sdk_root = android_sdk_root()?;
    let build_tools = sdk_root.join("build-tools");
    let mut candidates = fs::read_dir(&build_tools)?
        .filter_map(std::result::Result::ok)
        .map(|entry| entry.path().join("d8"))
        .filter(|path| path.is_file())
        .collect::<Vec<_>>();
    candidates.sort();
    candidates
        .pop()
        .ok_or_else(|| format!("d8 was not found under {}", build_tools.display()).into())
}

pub(crate) fn android_sdk_root() -> Result<PathBuf> {
    env::var_os("ANDROID_SDK_ROOT")
        .or_else(|| env::var_os("ANDROID_HOME"))
        .map(PathBuf::from)
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .map(|home| home.join("Library/Android/sdk"))
        })
        .ok_or_else(|| "could not determine the Android SDK directory".into())
}

pub(crate) fn find_android_platform_jar() -> Result<PathBuf> {
    let jar = android_sdk_root()?.join("platforms/android-36/android.jar");
    if !jar.is_file() {
        return Err(format!("Android API 36 platform JAR is missing: {}", jar.display()).into());
    }
    Ok(jar)
}
