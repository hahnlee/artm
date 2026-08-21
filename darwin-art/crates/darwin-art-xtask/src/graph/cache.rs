use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use super::super::{ninja_path, shell_quote};

#[derive(Clone)]
pub(crate) struct CachedNativeObject {
    pub(crate) object: PathBuf,
    pub(crate) source: PathBuf,
    pub(crate) command: String,
    pub(crate) shell_quoted: bool,
}

/// Recover the compiler command and source identity persisted by
/// `compile_with_dependency_cache`. This lets a later graph generation turn
/// an already materialized bootstrap into real per-object Ninja edges without
/// duplicating ART's long include/define command construction in xtask.
pub(crate) fn cached_native_objects(
    object_dir: &Path,
    archive: &Path,
    required_sources: &[&str],
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
    if required_sources.iter().any(|required| {
        !by_name.values().any(|object| {
            object.source.file_name().and_then(|name| name.to_str()) == Some(required)
        })
    }) {
        // A source-list change must not be hidden by an older archive cache.
        // Let the canonical builder materialize the missing TU and its
        // fingerprint before promoting the archive back to Ninja edges.
        return Ok(None);
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

pub(crate) fn emit_cached_native_graph(
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
        // Older canonical builders persisted command/fingerprint metadata but
        // did not emit depfiles.  Treat those objects as valid cache entries
        // instead of making Ninja rebuild the entire archive merely because
        // metadata is missing.  Once such a TU is rebuilt, the normal rule
        // above writes a depfile and the next graph generation promotes it to
        // dependency-aware scheduling.
        graph.push_str("rule native_cached_cpp_legacy\n");
        graph.push_str("  command = $compile_command\n");
        graph.push_str("  description = CXX $out (legacy cache)\n");
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
        // The canonical builder names depfiles `<source>.o.d`; using
        // `with_extension("d")` silently looked for `<source>.d` and kept
        // every promoted object on the legacy rule forever.
        let depfile = object.object.with_extension("o.d");
        graph.push_str("build ");
        graph.push_str(&output);
        graph.push_str(if depfile.is_file() {
            ": native_cached_cpp "
        } else {
            ": native_cached_cpp_legacy "
        });
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
