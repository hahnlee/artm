use super::*;

pub(crate) fn workspace_root() -> Result<PathBuf> {
    Ok(PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()?)
}

pub(crate) fn build_shell_gate(root: &Path, script: &str) -> Result<()> {
    build_shell_gate_with_args(root, script, &[])
}

pub(crate) fn build_shell_gate_with_args(root: &Path, script: &str, args: &[&str]) -> Result<()> {
    let script = root.join("tools").join(script);
    if !script.is_file() {
        return Err(format!("build gate script is missing: {}", script.display()).into());
    }
    run_command(
        Command::new("bash")
            .arg(script)
            .args(args)
            .current_dir(root),
    )
}

pub(crate) fn doctor() -> Result<()> {
    if env::consts::OS != "macos" || env::consts::ARCH != "aarch64" {
        return Err(format!(
            "requires arm64 macOS, found {}-{}",
            env::consts::ARCH,
            env::consts::OS
        )
        .into());
    }

    #[cfg(target_os = "macos")]
    let page_size = unsafe { getpagesize() };

    #[cfg(not(target_os = "macos"))]
    let page_size = 0;

    if page_size != 16 * 1024 {
        return Err(format!("expected a 16 KiB page size, found {page_size}").into());
    }
    for dependency in [
        "/opt/homebrew/opt/icu4c@78/include/unicode/ucnv.h",
        "/opt/homebrew/opt/icu4c@78/lib/libicui18n.dylib",
        "/opt/homebrew/opt/icu4c@78/lib/libicuuc.dylib",
    ] {
        if !Path::new(dependency).is_file() {
            return Err(format!("Homebrew icu4c@78 dependency is missing: {dependency}").into());
        }
    }

    let clang = command_output(Command::new("clang").arg("--version"))?;
    let first_line = clang.lines().next().unwrap_or("unknown clang");
    println!("doctor: arm64 macOS, page_size={page_size}, {first_line}");
    Ok(())
}

