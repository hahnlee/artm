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
        "build-dex" => build_dex_probe(&root),
        "build-runtime-platform" => build_runtime_platform(&root),
        "build-runtime-core" => build_runtime_core(&root),
        "probe-park" => probe_park(&root),
        "build-runtime-arm64" => build_runtime_arm64(&root),
        "build-interpreter-core" => build_interpreter_core(&root),
        "build-runtime-bootstrap" => build_runtime_bootstrap(&root),
        "audit-runtime-link" => audit_runtime_link(&root),
        "probe-runtime-dex" => probe_runtime_dex(&root),
        "verify-bootclasspath" => verify_bootclasspath(&root),
        "all" => {
            doctor()?;
            sync_sources(&root)?;
            probe_asm(&root)?;
            probe_page_size(&root)?;
            build_dex_probe(&root)?;
            build_runtime_platform(&root)?;
            build_runtime_core(&root)?;
            build_runtime_arm64(&root)?;
            build_interpreter_core(&root)?;
            build_runtime_bootstrap(&root)?;
            audit_runtime_link(&root)?;
            probe_runtime_dex(&root)?;
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
    println!("  build-dex  compile AOSP libdexfile and parse a generated classes.dex");
    println!("  build-runtime-platform  compile ART host platform sources as Mach-O");
    println!("  build-runtime-core  apply Darwin monitor patches and compile runtime core");
    println!("  probe-park  stress Darwin's pthread-backed LockSupport primitive");
    println!("  build-runtime-arm64  generate ABI constants and compile ARM64 context");
    println!("  build-interpreter-core  compile ART's C++ interpreter implementation");
    println!("  build-runtime-bootstrap  compile ART Runtime initialization for Darwin");
    println!("  audit-runtime-link  measure the remaining Runtime::Create link closure");
    println!("  probe-runtime-dex  execute Hello.answer() from DEX in ART's interpreter");
    println!("  verify-bootclasspath  verify extracted Android 16 core DEX files");
    println!("  all        run every check and probe");
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
        artbase.join("base/mem_map_unix.cc"),
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

    run_command(
        Command::new("javac")
            .args(["--release", "8", "-d"])
            .arg(&class_dir)
            .arg(root.join("probes/Hello.java")),
    )?;

    let hello_class = class_dir.join("dev/darwinart/probe/Hello.class");
    run_command(
        Command::new(find_d8()?)
            .args(["--output"])
            .arg(&dex_dir)
            .arg(&hello_class),
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
    let expected = "AOSP DEX: verified=yes version=35 classes=1 methods=5 class[0]=Ldev/darwinart/probe/Hello;";
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
    let sdk_root = env::var_os("ANDROID_SDK_ROOT")
        .or_else(|| env::var_os("ANDROID_HOME"))
        .map(PathBuf::from)
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .map(|home| home.join("Library/Android/sdk"))
        })
        .ok_or("could not determine the Android SDK directory")?;
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
    let dlmalloc = root.join("_aosp/external/dlmalloc");
    let odrefresh_include = root.join("_aosp/art/odrefresh/include");
    let sigchain = root.join("_aosp/art/sigchainlib");
    let unwindstack_include = root.join("_aosp/system/unwinding/libunwindstack/include");
    let compat = root.join("compat");
    let (ndk_include, ndk_arch_include) = find_ndk_headers()?;

    if !generated_dir.join("asm_defines.h").exists() {
        return Err(
            "generated ART ABI constants are missing; run `build-runtime-arm64` first".into(),
        );
    }

    let build_dir = root.join("_build/runtime-bootstrap");
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
    ] {
        run_command(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(root.join(patch))
                .current_dir(&patched_source_dir),
        )?;
    }

    let includes = [
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
        dlmalloc.as_path(),
        Path::new("/opt/homebrew/include"),
    ];
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
        "nterp_helpers.cc",
        "oat/image.cc",
        "oat/oat.cc",
        "oat/oat_file.cc",
        "oat/oat_quick_method_header.cc",
        "oat/stack_map.cc",
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
        "mirror/string.cc",
        "mirror/throwable.cc",
        "runtime_common.cc",
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
    for adapter_source in [
        "darwin_runtime_adapters.cc",
        "darwin_sigchain.cc",
        "fault_handler_arm64_darwin.cc",
    ] {
        let adapter_object = object_dir.join(format!("{adapter_source}.o"));
        let mut adapter_command = runtime_bootstrap_cpp_command(&includes);
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
    let archive = build_dir.join("libart-runtime-bootstrap-darwin.a");
    create_archive(&archive, &objects)?;
    println!(
        "build-runtime-bootstrap: ART runtime initialization spine Mach-O objects={} compiled={} cached={} archive={}",
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
    let object = build_dir.join("runtime_link_probe.cc.o");
    let executable = build_dir.join("runtime-link-probe");
    fs::create_dir_all(&build_dir)?;
    let includes = [
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

    let mut linker = Command::new("clang++");
    linker
        .arg("-Wl,-dead_strip")
        .arg(&object)
        .arg(root.join("_build/runtime-bootstrap/libart-runtime-bootstrap-darwin.a"))
        .arg(root.join("_build/interpreter-core/libart-interpreter-darwin.a"))
        .arg(root.join("_build/runtime-arm64/libart-arm64-darwin.a"))
        .arg(root.join("_build/runtime-core/libart-core-darwin.a"))
        .arg(root.join("_build/runtime-platform/libart-platform-darwin.a"))
        .arg(root.join("_build/dex-probe/libdexfile-darwin.a"))
        .arg(root.join("_build/foundation/libartbase-darwin.a"))
        .arg(root.join("_build/foundation/libandroid-base-darwin.a"))
        .arg(root.join("_build/foundation/libziparchive-darwin.a"))
        .args(["-L/opt/homebrew/lib", "-lfmt", "-llz4", "-lz", "-o"])
        .arg(&executable);
    let description = describe_command(&linker);
    let output = linker.output()?;
    if output.status.success() {
        println!("audit-runtime-link: closure complete undefined=0");
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

fn probe_runtime_dex(root: &Path) -> Result<()> {
    let executable = root.join("_build/runtime-link-probe/runtime-link-probe");
    let core_oj = root.join("_prebuilt/android-16/bootclasspath/core-oj.jar");
    let core_libart = root.join("_prebuilt/android-16/bootclasspath/core-libart.jar");
    let classes_dex = root.join("_build/dex-probe/dex/classes.dex");
    for input in [&executable, &core_oj, &core_libart, &classes_dex] {
        if !input.is_file() {
            return Err(format!(
                "runtime DEX probe input is missing: {}; run `art-bootstrap all` first",
                input.display()
            )
            .into());
        }
    }

    let output = command_output(
        Command::new(&executable)
            .arg(&core_oj)
            .arg(&core_libart)
            .arg(&classes_dex),
    )?;
    let expected = "ART Darwin Runtime::Create: ok\n\
                    ART Darwin app ClassLoader: PathClassLoader\n\
                    ART Darwin DEX interpreter: Hello.answer()=42\n\
                    ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42";
    if output.trim() != expected {
        return Err(format!("unexpected runtime DEX probe output: {output:?}").into());
    }
    println!(
        "probe-runtime-dex: PathClassLoader -> interpreter -> JNI -> Darwin -> nativeRoundTrip()=42"
    );
    Ok(())
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
