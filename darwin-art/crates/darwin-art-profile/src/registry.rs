use crate::{ProfileError, ProfilePaths};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::PathBuf;

const RECORD_VERSION: &[u8] = b"darwin-art-launch-v1\n";

pub(crate) struct PackageRegistry {
    directory: PathBuf,
}

impl PackageRegistry {
    pub(crate) fn new(paths: &ProfilePaths) -> Self {
        Self {
            directory: paths.mount.join("system/package-registry"),
        }
    }

    pub(crate) fn register(&self, package: &str, record: &[u8]) -> Result<(), ProfileError> {
        validate_package(package)?;
        validate_record(record)?;
        fs::create_dir_all(&self.directory)?;
        fs::set_permissions(&self.directory, fs::Permissions::from_mode(0o700))?;
        let stage = self
            .directory
            .join(format!(".{package}.register-{}", std::process::id()));
        let destination = self.directory.join(format!("{package}.launch"));
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&stage)?;
        let result = output
            .write_all(record)
            .and_then(|()| output.sync_all())
            .and_then(|()| fs::set_permissions(&stage, fs::Permissions::from_mode(0o400)))
            .and_then(|()| fs::rename(&stage, &destination))
            .and_then(|()| File::open(&self.directory)?.sync_all());
        if let Err(error) = result {
            let _ = fs::remove_file(&stage);
            return Err(error.into());
        }
        Ok(())
    }

    pub(crate) fn resolve(&self, package: &str) -> Result<Vec<u8>, ProfileError> {
        validate_package(package)?;
        let record = fs::read(self.directory.join(format!("{package}.launch")))?;
        validate_record(&record)?;
        Ok(record)
    }

    pub(crate) fn unregister(&self, package: &str) -> Result<(), ProfileError> {
        validate_package(package)?;
        fs::remove_file(self.directory.join(format!("{package}.launch")))?;
        File::open(&self.directory)?.sync_all()?;
        Ok(())
    }

    pub(crate) fn list(&self) -> Result<Vec<u8>, ProfileError> {
        let mut packages = Vec::new();
        match fs::read_dir(&self.directory) {
            Ok(entries) => {
                for entry in entries {
                    let entry = entry?;
                    let name = entry.file_name();
                    let Some(name) = name.to_str() else { continue };
                    let Some(package) = name.strip_suffix(".launch") else {
                        continue;
                    };
                    if entry.file_type()?.is_file() && validate_package(package).is_ok() {
                        packages.push(package.to_owned());
                    }
                }
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        packages.sort();
        let mut listing = packages.join("\n");
        if !listing.is_empty() {
            listing.push('\n');
        }
        Ok(listing.into_bytes())
    }
}

pub(crate) fn validate_package(package: &str) -> Result<(), ProfileError> {
    if package.is_empty()
        || package.len() > 255
        || package.starts_with('.')
        || package.ends_with('.')
        || package.contains("..")
        || !package
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    {
        return Err(ProfileError::Daemon("invalid Android package name".into()));
    }
    Ok(())
}

fn validate_record(record: &[u8]) -> Result<(), ProfileError> {
    if !record.starts_with(RECORD_VERSION)
        || record.len() > 60 * 1024
        || record.contains(&0)
        || !record.ends_with(b"\n")
    {
        return Err(ProfileError::Daemon("invalid launch record".into()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn package_names_cannot_escape_the_registry() {
        for valid in ["com.android.calculator2", "org.example.App"] {
            assert!(validate_package(valid).is_ok());
        }
        for invalid in ["", ".bad", "bad.", "a..b", "a/b"] {
            assert!(validate_package(invalid).is_err());
        }
    }
}
