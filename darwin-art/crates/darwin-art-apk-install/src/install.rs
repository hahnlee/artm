use crate::publish;
use sha2::{Digest, Sha256};
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;

const MAX_APK_SIZE: usize = 512 * 1024 * 1024;
const INSTALL_VERSION: &str = "darwin-art-apk-install-v1";

fn le16(input: &[u8], offset: usize, label: &str) -> Result<u16, String> {
    let bytes = input
        .get(offset..offset + 2)
        .ok_or_else(|| format!("{label} is outside APK"))?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn le32(input: &[u8], offset: usize, label: &str) -> Result<u32, String> {
    let bytes = input
        .get(offset..offset + 4)
        .ok_or_else(|| format!("{label} is outside APK"))?;
    Ok(u32::from_le_bytes(
        bytes.try_into().expect("four-byte slice"),
    ))
}

pub struct InstallRequest {
    pub apk: PathBuf,
    pub install_root: PathBuf,
    pub package: String,
    pub version_code: String,
    pub native_root: Option<String>,
    pub extractor: Option<PathBuf>,
    pub runtime_abi: String,
    pub splits: Vec<PathBuf>,
}

pub struct InstalledApk {
    pub apk_sha256: String,
    pub base_apk: PathBuf,
    pub native_root: Option<PathBuf>,
    pub existing: bool,
}

pub fn install(request: &InstallRequest) -> Result<InstalledApk, String> {
    validate_request(request)?;
    let apk = read_apk(&request.apk)?;
    let split_bytes = request
        .splits
        .iter()
        .map(|path| read_apk(path))
        .collect::<Result<Vec<_>, _>>()?;
    let apk_sha256 = identity_sha256(&apk, &split_bytes);
    let version_parent = request
        .install_root
        .join(&request.package)
        .join(&request.version_code);
    fs::create_dir_all(&version_parent)
        .map_err(|error| format!("could not create install parent: {error}"))?;
    let version_parent = fs::canonicalize(&version_parent)
        .map_err(|error| format!("could not canonicalize install parent: {error}"))?;
    let destination = version_parent.join(&apk_sha256);
    if destination.exists() {
        return validate_existing(request, &apk_sha256, &destination);
    }

    let mut stage = StageGuard::new(&version_parent)?;
    let base_apk = stage.path().join("base.apk");
    write_sealed_file(&base_apk, &apk)?;
    let mut installed_splits = Vec::with_capacity(split_bytes.len());
    for (index, bytes) in split_bytes.iter().enumerate() {
        let destination = stage.path().join(format!("split-{index}.apk"));
        write_sealed_file(&destination, bytes)?;
        installed_splits.push(destination);
    }

    if let Some(root) = &request.native_root {
        let extractor = request
            .extractor
            .as_ref()
            .ok_or_else(|| "native APK installation requires an extractor".to_owned())?;
        let native_parent = stage.path().join("android-elf");
        fs::create_dir(&native_parent)
            .map_err(|error| format!("could not create native parent: {error}"))?;
        let native_directory = native_parent.join("arm64-v8a");
        let mut native_sources = Vec::new();
        let mut native_archive_count = 0_usize;
        if archive_has_native_libraries(&apk)? {
            native_archive_count += 1;
        }
        if archive_contains_native_root(&apk, root)? {
            native_sources.push((base_apk.clone(), 0_usize));
        }
        for (index, (bytes, source)) in split_bytes.iter().zip(installed_splits.iter()).enumerate()
        {
            if archive_has_native_libraries(bytes)? {
                native_archive_count += 1;
            }
            if archive_contains_native_root(bytes, root)? {
                native_sources.push((source.clone(), index + 1));
            }
        }
        if native_archive_count != 1 {
            return Err(
                "native libraries must be contained in exactly one base/ABI split".to_owned(),
            );
        }
        if native_sources.is_empty() {
            return Err("selected native root is absent from all APKs".to_owned());
        }
        for (source, index) in native_sources {
            let extraction_directory = native_parent.join(format!(".native-{index}"));
            let output = Command::new(extractor)
                .arg(&source)
                .arg(&extraction_directory)
                .arg(root)
                .output()
                .map_err(|error| format!("could not run native extractor: {error}"))?;
            if output.status.success() {
                // The archive-count check above guarantees one native
                // source. Preserve the extractor's sealed directory and
                // publish it atomically; reopening it would weaken the
                // read-only boundary and makes rename-based cleanup fail.
                fs::rename(&extraction_directory, &native_directory).map_err(|error| {
                    format!("could not publish extracted native directory: {error}")
                })?;
                continue;
            }
            let detail = String::from_utf8_lossy(&output.stderr);
            return Err(format!("native extraction failed: {}", detail.trim()));
        }
        fs::set_permissions(&native_parent, fs::Permissions::from_mode(0o500))
            .map_err(|error| format!("could not seal native parent: {error}"))?;
    } else if request.extractor.is_some() {
        return Err("non-native APK installation must not supply an extractor".to_owned());
    }

    let contract = contract(request, &apk_sha256);
    write_sealed_file(&stage.path().join("install.contract"), contract.as_bytes())?;
    File::open(stage.path())
        .and_then(|directory| directory.sync_all())
        .map_err(|error| format!("could not sync install staging directory: {error}"))?;
    fs::set_permissions(stage.path(), fs::Permissions::from_mode(0o500))
        .map_err(|error| format!("could not seal install staging directory: {error}"))?;

    match publish::exclusive(stage.path(), &destination) {
        Ok(()) => stage.disarm(),
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
            return validate_existing(request, &apk_sha256, &destination);
        }
        Err(error) => {
            return Err(format!(
                "could not atomically publish installation: {error}"
            ));
        }
    }
    let _ = File::open(&version_parent).and_then(|directory| directory.sync_all());
    installed(request, apk_sha256, destination, false)
}

