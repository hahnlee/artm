use crate::filesystem::ProfileFilesystem;
use crate::registry::PackageRegistry;
use crate::{ProfileError, ProfilePaths, protocol, write_path};
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStringExt;
use std::os::unix::fs::{FileTypeExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

pub struct DaemonConfig {
    pub paths: ProfilePaths,
    pub idle_timeout: Duration,
}

struct State {
    filesystem: Mutex<ProfileFilesystem>,
    paths: ProfilePaths,
    registry: Mutex<PackageRegistry>,
    processes: Mutex<BTreeMap<u32, ProcessEntry>>,
    lease_gate: Mutex<()>,
    leases: AtomicUsize,
    handlers: AtomicUsize,
    shutdown: AtomicBool,
    last_activity: Mutex<Instant>,
}

struct ProcessEntry {
    package: String,
    leases: usize,
}

struct HandlerGuard(Arc<State>);
struct LeaseGuard {
    state: Arc<State>,
    pid: Option<u32>,
}

impl Drop for HandlerGuard {
    fn drop(&mut self) {
        self.0.handlers.fetch_sub(1, Ordering::SeqCst);
        *self.0.last_activity.lock().unwrap() = Instant::now();
    }
}

impl Drop for LeaseGuard {
    fn drop(&mut self) {
        self.state.leases.fetch_sub(1, Ordering::SeqCst);
        if let Some(pid) = self.pid {
            let mut processes = self.state.processes.lock().unwrap();
            if let Some(process) = processes.get_mut(&pid) {
                process.leases -= 1;
                if process.leases == 0 {
                    processes.remove(&pid);
                }
            }
        }
    }
}

pub fn run_daemon(config: DaemonConfig) -> Result<(), ProfileError> {
    fs::create_dir_all(&config.paths.profile_root)?;
    fs::set_permissions(
        &config.paths.profile_root,
        fs::Permissions::from_mode(0o700),
    )?;
    let _lock = acquire_lock(&config.paths)?;
    if config.paths.socket.as_os_str().as_encoded_bytes().len() >= 104 {
        return Err(ProfileError::Daemon(format!(
            "control socket path exceeds macOS's 103-byte limit; shorten {}",
            config.paths.profiles_root.display()
        )));
    }
    remove_stale_socket(&config.paths)?;
    let listener = UnixListener::bind(&config.paths.socket)?;
    fs::set_permissions(&config.paths.socket, fs::Permissions::from_mode(0o600))?;
    listener.set_nonblocking(true)?;
    let state = Arc::new(State {
        filesystem: Mutex::new(ProfileFilesystem::new(config.paths.clone())),
        paths: config.paths.clone(),
        registry: Mutex::new(PackageRegistry::new(&config.paths)),
        processes: Mutex::new(BTreeMap::new()),
        lease_gate: Mutex::new(()),
        leases: AtomicUsize::new(0),
        handlers: AtomicUsize::new(0),
        shutdown: AtomicBool::new(false),
        last_activity: Mutex::new(Instant::now()),
    });
    while !state.shutdown.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, _)) => {
                // Darwin inherits O_NONBLOCK from the listening socket. Client
                // streams are blocking so an acquire handler remains a lease
                // until the peer closes instead of treating EAGAIN as EOF.
                stream.set_nonblocking(false)?;
                state.handlers.fetch_add(1, Ordering::SeqCst);
                let state = Arc::clone(&state);
                thread::spawn(move || {
                    let _guard = HandlerGuard(Arc::clone(&state));
                    if let Err(error) = handle(stream, &state) {
                        eprintln!("darwin-artd: client error: {error}");
                    }
                });
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                if state.leases.load(Ordering::SeqCst) == 0
                    && state.handlers.load(Ordering::SeqCst) == 0
                    && state.last_activity.lock().unwrap().elapsed() >= config.idle_timeout
                {
                    break;
                }
                thread::sleep(Duration::from_millis(50));
            }
            Err(error) => return Err(error.into()),
        }
    }
    drop(listener);
    while state.handlers.load(Ordering::SeqCst) != 0 {
        thread::sleep(Duration::from_millis(10));
    }
    state.filesystem.lock().unwrap().detach()?;
    let _ = fs::remove_file(&config.paths.socket);
    Ok(())
}

