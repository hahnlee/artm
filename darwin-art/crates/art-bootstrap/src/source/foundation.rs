use super::*;

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