pub(crate) fn sync_sources(root: &Path) -> Result<()> {
    let lock = read_lock(root)?;
    let revision = lock_value(&lock, "ART_REVISION")?;
    materialize_archive(
        root,
        "platform/external/skia",
        lock_value(&lock, "SKIA_REVISION")?,
        "external-skia",
        "",
        "_aosp/external/skia",
        "BUILD.gn",
        lock_value(&lock, "SKIA_BUILD_GN_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/core",
        lock_value(&lock, "SYSTEM_CORE_REVISION")?,
        "system-core-libcutils",
        "libcutils",
        "_aosp/system/core/libcutils",
        "Android.bp",
        lock_value(&lock, "LIBCUTILS_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/core",
        lock_value(&lock, "SYSTEM_CORE_REVISION")?,
        "system-core-libutils",
        "libutils",
        "_aosp/system/core/libutils",
        "Android.bp",
        lock_value(&lock, "LIBUTILS_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/core",
        lock_value(&lock, "SYSTEM_CORE_REVISION")?,
        "system-core-libsystem",
        "libsystem",
        "_aosp/system/core/libsystem",
        "Android.bp",
        lock_value(&lock, "LIBSYSTEM_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/logging",
        lock_value(&lock, "SYSTEM_LOGGING_REVISION")?,
        "system-logging-liblog",
        "liblog",
        "_aosp/system/logging/liblog",
        "Android.bp",
        lock_value(&lock, "LIBLOG_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/libnativehelper",
        lock_value(&lock, "LIBNATIVEHELPER_REVISION")?,
        "libnativehelper-full",
        "",
        "_aosp/libnativehelper-full",
        "Android.bp",
        lock_value(&lock, "LIBNATIVEHELPER_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/libcore",
        lock_value(&lock, "LIBCORE_REVISION")?,
        "ojluni/src/main/native/Android.bp",
        "_aosp/libcore/ojluni/src/main/native/Android.bp",
        lock_value(&lock, "LIBCORE_OPENJDK_NATIVE_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/libcore",
        lock_value(&lock, "LIBCORE_REVISION")?,
        "ojluni/src/main/native/Math.c",
        "_aosp/libcore/ojluni/src/main/native/Math.c",
        lock_value(&lock, "LIBCORE_OPENJDK_MATH_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/frameworks/base",
        lock_value(&lock, "FRAMEWORKS_BASE_REVISION")?,
        "frameworks-base-nativehelper-jvm",
        "libs/nativehelper_jvm",
        "_aosp/frameworks/base/libs/nativehelper_jvm",
        "Android.bp",
        lock_value(&lock, "NATIVEHELPER_JVM_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/frameworks/native",
        lock_value(&lock, "FRAMEWORKS_NATIVE_REVISION")?,
        "frameworks-native-ui",
        "libs/ui",
        "_aosp/frameworks/native/libs/ui",
        "Android.bp",
        lock_value(&lock, "FRAMEWORKS_NATIVE_UI_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/frameworks/native",
        lock_value(&lock, "FRAMEWORKS_NATIVE_REVISION")?,
        "frameworks-native-arect",
        "libs/arect",
        "_aosp/frameworks/native/libs/arect",
        "Android.bp",
        lock_value(&lock, "FRAMEWORKS_NATIVE_ARECT_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/frameworks/native",
        lock_value(&lock, "FRAMEWORKS_NATIVE_REVISION")?,
        "frameworks-native-math",
        "libs/math",
        "_aosp/frameworks/native/libs/math",
        "Android.bp",
        lock_value(&lock, "FRAMEWORKS_NATIVE_MATH_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/zlib",
        lock_value(&lock, "ZLIB_REVISION")?,
        "external-zlib",
        "",
        "_aosp/external/zlib",
        "Android.bp",
        lock_value(&lock, "ZLIB_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/libpng",
        lock_value(&lock, "LIBPNG_REVISION")?,
        "external-libpng",
        "",
        "_aosp/external/libpng",
        "Android.bp",
        lock_value(&lock, "LIBPNG_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/freetype",
        lock_value(&lock, "FREETYPE_REVISION")?,
        "external-freetype",
        "",
        "_aosp/external/freetype",
        "Android.bp",
        lock_value(&lock, "FREETYPE_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/harfbuzz_ng",
        lock_value(&lock, "HARFBUZZ_NG_REVISION")?,
        "Android.bp",
        "_aosp/external/harfbuzz_ng/Android.bp",
        lock_value(&lock, "HARFBUZZ_NG_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/harfbuzz_ng",
        lock_value(&lock, "HARFBUZZ_NG_REVISION")?,
        "external-harfbuzz-src",
        "src",
        "_aosp/external/harfbuzz_ng/src",
        "hb.h",
        lock_value(&lock, "HARFBUZZ_NG_HB_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/frameworks/minikin",
        lock_value(&lock, "MINIKIN_REVISION")?,
        "frameworks-minikin",
        "",
        "_aosp/frameworks/minikin",
        "Android.bp",
        lock_value(&lock, "MINIKIN_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "Android.bp",
        "_aosp/external/icu-graphics/Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_ROOT_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "android_icu4c/Android.bp",
        "_aosp/external/icu-graphics/android_icu4c/Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_ANDROID_ICU4C_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "icu4c/source/Android.bp",
        "_aosp/external/icu-graphics/icu4c/source/Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_SOURCE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "graphics-icu-android-include",
        "android_icu4c/include",
        "_aosp/external/icu-graphics/android_icu4c/include",
        "uconfig_local.h",
        lock_value(&lock, "GRAPHICS_ICU_UCONFIG_LOCAL_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "graphics-icu-common",
        "icu4c/source/common",
        "_aosp/external/icu-graphics/icu4c/source/common",
        "Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_COMMON_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "graphics-icu-i18n",
        "icu4c/source/i18n",
        "_aosp/external/icu-graphics/icu4c/source/i18n",
        "Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_I18N_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "graphics-icu-stubdata",
        "icu4c/source/stubdata",
        "_aosp/external/icu-graphics/icu4c/source/stubdata",
        "Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_STUBDATA_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "graphics-icu-init",
        "libandroidicuinit",
        "_aosp/external/icu-graphics/libandroidicuinit",
        "Android.bp",
        lock_value(&lock, "GRAPHICS_ICU_INIT_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-runtime",
        "runtime",
        "_aosp/art/runtime",
        "Android.bp",
        lock_value(&lock, "ART_RUNTIME_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libartbase",
        "libartbase",
        "_aosp/art/libartbase",
        "Android.bp",
        lock_value(&lock, "ART_LIBARTBASE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libdexfile",
        "libdexfile",
        "_aosp/art/libdexfile",
        "Android.bp",
        lock_value(&lock, "ART_LIBDEXFILE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libprofile",
        "libprofile",
        "_aosp/art/libprofile",
        "Android.bp",
        lock_value(&lock, "ART_LIBPROFILE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-cmdline",
        "cmdline",
        "_aosp/art/cmdline",
        "Android.bp",
        lock_value(&lock, "ART_CMDLINE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libelffile",
        "libelffile",
        "_aosp/art/libelffile",
        "Android.bp",
        lock_value(&lock, "ART_LIBELFFILE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-odrefresh-include",
        "odrefresh/include",
        "_aosp/art/odrefresh/include",
        "odr_statslog/odr_statslog.h",
        lock_value(&lock, "ART_ODR_STATSLOG_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-sigchainlib",
        "sigchainlib",
        "_aosp/art/sigchainlib",
        "Android.bp",
        lock_value(&lock, "ART_SIGCHAIN_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libnativebridge-include",
        "libnativebridge/include",
        "_aosp/art/libnativebridge/include",
        "nativebridge/native_bridge.h",
        lock_value(&lock, "ART_NATIVEBRIDGE_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libnativeloader-include",
        "libnativeloader/include",
        "_aosp/art/libnativeloader/include",
        "nativeloader/native_loader.h",
        lock_value(&lock, "ART_NATIVELOADER_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-libartpalette-include",
        "libartpalette/include",
        "_aosp/art/libartpalette/include",
        "palette/palette.h",
        lock_value(&lock, "ART_PALETTE_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/art",
        revision,
        "art-cpp-define-generator",
        "tools/cpp-define-generator",
        "_aosp/art/tools/cpp-define-generator",
        "asm_defines.def",
        lock_value(&lock, "ART_ASM_DEFINES_DEF_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/art",
        revision,
        "tools/generate_operator_out.py",
        "_aosp/art/tools/generate_operator_out.py",
        lock_value(&lock, "ART_OPERATOR_GENERATOR_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/libbase",
        lock_value(&lock, "LIBBASE_REVISION")?,
        "system-libbase",
        "",
        "_aosp/system/libbase",
        "Android.bp",
        lock_value(&lock, "LIBBASE_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/tinyxml2",
        lock_value(&lock, "TINYXML2_REVISION")?,
        "external-tinyxml2",
        "",
        "_aosp/external/tinyxml2",
        "Android.bp",
        lock_value(&lock, "TINYXML2_ANDROID_BP_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/libnativehelper",
        lock_value(&lock, "LIBNATIVEHELPER_REVISION")?,
        "libnativehelper-jni",
        "include_jni",
        "_aosp/libnativehelper/include_jni",
        "jni.h",
        lock_value(&lock, "LIBNATIVEHELPER_JNI_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/libnativehelper",
        lock_value(&lock, "LIBNATIVEHELPER_REVISION")?,
        "libnativehelper-header-only",
        "header_only_include",
        "_aosp/libnativehelper/header_only_include",
        "nativehelper/scoped_local_ref.h",
        lock_value(&lock, "LIBNATIVEHELPER_SCOPED_LOCAL_REF_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/libnativehelper",
        lock_value(&lock, "LIBNATIVEHELPER_REVISION")?,
        "libnativehelper-platform-header-only",
        "include_platform_header_only",
        "_aosp/libnativehelper/platform_header_only_include",
        "nativehelper/jni_macros.h",
        lock_value(&lock, "LIBNATIVEHELPER_JNI_MACROS_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/external/dlmalloc",
        lock_value(&lock, "DLMALLOC_REVISION")?,
        "external-dlmalloc",
        "",
        "_aosp/external/dlmalloc",
        "dlmalloc.h",
        lock_value(&lock, "DLMALLOC_HEADER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/unwinding",
        lock_value(&lock, "UNWINDING_REVISION")?,
        "system-unwindstack-include",
        "libunwindstack/include",
        "_aosp/system/unwinding/libunwindstack/include",
        "unwindstack/AndroidUnwinder.h",
        lock_value(&lock, "UNWINDSTACK_ANDROID_UNWINDER_SHA256")?,
    )?;
    materialize_archive(
        root,
        "platform/system/libziparchive",
        lock_value(&lock, "LIBZIPARCHIVE_REVISION")?,
        "system-libziparchive",
        "",
        "_aosp/system/libziparchive",
        "Android.bp",
        lock_value(&lock, "LIBZIPARCHIVE_ANDROID_BP_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/prebuilts/runtime",
        lock_value(&lock, "RUNTIME_PREBUILTS_REVISION")?,
        "mainline/i18n/sdk/java/core-icu4j.jar",
        "_prebuilt/android-16/bootclasspath/core-icu4j.jar",
        lock_value(&lock, "CORE_ICU4J_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/prebuilts/runtime",
        lock_value(&lock, "RUNTIME_PREBUILTS_REVISION")?,
        "mainline/i18n/apex/com.android.i18n-arm64.apex",
        "_prebuilt/android-16/i18n/com.android.i18n-arm64.apex",
        lock_value(&lock, "I18N_ARM64_APEX_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "android_icu4j/libcore_bridge/src/java/com/android/i18n/util/ATrace.java",
        "_aosp/external/icu-runtime-bridge/com/android/i18n/util/ATrace.java",
        lock_value(&lock, "ICU_ATRACE_JAVA_SHA256")?,
    )?;
    materialize_file(
        root,
        "platform/external/icu",
        lock_value(&lock, "GRAPHICS_ICU_REVISION")?,
        "android_icu4j/libcore_bridge/src/java/com/android/icu/util/UResourceBundleNative.java",
        "_aosp/external/icu-runtime-bridge/com/android/icu/util/UResourceBundleNative.java",
        lock_value(&lock, "ICU_URESOURCE_BUNDLE_NATIVE_JAVA_SHA256")?,
    )?;
    Ok(())
}

pub(crate) fn materialize_file(
    root: &Path,
    project: &str,
    revision: &str,
    remote_file: &str,
    local_file: &str,
    expected_sha: &str,
) -> Result<()> {
    let destination = root.join(local_file);
    if destination.exists() {
        verify_sha256(&destination, expected_sha)?;
        println!(
            "sync: locked file already materialized at {}",
            destination.display()
        );
        return Ok(());
    }
    let downloads = root.join("_downloads");
    fs::create_dir_all(&downloads)?;
    let encoded = downloads.join(format!(
        "{}-{}.base64",
        remote_file.replace('/', "-"),
        revision
    ));
    let url = format!(
        "https://android.googlesource.com/{project}/+/{revision}/{remote_file}?format=TEXT"
    );
    run_command(
        Command::new("curl")
            .args(["-fL", "--retry", "3", "-o"])
            .arg(&encoded)
            .arg(url),
    )?;
    let decoded = Command::new("base64")
        .args(["-D", "-i"])
        .arg(&encoded)
        .output()?;
    if !decoded.status.success() {
        return Err(format!(
            "base64 decode failed for {remote_file}: {}",
            String::from_utf8_lossy(&decoded.stderr)
        )
        .into());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| format!("file has no parent: {}", destination.display()))?;
    fs::create_dir_all(parent)?;
    fs::write(&destination, decoded.stdout)?;
    verify_sha256(&destination, expected_sha)?;
    fs::remove_file(&encoded)?;
    println!(
        "sync: materialized locked file at {}",
        destination.display()
    );
    Ok(())
}

pub(crate) fn generate_operator_source(
    root: &Path,
    local_path: &Path,
    headers: &[&str],
    destination: &Path,
) -> Result<()> {
    if destination.is_file() {
        return Ok(());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| format!("file has no parent: {}", destination.display()))?;
    fs::create_dir_all(parent)?;
    let mut generate = Command::new("python3");
    generate
        .arg(root.join("_aosp/art/tools/generate_operator_out.py"))
        .arg(local_path);
    for header in headers {
        generate.arg(local_path.join(header));
    }
    fs::write(destination, command_output(&mut generate)?)?;
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn materialize_archive(
    root: &Path,
    project: &str,
    revision: &str,
    archive_name: &str,
    remote_subtree: &str,
    local_subtree: &str,
    verification_file: &str,
    expected_source_sha: &str,
) -> Result<()> {
    let source_dir = root.join(local_subtree);
    let marker = source_dir.join(".source-revision");

    if marker.exists() && fs::read_to_string(&marker)?.trim() == revision {
        println!(
            "sync: source already materialized at {}",
            source_dir.display()
        );
        return Ok(());
    }
    if source_dir.exists() && !marker.exists() {
        // Older local gates materialized a few source subtrees before the
        // provenance marker became mandatory. Adopt only a tree whose pinned
        // verification file already matches; arbitrary or modified source is
        // still rejected by the checksum.
        verify_sha256(&source_dir.join(verification_file), expected_source_sha)?;
        fs::write(&marker, format!("{revision}\n"))?;
        println!(
            "sync: adopted checksum-matched locked source at {}",
            source_dir.display()
        );
        return Ok(());
    }
    if source_dir.exists() {
        return Err(format!(
            "{} exists but does not match sources.lock; move it aside explicitly",
            source_dir.display()
        )
        .into());
    }

    let downloads = root.join("_downloads");
    fs::create_dir_all(&downloads)?;
    let archive = downloads.join(format!("{archive_name}-{revision}.tar.gz"));
    let archive_suffix = if remote_subtree.is_empty() {
        format!("{revision}.tar.gz")
    } else {
        format!("{revision}/{remote_subtree}.tar.gz")
    };
    let url = format!("https://android.googlesource.com/{project}/+archive/{archive_suffix}");

    if !archive.exists() {
        let partial = downloads.join(format!("{archive_name}-{revision}.tar.gz.partial"));
        run_command(
            Command::new("curl")
                .args(["-fL", "--retry", "3", "--output"])
                .arg(&partial)
                .arg(&url),
        )?;
        fs::rename(partial, &archive)?;
    }

    let staging_dir = source_dir.with_extension("partial");
    if staging_dir.exists() {
        fs::remove_dir_all(&staging_dir)?;
    }
    fs::create_dir_all(&staging_dir)?;
    run_command(
        Command::new("tar")
            .arg("-xzf")
            .arg(&archive)
            .arg("-C")
            .arg(&staging_dir),
    )?;
    verify_sha256(&staging_dir.join(verification_file), expected_source_sha)?;
    fs::write(
        staging_dir.join(".source-revision"),
        format!("{revision}\n"),
    )?;
    fs::rename(&staging_dir, &source_dir)?;

    println!(
        "sync: materialized locked source without Git metadata at {}",
        source_dir.display()
    );
    Ok(())
}

pub(crate) fn probe_asm(root: &Path) -> Result<()> {
    let source = root.join("_aosp/art/runtime/arch/arm64/asm_support_arm64.S");
    if !source.exists() {
        return Err("ARM64 source is missing; run `art-bootstrap sync` first".into());
    }

    let build_dir = root.join("_build/asm-probe");
    let generated_dir = build_dir.join("generated");
    let patched_source_dir = build_dir.join("patched-source");
    fs::create_dir_all(&generated_dir)?;
    fs::create_dir_all(patched_source_dir.join("runtime/arch/arm64"))?;

    let patched_source = patched_source_dir.join("runtime/arch/arm64/asm_support_arm64.S");
    fs::copy(&source, &patched_source)?;
    run_command(
        Command::new("patch")
            .args(["--batch", "--forward", "-p1", "-i"])
            .arg(root.join("patches/art/0001-arm64-mach-o-assembly.patch"))
            .current_dir(&patched_source_dir),
    )?;

    let patched = fs::read_to_string(&patched_source)?;
    let generated = isolate_asm_support(patched)?;
    let generated_support = generated_dir.join("asm_support_arm64_darwin.S");
    fs::write(&generated_support, generated)?;

    let object = build_dir.join("entrypoint_smoke.o");
    run_command(
        Command::new("clang")
            .args(["-arch", "arm64", "-x", "assembler-with-cpp"])
            .arg(format!("-I{}", generated_dir.display()))
            .arg("-c")
            .arg(root.join("probes/entrypoint_smoke.S"))
            .arg("-o")
            .arg(&object),
    )?;

    let executable = build_dir.join("entrypoint-smoke");
    run_command(
        Command::new("rustc")
            .args(["--edition=2024"])
            .arg(root.join("probes/call_asm.rs"))
            .arg("-C")
            .arg(format!("link-arg={}", object.display()))
            .arg("-o")
            .arg(&executable),
    )?;

    let output = command_output(&mut Command::new(&executable))?;
    if output.trim() != "ART Darwin ARM64 assembly result: 42" {
        return Err(format!("unexpected assembly probe output: {output:?}").into());
    }
    println!("probe-asm: {}", output.trim());
    Ok(())
}

pub(crate) fn isolate_asm_support(mut source: String) -> Result<String> {
    replace_required(
        &mut source,
        "#include \"asm_support_arm64.h\"\n#include \"interpreter/cfi_asm_support.h\"",
        "// Generated ART headers are intentionally reduced by this isolated ABI probe.\n\
#define FRAME_SIZE_SAVE_REFS_ONLY 96\n\
#define FRAME_SIZE_SAVE_REFS_AND_ARGS 224\n\
#define FRAME_SIZE_SAVE_ALL_CALLEE_SAVES 176",
    )?;
    Ok(source)
}

pub(crate) fn probe_page_size(root: &Path) -> Result<()> {
    let source = root.join("_aosp/art/libartbase/base/globals.h");
    if !source.exists() {
        return Err("libartbase source is missing; run `art-bootstrap sync` first".into());
    }

    let build_dir = root.join("_build/page-size-probe");
    let patched_source_dir = build_dir.join("patched-source");
    let patched_header = patched_source_dir.join("libartbase/base/globals.h");
    fs::create_dir_all(patched_header.parent().expect("globals.h has a parent"))?;
    fs::copy(&source, &patched_header)?;
    run_command(
        Command::new("patch")
            .args(["--batch", "--forward", "-p1", "-i"])
            .arg(root.join("patches/art/0002-darwin-dynamic-page-size.patch"))
            .current_dir(&patched_source_dir),
    )?;

    let include_dir = build_dir.join("include/base");
    fs::create_dir_all(&include_dir)?;
    fs::copy(&patched_header, include_dir.join("globals.h"))?;
    fs::write(
        include_dir.join("macros.h"),
        "#pragma once\n#include <unistd.h>\n#define ALWAYS_INLINE __attribute__((always_inline))\n",
    )?;

    let executable = build_dir.join("page-size");
    run_command(
        Command::new("clang++")
            .args(["-std=c++20", "-DART_PAGE_SIZE_AGNOSTIC"])
            .arg(format!("-I{}", build_dir.join("include").display()))
            .arg(root.join("probes/page_size.cc"))
            .arg("-o")
            .arg(&executable),
    )?;

    let output = command_output(&mut Command::new(&executable))?;
    if output.trim() != "ART Darwin page size: 16384" {
        return Err(format!("unexpected page-size probe output: {output:?}").into());
    }
    println!("probe-pagesize: {}", output.trim());
    Ok(())
}

pub(crate) fn build_foundation(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let libbase = root.join("_aosp/system/libbase");
    let libziparchive = root.join("_aosp/system/libziparchive");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    if !artbase.join("Android.bp").exists() || !libbase.join("Android.bp").exists() {
        return Err("foundation sources are missing; run `art-bootstrap sync` first".into());
    }

    let build_dir = root.join("_build/foundation");
    let patched_source_dir = build_dir.join("patched-source");
    let patched_artbase = patched_source_dir.join("libartbase");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(patched_artbase.join("base"))?;
    fs::create_dir_all(&object_dir)?;
    fs::copy(
        artbase.join("base/globals.h"),
        patched_artbase.join("base/globals.h"),
    )?;
    let stale_mem_map_overlay = patched_artbase.join("base/mem_map.h");
    if stale_mem_map_overlay.exists() {
        fs::remove_file(stale_mem_map_overlay)?;
    }
    let stale_utils_overlay = patched_artbase.join("base/utils.h");
    if stale_utils_overlay.exists() {
        fs::remove_file(stale_utils_overlay)?;
    }
    fs::copy(
        artbase.join("base/mem_map.cc"),
        patched_artbase.join("base/mem_map.cc"),
    )?;
    fs::copy(
        artbase.join("base/mem_map_unix.cc"),
        patched_artbase.join("base/mem_map_unix.cc"),
    )?;
    for patch in [
        "patches/art/0002-darwin-dynamic-page-size.patch",
        "patches/art/0020-darwin-low4g-mach-reservation.patch",
        "patches/art/0021-darwin-compressed-reference-window.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(&patched_source_dir),
        )?;
    }
    let libbase_include = libbase.join("include");
    let artbase_base = artbase.join("base");
    let libziparchive_include = libziparchive.join("include");
    let libziparchive_incfs_include = libziparchive.join("incfs_support/include");
    let compat = root.join("compat");
    let includes = [
        compat.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        artbase_base.as_path(),
        libbase_include.as_path(),
        libziparchive_include.as_path(),
        libziparchive_incfs_include.as_path(),
        tinyxml2.as_path(),
        Path::new("/opt/homebrew/include"),
    ];

    let android_base_sources = [
        root.join("compat/android_base_logging.cc"),
        libbase.join("file.cpp"),
        libbase.join("mapped_file.cpp"),
        libbase.join("parsebool.cpp"),
        libbase.join("properties.cpp"),
        libbase.join("stringprintf.cpp"),
        libbase.join("strings.cpp"),
    ];
    let mut android_base_objects = Vec::new();
    for source in android_base_sources {
        android_base_objects.push(compile_cpp(&source, &object_dir, &includes)?);
    }
    let android_base_archive = build_dir.join("libandroid-base-darwin.a");
    create_archive(&android_base_archive, &android_base_objects)?;

    let zip_object_dir = build_dir.join("zip-objects");
    fs::create_dir_all(&zip_object_dir)?;
    let zip_sources = [
        "zip_archive.cc",
        "zip_archive_stream_entry.cc",
        "zip_cd_entry_map.cc",
        "zip_error.cpp",
    ];
    let mut zip_objects = Vec::new();
    for source in zip_sources {
        let source_path = libziparchive.join(source);
        let object = zip_object_dir.join(format!("{source}.o"));
        run_command(
            common_cpp_command(&includes)
                .arg("-DZLIB_CONST")
                .arg("-D_FILE_OFFSET_BITS=64")
                .arg("-DINCFS_SUPPORT_DISABLED=1")
                .arg("-c")
                .arg(&source_path)
                .arg("-o")
                .arg(&object),
        )?;
        zip_objects.push(object);
    }
    create_archive(&build_dir.join("libziparchive-darwin.a"), &zip_objects)?;

    let artbase_operator_source = build_dir.join("generated/artbase_operator_out.cc");
    generate_operator_source(
        root,
        &artbase,
        &[
            "arch/instruction_set.h",
            "base/allocator.h",
            "base/unix_file/fd_file.h",
        ],
        &artbase_operator_source,
    )?;

    let artbase_sources = [
        artbase_operator_source,
        artbase.join("arch/instruction_set.cc"),
        artbase.join("base/allocator.cc"),
        artbase.join("base/arena_allocator.cc"),
        artbase.join("base/arena_bit_vector.cc"),
        artbase.join("base/bit_vector.cc"),
        artbase.join("base/compiler_filter.cc"),
        artbase.join("base/file_magic.cc"),
        artbase.join("base/file_utils.cc"),
        artbase.join("base/flags.cc"),
        artbase.join("base/hex_dump.cc"),
        artbase.join("base/logging.cc"),
        artbase.join("base/malloc_arena_pool.cc"),
        artbase.join("base/membarrier.cc"),
        artbase.join("base/memfd.cc"),
        artbase.join("base/memory_region.cc"),
        patched_artbase.join("base/mem_map.cc"),
        artbase.join("base/metrics/metrics_common.cc"),
        artbase.join("base/os_linux.cc"),
        artbase.join("base/pointer_size.cc"),
        artbase.join("base/runtime_debug.cc"),
        artbase.join("base/scoped_arena_allocator.cc"),
        artbase.join("base/scoped_flock.cc"),
        artbase.join("base/socket_peer_is_trusted.cc"),
        artbase.join("base/time_utils.cc"),
        artbase.join("base/unix_file/fd_file.cc"),
        artbase.join("base/unix_file/random_access_file_utils.cc"),
        artbase.join("base/utils.cc"),
        artbase.join("base/zip_archive.cc"),
        artbase.join("base/globals_unix.cc"),
        patched_artbase.join("base/mem_map_unix.cc"),
        tinyxml2.join("tinyxml2.cpp"),
    ];
    let mut artbase_objects = Vec::new();
    for source in artbase_sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        artbase_objects.push(object);
    }
    let artbase_archive = build_dir.join("libartbase-darwin.a");
    create_archive(&artbase_archive, &artbase_objects)?;

    let probe = build_dir.join("foundation-probe");
    run_command(
        common_cpp_command(&includes)
            .arg(root.join("probes/foundation.cc"))
            .arg(&artbase_archive)
            .arg(&android_base_archive)
            .arg("-o")
            .arg(&probe),
    )?;
    let output = command_output(&mut Command::new(&probe))?;
    if output.trim() != "libartbase Darwin: 1.500ms" {
        return Err(format!("unexpected foundation probe output: {output:?}").into());
    }
    println!("build-foundation: {}", output.trim());
    Ok(())
}

pub(crate) fn build_skia(root: &Path) -> Result<()> {
    const EXPECTED_OUTPUT: &str = "Skia Darwin raster: 64x64 rowBytes=256 hash=a4bb4cdb0b4779ea";
    const EXPECTED_FRAMEWORK_OUTPUT: &str =
        "Skia Android framework utils: base-canvas=same surface=same reset-clip=64x64";
    const EXPECTED_IOSURFACE_OUTPUT: &str = "darwin-skia-surface: direct-iosurface frames=120 staging-copies=0 \
         final-hash=b32235958413af5e sequence-hash=5fd4ea042fd71d4e";

    let lock = read_lock(root)?;
    let skia = root.join("_aosp/external/skia");
    if !skia.join("BUILD.gn").is_file() {
        return Err("Skia source is missing; run `art-bootstrap sync` first".into());
    }
    let source_revision = fs::read_to_string(skia.join(".source-revision"))?;
    if source_revision.trim() != lock_value(&lock, "SKIA_REVISION")? {
        return Err("materialized Skia revision does not match sources.lock".into());
    }
    let libcutils = root.join("_aosp/system/core/libcutils");
    let libcutils_include = libcutils.join("include");
    let libcutils_revision = fs::read_to_string(libcutils.join(".source-revision"))?;
    if libcutils_revision.trim() != lock_value(&lock, "SYSTEM_CORE_REVISION")? {
        return Err("materialized libcutils headers do not match sources.lock".into());
    }
    verify_sha256(
        &libcutils_include.join("cutils/trace.h"),
        lock_value(&lock, "LIBCUTILS_TRACE_HEADER_SHA256")?,
    )?;

    let gn = skia.join("bin/gn");
    if !gn.is_file() {
        run_command(
            Command::new("python3")
                .arg("bin/fetch-gn")
                .current_dir(&skia),
        )?;
    }
    verify_sha256(&gn, lock_value(&lock, "SKIA_GN_DARWIN_ARM64_SHA256")?)?;

    let ninja = skia.join("third_party/ninja/ninja");
    if !ninja.is_file() {
        run_command(
            Command::new("python3")
                .arg("bin/fetch-ninja")
                .current_dir(&skia),
        )?;
    }
    verify_sha256(&ninja, lock_value(&lock, "SKIA_NINJA_DARWIN_ARM64_SHA256")?)?;

    let build_dir = root.join("_build/skia");
    fs::create_dir_all(&build_dir)?;
    let compat = root.join("compat");
    let gn_args = format!(
        "is_official_build=true is_debug=false target_cpu=\"arm64\" \
         skia_enable_gpu=false skia_enable_graphite=false skia_enable_pdf=false \
         skia_enable_skottie=false skia_enable_svg=false skia_enable_precompile=false \
         skia_enable_tools=false skia_enable_fontmgr_empty=true skia_use_expat=false \
         skia_use_fonthost_mac=false skia_use_freetype=false skia_use_fontconfig=false \
         skia_use_harfbuzz=false skia_use_icu=false skia_use_perfetto=false \
         skia_disable_tracing=false \
         skia_use_gl=false skia_use_metal=false skia_use_vulkan=false \
         skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false \
         skia_use_no_jpeg_encode=true skia_use_libpng_decode=false \
         skia_use_libpng_encode=false skia_use_no_png_encode=true \
         skia_use_libwebp_decode=false skia_use_libwebp_encode=false \
         skia_use_no_webp_encode=true skia_use_wuffs=false skia_use_piex=false \
         skia_use_xps=false skia_use_zlib=false skia_use_dng_sdk=false \
         skia_use_libheif=false skia_use_crabbyavif=false skia_use_libjxl_decode=false \
         skia_use_libavif=false skia_use_bidi=false skia_use_libgrapheme=false \
        skia_build_rust_targets=false \
         extra_cflags=[\"-I{}\",\"-I{}\",\"-DSK_BUILD_FOR_ANDROID_FRAMEWORK\"]",
        compat.display(),
        libcutils_include.display()
    );
    run_command(
        Command::new(&gn)
            .arg("gen")
            .arg(&build_dir)
            .arg(format!("--args={gn_args}"))
            .current_dir(&skia),
    )?;
    run_command(
        Command::new(&ninja)
            .arg("-C")
            .arg(&build_dir)
            .arg("skia")
            .current_dir(&skia),
    )?;

    let skia_archive = build_dir.join("libskia.a");
    let skcms_archive = build_dir.join("libskcms.a");
    for archive in [&skia_archive, &skcms_archive] {
        if !archive.is_file() {
            return Err(format!("Skia archive is missing: {}", archive.display()).into());
        }
    }

    let skia_symbols = command_output(Command::new("nm").args(["-gU"]).arg(&skia_archive))?;
    for symbol in [
        "__ZN23SkAndroidFrameworkUtils20getBaseWrappedCanvasEP8SkCanvas",
        "__ZN23SkAndroidFrameworkUtils20getSurfaceFromCanvasEP8SkCanvas",
        "__ZN23SkAndroidFrameworkUtils9ResetClipEP8SkCanvas",
    ] {
        if !skia_symbols.contains(symbol) {
            return Err(
                format!("Skia archive is missing Android framework symbol {symbol}").into(),
            );
        }
    }

    let framework_smoke = build_dir.join("skia-android-framework-smoke");
    run_command(
        Command::new("clang++")
            .args([
                "-std=c++20",
                "-O2",
                "-DNDEBUG",
                "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
            ])
            .arg(format!("-I{}", compat.display()))
            .arg(format!("-I{}", skia.display()))
            .arg(root.join("probes/skia_android_framework_smoke.cc"))
            .arg(&skia_archive)
            .arg(&skcms_archive)
            .args(["-framework", "CoreGraphics", "-framework", "CoreFoundation"])
            .arg("-o")
            .arg(&framework_smoke),
    )?;
    let framework_output = command_output(&mut Command::new(&framework_smoke))?;
    if framework_output.trim() != EXPECTED_FRAMEWORK_OUTPUT {
        return Err(
            format!("unexpected Skia Android framework output: {framework_output:?}").into(),
        );
    }

    let smoke = build_dir.join("skia-raster-smoke");
    run_command(
        Command::new("clang++")
            .args([
                "-std=c++20",
                "-O2",
                "-DNDEBUG",
                "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
            ])
            .arg(format!("-I{}", compat.display()))
            .arg(format!("-I{}", skia.display()))
            .arg(root.join("probes/skia_raster_smoke.cc"))
            .arg(&skia_archive)
            .arg(&skcms_archive)
            .args(["-framework", "CoreGraphics", "-framework", "CoreFoundation"])
            .arg("-o")
            .arg(&smoke),
    )?;
    let kind = command_output(Command::new("file").arg(&smoke))?;
    if !kind.contains("Mach-O 64-bit executable arm64") {
        return Err(format!("unexpected Skia smoke format: {kind}").into());
    }
    let output = command_output(&mut Command::new(&smoke))?;
    if output.trim() != EXPECTED_OUTPUT {
        return Err(format!("unexpected Skia raster output: {output:?}").into());
    }

    let surface_smoke = build_dir.join("darwin-skia-surface-smoke");
    run_command(
        Command::new("clang++")
            .args([
                "-std=c++20",
                "-O2",
                "-DNDEBUG",
                "-DSK_BUILD_FOR_ANDROID_FRAMEWORK",
                "-fobjc-arc",
                "-Wall",
                "-Wextra",
                "-Werror",
            ])
            .arg(format!("-I{}", compat.display()))
            .arg(format!("-I{}", skia.display()))
            .arg(root.join("compat/darwin_surface_bridge.mm"))
            .arg(root.join("probes/darwin_skia_surface_smoke.mm"))
            .arg(&skia_archive)
            .arg(&skcms_archive)
            .args([
                "-framework",
                "AppKit",
                "-framework",
                "CoreGraphics",
                "-framework",
                "CoreFoundation",
                "-framework",
                "IOSurface",
                "-framework",
                "Metal",
                "-framework",
                "QuartzCore",
                "-o",
            ])
            .arg(&surface_smoke),
    )?;
    let iosurface_output =
        command_output(Command::new(&surface_smoke).env("DARWIN_ART_SURFACE_HEADLESS", "1"))?;
    if iosurface_output.trim() != EXPECTED_IOSURFACE_OUTPUT {
        return Err(
            format!("unexpected direct Skia IOSurface output: {iosurface_output:?}").into(),
        );
    }
    println!(
        "build-skia: {} {} archive_bytes={} {}",
        output.trim(),
        framework_output.trim(),
        fs::metadata(&skia_archive)?.len(),
        iosurface_output.trim()
    );
    Ok(())
}