fn handle(mut stream: UnixStream, state: &Arc<State>) -> Result<(), ProfileError> {
    verify_same_user(&stream)?;
    let message = protocol::read_message(&mut stream)?;
    match message.operation {
        protocol::OP_ENSURE => {
            require_empty(&message.payload)?;
            let filesystem = state.filesystem.lock().unwrap();
            match filesystem.ensure() {
                Ok(path) => protocol::write_response(
                    &mut stream,
                    message.operation,
                    0,
                    write_path(path.as_os_str()),
                )?,
                Err(error) => protocol::write_response(
                    &mut stream,
                    message.operation,
                    5,
                    error.to_string().as_bytes(),
                )?,
            }
        }
        protocol::OP_ACQUIRE => {
            let identity = parse_process_identity(&message.payload)?;
            let gate = state.lease_gate.lock().unwrap();
            if state.shutdown.load(Ordering::SeqCst) {
                protocol::write_response(
                    &mut stream,
                    message.operation,
                    108,
                    b"daemon is shutting down",
                )?;
                return Ok(());
            }
            if !state.filesystem.lock().unwrap().is_mounted()? {
                protocol::write_response(
                    &mut stream,
                    message.operation,
                    2,
                    b"profile is not ready",
                )?;
                return Ok(());
            }
            state.leases.fetch_add(1, Ordering::SeqCst);
            if let Some((pid, package)) = &identity {
                let mut processes = state.processes.lock().unwrap();
                let process = processes.entry(*pid).or_insert_with(|| ProcessEntry {
                    package: package.clone(),
                    leases: 0,
                });
                if process.package != *package {
                    state.leases.fetch_sub(1, Ordering::SeqCst);
                    protocol::write_response(
                        &mut stream,
                        message.operation,
                        22,
                        b"PID is already registered to another package",
                    )?;
                    return Ok(());
                }
                process.leases += 1;
            }
            let _lease = LeaseGuard {
                state: Arc::clone(state),
                pid: identity.map(|(pid, _)| pid),
            };
            drop(gate);
            protocol::write_response(&mut stream, message.operation, 0, b"")?;
            let mut byte = [0_u8; 1];
            while stream.read(&mut byte).is_ok_and(|count| count != 0) {}
        }
        protocol::OP_STATUS => {
            require_empty(&message.payload)?;
            let mounted = state.filesystem.lock().unwrap().is_mounted()?;
            let status = format!(
                "profile={} mounted={} leases={}",
                state.paths.profile_id,
                mounted,
                state.leases.load(Ordering::SeqCst)
            );
            protocol::write_response(&mut stream, message.operation, 0, status.as_bytes())?;
        }
        protocol::OP_SHUTDOWN => {
            require_empty(&message.payload)?;
            let _gate = state.lease_gate.lock().unwrap();
            let leases = state.leases.load(Ordering::SeqCst);
            if leases == 0 {
                protocol::write_response(&mut stream, message.operation, 0, b"")?;
                state.shutdown.store(true, Ordering::SeqCst);
            } else {
                protocol::write_response(
                    &mut stream,
                    message.operation,
                    16,
                    format!("{leases} active lease(s)").as_bytes(),
                )?;
            }
        }
        protocol::OP_REGISTER => {
            let separator = message
                .payload
                .iter()
                .position(|byte| *byte == 0)
                .ok_or_else(|| ProfileError::Daemon("register request has no package".into()))?;
            let package = std::str::from_utf8(&message.payload[..separator])
                .map_err(|_| ProfileError::Daemon("package is not UTF-8".into()))?;
            let record = &message.payload[separator + 1..];
            state.filesystem.lock().unwrap().ensure()?;
            state.registry.lock().unwrap().register(package, record)?;
            protocol::write_response(&mut stream, message.operation, 0, b"")?;
        }
        protocol::OP_RESOLVE => {
            let package = std::str::from_utf8(&message.payload)
                .map_err(|_| ProfileError::Daemon("package is not UTF-8".into()))?;
            state.filesystem.lock().unwrap().ensure()?;
            let record = state.registry.lock().unwrap().resolve(package)?;
            protocol::write_response(&mut stream, message.operation, 0, &record)?;
        }
        protocol::OP_LIST => {
            require_empty(&message.payload)?;
            state.filesystem.lock().unwrap().ensure()?;
            let packages = state.registry.lock().unwrap().list()?;
            protocol::write_response(&mut stream, message.operation, 0, &packages)?;
        }
        protocol::OP_PROCESSES => {
            require_empty(&message.payload)?;
            let mut processes = state
                .processes
                .lock()
                .unwrap()
                .iter()
                .map(|(pid, process)| format!("{pid}\t{}", process.package))
                .collect::<Vec<_>>()
                .join("\n");
            if !processes.is_empty() {
                processes.push('\n');
            }
            protocol::write_response(&mut stream, message.operation, 0, processes.as_bytes())?;
        }
        protocol::OP_UNREGISTER => {
            if message.payload.len() < 2 || message.payload[0] > 1 {
                protocol::write_response(
                    &mut stream,
                    message.operation,
                    22,
                    b"unregister requires a data policy and package",
                )?;
                return Ok(());
            }
            let remove_data = message.payload[0] == 1;
            let package = std::str::from_utf8(&message.payload[1..])
                .map_err(|_| ProfileError::Daemon("package is not UTF-8".into()))?;
            crate::registry::validate_package(package)?;
            let _gate = state.lease_gate.lock().unwrap();
            if state
                .processes
                .lock()
                .unwrap()
                .values()
                .any(|process| process.package == package)
            {
                protocol::write_response(
                    &mut stream,
                    message.operation,
                    16,
                    b"package is running",
                )?;
                return Ok(());
            }
            state.filesystem.lock().unwrap().ensure()?;
            let registry = state.registry.lock().unwrap();
            match unregister_package_files(&state.paths, &registry, package, remove_data) {
                Ok(()) => protocol::write_response(&mut stream, message.operation, 0, b"")?,
                Err(error) => protocol::write_response(
                    &mut stream,
                    message.operation,
                    5,
                    error.to_string().as_bytes(),
                )?,
            }
        }
        protocol::OP_DAEMONIZE => {
            let (package, arguments, environment) = parse_daemonize(&message.payload)?;
            crate::registry::validate_package(&package)?;
            state.filesystem.lock().unwrap().ensure()?;
            let log_path = environment
                .iter()
                .find(|(key, _)| key.to_str() == Some("DARWIN_ART_DAEMONIZED_LOG"))
                .map(|(_, value)| std::path::PathBuf::from(value));
            let mut command = Command::new(&arguments[0]);
            command
                .args(&arguments[1..])
                .env_clear()
                .envs(environment)
                .stdin(Stdio::null());
            if let Some(log_path) = log_path {
                let log = OpenOptions::new()
                    .create(true)
                    .append(true)
                    .mode(0o600)
                    .open(log_path)?;
                command.stdout(Stdio::from(log.try_clone()?));
                command.stderr(Stdio::from(log));
            }
            let mut child = command.spawn()?;
            let pid = child.id();
            {
                let _gate = state.lease_gate.lock().unwrap();
                state.leases.fetch_add(1, Ordering::SeqCst);
                state
                    .processes
                    .lock()
                    .unwrap()
                    .insert(pid, ProcessEntry { package, leases: 1 });
            }
            let owner = Arc::clone(state);
            thread::spawn(move || {
                let _ = child.wait();
                let _gate = owner.lease_gate.lock().unwrap();
                owner.processes.lock().unwrap().remove(&pid);
                owner.leases.fetch_sub(1, Ordering::SeqCst);
                *owner.last_activity.lock().unwrap() = Instant::now();
            });
            protocol::write_response(&mut stream, message.operation, 0, &pid.to_le_bytes())?;
        }
        _ => protocol::write_response(&mut stream, message.operation, 38, b"unknown operation")?,
    }
    Ok(())
}

