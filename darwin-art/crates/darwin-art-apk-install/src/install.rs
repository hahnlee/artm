use crate::publish;
use sha2::{Digest, Sha256};
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;

const MAX_APK_SIZE: usize = 512 * 1024 * 1024;
const INSTALL_VERSION: &str = "darwin-art-apk-install-v1";

pub struct InstallRequest {
    pub apk: PathBuf,
    pub install_root: PathBuf,
    pub package: String,
    pub version_code: String,
    pub native_root: Option<String>,
    pub extractor: Option<PathBuf>,
    pub runtime_abi: String,
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
    let apk_sha256 = format!("{:x}", Sha256::digest(&apk));
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

    if let Some(root) = &request.native_root {
        let extractor = request
            .extractor
            .as_ref()
            .ok_or_else(|| "native APK installation requires an extractor".to_owned())?;
        let native_parent = stage.path().join("android-elf");
        fs::create_dir(&native_parent)
            .map_err(|error| format!("could not create native parent: {error}"))?;
        let native_directory = native_parent.join("arm64-v8a");
        let output = Command::new(extractor)
            .arg(&base_apk)
            .arg(&native_directory)
            .arg(root)
            .output()
            .map_err(|error| format!("could not run native extractor: {error}"))?;
        if !output.status.success() {
            return Err(format!(
                "native extraction failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ));
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
    format!(
        "{INSTALL_VERSION}\npackage={}\nversion-code={}\napk-sha256={apk_sha256}\nnative-root={}\nruntime-abi={}\n",
        request.package,
        request.version_code,
        request.native_root.as_deref().unwrap_or("none"),
        request.runtime_abi
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
    if format!("{:x}", Sha256::digest(&existing_apk)) != apk_sha256 {
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
    if !base_apk.is_file() || native_root.as_ref().is_some_and(|path| !path.is_file()) {
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