fn archive_contains_native_root(bytes: &[u8], root: &str) -> Result<bool, String> {
    let floor = bytes.len().saturating_sub(22 + usize::from(u16::MAX));
    let mut eocd = None;
    for offset in (floor..bytes.len().saturating_sub(21)).rev() {
        if bytes.get(offset..offset + 4) == Some(&0x0605_4b50_u32.to_le_bytes()) {
            let comment = usize::from(le16(bytes, offset + 20, "APK EOCD comment")?);
            if offset
                .checked_add(22 + comment)
                .is_some_and(|end| end == bytes.len())
            {
                eocd = Some(offset);
                break;
            }
        }
    }
    let eocd = eocd.ok_or_else(|| "APK has no valid ZIP EOCD".to_owned())?;
    let count = usize::from(le16(bytes, eocd + 10, "APK EOCD entry count")?);
    let central = usize::try_from(le32(bytes, eocd + 16, "APK central offset")?)
        .map_err(|_| "APK central offset is too large".to_owned())?;
    let wanted = format!("lib/arm64-v8a/{root}").into_bytes();
    let mut cursor = central;
    for _ in 0..count {
        if le32(bytes, cursor, "APK central signature")? != 0x0201_4b50 {
            return Err("APK central directory signature mismatch".to_owned());
        }
        let name_length = usize::from(le16(bytes, cursor + 28, "APK central name length")?);
        let extra_length = usize::from(le16(bytes, cursor + 30, "APK central extra length")?);
        let comment_length = usize::from(le16(bytes, cursor + 32, "APK central comment length")?);
        let name_start = cursor
            .checked_add(46)
            .ok_or_else(|| "APK central name offset overflow".to_owned())?;
        let name_end = name_start
            .checked_add(name_length)
            .ok_or_else(|| "APK central name length overflow".to_owned())?;
        if bytes.get(name_start..name_end) == Some(wanted.as_slice()) {
            return Ok(true);
        }
        cursor = name_end
            .checked_add(extra_length)
            .and_then(|value| value.checked_add(comment_length))
            .ok_or_else(|| "APK central record length overflow".to_owned())?;
    }
    Ok(false)
}

