use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;

use crate::Result;
pub(crate) use crate::native_cache::{
    FileHashCache, compile_with_dependency_cache, link_with_cache,
};
use crate::support::run_command;

pub(crate) struct PendingNativeCompile {
    pub(crate) command: Command,
    pub(crate) object: PathBuf,
}

pub(crate) fn common_cpp_command(includes: &[&Path]) -> Command {
    let mut command = Command::new("clang++");
    command.args([
        "-std=c++20",
        "-O2",
        "-DNDEBUG",
        "-DART_PAGE_SIZE_AGNOSTIC",
        "-ftrivial-auto-var-init=zero",
        "-ffunction-sections",
        "-fdata-sections",
    ]);
    for include in includes {
        command.arg(format!("-I{}", include.display()));
    }
    command.args(["-include", "base/globals.h"]);
    command
}

pub(crate) fn compile_pending_native(
    jobs: Vec<PendingNativeCompile>,
    compiler_identity: &str,
) -> Result<(Vec<PathBuf>, usize, usize)> {
    if jobs.is_empty() {
        return Ok((Vec::new(), 0, 0));
    }
    let default_jobs = thread::available_parallelism()
        .map(|count| count.get().min(8))
        .unwrap_or(4)
        .max(1);
    let workers = std::env::var("DARWIN_ART_NATIVE_JOBS")
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(default_jobs)
        .min(8);
    let mut pending = jobs;
    let mut objects = Vec::new();
    let mut compiled = 0;
    let mut cached = 0;
    while !pending.is_empty() {
        let batch_len = pending.len().min(workers);
        let batch: Vec<_> = pending.drain(..batch_len).collect();
        let results = thread::scope(|scope| {
            let mut handles = Vec::with_capacity(batch.len());
            for job in batch {
                let identity = compiler_identity.to_owned();
                handles.push(scope.spawn(move || {
                    let cache_path = job.object.with_extension("hashes.cache");
                    let mut cache =
                        FileHashCache::load(&cache_path).map_err(|error| error.to_string())?;
                    let mut command = job.command;
                    let did_compile = compile_with_dependency_cache(
                        &mut command,
                        &job.object,
                        &identity,
                        &mut cache,
                    )
                    .map_err(|error| error.to_string())?;
                    cache.save(&cache_path).map_err(|error| error.to_string())?;
                    Ok::<_, String>((job.object, did_compile))
                }));
            }
            handles
                .into_iter()
                .map(|handle| {
                    handle
                        .join()
                        .map_err(|_| "native compile worker panicked".to_owned())?
                })
                .collect::<std::result::Result<Vec<_>, String>>()
        })
        .map_err(|error| -> Box<dyn std::error::Error> { error.into() })?;
        for (object, did_compile) in results {
            record_cache_result(did_compile, &mut compiled, &mut cached);
            objects.push(object);
        }
    }
    Ok((objects, compiled, cached))
}

pub(crate) fn compile_cpp(source: &Path, object_dir: &Path, includes: &[&Path]) -> Result<PathBuf> {
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

pub(crate) fn record_cache_result(
    compiled: bool,
    compiled_objects: &mut usize,
    cached_objects: &mut usize,
) {
    if compiled {
        *compiled_objects += 1;
    } else {
        *cached_objects += 1;
    }
}

pub(crate) fn create_archive(archive: &Path, objects: &[PathBuf]) -> Result<()> {
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
