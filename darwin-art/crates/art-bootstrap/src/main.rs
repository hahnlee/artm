use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::io::Read;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn getpagesize() -> i32;
}

fn main() {
    if let Err(error) = run() {
        eprintln!("art-bootstrap: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let command = env::args().nth(1).unwrap_or_else(|| "help".to_owned());
    let root = workspace_root()?;

    match command.as_str() {
        "doctor" => doctor(),
        "sync" => sync_sources(&root),
        "probe-asm" => probe_asm(&root),
        "probe-pagesize" => probe_page_size(&root),
        "build-foundation" => build_foundation(&root),
        "build-skia" => build_skia(&root),
        "build-hwui-canvas" => build_shell_gate(&root, "compile-android16-hwui-canvas-gate.sh"),
        "build-android-graphics-jni" => build_shell_gate_with_args(
            &root,
            "build-android16-android-graphics-jni.sh",
            &["--object-audit"],
        ),
        "build-hwui-static" => build_shell_gate(&root, "build-android16-hwui-static-foundation.sh"),
        "build-androidfw" => build_shell_gate(&root, "build-android16-androidfw-foundation.sh"),
        "build-ziparchive-incfs" => build_shell_gate(&root, "build-android16-ziparchive-incfs.sh"),
        "build-hostgraphics" => build_shell_gate(&root, "build-android16-hostgraphics.sh"),
        "build-skia-hwui" => build_shell_gate(&root, "build-android16-skia-hwui-force-load.sh"),
        "build-graphics-codec-modules" => {
            build_shell_gate(&root, "build-android16-codec-foundation.sh")
        }
        "build-libbase" => build_shell_gate(&root, "build-android16-libbase-foundation.sh"),
        "build-icu-runtime-adapters" => {
            build_shell_gate(&root, "build-android16-icu-runtime-adapters.sh")
        }
        "build-graphics-foundations" => {
            build_shell_gate(&root, "build-android16-graphics-foundations.sh")
        }
        "build-nativehelper" => {
            build_shell_gate(&root, "build-android16-nativehelper-foundation.sh")
        }
        "build-ui-types" => build_shell_gate(&root, "build-android16-ui-types-foundation.sh"),
        "build-graphics-codecs" => build_shell_gate(&root, "build-android16-graphics-codecs.sh"),
        "build-harfbuzz" => build_shell_gate_with_args(
            &root,
            "build-android16-harfbuzz-foundation.sh",
            &["--archive-only"],
        ),
        "build-minikin" => build_shell_gate(&root, "build-android16-minikin-foundation.sh"),
        "build-skia-text" => build_shell_gate(&root, "build-android16-skia-text-raster.sh"),
        "build-icu" => build_shell_gate(&root, "build-android16-icu-foundation.sh"),
        "probe-minikin-shaping" => {
            build_shell_gate(&root, "run-android16-minikin-shaping-acceptance.sh")
        }
        "check-text-shaping" => build_shell_gate(&root, "check-android16-text-shaping-inputs.sh"),
        "build-dex" => build_dex_probe(&root),
        "build-runtime-platform" => build_runtime_platform(&root),
        "build-runtime-core" => build_runtime_core(&root),
        "probe-park" => probe_park(&root),
        "build-runtime-arm64" => build_runtime_arm64(&root),
        "build-interpreter-core" => build_interpreter_core(&root),
        "build-runtime-bootstrap" => build_runtime_bootstrap(&root),
        "build-runtime-graphics-bootstrap" => build_runtime_graphics_bootstrap(&root),
        "audit-runtime-link" => audit_runtime_link(&root),
        "audit-runtime-graphics-link" => audit_runtime_graphics_link(&root),
        "audit-graphics-closure" => build_shell_gate(&root, "audit-android16-graphics-closure.sh"),
        "probe-runtime-dex" => probe_runtime_dex(&root, false),
        "probe-window" => probe_runtime_dex(&root, true),
        "probe-runtime-graphics" => probe_runtime_graphics(&root),
        "probe-runtime-graphics-window" => probe_runtime_graphics_window(&root),
        "verify-bootclasspath" => verify_bootclasspath(&root),
        "all" => {
            doctor()?;
            sync_sources(&root)?;
            probe_asm(&root)?;
            probe_page_size(&root)?;
            build_skia(&root)?;
            build_dex_probe(&root)?;
            build_runtime_platform(&root)?;
            build_runtime_core(&root)?;
            build_runtime_arm64(&root)?;
            build_interpreter_core(&root)?;
            build_runtime_bootstrap(&root)?;
            audit_runtime_link(&root)?;
            probe_runtime_dex(&root, false)?;
            probe_park(&root)
        }
        "help" | "--help" | "-h" => {
            print_help();
            Ok(())
        }
        other => Err(format!("unknown command: {other}").into()),
    }
}

fn workspace_root() -> Result<PathBuf> {
    Ok(PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()?)
}

fn print_help() {
    println!("ART Darwin bootstrap");
    println!();
    println!("  doctor     verify the native Apple Silicon environment");
    println!("  sync       fetch locked ART subtrees without .git metadata");
    println!("  probe-asm  compile and execute an ART-derived Mach-O ARM64 entrypoint");
    println!("  probe-pagesize  compile ART's dynamic page-size path for Darwin");
    println!("  build-foundation  build and execute the minimal libartbase archive");
    println!("  build-skia  build upstream Skia CPU raster core and pixel smoke test");
    println!("  build-hwui-canvas  compile the first upstream HWUI Canvas/Paint gate");
    println!("  build-android-graphics-jni  compile the complete Android GraphicsJNI host module");
    println!("  build-hwui-static  compile the complete Android HWUI host core module");
    println!("  build-androidfw  build the complete Android resource framework archive");
    println!("  build-ziparchive-incfs  build Android's incremental ZIP archive variant");
    println!("  build-hostgraphics  build Android's Darwin native-window host module");
    println!("  build-skia-hwui  build the CoreText-free framework Skia closure");
    println!("  build-graphics-codec-modules  build image_io/JPEG/UltraHDR modules");
    println!("  build-libbase  build the complete Android host libbase provider");
    println!("  build-icu-runtime-adapters  verify ART adapters against Android ICU 76");
    println!("  build-graphics-foundations  build Darwin liblog/libcutils archives");
    println!("  build-nativehelper  build Darwin nativehelper host archives");
    println!("  build-ui-types  build the Darwin Android libui-types archive");
    println!("  build-graphics-codecs  build Darwin zlib/libpng/FreeType archives");
    println!("  build-harfbuzz  build the complete Darwin HarfBuzz archive");
    println!("  build-minikin  build the complete Darwin Minikin archive");
    println!("  build-skia-text  raster pinned Roboto through AOSP FreeType and Skia");
    println!("  build-icu  build Android ICU common/i18n/data for Darwin");
    println!("  probe-minikin-shaping  run real Minikin/HarfBuzz/ICU shaping cases");
    println!("  check-text-shaping  audit the full shaping and raster input closure");
    println!("  build-dex  compile AOSP libdexfile and parse a generated classes.dex");
    println!("  build-runtime-platform  compile ART host platform sources as Mach-O");
    println!("  build-runtime-core  apply Darwin monitor patches and compile runtime core");
    println!("  probe-park  stress Darwin's pthread-backed LockSupport primitive");
    println!("  build-runtime-arm64  generate ABI constants and compile ARM64 context");
    println!("  build-interpreter-core  compile ART's C++ interpreter implementation");
    println!("  build-runtime-bootstrap  compile ART Runtime initialization for Darwin");
    println!(
        "  build-runtime-graphics-bootstrap  compile the isolated real-graphics Runtime flavor"
    );
    println!("  audit-runtime-link  measure the remaining Runtime::Create link closure");
    println!("  audit-runtime-graphics-link  link ART with the strict Android graphics closure");
    println!("  audit-graphics-closure  verify the 32-archive Android graphics closure");
    println!("  probe-runtime-dex  launch Java main(String[]) with Android stdout");
    println!("  probe-window  display the Android View probe in a native NSWindow");
    println!("  probe-runtime-graphics  draw DecorView through Bitmap-backed AOSP Canvas");
    println!("  probe-runtime-graphics-window  display the real Android Canvas frame in NSWindow");
    println!("  verify-bootclasspath  verify extracted Android 16 core DEX files");
    println!("  all        run every check and probe");
}

fn build_shell_gate(root: &Path, script: &str) -> Result<()> {
    build_shell_gate_with_args(root, script, &[])
}

fn build_shell_gate_with_args(root: &Path, script: &str, args: &[&str]) -> Result<()> {
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

fn doctor() -> Result<()> {
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

fn sync_sources(root: &Path) -> Result<()> {
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

fn materialize_file(
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

fn generate_operator_source(
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
fn materialize_archive(
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

fn probe_asm(root: &Path) -> Result<()> {
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

fn isolate_asm_support(mut source: String) -> Result<String> {
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

fn probe_page_size(root: &Path) -> Result<()> {
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

fn build_foundation(root: &Path) -> Result<()> {
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

fn build_skia(root: &Path) -> Result<()> {
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

fn build_dex_probe(root: &Path) -> Result<()> {
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
            .arg(root.join("probes/compile-stubs/android/content/IContentProvider.java")),
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
    let expected = "AOSP DEX: verified=yes version=35 classes=12 methods=293 \
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

fn find_d8() -> Result<PathBuf> {
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

fn android_sdk_root() -> Result<PathBuf> {
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

fn find_android_platform_jar() -> Result<PathBuf> {
    let jar = android_sdk_root()?.join("platforms/android-36/android.jar");
    if !jar.is_file() {
        return Err(format!("Android API 36 platform JAR is missing: {}", jar.display()).into());
    }
    Ok(jar)
}

fn build_runtime_platform(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    if !runtime.join("Android.bp").exists() || !tinyxml2.join("tinyxml2.h").exists() {
        return Err("runtime platform sources are missing; run `art-bootstrap sync` first".into());
    }

    let includes = [
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let build_dir = root.join("_build/runtime-platform");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&object_dir)?;

    let sources = [
        runtime.join("runtime_linux.cc"),
        runtime.join("thread_linux.cc"),
        runtime.join("monitor_linux.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
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
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected runtime object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-platform-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-platform: Mach-O arm64 objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

fn build_runtime_core(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");

    let build_dir = root.join("_build/runtime-core");
    let patched_runtime = build_dir.join("patched-source/runtime");
    let patched_base = patched_runtime.join("base");
    let patched_mirror = patched_runtime.join("mirror");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&patched_base)?;
    fs::create_dir_all(&patched_mirror)?;
    fs::create_dir_all(&object_dir)?;
    fs::copy(
        runtime.join("monitor.cc"),
        patched_runtime.join("monitor.cc"),
    )?;
    fs::copy(runtime.join("base/mutex.h"), patched_base.join("mutex.h"))?;
    fs::copy(runtime.join("base/mutex.cc"), patched_base.join("mutex.cc"))?;
    fs::copy(
        runtime.join("mirror/object_reference.h"),
        patched_mirror.join("object_reference.h"),
    )?;

    for patch in [
        "patches/art/0003-darwin-allow-pthread-monitors.patch",
        "patches/art/0004-darwin-uncontended-monitor-lock.patch",
        "patches/art/0026-darwin-base-relative-object-references-only.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(build_dir.join("patched-source")),
        )?;
    }

    let includes = [
        patched_runtime.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let sources = [
        patched_base.join("mutex.cc"),
        patched_runtime.join("monitor.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected runtime core object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-core-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-core: pthread monitor bootstrap objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

fn runtime_cpp_command(includes: &[&Path]) -> Command {
    let mut command = common_cpp_command(includes);
    command.args([
        "-DBUILDING_LIBART",
        // AOSP's Android 16 boot class path is built with D8 desugaring. Soong
        // normally injects this and String::ClassSize depends on it.
        "-DUSE_D8_DESUGAR",
        // Concurrent Mark Compact depends on Linux userfaultfd/mremap. Keep it
        // compilable in fallback mode, but make portable CMS the Darwin MVP default.
        "-DART_DEFAULT_GC_TYPE_IS_CMS",
        "-DART_FRAME_SIZE_LIMIT=1744",
        // These are normally injected by AOSP's Soong art-global defaults.
        "-DART_BASE_ADDRESS=0x70000000",
        "-DART_BASE_ADDRESS_MIN_DELTA=(-0x1000000)",
        "-DART_BASE_ADDRESS_MAX_DELTA=0x1000000",
        "-DART_STACK_OVERFLOW_GAP_arm=8192",
        "-DART_STACK_OVERFLOW_GAP_arm64=8192",
        "-DART_STACK_OVERFLOW_GAP_riscv64=8192",
        "-DART_STACK_OVERFLOW_GAP_x86=8192",
        "-DART_STACK_OVERFLOW_GAP_x86_64=8192",
        "-Wno-invalid-offsetof",
        "-Wno-unsupported-visibility",
        "-Wno-deprecated-enum-enum-conversion",
        "-Wno-nontrivial-memcall",
    ]);
    command
}

fn runtime_bootstrap_cpp_command(includes: &[&Path]) -> Command {
    let mut command = runtime_cpp_command(includes);
    // Original runtime headers include mirror/object_reference.h relative to
    // their own AOSP directory. Force the Darwin overlay before that include
    // guard can select ART's absolute-low-32-bit representation.
    command.args([
        "-include",
        "mirror/object_reference.h",
        "-include",
        "mirror/string-inl.h",
    ]);
    command
}

fn probe_park(root: &Path) -> Result<()> {
    let mutex_object = root.join("_build/runtime-core/objects/mutex.cc.o");
    let patched_runtime = root.join("_build/runtime-bootstrap/patched-source/runtime");
    if !mutex_object.exists() || !patched_runtime.join("thread.h").exists() {
        return Err(
            "Darwin runtime objects are missing; run `build-runtime-bootstrap` first".into(),
        );
    }

    let build_dir = root.join("_build/park-probe");
    fs::create_dir_all(&build_dir)?;
    let object = build_dir.join("park_probe.cc.o");
    let executable = build_dir.join("park-probe");
    let generated = root.join("_build/runtime-arm64/generated");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_include = root.join("_aosp/libnativehelper/header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let includes = [
        patched_runtime.as_path(),
        generated.as_path(),
        generator.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        libbase_include.as_path(),
        palette_include.as_path(),
        jni_include.as_path(),
        nativehelper_include.as_path(),
        dlmalloc.as_path(),
        tinyxml2.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    run_command(
        common_cpp_command(&includes)
            .arg("-Wno-invalid-offsetof")
            .arg("-c")
            .arg(root.join("probes/park_probe.cc"))
            .arg("-o")
            .arg(&object),
    )?;
    run_command(
        Command::new("clang++")
            .arg("-Wl,-dead_strip")
            .arg(&object)
            .arg(&mutex_object)
            .arg(root.join("_build/foundation/libartbase-darwin.a"))
            .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
            .args(["-L/opt/homebrew/lib", "-lfmt"])
            .arg("-o")
            .arg(&executable),
    )?;
    let output = command_output(&mut Command::new(&executable))?;
    let expected = "ART Darwin park: pre-permit=yes wakeups=200 timeout=yes";
    if output.trim() != expected {
        return Err(format!("unexpected Darwin park probe output: {output:?}").into());
    }
    println!("probe-park: {}", output.trim());
    Ok(())
}

fn build_runtime_arm64(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_abi = root.join("_build/runtime-core/patched-source/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let compat = root.join("compat");
    if !generator.join("asm_defines.cc").exists() {
        return Err("ART ABI generator is missing; run `art-bootstrap sync` first".into());
    }

    let build_dir = root.join("_build/runtime-arm64");
    let generated_dir = build_dir.join("generated");
    let patched_runtime = build_dir.join("patched-source/runtime");
    let patched_arm64 = patched_runtime.join("arch/arm64");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&generated_dir)?;
    fs::create_dir_all(&patched_arm64)?;
    fs::create_dir_all(&object_dir)?;
    fs::copy(
        runtime_arm64.join("context_arm64.h"),
        patched_arm64.join("context_arm64.h"),
    )?;
    fs::copy(
        runtime_arm64.join("context_arm64.cc"),
        patched_arm64.join("context_arm64.cc"),
    )?;
    fs::copy(
        runtime_arm64.join("instruction_set_features_arm64.cc"),
        patched_arm64.join("instruction_set_features_arm64.cc"),
    )?;
    for assembly in [
        "asm_support_arm64.S",
        "jni_entrypoints_arm64.S",
        "memcmp16_arm64.S",
        "quick_entrypoints_arm64.S",
        "native_entrypoints_arm64.S",
    ] {
        fs::copy(runtime_arm64.join(assembly), patched_arm64.join(assembly))?;
    }
    for patch in [
        "patches/art/0001-arm64-mach-o-assembly.patch",
        "patches/art/0005-darwin-arm64-context-word-type.patch",
        "patches/art/0012-darwin-arm64-quick-symbols.patch",
        "patches/art/0015-darwin-arm64-feature-fallback.patch",
        "patches/art/0016-darwin-arm64-direct-c-symbols.patch",
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(build_dir.join("patched-source")),
        )?;
    }

    let includes = [
        generated_dir.as_path(),
        compat.as_path(),
        generator.as_path(),
        patched_runtime.as_path(),
        runtime_abi.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];

    let generated_assembly = generated_dir.join("asm_defines.s");
    run_command(
        runtime_cpp_command(&includes)
            .args(["-include", "mirror/object_reference.h"])
            .arg("-S")
            .arg(generator.join("asm_defines.cc"))
            .arg("-o")
            .arg(&generated_assembly),
    )?;
    let generated_header = command_output(
        Command::new("python3")
            .arg(generator.join("make_header.py"))
            .arg(&generated_assembly),
    )?;
    for required in [
        "#define THREAD_FLAGS_OFFSET",
        "#define THREAD_CARD_TABLE_OFFSET",
        "#define THREAD_EXCEPTION_OFFSET",
        "#define THREAD_ID_OFFSET",
    ] {
        if !generated_header.contains(required) {
            return Err(format!("generated asm_defines.h is missing {required}").into());
        }
    }
    fs::write(generated_dir.join("asm_defines.h"), generated_header)?;

    let sources = [
        patched_arm64.join("context_arm64.cc"),
        runtime.join("arch/context.cc"),
        runtime.join("arch/instruction_set_features.cc"),
        runtime_arm64.join("thread_arm64.cc"),
        runtime_arm64.join("entrypoints_init_arm64.cc"),
        patched_arm64.join("instruction_set_features_arm64.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-Wno-deprecated-anon-enum-enum-conversion")
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected ARM64 runtime object format: {kind}").into());
        }
        objects.push(object);
    }

    // Apple's integrated assembler rejects a small subset of ART's otherwise
    // valid DWARF CFI state machine. The entrypoints themselves are usable for
    // this bootstrap, so emit a clearly named no-CFI PoC artifact. Restoring
    // correct Darwin unwind metadata is required before production use.
    for assembly in [
        "jni_entrypoints_arm64.S",
        "memcmp16_arm64.S",
        "quick_entrypoints_arm64.S",
        "native_entrypoints_arm64.S",
    ] {
        let source = patched_arm64.join(assembly);
        let preprocessed = command_output(
            Command::new("clang")
                .args(["-E", "-x", "assembler-with-cpp", "-DART_PAGE_SIZE_AGNOSTIC"])
                .arg(format!("-I{}", generated_dir.display()))
                .arg(format!("-I{}", patched_arm64.display()))
                .arg(format!("-I{}", runtime_arm64.display()))
                .arg(format!("-I{}", runtime.display()))
                .arg(&source),
        )?;
        let mut stripped_cfi = 0usize;
        let mut darwin_assembly = String::with_capacity(preprocessed.len());
        for line in preprocessed.lines() {
            if line.trim_start().starts_with(".cfi_") {
                stripped_cfi += 1;
            } else {
                darwin_assembly.push_str(line);
                darwin_assembly.push('\n');
            }
        }
        if stripped_cfi == 0 {
            return Err(format!("expected CFI directives in {assembly}").into());
        }
        let generated_source = generated_dir.join(format!("{assembly}.darwin-no-cfi.S"));
        fs::write(&generated_source, darwin_assembly)?;
        let object = object_dir.join(format!("{assembly}.o"));
        run_command(
            Command::new("clang")
                .args(["-arch", "arm64", "-x", "assembler", "-c"])
                .arg(&generated_source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected ARM64 assembly object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-arm64-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-arm64: generated ABI constants, Mach-O objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

fn build_interpreter_core(root: &Path) -> Result<()> {
    let artbase = root.join("_aosp/art/libartbase");
    let patched_artbase = root.join("_build/foundation/patched-source/libartbase");
    let libdexfile = root.join("_aosp/art/libdexfile");
    let runtime = root.join("_aosp/art/runtime");
    let runtime_abi = root.join("_build/runtime-core/patched-source/runtime");
    let runtime_base = runtime.join("base");
    let runtime_arm64 = runtime.join("arch/arm64");
    let generator = root.join("_aosp/art/tools/cpp-define-generator");
    let generated_dir = root.join("_build/runtime-arm64/generated");
    let palette_include = root.join("_aosp/art/libartpalette/include");
    let libbase_include = root.join("_aosp/system/libbase/include");
    let tinyxml2 = root.join("_aosp/external/tinyxml2");
    let android_jni_include = root.join("_aosp/libnativehelper/include_jni");
    let nativehelper_headers = root.join("_aosp/libnativehelper/header_only_include");
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    if !generated_dir.join("asm_defines.h").exists() {
        return Err(
            "generated ART ABI constants are missing; run `build-runtime-arm64` first".into(),
        );
    }

    let includes = [
        generated_dir.as_path(),
        generator.as_path(),
        runtime_abi.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        libdexfile.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        nativehelper_headers.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
    let build_dir = root.join("_build/interpreter-core");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&object_dir)?;
    let sources = [
        runtime.join("interpreter/interpreter.cc"),
        runtime.join("interpreter/interpreter_cache.cc"),
        runtime.join("interpreter/interpreter_common.cc"),
        runtime.join("interpreter/interpreter_switch_impl0.cc"),
        runtime.join("interpreter/lock_count_data.cc"),
        runtime.join("interpreter/shadow_frame.cc"),
        runtime.join("interpreter/unstarted_runtime.cc"),
    ];
    let mut objects = Vec::new();
    for source in sources {
        let file_name = source
            .file_name()
            .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
        let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
        run_command(
            runtime_cpp_command(&includes)
                .args(["-include", "mirror/object_reference.h"])
                .arg("-c")
                .arg(&source)
                .arg("-o")
                .arg(&object),
        )?;
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected interpreter object format: {kind}").into());
        }
        objects.push(object);
    }

    let archive = build_dir.join("libart-interpreter-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-interpreter-core: AOSP C++ interpreter Mach-O objects={} archive={}",
        objects.len(),
        archive.display()
    );
    Ok(())
}

fn build_runtime_bootstrap(root: &Path) -> Result<()> {
    build_runtime_bootstrap_flavor(root, false)
}

fn build_runtime_graphics_bootstrap(root: &Path) -> Result<()> {
    build_runtime_bootstrap_flavor(root, true)
}

fn build_runtime_bootstrap_flavor(root: &Path, real_graphics: bool) -> Result<()> {
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
    let graphics_registration_include = root.join("_build/android-graphics-jni/generated");
    let android_icu_include = root.join("_aosp/external/icu-graphics/libandroidicuinit/include");
    let android_graphics_apex_include = root.join("_aosp/frameworks/base/libs/hwui/apex/include");
    let aosp_fmt_include = root.join("_aosp/external/fmtlib/include");
    let libcutils_include = root.join("_aosp/system/core/libcutils/include");
    let liblog_include = root.join("_aosp/system/logging/liblog/include");
    let nativehelper_full_include = root.join("_aosp/libnativehelper-full/include");
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

    let build_dir = root.join(if real_graphics {
        "_build/runtime-graphics-bootstrap"
    } else {
        "_build/runtime-bootstrap"
    });
    let runtime_generated_dir = build_dir.join("generated");
    let patched_source_dir = build_dir.join("patched-source");
    let patched_runtime = patched_source_dir.join("runtime");
    let object_dir = build_dir.join("objects");
    fs::create_dir_all(&patched_runtime)?;
    fs::create_dir_all(&object_dir)?;
    fs::create_dir_all(&runtime_generated_dir)?;
    fs::copy(
        runtime.join("runtime.cc"),
        patched_runtime.join("runtime.cc"),
    )?;
    fs::copy(
        runtime.join("signal_set.h"),
        patched_runtime.join("signal_set.h"),
    )?;
    fs::copy(
        runtime.join("class_linker.cc"),
        patched_runtime.join("class_linker.cc"),
    )?;
    fs::copy(
        runtime.join("class_linker.h"),
        patched_runtime.join("class_linker.h"),
    )?;
    fs::create_dir_all(patched_runtime.join("mirror"))?;
    fs::copy(
        runtime.join("mirror/object_reference.h"),
        patched_runtime.join("mirror/object_reference.h"),
    )?;
    fs::copy(
        runtime.join("mirror/string-inl.h"),
        patched_runtime.join("mirror/string-inl.h"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc"))?;
    fs::copy(
        runtime.join("gc/heap.cc"),
        patched_runtime.join("gc/heap.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc/space"))?;
    fs::copy(
        runtime.join("gc/space/malloc_space.cc"),
        patched_runtime.join("gc/space/malloc_space.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("entrypoints/quick"))?;
    fs::copy(
        runtime.join("entrypoints/quick/quick_alloc_entrypoints.cc"),
        patched_runtime.join("entrypoints/quick/quick_alloc_entrypoints.cc"),
    )?;
    fs::copy(
        runtime.join("entrypoints/quick/callee_save_frame.h"),
        patched_runtime.join("entrypoints/quick/callee_save_frame.h"),
    )?;
    fs::copy(
        runtime.join("entrypoints/quick/quick_trampoline_entrypoints.cc"),
        patched_runtime.join("entrypoints/quick/quick_trampoline_entrypoints.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("arch/arm64"))?;
    fs::copy(
        runtime.join("arch/arm64/jni_frame_arm64.h"),
        patched_runtime.join("arch/arm64/jni_frame_arm64.h"),
    )?;
    fs::copy(
        runtime.join("runtime_common.cc"),
        patched_runtime.join("runtime_common.cc"),
    )?;
    fs::copy(runtime.join("runtime.h"), patched_runtime.join("runtime.h"))?;
    fs::copy(runtime.join("thread.cc"), patched_runtime.join("thread.cc"))?;
    fs::copy(runtime.join("thread.h"), patched_runtime.join("thread.h"))?;
    fs::copy(
        runtime.join("thread_list.cc"),
        patched_runtime.join("thread_list.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("gc/collector"))?;
    fs::copy(
        runtime.join("gc/collector/garbage_collector.cc"),
        patched_runtime.join("gc/collector/garbage_collector.cc"),
    )?;
    fs::copy(
        runtime.join("gc/collector/mark_compact.cc"),
        patched_runtime.join("gc/collector/mark_compact.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("oat"))?;
    fs::copy(
        runtime.join("oat/oat_file.cc"),
        patched_runtime.join("oat/oat_file.cc"),
    )?;
    fs::copy(
        runtime.join("exec_utils.cc"),
        patched_runtime.join("exec_utils.cc"),
    )?;
    fs::copy(
        runtime.join("signal_catcher.cc"),
        patched_runtime.join("signal_catcher.cc"),
    )?;
    fs::copy(
        runtime.join("nterp_helpers.cc"),
        patched_runtime.join("nterp_helpers.cc"),
    )?;
    fs::create_dir_all(patched_runtime.join("interpreter/mterp"))?;
    fs::copy(
        runtime.join("interpreter/mterp/nterp.cc"),
        patched_runtime.join("interpreter/mterp/nterp.cc"),
    )?;
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
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(&patched_source_dir),
        )?;
    }

    let mut includes = vec![
        compat.as_path(),
        generated_dir.as_path(),
        generator.as_path(),
        patched_runtime.as_path(),
        patched_artbase.as_path(),
        artbase.as_path(),
        cmdline.as_path(),
        libdexfile.as_path(),
        libelffile.as_path(),
        libprofile.as_path(),
        nativebridge_include.as_path(),
        nativeloader_include.as_path(),
        odrefresh_include.as_path(),
        sigchain.as_path(),
        runtime.as_path(),
        runtime_base.as_path(),
        runtime_arm64.as_path(),
        runtime_gc.as_path(),
        runtime_gc_collector.as_path(),
        runtime_gc_space.as_path(),
        runtime_quick_entrypoints.as_path(),
        runtime_mterp.as_path(),
        runtime_oat.as_path(),
        palette_include.as_path(),
        libbase_include.as_path(),
        unwindstack_include.as_path(),
        tinyxml2.as_path(),
        android_jni_include.as_path(),
        nativehelper_headers.as_path(),
        nativehelper_platform_headers.as_path(),
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/opt/icu4c@78/include"),
        Path::new("/opt/homebrew/include"),
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
        includes.insert(0, graphics_registration_include.as_path());
        includes.insert(1, android_icu_include.as_path());
        includes.insert(2, android_graphics_apex_include.as_path());
        includes.insert(3, aosp_fmt_include.as_path());
    }
    let sources = [
        "fault_handler.cc",
        "interpreter/mterp/nterp.cc",
        "dex_register_location.cc",
        "handle.cc",
        "java_frame_root_info.cc",
        "jit/jit_memory_region.cc",
        "jit/profile_saver.cc",
        "jit/small_pattern_matcher.cc",
        "offsets.cc",
        "reflective_value_visitor.cc",
        "jit/jit.cc",
        "jit/jit_code_cache.cc",
        "jit/jit_options.cc",
        "jit/profiling_info.cc",
        "mirror/emulated_stack_frame.cc",
        "mirror/executable.cc",
        "monitor_objects_stack_visitor.cc",
        "signal_catcher.cc",
        "debug_print.cc",
        "debugger.cc",
        "dex/dex_file_annotations.cc",
        "exec_utils.cc",
        "hidden_api.cc",
        "jni/check_jni.cc",
        "jni/jni_internal.cc",
        "method_handles.cc",
        "mirror/class_ext.cc",
        "mirror/field.cc",
        "mirror/method.cc",
        "mirror/method_handle_impl.cc",
        "mirror/method_handles_lookup.cc",
        "mirror/method_type.cc",
        "mirror/var_handle.cc",
        "native_bridge_art_interface.cc",
        "native_stack_dump.cc",
        "oat/elf_file.cc",
        "oat/index_bss_mapping.cc",
        "oat/jni_stub_hash_map.cc",
        "plugin.cc",
        "scoped_thread_state_change.cc",
        "startup_completed_task.cc",
        "string_builder_append.cc",
        "ti/agent.cc",
        "trace.cc",
        "trace_profile.cc",
        "var_handles.cc",
        "verifier/class_verifier.cc",
        "verifier/instruction_flags.cc",
        "verifier/method_verifier.cc",
        "verifier/reg_type.cc",
        "verifier/reg_type_cache.cc",
        "verifier/register_line.cc",
        "verifier/verifier_deps.cc",
        "backtrace_helper.cc",
        "jit/debugger_interface.cc",
        "metrics/reporter.cc",
        "monitor_pool.cc",
        "non_debuggable_classes.cc",
        "nterp_helpers.cc",
        "oat/image.cc",
        "oat/oat.cc",
        "oat/oat_file.cc",
        "oat/oat_file_assistant.cc",
        "oat/oat_file_assistant_context.cc",
        "oat/oat_quick_method_header.cc",
        "oat/stack_map.cc",
        "oat/sdc_file.cc",
        "object_lock.cc",
        "quick_exception_handler.cc",
        "reference_table.cc",
        "reflection.cc",
        "reflective_handle_scope.cc",
        "stack.cc",
        "thread_pool.cc",
        "vdex_file.cc",
        "entrypoints/entrypoint_utils.cc",
        "entrypoints/jni/jni_entrypoints.cc",
        "entrypoints/math_entrypoints.cc",
        "entrypoints/quick/quick_alloc_entrypoints.cc",
        "entrypoints/quick/quick_cast_entrypoints.cc",
        "entrypoints/quick/quick_deoptimization_entrypoints.cc",
        "entrypoints/quick/quick_dexcache_entrypoints.cc",
        "entrypoints/quick/quick_entrypoints_enum.cc",
        "entrypoints/quick/quick_field_entrypoints.cc",
        "entrypoints/quick/quick_fillarray_entrypoints.cc",
        "entrypoints/quick/quick_jni_entrypoints.cc",
        "entrypoints/quick/quick_lock_entrypoints.cc",
        "entrypoints/quick/quick_math_entrypoints.cc",
        "entrypoints/quick/quick_string_builder_append_entrypoints.cc",
        "entrypoints/quick/quick_thread_entrypoints.cc",
        "entrypoints/quick/quick_throw_entrypoints.cc",
        "entrypoints/quick/quick_trampoline_entrypoints.cc",
        "runtime.cc",
        "class_linker.cc",
        "thread.cc",
        "thread_list.cc",
        "gc/heap.cc",
        "intern_table.cc",
        "instrumentation.cc",
        "runtime_callbacks.cc",
        "oat/oat_file_manager.cc",
        "jni/java_vm_ext.cc",
        "runtime_options.cc",
        "base/locks.cc",
        "base/gc_visited_arena_pool.cc",
        "base/mem_map_arena_pool.cc",
        "base/quasi_atomic.cc",
        "base/timing_logger.cc",
        "gc/accounting/bitmap.cc",
        "gc/accounting/card_table.cc",
        "gc/accounting/heap_bitmap.cc",
        "gc/accounting/mod_union_table.cc",
        "gc/accounting/remembered_set.cc",
        "gc/accounting/space_bitmap.cc",
        "gc/allocation_record.cc",
        "gc/allocator/art-dlmalloc.cc",
        "gc/allocator/rosalloc.cc",
        "gc/collector/concurrent_copying.cc",
        "gc/collector/garbage_collector.cc",
        "gc/collector/immune_region.cc",
        "gc/collector/immune_spaces.cc",
        "gc/collector/mark_compact.cc",
        "gc/collector/mark_sweep.cc",
        "gc/collector/partial_mark_sweep.cc",
        "gc/collector/semi_space.cc",
        "gc/collector/sticky_mark_sweep.cc",
        "gc/gc_cause.cc",
        "gc/reference_processor.cc",
        "gc/reference_queue.cc",
        "gc/scoped_gc_critical_section.cc",
        "gc/space/bump_pointer_space.cc",
        "gc/space/dlmalloc_space.cc",
        "gc/space/image_space.cc",
        "gc/space/large_object_space.cc",
        "gc/space/malloc_space.cc",
        "gc/space/region_space.cc",
        "gc/space/rosalloc_space.cc",
        "gc/space/space.cc",
        "gc/space/zygote_space.cc",
        "gc/task_processor.cc",
        "gc/verification.cc",
        "javaheapprof/javaheapsampler.cc",
        "app_info.cc",
        "art_field.cc",
        "art_method.cc",
        "barrier.cc",
        "cha.cc",
        "class_loader_context.cc",
        "class_root.cc",
        "class_table.cc",
        "common_throws.cc",
        "compat_framework.cc",
        "indirect_reference_table.cc",
        "jni/jni_env_ext.cc",
        "jni/local_reference_table.cc",
        "jni/jni_id_manager.cc",
        "mirror/array.cc",
        "mirror/class.cc",
        "mirror/dex_cache.cc",
        "mirror/object.cc",
        "mirror/stack_frame_info.cc",
        "mirror/stack_trace_element.cc",
        "mirror/string.cc",
        "mirror/throwable.cc",
        "native/dalvik_system_BaseDexClassLoader.cc",
        "native/dalvik_system_DexFile.cc",
        "native/dalvik_system_VMDebug.cc",
        "native/dalvik_system_VMRuntime.cc",
        "native/dalvik_system_VMStack.cc",
        "native/dalvik_system_ZygoteHooks.cc",
        "native/java_lang_Class.cc",
        "native/java_lang_Object.cc",
        "native/java_lang_StackStreamFactory.cc",
        "native/java_lang_String.cc",
        "native/java_lang_StringFactory.cc",
        "native/java_lang_System.cc",
        "native/java_lang_Thread.cc",
        "native/java_lang_Throwable.cc",
        "native/java_lang_VMClassLoader.cc",
        "native/java_lang_invoke_MethodHandle.cc",
        "native/java_lang_invoke_MethodHandleImpl.cc",
        "native/java_lang_ref_FinalizerReference.cc",
        "native/java_lang_ref_Reference.cc",
        "native/java_lang_reflect_Array.cc",
        "native/java_lang_reflect_Constructor.cc",
        "native/java_lang_reflect_Executable.cc",
        "native/java_lang_reflect_Field.cc",
        "native/java_lang_reflect_Method.cc",
        "native/java_lang_reflect_Parameter.cc",
        "native/java_lang_reflect_Proxy.cc",
        "native/java_util_concurrent_atomic_AtomicLong.cc",
        "native/jdk_internal_misc_Unsafe.cc",
        "native/libcore_io_Memory.cc",
        "native/libcore_util_CharsetUtils.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmServer.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc",
        "native/sun_misc_Unsafe.cc",
        "runtime_common.cc",
        "runtime_intrinsics.cc",
        "well_known_classes.cc",
    ];
    // Soong generates this translation unit from ART's enum declarations. Use
    // the pinned upstream generator instead of maintaining Darwin-only copies
    // of dozens of operator<< implementations.
    let operator_headers = [
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
    ];
    let operator_source = runtime_generated_dir.join("operator_out.cc");
    if !operator_source.is_file() {
        let mut generate = Command::new("python3");
        generate
            .arg(root.join("_aosp/art/tools/generate_operator_out.py"))
            .arg(&runtime);
        for header in operator_headers {
            generate.arg(runtime.join(header));
        }
        fs::write(&operator_source, command_output(&mut generate)?)?;
    }

    let compiler_identity = format!(
        "{}macOS {} ({})",
        command_output(Command::new("clang++").arg("--version"))?,
        command_output(Command::new("sw_vers").arg("-productVersion"))?.trim(),
        command_output(Command::new("sw_vers").arg("-buildVersion"))?.trim()
    );
    let file_hash_cache_path = build_dir.join("file-hashes.cache");
    let mut file_hash_cache = FileHashCache::load(&file_hash_cache_path)?;
    let mut objects = Vec::new();
    let mut compiled_objects = 0usize;
    let mut cached_objects = 0usize;
    let operator_object = object_dir.join("generated_operator_out.cc.o");
    let mut operator_command = runtime_bootstrap_cpp_command(&includes);
    operator_command
        .arg("-idirafter")
        .arg(&ndk_arch_include)
        .arg("-idirafter")
        .arg(&ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-c")
        .arg(&operator_source)
        .arg("-o")
        .arg(&operator_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut operator_command,
            &operator_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(operator_object);

    for profile_source in [
        "profile/profile_boot_info.cc",
        "profile/profile_compilation_info.cc",
    ] {
        let profile_object =
            object_dir.join(format!("libprofile_{}.o", profile_source.replace('/', "_")));
        let mut profile_command = runtime_bootstrap_cpp_command(&includes);
        profile_command
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(libprofile.join(profile_source))
            .arg("-o")
            .arg(&profile_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut profile_command,
                &profile_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
        objects.push(profile_object);
    }
    if real_graphics {
        // The default foundation archive was compiled against the developer
        // machine's fmt headers. Recompile the only fmt-using libartbase TU in
        // this isolated flavor so ART and module-complete AOSP libbase share
        // the pinned fmt ABI without a Homebrew fmt provider.
        let os_linux_object = object_dir.join("artbase_os_linux_aosp_fmt.cc.o");
        let mut os_linux_command = runtime_cpp_command(&includes);
        os_linux_command
            .arg("-c")
            .arg(artbase.join("base/os_linux.cc"))
            .arg("-o")
            .arg(&os_linux_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut os_linux_command,
                &os_linux_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
    }
    // Compile Android 16 libcore's complete java.lang.Math native table as C.
    // Keeping this TU unchanged preserves AOSP's FastNative ABI and avoids a
    // Darwin-specific reimplementation of its 23 libm entry points.
    let openjdk_math_object = object_dir.join("libcore_openjdk_Math.c.o");
    let mut openjdk_math_command = Command::new("clang");
    openjdk_math_command
        .args([
            "-std=gnu11",
            "-O2",
            "-DNDEBUG",
            "-ftrivial-auto-var-init=zero",
            "-ffunction-sections",
            "-fdata-sections",
        ])
        .arg(format!("-I{}", android_jni_include.display()))
        .arg(format!("-I{}", nativehelper_full_include.display()))
        .arg(format!("-I{}", nativehelper_platform_headers.display()))
        .arg(format!("-I{}", liblog_include.display()))
        .arg("-c")
        .arg(&openjdk_math_source)
        .arg("-o")
        .arg(&openjdk_math_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut openjdk_math_command,
            &openjdk_math_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(openjdk_math_object);
    for adapter_source in [
        "darwin_framework_natives.cc",
        "darwin_icu_natives.cc",
        "darwin_icu_jni_bridge.cc",
        "darwin_libcore_natives.cc",
        "darwin_runtime_adapters.cc",
        "darwin_sigchain.cc",
        "fault_handler_arm64_darwin.cc",
    ] {
        if (real_graphics
            && matches!(
                adapter_source,
                "darwin_icu_natives.cc" | "darwin_libcore_natives.cc"
            ))
            || (!real_graphics && adapter_source == "darwin_icu_jni_bridge.cc")
        {
            continue;
        }
        let adapter_object = object_dir.join(format!("{adapter_source}.o"));
        let mut adapter_command = runtime_bootstrap_cpp_command(&includes);
        if real_graphics && adapter_source == "darwin_framework_natives.cc" {
            adapter_command
                .arg("-DDARWIN_ART_REAL_GRAPHICS")
                .arg("-I")
                .arg(&libcutils_include);
        }
        if real_graphics && adapter_source == "darwin_icu_jni_bridge.cc" {
            adapter_command.arg("-I").arg(root.join("include"));
        }
        adapter_command
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(root.join("compat").join(adapter_source))
            .arg("-o")
            .arg(&adapter_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut adapter_command,
                &adapter_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
        objects.push(adapter_object);
    }
    for source in sources {
        let object_name = source.replace('/', "_");
        let object = object_dir.join(format!("{object_name}.o"));
        let source_path = if matches!(
            source,
            "runtime.cc"
                | "class_linker.cc"
                | "thread.cc"
                | "thread_list.cc"
                | "gc/heap.cc"
                | "gc/collector/garbage_collector.cc"
                | "gc/collector/mark_compact.cc"
                | "gc/space/malloc_space.cc"
                | "entrypoints/quick/quick_alloc_entrypoints.cc"
                | "entrypoints/quick/quick_trampoline_entrypoints.cc"
                | "runtime_common.cc"
                | "oat/oat_file.cc"
                | "exec_utils.cc"
                | "signal_catcher.cc"
                | "nterp_helpers.cc"
                | "interpreter/mterp/nterp.cc"
        ) {
            patched_runtime.join(source)
        } else {
            runtime.join(source)
        };
        let mut compile_command = runtime_bootstrap_cpp_command(&includes);
        if matches!(
            source,
            "entrypoints/jni/jni_entrypoints.cc" | "oat/jni_stub_hash_map.cc"
        ) {
            // These two upstream TUs include the arm64 frame header through a
            // source-relative path. Select the Darwin PCS overlay before that
            // include guard can lock in Android's AAPCS64 stack layout.
            compile_command.args(["-include", "arch/arm64/jni_frame_arm64.h"]);
        }
        compile_command
            // Darwin has no system <elf.h>. The NDK copy is used as a
            // lowest-priority, headers-only definition of the Android ELF ABI.
            .arg("-idirafter")
            .arg(&ndk_arch_include)
            .arg("-idirafter")
            .arg(&ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(source_path)
            .arg("-o")
            .arg(&object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut compile_command,
                &object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected Runtime object format: {kind}").into());
        }
        objects.push(object);
    }
    let runtime_object = object_dir.join("runtime.cc.o");
    let symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_object))?;
    if !symbols.contains("_ZN3art7Runtime6Create") {
        return Err("compiled Runtime object does not export Runtime::Create".into());
    }
    file_hash_cache.save(&file_hash_cache_path)?;
    let archive = build_dir.join(if real_graphics {
        "libart-runtime-graphics-bootstrap-darwin.a"
    } else {
        "libart-runtime-bootstrap-darwin.a"
    });
    create_archive(&archive, &objects)?;
    println!(
        "{}: ART runtime initialization spine Mach-O objects={} compiled={} cached={} archive={}",
        if real_graphics {
            "build-runtime-graphics-bootstrap"
        } else {
            "build-runtime-bootstrap"
        },
        objects.len(),
        compiled_objects,
        cached_objects,
        archive.display()
    );
    Ok(())
}

fn audit_runtime_link(root: &Path) -> Result<()> {
    const MAX_EXPECTED_UNDEFINED: usize = 365;

    let runtime = root.join("_aosp/art/runtime");
    let build_dir = root.join("_build/runtime-link-probe");
    let object = build_dir.join("darwin_art_runtime.cc.o");
    let surface_object = build_dir.join("darwin_surface_bridge.mm.o");
    let runtime_library = build_dir.join("libdarwin_art_runtime.dylib");
    fs::create_dir_all(&build_dir)?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-bootstrap/patched-source/runtime"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/cmdline"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/libprofile"),
        runtime.clone(),
        runtime.join("base"),
        runtime.join("arch/arm64"),
        root.join("_aosp/art/libartpalette/include"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("_aosp/external/dlmalloc"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    run_command(
        runtime_cpp_command(&include_refs)
            .args(["-include", "mirror/object_reference.h"])
            .arg("-idirafter")
            .arg(ndk_arch_include)
            .arg("-idirafter")
            .arg(ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(root.join("probes/runtime_link_probe.cc"))
            .arg("-o")
            .arg(&object),
    )?;
    run_command(
        Command::new("clang++")
            .args(["-std=c++20", "-fobjc-arc", "-Wall", "-Wextra", "-c"])
            .arg(root.join("compat/darwin_surface_bridge.mm"))
            .arg("-I")
            .arg(root.join("compat"))
            .arg("-I")
            .arg(root.join("include"))
            .arg("-o")
            .arg(&surface_object),
    )?;

    let mut linker = Command::new("clang++");
    linker
        .arg("-dynamiclib")
        .arg("-Wl,-install_name,@rpath/libdarwin_art_runtime.dylib")
        .arg("-Wl,-exported_symbol,_darwin_art_run_process")
        .arg("-Wl,-exported_symbol,_darwin_art_shutdown_process")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_create")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_update")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_map_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_unmap_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_present")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_pump_events")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_destroy")
        .arg("-Wl,-dead_strip")
        .arg(&object)
        .arg(&surface_object)
        .arg(root.join("_build/runtime-bootstrap/libart-runtime-bootstrap-darwin.a"))
        .arg(root.join("_build/interpreter-core/libart-interpreter-darwin.a"))
        .arg(root.join("_build/runtime-arm64/libart-arm64-darwin.a"))
        .arg(root.join("_build/runtime-core/libart-core-darwin.a"))
        .arg(root.join("_build/runtime-platform/libart-platform-darwin.a"))
        .arg(root.join("_build/dex-probe/libdexfile-darwin.a"))
        .arg(root.join("_build/foundation/libartbase-darwin.a"))
        .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
        .arg(root.join("_build/foundation/libziparchive-darwin.a"))
        .arg(root.join("_build/nativehelper-foundation/libnativehelper_jvm.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .args([
            "-L/opt/homebrew/lib",
            "-L/opt/homebrew/opt/icu4c@78/lib",
            "-lfmt",
            "-llz4",
            "-licui18n",
            "-licuuc",
            "-licudata",
            "-lz",
            "-framework",
            "AppKit",
            "-framework",
            "IOSurface",
            "-framework",
            "Metal",
            "-framework",
            "QuartzCore",
            "-o",
        ])
        .arg(&runtime_library);
    let description = describe_command(&linker);
    let output = linker.output()?;
    if output.status.success() {
        let symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_library))?;
        for required in [
            "_darwin_art_run_process",
            "_darwin_art_shutdown_process",
            "_darwin_art_surface_create",
            "_darwin_art_surface_update",
            "_darwin_art_surface_map_producer",
            "_darwin_art_surface_unmap_producer",
            "_darwin_art_surface_present",
            "_darwin_art_surface_pump_events",
            "_darwin_art_surface_destroy",
        ] {
            if !symbols.contains(required) {
                return Err(format!(
                    "runtime C ABI library does not export required symbol {required}"
                )
                .into());
            }
        }
        build_runtime_host(root)?;
        println!("audit-runtime-link: C ABI dylib closure complete undefined=0 exports=9");
        return Ok(());
    }

    let stderr = String::from_utf8(output.stderr)?;
    fs::write(build_dir.join("link.err"), &stderr)?;
    if !stderr.contains("Undefined symbols for architecture arm64") {
        return Err(format!("unexpected Runtime link failure: {description}\n{stderr}").into());
    }
    let undefined = stderr
        .lines()
        .filter_map(|line| {
            line.trim_start()
                .strip_prefix('"')?
                .split_once("\", referenced from:")
                .map(|(symbol, _)| symbol.to_owned())
        })
        .collect::<BTreeSet<_>>();
    let symbol_list = undefined.iter().cloned().collect::<Vec<_>>().join("\n") + "\n";
    fs::write(build_dir.join("link.undefined"), symbol_list)?;
    let quick = undefined
        .iter()
        .filter(|symbol| symbol.starts_with("_art_quick_"))
        .count();
    let jni = undefined
        .iter()
        .filter(|symbol| symbol.starts_with("_art_jni_"))
        .count();
    let context = usize::from(undefined.contains("_artContextCopyForLongJump"));
    if quick != 0 || jni != 0 || context != 0 {
        return Err(format!(
            "ARM64 entrypoint link regression: quick={quick} jni={jni} context={context}"
        )
        .into());
    }
    if undefined.len() > MAX_EXPECTED_UNDEFINED {
        return Err(format!(
            "Runtime link closure regressed: undefined={} maximum={MAX_EXPECTED_UNDEFINED}",
            undefined.len()
        )
        .into());
    }
    println!(
        "audit-runtime-link: closure incomplete undefined={} quick=0 jni=0 context=0",
        undefined.len()
    );
    Ok(())
}

fn audit_runtime_graphics_link(root: &Path) -> Result<()> {
    build_shell_gate(root, "audit-android16-graphics-closure.sh")?;

    let runtime = root.join("_aosp/art/runtime");
    let build_dir = root.join("_build/runtime-graphics-link-probe");
    let object = build_dir.join("darwin_art_runtime.cc.o");
    let surface_object = build_dir.join("darwin_surface_bridge.mm.o");
    let runtime_library = build_dir.join("libdarwin_art_runtime_graphics.dylib");
    let graphics_closure = root.join("_build/graphics-closure-audit/android16-graphics-closure.o");
    let bootstrap =
        root.join("_build/runtime-graphics-bootstrap/libart-runtime-graphics-bootstrap-darwin.a");
    let icu_adapter_dir = root.join("_build/icu-runtime-adapters");
    let libcore_adapter = icu_adapter_dir.join("darwin_libcore_natives.o");
    let icu_jni_archive = root.join("_build/icu-jni-foundation/libicu-jni-darwin.a");
    for input in [
        &graphics_closure,
        &bootstrap,
        &libcore_adapter,
        &icu_jni_archive,
    ] {
        if !input.is_file() {
            return Err(format!(
                "real-graphics runtime input is missing: {}",
                input.display()
            )
            .into());
        }
    }
    let icu_jni_members = command_output(Command::new("ar").arg("-t").arg(&icu_jni_archive))?
        .lines()
        .filter(|member| *member != "__.SYMDEF")
        .count();
    if icu_jni_members != 15 {
        return Err(format!(
            "module-complete Android ICU JNI archive has {icu_jni_members} members, expected 15"
        )
        .into());
    }

    fs::create_dir_all(&build_dir)?;
    let includes = [
        root.join("include"),
        root.join("compat"),
        root.join("_build/runtime-arm64/generated"),
        root.join("_build/runtime-graphics-bootstrap/patched-source/runtime"),
        root.join("_build/runtime-core/patched-source/runtime"),
        root.join("_build/foundation/patched-source/libartbase"),
        root.join("_aosp/art/libartbase"),
        root.join("_aosp/art/cmdline"),
        root.join("_aosp/art/libdexfile"),
        root.join("_aosp/art/libelffile"),
        root.join("_aosp/art/libprofile"),
        runtime.clone(),
        runtime.join("base"),
        runtime.join("arch/arm64"),
        root.join("_aosp/art/libartpalette/include"),
        root.join("_aosp/system/libbase/include"),
        root.join("_aosp/external/tinyxml2"),
        root.join("_aosp/libnativehelper/include_jni"),
        root.join("_aosp/libnativehelper/header_only_include"),
        root.join("_aosp/libnativehelper/platform_header_only_include"),
        root.join("_aosp/external/dlmalloc"),
        PathBuf::from("/opt/homebrew/include"),
    ];
    let include_refs = includes.iter().map(PathBuf::as_path).collect::<Vec<_>>();
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;
    // Keep the process probe flavor-neutral. The linked compatibility object is
    // the sole owner of DARWIN_ART_REAL_GRAPHICS and chooses the real backend.
    run_command(
        runtime_cpp_command(&include_refs)
            .args(["-include", "mirror/object_reference.h"])
            .arg("-idirafter")
            .arg(ndk_arch_include)
            .arg("-idirafter")
            .arg(ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(root.join("probes/runtime_link_probe.cc"))
            .arg("-o")
            .arg(&object),
    )?;
    run_command(
        Command::new("clang++")
            .args(["-std=c++20", "-fobjc-arc", "-Wall", "-Wextra", "-c"])
            .arg(root.join("compat/darwin_surface_bridge.mm"))
            .arg("-I")
            .arg(root.join("compat"))
            .arg("-I")
            .arg(root.join("include"))
            .arg("-o")
            .arg(&surface_object),
    )?;

    let link_map = build_dir.join("runtime-graphics-link.map");
    let mut linker = Command::new("clang++");
    linker
        .arg("-dynamiclib")
        .arg("-Wl,-install_name,@rpath/libdarwin_art_runtime_graphics.dylib")
        .arg("-Wl,-exported_symbol,_darwin_art_run_process")
        .arg("-Wl,-exported_symbol,_darwin_art_shutdown_process")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_create")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_update")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_map_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_unmap_producer")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_present")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_pump_events")
        .arg("-Wl,-exported_symbol,_darwin_art_surface_destroy")
        .arg("-Wl,-dead_strip")
        .arg(format!("-Wl,-map,{}", link_map.display()))
        .arg(&object)
        .arg(&surface_object)
        // This is the already-audited force/normal composition of all 32
        // graphics archives. Place its fixed definitions before ART's normal
        // archives so the latter extract only additional runtime providers.
        .arg(&graphics_closure)
        .arg(&bootstrap)
        .arg(root.join("_build/interpreter-core/libart-interpreter-darwin.a"))
        .arg(root.join("_build/runtime-arm64/libart-arm64-darwin.a"))
        .arg(root.join("_build/runtime-core/libart-core-darwin.a"))
        .arg(root.join("_build/runtime-platform/libart-platform-darwin.a"))
        .arg(root.join("_build/dex-probe/libdexfile-darwin.a"))
        .arg(root.join("_build/runtime-graphics-bootstrap/objects/artbase_os_linux_aosp_fmt.cc.o"))
        .arg(root.join("_build/foundation/libartbase-darwin.a"))
        .arg(root.join("_build/ziparchive-incfs/libziparchive-for-incfs-darwin.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .arg(&libcore_adapter)
        .arg(format!("-Wl,-force_load,{}", icu_jni_archive.display()))
        // ld64 does not rescan archives that appeared before this new force-
        // loaded root. Re-supply libicu_jni's normal module providers in the
        // Android.bp dependency order.
        .arg(root.join("_build/nativehelper-foundation/libnativehelper_jvm.a"))
        .arg(root.join("_build/graphics-foundations/libcutils-darwin.a"))
        .arg(root.join("_build/graphics-foundations/liblog-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicui18n-darwin.a"))
        .arg(root.join("_build/icu-foundation/libicuuc-common-darwin.a"))
        .arg(root.join("_build/libbase-foundation/libandroid-base-darwin.a"))
        .args([
            "-L/opt/homebrew/lib",
            "-llz4",
            "-lz",
            "-framework",
            "CoreFoundation",
            "-framework",
            "CoreGraphics",
            "-framework",
            "ImageIO",
            "-framework",
            "Foundation",
            "-framework",
            "AppKit",
            "-framework",
            "IOSurface",
            "-framework",
            "Metal",
            "-framework",
            "QuartzCore",
            "-o",
        ])
        .arg(&runtime_library);
    let description = describe_command(&linker);
    let output = linker.output()?;
    if !output.status.success() {
        let stderr = String::from_utf8(output.stderr)?;
        fs::write(build_dir.join("link.err"), &stderr)?;
        return Err(format!("real-graphics Runtime link failed: {description}\n{stderr}").into());
    }

    let global_symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_library))?;
    for required in [
        "_darwin_art_run_process",
        "_darwin_art_shutdown_process",
        "_darwin_art_surface_create",
        "_darwin_art_surface_update",
        "_darwin_art_surface_map_producer",
        "_darwin_art_surface_unmap_producer",
        "_darwin_art_surface_present",
        "_darwin_art_surface_pump_events",
        "_darwin_art_surface_destroy",
    ] {
        if !global_symbols.contains(required) {
            return Err(format!("real-graphics Runtime lacks required symbol {required}").into());
        }
    }
    let all_symbols = command_output(Command::new("nm").arg(&runtime_library))?;
    for registrar in [
        "_init_android_graphics",
        "_register_android_graphics_classes",
    ] {
        if !all_symbols.contains(registrar) {
            return Err(format!("real-graphics Runtime lacks registrar symbol {registrar}").into());
        }
    }
    for forbidden in ["DarwinPaint", "DarwinRenderNode", "ProbeCanvas"] {
        if all_symbols.contains(forbidden) {
            return Err(format!("real-graphics Runtime contains fake symbol {forbidden}").into());
        }
    }
    let has_icu78 = all_symbols.lines().any(|line| {
        line.split_whitespace().last().is_some_and(|symbol| {
            !symbol.starts_with("_OUTLINED_FUNCTION_") && symbol.ends_with("_78")
        })
    });
    let has_icu76 = all_symbols.lines().any(|line| {
        line.split_whitespace().last().is_some_and(|symbol| {
            !symbol.starts_with("_OUTLINED_FUNCTION_") && symbol.ends_with("_76")
        })
    });
    if has_icu78 || !has_icu76 {
        return Err("real-graphics Runtime did not retain a pure AOSP ICU76 ABI".into());
    }
    let registration_header = fs::read_to_string(
        root.join("_build/android-graphics-jni/generated/darwin_android_graphics_registration.h"),
    )?;
    if !registration_header.contains("kNativeClassCount = 51") {
        return Err("real-graphics registrar is not the verified 51-class set".into());
    }
    let dependencies = command_output(Command::new("otool").arg("-L").arg(&runtime_library))?;
    for forbidden in ["CoreText", "libicu", "libfmt", "libfreetype"] {
        if dependencies.contains(forbidden) {
            return Err(format!("forbidden real-graphics host dependency: {forbidden}").into());
        }
    }
    let link_map_contents = fs::read_to_string(&link_map)?;
    if link_map_contents.contains("/opt/homebrew/opt/icu")
        || link_map_contents.contains("/opt/homebrew/Cellar/icu")
        || link_map_contents.contains("/opt/homebrew/opt/fmt")
        || link_map_contents.contains("/opt/homebrew/Cellar/fmt")
    {
        return Err("real-graphics link map consumed a Homebrew ICU/fmt provider".into());
    }
    build_runtime_host(root)?;
    println!(
        "audit-runtime-graphics-link: closure complete registrar=51 fake-symbols=0 host-icu=0 host-fmt=0 CoreText=0"
    );
    Ok(())
}

fn build_runtime_host(root: &Path) -> Result<()> {
    let status = Command::new("cargo")
        .args(["build", "-p", "darwin-art-host"])
        .current_dir(root)
        .status()?;
    if !status.success() {
        return Err("failed to build the Rust darwin-art-host launcher".into());
    }
    let executable = root.join("target/debug/darwin-art-host");
    if !executable.is_file() {
        return Err(format!("Rust runtime host is missing: {}", executable.display()).into());
    }
    Ok(())
}

fn probe_runtime_dex(root: &Path, show_window: bool) -> Result<()> {
    probe_runtime_dex_flavor(root, show_window, false)
}

fn probe_runtime_graphics(root: &Path) -> Result<()> {
    probe_runtime_dex_flavor(root, false, true)
}

fn probe_runtime_graphics_window(root: &Path) -> Result<()> {
    probe_runtime_dex_flavor(root, true, true)
}

fn probe_runtime_dex_flavor(root: &Path, show_window: bool, real_graphics: bool) -> Result<()> {
    let core_icu4j = if real_graphics {
        prepare_icu_runtime_bootclasspath(root)?
    } else {
        prepare_icu_bootclasspath(root)?;
        root.join("_build/bootclasspath/core-icu4j.jar")
    };
    let executable = root.join("target/debug/darwin-art-host");
    let runtime_library = root.join(if real_graphics {
        "_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
    } else {
        "_build/runtime-link-probe/libdarwin_art_runtime.dylib"
    });
    let core_oj = root.join("_prebuilt/android-16/bootclasspath/core-oj.jar");
    let core_libart = root.join("_prebuilt/android-16/bootclasspath/core-libart.jar");
    let framework = root.join("_prebuilt/android-16/bootclasspath/framework.jar");
    let classes_dex = root.join("_build/dex-probe/dex/classes.dex");
    for input in [
        &executable,
        &runtime_library,
        &core_oj,
        &core_libart,
        &framework,
        &core_icu4j,
        &classes_dex,
    ] {
        if !input.is_file() {
            return Err(format!(
                "runtime DEX probe input is missing: {}; run `art-bootstrap all` first",
                input.display()
            )
            .into());
        }
    }

    let mut command = Command::new(&executable);
    if show_window {
        command.args(["--window-seconds", "3"]);
    }
    command
        .arg(&runtime_library)
        .arg(&core_oj)
        .arg(&core_libart)
        .arg(&framework)
        .arg(&core_icu4j)
        .arg(&classes_dex);
    if real_graphics {
        let icu_runtime = root.join("_build/icu-runtime-adapters/runtime");
        let i18n_root = icu_runtime.join("i18n");
        let data_root = icu_runtime.join("data");
        let tzdata_root = icu_runtime.join("tzdata");
        let icu_data = i18n_root.join("etc/icu/icudt76l.dat");
        if !icu_data.is_file() {
            return Err(format!(
                "Android ICU76 runtime data is missing: {}; run `build-icu-runtime-adapters` first",
                icu_data.display()
            )
            .into());
        }
        command
            .env("ANDROID_I18N_ROOT", &i18n_root)
            .env("ANDROID_DATA", &data_root)
            .env("ANDROID_TZDATA_ROOT", &tzdata_root);
    }
    let output = command_output(&mut command)?;
    let expected = "Hello from Darwin ART main: 안녕\n\
                    ART Darwin Runtime::Create: ok\n\
                    ART Darwin app ClassLoader: PathClassLoader\n\
                    ART Darwin DEX interpreter: Hello.answer()=42\n\
                    ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
                    ART runtime native: System.arraycopy()=42\n\
                    ART Android framework: ProbeActivity().probeValue()=42\n\
                    ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
                    ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=640x360\n\
                    ART Android lifecycle: Activity.onCreate()=43\n\
                    ART Darwin launcher: main(String[])=ok";
    if output.trim() != expected {
        return Err(format!("unexpected runtime DEX probe output: {output:?}").into());
    }
    if real_graphics {
        if show_window {
            println!(
                "probe-runtime-graphics-window: Bitmap-backed Canvas -> DecorView.draw() -> NSWindow"
            );
        } else {
            println!(
                "probe-runtime-graphics: Bitmap-backed Canvas -> DecorView.draw() -> frame -> shutdown"
            );
        }
    } else if show_window {
        println!("probe-window: PhoneWindow -> DecorView.draw(Canvas) -> NSWindow");
    } else {
        println!(
            "probe-runtime-dex: Activity.attach() + PhoneWindow + DecorView.draw(Canvas) -> Darwin"
        );
    }
    Ok(())
}

fn prepare_icu_bootclasspath(root: &Path) -> Result<()> {
    let source_lock = read_lock(root)?;
    let source = root.join("_prebuilt/android-16/bootclasspath/core-icu4j.jar");
    if !source.is_file() {
        return Err(format!(
            "{} is missing; run `art-bootstrap sync` first",
            source.display()
        )
        .into());
    }
    verify_sha256(&source, lock_value(&source_lock, "CORE_ICU4J_SHA256")?)?;

    let build_dir = root.join("_build/bootclasspath/core-icu4j");
    let output = root.join("_build/bootclasspath/core-icu4j.jar");
    fs::create_dir_all(&build_dir)?;
    if !output.is_file() {
        run_command(
            Command::new(find_d8()?)
                .args(["--min-api", "36", "--output"])
                .arg(&output)
                .arg(&source),
        )?;
    }
    run_command(
        Command::new("unzip")
            .args(["-o", "-q"])
            .arg(&output)
            .arg("classes.dex")
            .arg("-d")
            .arg(&build_dir),
    )?;
    let probe = root.join("_build/dex-probe/dex-probe");
    if !probe.is_file() {
        return Err("DEX probe is missing; run `build-dex` first".into());
    }
    let summary = command_output(
        Command::new(&probe)
            .arg("--summary")
            .arg(build_dir.join("classes.dex")),
    )?;
    let expected = "AOSP DEX: verified=yes version=39 classes=1596 methods=14990 \
                    class[0]=Landroid/icu/impl/Assert;";
    if summary.trim() != expected {
        return Err(format!("unexpected core-icu4j DEX summary: {summary:?}").into());
    }
    Ok(())
}

fn prepare_icu_runtime_bootclasspath(root: &Path) -> Result<PathBuf> {
    let source_lock = read_lock(root)?;
    let apex = root.join("_prebuilt/android-16/i18n/com.android.i18n-arm64.apex");
    if !apex.is_file() {
        return Err(format!(
            "{} is missing; run `art-bootstrap sync` first",
            apex.display()
        )
        .into());
    }
    verify_sha256(&apex, lock_value(&source_lock, "I18N_ARM64_APEX_SHA256")?)?;

    let build_dir = root.join("_build/bootclasspath/core-icu4j-apex");
    let extracted = root.join("_build/bootclasspath/core-icu4j-apex.jar");
    let classes_dir = build_dir.join("runtime-bridge-classes");
    let bridge_jar = build_dir.join("runtime-bridge.jar");
    let output = root.join("_build/bootclasspath/core-icu4j-apex-reconciled.jar");
    fs::create_dir_all(&build_dir)?;
    if !extracted.is_file() {
        run_command(
            Command::new("cargo")
                .args(["run", "--quiet", "--manifest-path"])
                .arg(root.join("tools/apex-ext2-extract/Cargo.toml"))
                .arg("--")
                .arg(&apex)
                .arg(&extracted),
        )?;
    }
    verify_sha256(
        &extracted,
        lock_value(&source_lock, "I18N_CORE_ICU4J_SHA256")?,
    )?;
    let bridge_sources = [
        (
            root.join("_aosp/external/icu-runtime-bridge/com/android/i18n/util/ATrace.java"),
            "ICU_ATRACE_JAVA_SHA256",
        ),
        (
            root.join(
                "_aosp/external/icu-runtime-bridge/com/android/icu/util/UResourceBundleNative.java",
            ),
            "ICU_URESOURCE_BUNDLE_NATIVE_JAVA_SHA256",
        ),
    ];
    for (source, key) in &bridge_sources {
        if !source.is_file() {
            return Err(format!(
                "{} is missing; run `art-bootstrap sync` first",
                source.display()
            )
            .into());
        }
        verify_sha256(source, lock_value(&source_lock, key)?)?;
    }
    if !output.is_file() {
        fs::create_dir_all(&classes_dir)?;
        let mut javac = Command::new("javac");
        javac.args(["--release", "17", "-d"]).arg(&classes_dir);
        for (source, _) in &bridge_sources {
            javac.arg(source);
        }
        run_command(&mut javac)?;
        run_command(
            Command::new("jar")
                .args(["cf"])
                .arg(&bridge_jar)
                .arg("-C")
                .arg(&classes_dir)
                .arg("."),
        )?;
        run_command(
            Command::new(find_d8()?)
                .args(["--min-api", "36", "--output"])
                .arg(&output)
                .arg(&extracted)
                .arg(&bridge_jar),
        )?;
    }
    run_command(
        Command::new("unzip")
            .args(["-o", "-q"])
            .arg(&output)
            .arg("classes.dex")
            .arg("-d")
            .arg(&build_dir),
    )?;
    let dex = build_dir.join("classes.dex");
    let dexdump = find_d8()?.with_file_name("dexdump");
    let class_inventory = command_output(Command::new(&dexdump).args(["-l", "plain"]).arg(&dex))?;
    for registrar_target in [
        "Lcom/android/icu/text/TimeZoneNamesNative;",
        "Lcom/android/i18n/timezone/internal/Memory;",
        "Lcom/android/i18n/util/ATrace;",
        "Lcom/android/i18n/util/Log;",
        "Lcom/android/icu/util/CaseMapperNative;",
        "Lcom/android/icu/util/Icu4cMetadata;",
        "Lcom/android/icu/util/LocaleNative;",
        "Lcom/android/icu/util/UResourceBundleNative;",
        "Lcom/android/icu/util/regex/PatternNative;",
        "Lcom/android/icu/util/regex/MatcherNative;",
        "Lcom/android/icu/charset/NativeConverter;",
    ] {
        let descriptor = format!("Class descriptor  : '{registrar_target}'");
        if !class_inventory.contains(&descriptor) {
            return Err(
                format!("runtime core-icu4j lacks registrar target {registrar_target}").into(),
            );
        }
    }
    let probe = root.join("_build/dex-probe/dex-probe");
    let summary = command_output(Command::new(&probe).arg("--summary").arg(&dex))?;
    let expected = "AOSP DEX: verified=yes version=39 classes=1598 methods=14998 \
                    class[0]=Landroid/icu/impl/Assert;";
    if summary.trim() != expected {
        return Err(format!("unexpected runtime core-icu4j DEX summary: {summary:?}").into());
    }
    Ok(output)
}

fn find_ndk_headers() -> Result<(PathBuf, PathBuf)> {
    let mut ndk_roots = Vec::new();
    for variable in ["ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"] {
        if let Some(value) = env::var_os(variable) {
            ndk_roots.push(PathBuf::from(value));
        }
    }
    if let Some(home) = env::var_os("HOME") {
        let ndk_parent = PathBuf::from(home).join("Library/Android/sdk/ndk");
        if let Ok(entries) = fs::read_dir(ndk_parent) {
            let mut installed: Vec<PathBuf> = entries
                .filter_map(std::result::Result::ok)
                .map(|entry| entry.path())
                .filter(|path| path.is_dir())
                .collect();
            installed.sort();
            installed.reverse();
            ndk_roots.extend(installed);
        }
    }

    for ndk_root in ndk_roots {
        let prebuilt = ndk_root.join("toolchains/llvm/prebuilt");
        let Ok(hosts) = fs::read_dir(prebuilt) else {
            continue;
        };
        for host in hosts.filter_map(std::result::Result::ok) {
            let include = host.path().join("sysroot/usr/include");
            let arch = include.join("aarch64-linux-android");
            if include.join("elf.h").exists() && arch.is_dir() {
                return Ok((include, arch));
            }
        }
    }
    Err("Android NDK headers are required for the Android ELF ABI (<elf.h>)".into())
}

fn verify_bootclasspath(root: &Path) -> Result<()> {
    let prebuilt = root.join("_prebuilt/android-16/bootclasspath");
    let probe = root.join("_build/dex-probe/dex-probe");
    let manifest = read_key_value_file(&root.join("bootclasspath.lock"))?;
    if !probe.exists() {
        return Err("DEX probe is missing; run `build-dex` first".into());
    }

    let entries = [
        (
            "core-oj",
            "CORE_OJ_SHA256",
            "CORE_OJ_SIZE",
            "AOSP DEX: verified=yes version=39 classes=4188 methods=41526 \
             class[0]=Ljava/lang/Object;",
        ),
        (
            "core-libart",
            "CORE_LIBART_SHA256",
            "CORE_LIBART_SIZE",
            "AOSP DEX: verified=yes version=39 classes=543 methods=5453 \
             class[0]=Landroid/compat/Compatibility$BehaviorChangeDelegate;",
        ),
    ];
    for (name, sha_key, size_key, expected) in entries {
        let jar = prebuilt.join(format!("{name}.jar"));
        if !jar.exists() {
            return Err(format!(
                "{} is missing; extract matching Android 16 boot JARs first",
                jar.display()
            )
            .into());
        }
        verify_sha256(&jar, lock_value(&manifest, sha_key)?)?;
        let expected_size: u64 = lock_value(&manifest, size_key)?.parse()?;
        let actual_size = fs::metadata(&jar)?.len();
        if actual_size != expected_size {
            return Err(format!(
                "size mismatch for {}: expected {expected_size}, found {actual_size}",
                jar.display()
            )
            .into());
        }
        let extract_dir = root.join(format!("_build/bootclasspath/{name}"));
        fs::create_dir_all(&extract_dir)?;
        run_command(
            Command::new("unzip")
                .args(["-o", "-q"])
                .arg(&jar)
                .arg("classes.dex")
                .arg("-d")
                .arg(&extract_dir),
        )?;
        let output = command_output(
            Command::new(&probe)
                .arg("--summary")
                .arg(extract_dir.join("classes.dex")),
        )?;
        if output.trim() != expected {
            return Err(format!("unexpected {name} DEX summary: {output:?}").into());
        }
        println!("verify-bootclasspath: {name} {}", output.trim());
    }
    let framework = prebuilt.join("framework.jar");
    if !framework.exists() {
        return Err(format!(
            "{} is missing; pull /system/framework/framework.jar from the matching Android 16 image",
            framework.display()
        )
        .into());
    }
    verify_sha256(&framework, lock_value(&manifest, "FRAMEWORK_SHA256")?)?;
    let expected_framework_size: u64 = lock_value(&manifest, "FRAMEWORK_SIZE")?.parse()?;
    if fs::metadata(&framework)?.len() != expected_framework_size {
        return Err(format!("size mismatch for {}", framework.display()).into());
    }
    let framework_summaries = [
        (
            "classes.dex",
            "AOSP DEX: verified=yes version=39 classes=6609 methods=65389 \
             class[0]=Landroid/Manifest$permission;",
        ),
        (
            "classes2.dex",
            "AOSP DEX: verified=yes version=39 classes=7041 methods=65454 \
             class[0]=Landroid/hardware/camera2/impl/CallbackProxies$SessionStateCallbackProxy$$ExternalSyntheticLambda0;",
        ),
        (
            "classes3.dex",
            "AOSP DEX: verified=yes version=39 classes=8736 methods=65454 \
             class[0]=Landroid/os/IInterface;",
        ),
        (
            "classes4.dex",
            "AOSP DEX: verified=yes version=39 classes=6855 methods=65167 \
             class[0]=Lcom/android/ims/internal/IImsServiceController;",
        ),
        (
            "classes5.dex",
            "AOSP DEX: verified=yes version=39 classes=6478 methods=56051 \
             class[0]=Lcom/android/internal/hidden_from_bootclasspath/android/app/admin/flags/CustomFeatureFlags$$ExternalSyntheticLambda0;",
        ),
    ];
    let extract_dir = root.join("_build/bootclasspath/framework");
    fs::create_dir_all(&extract_dir)?;
    for (dex_name, expected) in framework_summaries {
        run_command(
            Command::new("unzip")
                .args(["-o", "-q"])
                .arg(&framework)
                .arg(dex_name)
                .arg("-d")
                .arg(&extract_dir),
        )?;
        let output = command_output(
            Command::new(&probe)
                .arg("--summary")
                .arg(extract_dir.join(dex_name)),
        )?;
        if output.trim() != expected {
            return Err(format!("unexpected framework {dex_name} DEX summary: {output:?}").into());
        }
        println!(
            "verify-bootclasspath: framework/{dex_name} {}",
            output.trim()
        );
    }
    let icu_source = prebuilt.join("core-icu4j.jar");
    let expected_icu_size: u64 = lock_value(&manifest, "CORE_ICU4J_SOURCE_SIZE")?.parse()?;
    if fs::metadata(&icu_source)?.len() != expected_icu_size {
        return Err(format!("size mismatch for {}", icu_source.display()).into());
    }
    prepare_icu_bootclasspath(root)?;
    println!(
        "verify-bootclasspath: core-icu4j AOSP DEX: verified=yes version=39 \
         classes=1596 methods=14990 class[0]=Landroid/icu/impl/Assert;"
    );
    Ok(())
}

fn common_cpp_command(includes: &[&Path]) -> Command {
    let mut command = Command::new("clang++");
    command.args([
        "-std=c++20",
        "-O2",
        "-DNDEBUG",
        "-DART_PAGE_SIZE_AGNOSTIC",
        // Android's platform Clang configuration zero-initializes trivial
        // automatic storage. ART's Soong build is compiled under that contract.
        "-ftrivial-auto-var-init=zero",
        "-ffunction-sections",
        "-fdata-sections",
    ]);
    for include in includes {
        command.arg(format!("-I{}", include.display()));
    }
    // Many AOSP headers include libartbase's globals.h relative to their own
    // directory. Force the patched Darwin copy first so the original include
    // guard cannot lock in the 4 KiB non-Linux fallback.
    command.args(["-include", "base/globals.h"]);
    command
}

fn compile_cpp(source: &Path, object_dir: &Path, includes: &[&Path]) -> Result<PathBuf> {
    let file_name = source
        .file_name()
        .ok_or_else(|| format!("source has no file name: {}", source.display()))?;
    let object = object_dir.join(format!("{}.o", file_name.to_string_lossy()));
    run_command(
        common_cpp_command(includes)
            .arg("-c")
            .arg(source)
            .arg("-o")
            .arg(&object),
    )?;
    Ok(object)
}

fn record_cache_result(compiled: bool, compiled_objects: &mut usize, cached_objects: &mut usize) {
    if compiled {
        *compiled_objects += 1;
    } else {
        *cached_objects += 1;
    }
}

fn compile_with_dependency_cache(
    command: &mut Command,
    object: &Path,
    compiler_identity: &str,
    file_hash_cache: &mut FileHashCache,
) -> Result<bool> {
    let depfile = object.with_extension("o.d");
    let fingerprint = object.with_extension("o.fingerprint");
    command.arg("-MMD").arg("-MF").arg(&depfile);
    let command_description = describe_command(command);

    if object.is_file()
        && depfile.is_file()
        && fingerprint.is_file()
        && let Some(current) = dependency_fingerprint(
            &depfile,
            &command_description,
            compiler_identity,
            file_hash_cache,
        )?
        && fs::read_to_string(&fingerprint).is_ok_and(|cached| cached == current)
    {
        return Ok(false);
    }

    run_command(command)?;
    let current = dependency_fingerprint(
        &depfile,
        &command_description,
        compiler_identity,
        file_hash_cache,
    )?
    .ok_or_else(|| {
        format!(
            "compiler did not produce a usable dependency file for {}",
            object.display()
        )
    })?;
    fs::write(fingerprint, current)?;
    Ok(true)
}

fn dependency_fingerprint(
    depfile: &Path,
    command_description: &str,
    compiler_identity: &str,
    file_hash_cache: &mut FileHashCache,
) -> Result<Option<String>> {
    let Ok(depfile_text) = fs::read_to_string(depfile) else {
        return Ok(None);
    };
    let normalized = depfile_text.replace("\\\r\n", " ").replace("\\\n", " ");
    let Some((_, dependency_text)) = normalized.split_once(':') else {
        return Ok(None);
    };
    let mut dependencies = parse_makefile_words(dependency_text)
        .into_iter()
        .map(PathBuf::from)
        .collect::<Vec<_>>();
    dependencies.sort();
    dependencies.dedup();
    if dependencies.is_empty() || dependencies.iter().any(|path| !path.is_file()) {
        return Ok(None);
    }

    let mut dependency_hashes = String::new();
    for dependency in &dependencies {
        dependency_hashes.push_str(file_hash_cache.hash(dependency)?);
        dependency_hashes.push_str("  ");
        dependency_hashes.push_str(&dependency.to_string_lossy());
        dependency_hashes.push('\n');
    }
    Ok(Some(format!(
        "cache-format=1\ncompiler={compiler_identity}\ncommand={command_description}\n{dependency_hashes}"
    )))
}

#[derive(Default)]
struct FileHashCache {
    entries: BTreeMap<String, FileHashEntry>,
}

struct FileHashEntry {
    metadata: String,
    sha256: String,
}

impl FileHashCache {
    fn load(path: &Path) -> Result<Self> {
        let Ok(contents) = fs::read_to_string(path) else {
            return Ok(Self::default());
        };
        let mut entries = BTreeMap::new();
        for line in contents.lines() {
            let mut fields = line.splitn(3, '\t');
            let (Some(path), Some(metadata), Some(sha256)) =
                (fields.next(), fields.next(), fields.next())
            else {
                continue;
            };
            entries.insert(
                path.to_owned(),
                FileHashEntry {
                    metadata: metadata.to_owned(),
                    sha256: sha256.to_owned(),
                },
            );
        }
        Ok(Self { entries })
    }

    fn hash(&mut self, path: &Path) -> Result<&str> {
        let path_key = path.to_string_lossy().into_owned();
        let metadata = fs::metadata(path)?;
        let metadata_key = format!(
            "{}:{}:{}:{}:{}:{}:{}",
            metadata.dev(),
            metadata.ino(),
            metadata.size(),
            metadata.mtime(),
            metadata.mtime_nsec(),
            metadata.ctime(),
            metadata.ctime_nsec()
        );
        let cache_hit = self
            .entries
            .get(&path_key)
            .is_some_and(|entry| entry.metadata == metadata_key);
        if !cache_hit {
            let mut file = fs::File::open(path)?;
            let mut digest = Sha256::new();
            let mut buffer = [0_u8; 64 * 1024];
            loop {
                let count = file.read(&mut buffer)?;
                if count == 0 {
                    break;
                }
                digest.update(&buffer[..count]);
            }
            let sha256 = format!("{:x}", digest.finalize());
            self.entries.insert(
                path_key.clone(),
                FileHashEntry {
                    metadata: metadata_key,
                    sha256,
                },
            );
        }
        Ok(&self.entries[&path_key].sha256)
    }

    fn save(&self, path: &Path) -> Result<()> {
        let mut contents = String::new();
        for (path, entry) in &self.entries {
            contents.push_str(path);
            contents.push('\t');
            contents.push_str(&entry.metadata);
            contents.push('\t');
            contents.push_str(&entry.sha256);
            contents.push('\n');
        }
        fs::write(path, contents)?;
        Ok(())
    }
}

fn parse_makefile_words(input: &str) -> Vec<String> {
    let mut words = Vec::new();
    let mut word = String::new();
    let mut escaped = false;
    for character in input.chars() {
        if escaped {
            word.push(character);
            escaped = false;
        } else if character == '\\' {
            escaped = true;
        } else if character.is_whitespace() {
            if !word.is_empty() {
                words.push(std::mem::take(&mut word));
            }
        } else {
            word.push(character);
        }
    }
    if escaped {
        word.push('\\');
    }
    if !word.is_empty() {
        words.push(word);
    }
    words
}

fn create_archive(archive: &Path, objects: &[PathBuf]) -> Result<()> {
    // `ar r` replaces members with matching names but leaves every stale member
    // whose name disappeared from the new object list. Build flavors with a
    // different module composition must therefore publish from an empty archive.
    if archive.exists() {
        fs::remove_file(archive)?;
    }
    let mut command = Command::new("ar");
    command.arg("rcs").arg(archive);
    for object in objects {
        command.arg(object);
    }
    run_command(&mut command)
}

fn replace_required(source: &mut String, from: &str, to: &str) -> Result<()> {
    if !source.contains(from) {
        return Err(
            format!("locked ART source no longer contains expected fragment: {from}").into(),
        );
    }
    *source = source.replacen(from, to, 1);
    Ok(())
}

fn read_lock(root: &Path) -> Result<BTreeMap<String, String>> {
    read_key_value_file(&root.join("sources.lock"))
}

fn read_key_value_file(path: &Path) -> Result<BTreeMap<String, String>> {
    let mut values = BTreeMap::new();
    for line in fs::read_to_string(path)?.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, value) = line
            .split_once('=')
            .ok_or_else(|| format!("invalid line in {}: {line}", path.display()))?;
        values.insert(key.to_owned(), value.to_owned());
    }
    Ok(values)
}

fn lock_value<'a>(lock: &'a BTreeMap<String, String>, key: &str) -> Result<&'a str> {
    lock.get(key)
        .map(String::as_str)
        .ok_or_else(|| format!("missing {key} in lock file").into())
}

fn verify_sha256(path: &Path, expected: &str) -> Result<()> {
    let output = command_output(Command::new("shasum").args(["-a", "256"]).arg(path))?;
    let actual = output.split_whitespace().next().unwrap_or_default();
    if actual != expected {
        return Err(format!(
            "SHA-256 mismatch for {}: expected {expected}, found {actual}",
            path.display()
        )
        .into());
    }
    Ok(())
}

fn run_command(command: &mut Command) -> Result<()> {
    let description = describe_command(command);
    let status = command.status()?;
    if !status.success() {
        return Err(format!("command failed ({status}): {description}").into());
    }
    Ok(())
}

fn command_output(command: &mut Command) -> Result<String> {
    let description = describe_command(command);
    let Output {
        status,
        stdout,
        stderr,
    } = command.output()?;
    if !status.success() {
        return Err(format!(
            "command failed ({status}): {description}\n{}",
            String::from_utf8_lossy(&stderr)
        )
        .into());
    }
    Ok(String::from_utf8(stdout)?)
}

fn describe_command(command: &Command) -> String {
    let program = command.get_program().to_string_lossy();
    let args = command
        .get_args()
        .map(OsStr::to_string_lossy)
        .collect::<Vec<_>>()
        .join(" ");
    format!("{program} {args}")
}

#[cfg(test)]
mod tests {
    use super::parse_makefile_words;

    #[test]
    fn parses_escaped_makefile_dependency_paths() {
        assert_eq!(
            parse_makefile_words(" source.cc include/header.h path\\ with\\ spaces/header.h "),
            ["source.cc", "include/header.h", "path with spaces/header.h"]
        );
    }
}
