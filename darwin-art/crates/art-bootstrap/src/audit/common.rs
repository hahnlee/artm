use super::*;

pub(super) fn require_file(path: &Path, description: &str) -> Result<()> {
    if path.is_file() {
        return Ok(());
    }
    Err(format!("{description}: {}", path.display()).into())
}