fn archive_has_native_libraries(bytes: &[u8]) -> Result<bool, String> {
    let floor = bytes.len().saturating_sub(22 + usize::from(u16::MAX));
    let mut eocd = None;
    for offset in (floor..bytes.len().saturating_sub(21)).rev() {
        if bytes.get(offset..offset + 4) == Some(&0x0605_4b50_u32.to_le_bytes()) {
            let comment = usize::from(le16(bytes, offset + 20, "APK EOCD comment")?);
            if offset
                .checked_add(22 + comment)
                .is_some_and(|end| end == bytes.len())
            {
                eocd = Some(offset);
                break;
            }
        }
    }
    let eocd = eocd.ok_or_else(|| "APK has no valid ZIP EOCD".to_owned())?;
    let count = usize::from(le16(bytes, eocd + 10, "APK EOCD entry count")?);
    let central = usize::try_from(le32(bytes, eocd + 16, "APK central offset")?)
        .map_err(|_| "APK central offset is too large".to_owned())?;
    let mut cursor = central;
    for _ in 0..count {
        if le32(bytes, cursor, "APK central signature")? != 0x0201_4b50 {
            return Err("APK central directory signature mismatch".to_owned());
        }
        let name_length = usize::from(le16(bytes, cursor + 28, "APK central name length")?);
        let extra_length = usize::from(le16(bytes, cursor + 30, "APK central extra length")?);
        let comment_length = usize::from(le16(bytes, cursor + 32, "APK central comment length")?);
        let name_start = cursor
            .checked_add(46)
            .ok_or_else(|| "APK central name offset overflow".to_owned())?;
        let name_end = name_start
            .checked_add(name_length)
            .ok_or_else(|| "APK central name length overflow".to_owned())?;
        let name = bytes
            .get(name_start..name_end)
            .ok_or_else(|| "APK central name exceeds APK".to_owned())?;
        if name.starts_with(b"lib/arm64-v8a/")
            && name.ends_with(b".so")
            && !name[14..].contains(&b'/')
        {
            return Ok(true);
        }
        cursor = name_end
            .checked_add(extra_length)
            .and_then(|value| value.checked_add(comment_length))
            .ok_or_else(|| "APK central record length overflow".to_owned())?;
    }
    Ok(false)
}

fn validate_request(request: &InstallRequest) -> Result<(), String> {
    if !component(&request.package, true)
        || !component(&request.version_code, false)
        || !component(&request.runtime_abi, true)
    {
        return Err("package, version code, or runtime ABI is not a safe component".to_owned());
    }
    if let Some(root) = &request.native_root
        && (!root.starts_with("lib")
            || !root.ends_with(".so")
            || root
                .bytes()
                .any(|byte| !byte.is_ascii_graphic() || matches!(byte, b'/' | b'\\' | b'\0')))
    {
        return Err("native root is not a direct Android SONAME".to_owned());
    }
    for split in &request.splits {
        if split.as_os_str().is_empty() {
            return Err("split APK path is empty".to_owned());
        }
    }
    Ok(())
}

fn component(value: &str, punctuation: bool) -> bool {
    !value.is_empty()
        && value != "."
        && value != ".."
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || (punctuation && matches!(byte, b'.' | b'_' | b'-'))
        })
}

fn read_apk(path: &Path) -> Result<Vec<u8>, String> {
    let mut file = File::open(path).map_err(|error| format!("could not open APK: {error}"))?;
    let length = usize::try_from(
        file.metadata()
            .map_err(|error| format!("could not inspect APK: {error}"))?
            .len(),
    )
    .map_err(|_| "APK length exceeds addressable size".to_owned())?;
    if length == 0 || length > MAX_APK_SIZE {
        return Err(format!("APK is outside the 1..={MAX_APK_SIZE} byte cap"));
    }
    let mut bytes = Vec::with_capacity(length);
    Read::by_ref(&mut file)
        .take((MAX_APK_SIZE + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|error| format!("could not read APK: {error}"))?;
    if bytes.len() != length {
        return Err("APK changed size while its descriptor was read".to_owned());
    }
    Ok(bytes)
}

fn identity_sha256(base: &[u8], splits: &[Vec<u8>]) -> String {
    if splits.is_empty() {
        return format!("{:x}", Sha256::digest(base));
    }
    let mut digest = Sha256::new();
    digest.update((base.len() as u64).to_le_bytes());
    digest.update(base);
    for split in splits {
        digest.update((split.len() as u64).to_le_bytes());
        digest.update(split);
    }
    format!("{:x}", digest.finalize())
}

fn write_sealed_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|error| format!("could not create {}: {error}", path.display()))?;
    file.write_all(bytes)
        .and_then(|()| file.sync_all())
        .map_err(|error| format!("could not persist {}: {error}", path.display()))?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o400))
        .map_err(|error| format!("could not seal {}: {error}", path.display()))
}