fn unregister_package_files(
    paths: &ProfilePaths,
    registry: &PackageRegistry,
    package: &str,
    remove_data: bool,
) -> Result<(), ProfileError> {
    registry.resolve(package)?;
    let trash = paths.mount.join("run").join(format!(
        ".uninstall.{}.{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
    ));
    fs::create_dir(&trash)?;
    let mut moved: Vec<(std::path::PathBuf, std::path::PathBuf)> = Vec::new();
    let mut candidates = vec![
        (
            paths.mount.join("packages").join(package),
            trash.join("package"),
        ),
        (
            paths.mount.join("system/package-code").join(package),
            trash.join("code"),
        ),
    ];
    if remove_data {
        candidates.push((
            paths.mount.join("data/apps").join(package),
            trash.join("data"),
        ));
    }
    for (source, destination) in candidates {
        if !source.exists() {
            continue;
        }
        if let Err(error) = fs::rename(&source, &destination) {
            for (restore_from, restore_to) in moved.into_iter().rev() {
                let _ = fs::rename(restore_from, restore_to);
            }
            let _ = fs::remove_dir(&trash);
            return Err(error.into());
        }
        moved.push((destination, source));
    }
    if let Err(error) = registry.unregister(package) {
        for (restore_from, restore_to) in moved.into_iter().rev() {
            let _ = fs::rename(restore_from, restore_to);
        }
        let _ = fs::remove_dir(&trash);
        return Err(error);
    }
    if make_tree_removable(&trash).is_ok() {
        let _ = fs::remove_dir_all(&trash);
    }
    Ok(())
}

