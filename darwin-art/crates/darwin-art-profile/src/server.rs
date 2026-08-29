use crate::filesystem::ProfileFilesystem;
use crate::registry::PackageRegistry;
use crate::{ProfileError, ProfilePaths, protocol, write_path};
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{FileTypeExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
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
        _ => protocol::write_response(&mut stream, message.operation, 38, b"unknown operation")?,
    }
    Ok(())
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