fn contract(request: &InstallRequest, apk_sha256: &str) -> String {
    if request.splits.is_empty() {
        return format!(
            "{INSTALL_VERSION}\npackage={}\nversion-code={}\napk-sha256={apk_sha256}\nnative-root={}\nruntime-abi={}\n",
            request.package,
            request.version_code,
            request.native_root.as_deref().unwrap_or("none"),
            request.runtime_abi,
        );
    }
    format!(
        "{INSTALL_VERSION}\npackage={}\nversion-code={}\napk-sha256={apk_sha256}\nsplit-count={}\nnative-root={}\nruntime-abi={}\n",
        request.package,
        request.version_code,
        request.splits.len(),
        request.native_root.as_deref().unwrap_or("none"),
        request.runtime_abi,
    )
}

fn validate_existing(
    request: &InstallRequest,
    apk_sha256: &str,
    destination: &Path,
) -> Result<InstalledApk, String> {
    let contract_bytes = fs::read(destination.join("install.contract"))
        .map_err(|error| format!("existing installation contract is unreadable: {error}"))?;
    if contract_bytes != contract(request, apk_sha256).as_bytes() {
        return Err("existing installation contract does not match request".to_owned());
    }
    let existing_apk = read_apk(&destination.join("base.apk"))?;
    let existing_splits = (0..request.splits.len())
        .map(|index| read_apk(&destination.join(format!("split-{index}.apk"))))
        .collect::<Result<Vec<_>, _>>()?;
    if identity_sha256(&existing_apk, &existing_splits) != apk_sha256 {
        return Err("existing installed APK hash is corrupt".to_owned());
    }
    installed(
        request,
        apk_sha256.to_owned(),
        destination.to_path_buf(),
        true,
    )
}

fn installed(
    request: &InstallRequest,
    apk_sha256: String,
    destination: PathBuf,
    existing: bool,
) -> Result<InstalledApk, String> {
    let base_apk = destination.join("base.apk");
    let native_root = request
        .native_root
        .as_ref()
        .map(|root| destination.join("android-elf").join("arm64-v8a").join(root));
    let split_apks = (0..request.splits.len())
        .map(|index| destination.join(format!("split-{index}.apk")))
        .collect::<Vec<_>>();
    if !base_apk.is_file()
        || split_apks.iter().any(|path| !path.is_file())
        || native_root.as_ref().is_some_and(|path| !path.is_file())
    {
        return Err("published installation is incomplete".to_owned());
    }
    Ok(InstalledApk {
        apk_sha256,
        base_apk,
        native_root,
        existing,
    })
}

struct StageGuard(Option<PathBuf>);

impl StageGuard {
    fn new(parent: &Path) -> Result<Self, String> {
        for attempt in 0..128_u32 {
            let path = parent.join(format!(
                ".darwin-art-install.{}.{}",
                std::process::id(),
                attempt
            ));
            let mut builder = DirBuilder::new();
            builder.mode(0o700);
            match builder.create(&path) {
                Ok(()) => return Ok(Self(Some(path))),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(format!("could not create install staging: {error}")),
            }
        }
        Err("could not allocate install staging directory".to_owned())
    }

    fn path(&self) -> &Path {
        self.0.as_deref().expect("live stage has a path")
    }

    fn disarm(&mut self) {
        self.0 = None;
    }
}

impl Drop for StageGuard {
    fn drop(&mut self) {
        if let Some(path) = &self.0 {
            make_writable(path);
            let _ = fs::remove_dir_all(path);
        }
    }
}

fn make_writable(path: &Path) {
    let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o700));
    if let Ok(entries) = fs::read_dir(path) {
        for entry in entries.flatten() {
            let child = entry.path();
            if child.is_dir() {
                make_writable(&child);
            } else {
                let _ = fs::set_permissions(child, fs::Permissions::from_mode(0o600));
            }
        }
    }
}