fn make_tree_removable(path: &std::path::Path) -> Result<(), ProfileError> {
    let metadata = fs::symlink_metadata(path)?;
    if metadata.file_type().is_symlink() {
        return Ok(());
    }
    if metadata.is_dir() {
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
        for entry in fs::read_dir(path)? {
            make_tree_removable(&entry?.path())?;
        }
    } else {
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}

fn parse_daemonize(
    payload: &[u8],
) -> Result<
    (
        String,
        Vec<std::ffi::OsString>,
        Vec<(std::ffi::OsString, std::ffi::OsString)>,
    ),
    ProfileError,
> {
    let mut cursor = 0_usize;
    let package = String::from_utf8(read_field(payload, &mut cursor)?.to_vec())
        .map_err(|_| ProfileError::Daemon("daemonize package is not UTF-8".into()))?;
    let argument_count = read_u32(payload, &mut cursor)? as usize;
    if argument_count == 0 || argument_count > 64 {
        return Err(ProfileError::Daemon(
            "invalid daemonize argument count".into(),
        ));
    }
    let mut arguments = Vec::with_capacity(argument_count);
    for _ in 0..argument_count {
        arguments.push(std::ffi::OsString::from_vec(
            read_field(payload, &mut cursor)?.to_vec(),
        ));
    }
    let environment_count = read_u32(payload, &mut cursor)? as usize;
    if environment_count > 256 {
        return Err(ProfileError::Daemon(
            "invalid daemonize environment count".into(),
        ));
    }
    let mut environment = Vec::with_capacity(environment_count);
    for _ in 0..environment_count {
        let key = std::ffi::OsString::from_vec(read_field(payload, &mut cursor)?.to_vec());
        let value = std::ffi::OsString::from_vec(read_field(payload, &mut cursor)?.to_vec());
        environment.push((key, value));
    }
    if cursor != payload.len() {
        return Err(ProfileError::Daemon("trailing daemonize payload".into()));
    }
    Ok((package, arguments, environment))
}

fn read_u32(payload: &[u8], cursor: &mut usize) -> Result<u32, ProfileError> {
    let end = cursor
        .checked_add(4)
        .filter(|end| *end <= payload.len())
        .ok_or_else(|| ProfileError::Daemon("truncated daemonize payload".into()))?;
    let value = u32::from_le_bytes(payload[*cursor..end].try_into().unwrap());
    *cursor = end;
    Ok(value)
}

fn read_field<'a>(payload: &'a [u8], cursor: &mut usize) -> Result<&'a [u8], ProfileError> {
    let length = read_u32(payload, cursor)? as usize;
    let end = cursor
        .checked_add(length)
        .filter(|end| *end <= payload.len())
        .ok_or_else(|| ProfileError::Daemon("truncated daemonize field".into()))?;
    let field = &payload[*cursor..end];
    if field.contains(&0) {
        return Err(ProfileError::Daemon("NUL in daemonize field".into()));
    }
    *cursor = end;
    Ok(field)
}

fn require_empty(payload: &[u8]) -> Result<(), ProfileError> {
    if payload.is_empty() {
        Ok(())
    } else {
        Err(ProfileError::Daemon("unexpected request payload".into()))
    }
}

fn parse_process_identity(payload: &[u8]) -> Result<Option<(u32, String)>, ProfileError> {
    if payload.is_empty() {
        return Ok(None);
    }
    if payload.len() < 5 {
        return Err(ProfileError::Daemon("process identity is truncated".into()));
    }
    let pid = u32::from_le_bytes(payload[..4].try_into().unwrap());
    if pid == 0 {
        return Err(ProfileError::Daemon("process PID must be non-zero".into()));
    }
    let package = std::str::from_utf8(&payload[4..])
        .map_err(|_| ProfileError::Daemon("package is not UTF-8".into()))?;
    crate::registry::validate_package(package)?;
    Ok(Some((pid, package.to_owned())))
}

fn acquire_lock(paths: &ProfilePaths) -> Result<File, ProfileError> {
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .mode(0o600)
        .open(paths.profile_root.join("darwin-artd.lock"))?;
    let result = unsafe { flock(lock.as_raw_fd(), LOCK_EX | LOCK_NB) };
    if result != 0 {
        return Err(ProfileError::Daemon(
            "profile daemon is already running".into(),
        ));
    }
    Ok(lock)
}

