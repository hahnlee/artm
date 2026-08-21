use std::fs;
use std::io;
use std::path::Path;

/// Publish graph metadata as one complete file. A killed xtask must leave the
/// previous Ninja graph/cache stamp usable rather than a truncated file.
pub(crate) fn write(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension(format!("darwin-art-xtask-tmp-{}", std::process::id()));
    fs::write(&temporary, bytes)?;
    fs::rename(temporary, path)
}