fn remove_stale_socket(paths: &ProfilePaths) -> Result<(), ProfileError> {
    match fs::symlink_metadata(&paths.socket) {
        Ok(metadata) if metadata.file_type().is_socket() => fs::remove_file(&paths.socket)?,
        Ok(_) => {
            return Err(ProfileError::Daemon(format!(
                "control socket path is not a socket: {}",
                paths.socket.display()
            )));
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error.into()),
    }
    Ok(())
}

fn verify_same_user(stream: &UnixStream) -> Result<(), ProfileError> {
    let mut uid = 0_u32;
    let mut gid = 0_u32;
    if unsafe { getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) } != 0
        || uid != unsafe { geteuid() }
    {
        return Err(ProfileError::Daemon(
            "client uid does not own this daemon".into(),
        ));
    }
    Ok(())
}

const LOCK_EX: i32 = 2;
const LOCK_NB: i32 = 4;

unsafe extern "C" {
    fn flock(fd: i32, operation: i32) -> i32;
    fn getpeereid(socket: i32, effective_user: *mut u32, effective_group: *mut u32) -> i32;
    fn geteuid() -> u32;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::ffi::OsStrExt;

    fn temporary_paths(test: &str) -> ProfilePaths {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        ProfilePaths::new(
            std::env::temp_dir().join(format!("darwin-art-profile-{test}-{nonce}")),
            "test",
        )
        .unwrap()
    }

    fn field(payload: &mut Vec<u8>, value: &[u8]) {
        payload.extend_from_slice(&(value.len() as u32).to_le_bytes());
        payload.extend_from_slice(value);
    }

    #[test]
    fn daemonize_payload_preserves_argv_and_environment_bytes() {
        let mut payload = Vec::new();
        field(&mut payload, b"android.system");
        payload.extend_from_slice(&2_u32.to_le_bytes());
        field(&mut payload, b"/runtime host");
        field(&mut payload, b"--window-seconds=0");
        payload.extend_from_slice(&1_u32.to_le_bytes());
        field(&mut payload, b"DARWIN_ART_MODE");
        field(&mut payload, b"system server");

        let (package, arguments, environment) = parse_daemonize(&payload).unwrap();
        assert_eq!(package, "android.system");
        assert_eq!(arguments[0].as_bytes(), b"/runtime host");
        assert_eq!(arguments[1].as_bytes(), b"--window-seconds=0");
        assert_eq!(environment[0].0.as_bytes(), b"DARWIN_ART_MODE");
        assert_eq!(environment[0].1.as_bytes(), b"system server");
    }

    #[test]
    fn daemonize_payload_rejects_nul_and_trailing_bytes() {
        let mut payload = Vec::new();
        field(&mut payload, b"android.system");
        payload.extend_from_slice(&1_u32.to_le_bytes());
        field(&mut payload, b"bad\0program");
        payload.extend_from_slice(&0_u32.to_le_bytes());
        assert!(parse_daemonize(&payload).is_err());

        let mut valid = Vec::new();
        field(&mut valid, b"android.system");
        valid.extend_from_slice(&1_u32.to_le_bytes());
        field(&mut valid, b"/runtime");
        valid.extend_from_slice(&0_u32.to_le_bytes());
        valid.push(0);
        assert!(parse_daemonize(&valid).is_err());
    }

    #[test]
    fn unregister_removes_code_and_honors_the_data_policy() {
        let paths = temporary_paths("unregister");
        let package = "com.example.app";
        fs::create_dir_all(paths.mount.join("run")).unwrap();
        fs::create_dir_all(paths.mount.join("packages").join(package)).unwrap();
        fs::create_dir_all(paths.mount.join("system/package-code").join(package)).unwrap();
        fs::create_dir_all(paths.mount.join("data/apps").join(package)).unwrap();
        let registry = PackageRegistry::new(&paths);
        registry
            .register(package, b"darwin-art-launch-v1\npackage=com.example.app\n")
            .unwrap();

        unregister_package_files(&paths, &registry, package, false).unwrap();

        assert!(registry.resolve(package).is_err());
        assert!(!paths.mount.join("packages").join(package).exists());
        assert!(
            !paths
                .mount
                .join("system/package-code")
                .join(package)
                .exists()
        );
        assert!(paths.mount.join("data/apps").join(package).exists());

        fs::create_dir_all(paths.mount.join("packages").join(package)).unwrap();
        registry
            .register(package, b"darwin-art-launch-v1\npackage=com.example.app\n")
            .unwrap();
        unregister_package_files(&paths, &registry, package, true).unwrap();
        assert!(!paths.mount.join("data/apps").join(package).exists());
        fs::remove_dir_all(paths.profiles_root).unwrap();
    }
}
