#![forbid(unsafe_op_in_unsafe_fn)]

use darwin_art_fs_broker::{BrokerError, ReadOnlyBroker};
use darwin_art_prefix::{MountKind, MountTable, PrefixError, Resolution};
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt::Write as _;
use std::fs::{self, File, Metadata, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, IntoRawFd};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::FileExt;
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;
use std::ptr;
use std::rc::Rc;
use std::slice;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, LazyLock, Mutex, MutexGuard};

const AT_FDCWD: c_int = -100;
const O_ACCMODE: c_int = 3;
const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_RDWR: c_int = 2;
const O_CREAT: c_int = 64;
const O_EXCL: c_int = 128;
const O_TRUNC: c_int = 512;
const O_APPEND: c_int = 1024;
const O_NONBLOCK: c_int = 2048;
const O_DSYNC: c_int = 4096;
const O_DIRECTORY: c_int = 16384;
const O_NOFOLLOW: c_int = 32768;
const O_LARGEFILE: c_int = 131072;
const O_CLOEXEC: c_int = 524288;
const O_SYNC: c_int = 1_052_672;
const O_TMPFILE: c_int = 4_210_688;
const ACCEPTED_FLAGS: c_int = O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_LARGEFILE | O_CLOEXEC;
const WRITE_FLAGS: c_int = O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DSYNC | O_SYNC;

const ANDROID_EBADF: i32 = 9;
const ANDROID_EACCES: i32 = 13;
const ANDROID_EFAULT: i32 = 14;
const ANDROID_EIO: i32 = 5;
const ANDROID_ENOENT: i32 = 2;
const ANDROID_EEXIST: i32 = 17;
const ANDROID_ENOTDIR: i32 = 20;
const ANDROID_EISDIR: i32 = 21;
const ANDROID_EINVAL: i32 = 22;
const ANDROID_EMFILE: i32 = 24;
const ANDROID_ENOTTY: i32 = 25;
const ANDROID_EROFS: i32 = 30;
const ANDROID_ERANGE: i32 = 34;
const ANDROID_EOPNOTSUPP: i32 = 95;
const DARWIN_ELOOP: i32 = 62;
const AT_SYMLINK_NOFOLLOW: c_int = 0x100;
const AT_REMOVEDIR: c_int = 0x200;
const ANDROID_ST_RDONLY: u64 = 0x0001;
const ANDROID_S_IFCHR: u32 = 0o020000;
const ANDROID_S_IFDIR: u32 = 0o040000;
const ANDROID_S_IFREG: u32 = 0o100000;
const ANDROID_RANDOM_MODE: u32 = ANDROID_S_IFCHR | 0o666;
const ANDROID_RANDOM_RDEV: u64 = 0x108;
const ANDROID_URANDOM_RDEV: u64 = 0x109;
const PATHCONF_MIN: c_int = 0;
const PATHCONF_MAX: c_int = 19;

unsafe extern "C" {
    fn darwin_art_bionic_errno_store(android_errno: i32);
    fn darwin_art_bionic_errno_set_from_darwin(darwin_errno: i32) -> c_int;
    #[link_name = "close"]
    fn host_close(fd: c_int) -> c_int;
    #[link_name = "flock"]
    fn host_flock(fd: c_int, operation: c_int) -> c_int;
    #[link_name = "fcntl"]
    fn host_fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn darwin_art_bionic_fs_host_record_lock(
        host_fd: c_int,
        android_command: c_int,
        android_lock: isize,
        host_errno: *mut c_int,
    ) -> c_int;
    fn darwin_art_bionic_fs_host_fdopendir(fd: c_int, host_errno: *mut c_int) -> *mut c_void;
    fn darwin_art_bionic_fs_host_readdir(
        directory: *mut c_void,
        entry: *mut HostDirent,
        host_errno: *mut c_int,
    ) -> c_int;
    fn darwin_art_bionic_fs_host_rewinddir(directory: *mut c_void);
    fn darwin_art_bionic_fs_host_closedir(directory: *mut c_void, host_errno: *mut c_int) -> c_int;
    fn darwin_art_bionic_fs_host_fpathconf(
        fd: c_int,
        semantic_name: c_int,
        value: *mut i64,
        host_errno: *mut c_int,
    ) -> c_int;
    fn darwin_art_bionic_fs_host_fstatvfs(
        fd: c_int,
        status: *mut HostStatvfs,
        host_errno: *mut c_int,
    ) -> c_int;
    #[link_name = "kSecRandomDefault"]
    static SEC_RANDOM_DEFAULT: *const c_void;
    #[link_name = "SecRandomCopyBytes"]
    fn sec_random_copy_bytes(random: *const c_void, count: usize, bytes: *mut u8) -> c_int;
    fn darwin_art_bionic_fs_host_enumerate_regions(
        callback: unsafe extern "C" fn(*mut c_void, u64, u64, c_int) -> c_int,
        context: *mut c_void,
    ) -> c_int;
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct AndroidTimespec {
    pub tv_sec: i64,
    pub tv_nsec: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct AndroidStat {
    pub st_dev: u64,
    pub st_ino: u64,
    pub st_mode: u32,
    pub st_nlink: u32,
    pub st_uid: u32,
    pub st_gid: u32,
    pub st_rdev: u64,
    pub pad1: u64,
    pub st_size: i64,
    pub st_blksize: i32,
    pub pad2: i32,
    pub st_blocks: i64,
    pub st_atim: AndroidTimespec,
    pub st_mtim: AndroidTimespec,
    pub st_ctim: AndroidTimespec,
    pub unused4: u32,
    pub unused5: u32,
}

const _: () = assert!(size_of::<AndroidStat>() == 128);
const _: () = assert!(size_of::<AndroidTimespec>() == 16);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AndroidDirent {
    pub d_ino: u64,
    pub d_off: i64,
    pub d_reclen: u16,
    pub d_type: u8,
    pub d_name: [u8; 256],
}

impl Default for AndroidDirent {
    fn default() -> Self {
        Self {
            d_ino: 0,
            d_off: 0,
            d_reclen: 0,
            d_type: 0,
            d_name: [0; 256],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct HostDirent {
    d_ino: u64,
    d_name_length: u16,
    d_type: u8,
    d_name: [u8; 256],
}

impl Default for HostDirent {
    fn default() -> Self {
        Self {
            d_ino: 0,
            d_name_length: 0,
            d_type: 0,
            d_name: [0; 256],
        }
    }
}

const _: () = assert!(size_of::<AndroidDirent>() == 280);
const _: () = assert!(std::mem::offset_of!(AndroidDirent, d_name) == 19);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct AndroidStatvfs {
    pub f_bsize: u64,
    pub f_frsize: u64,
    pub f_blocks: u64,
    pub f_bfree: u64,
    pub f_bavail: u64,
    pub f_files: u64,
    pub f_ffree: u64,
    pub f_favail: u64,
    pub f_fsid: u64,
    pub f_flag: u64,
    pub f_namemax: u64,
    pub reserved: [u32; 6],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct HostStatvfs {
    f_bsize: u64,
    f_frsize: u64,
    f_blocks: u64,
    f_bfree: u64,
    f_bavail: u64,
    f_files: u64,
    f_ffree: u64,
    f_favail: u64,
    f_fsid: u64,
    f_flag: u64,
    f_namemax: u64,
}

const _: () = assert!(size_of::<AndroidStatvfs>() == 112);
const _: () = assert!(std::mem::offset_of!(AndroidStatvfs, f_flag) == 72);
const _: () = assert!(size_of::<HostStatvfs>() == 88);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RandomDeviceKind {
    Random,
    Urandom,
}

enum Descriptor {
    File(File),
    PrivateFile(File),
    Random(RandomDeviceKind),
    Overlay(OverlayDescriptor),
}

struct OverlayFile {
    inode: u64,
    mode: u32,
    data: Vec<u8>,
}

#[derive(Clone, Copy)]
struct OverlayDirectory {
    inode: u64,
    mode: u32,
}

enum OverlayEntry {
    File(Arc<Mutex<OverlayFile>>),
    Directory(OverlayDirectory),
}

struct OverlayState {
    entries: BTreeMap<Vec<u8>, OverlayEntry>,
    next_inode: u64,
}

impl Default for OverlayState {
    fn default() -> Self {
        let mut entries = BTreeMap::new();
        entries.insert(
            Vec::new(),
            OverlayEntry::Directory(OverlayDirectory {
                inode: 1,
                mode: ANDROID_S_IFDIR | 0o700,
            }),
        );
        Self {
            entries,
            next_inode: 2,
        }
    }
}

struct OverlayDescriptor {
    node: Arc<Mutex<OverlayFile>>,
    offset: u64,
    readable: bool,
    writable: bool,
}

struct DescriptorTable {
    next: c_int,
    free: Vec<c_int>,
    entries: BTreeMap<c_int, Descriptor>,
}

const CENTRAL_BROKER_TOKEN_MARKER: c_int = 0x4000_0000;

impl Default for DescriptorTable {
    fn default() -> Self {
        Self {
            next: 10_000,
            free: Vec::new(),
            entries: BTreeMap::new(),
        }
    }
}

impl DescriptorTable {
    fn insert(&mut self, descriptor: Descriptor) -> Result<c_int, ()> {
        if let Some(fd) = self.free.pop() {
            assert!(self.entries.insert(fd, descriptor).is_none());
            return Ok(fd);
        }
        for _ in 0..100_000 {
            let candidate = self.next;
            self.next = if self.next >= CENTRAL_BROKER_TOKEN_MARKER - 1 {
                10_000
            } else {
                self.next + 1
            };
            if candidate & CENTRAL_BROKER_TOKEN_MARKER != 0 {
                continue;
            }
            if let std::collections::btree_map::Entry::Vacant(entry) = self.entries.entry(candidate)
            {
                entry.insert(descriptor);
                return Ok(candidate);
            }
        }
        Err(())
    }

    fn close_entry(&mut self, fd: c_int) -> Option<Descriptor> {
        let descriptor = self.entries.remove(&fd)?;
        self.free.push(fd);
        Some(descriptor)
    }

    fn take(&mut self, fd: c_int) -> Option<Descriptor> {
        self.entries.remove(&fd)
    }

    fn restore(&mut self, fd: c_int, descriptor: Descriptor) {
        assert!(self.entries.insert(fd, descriptor).is_none());
    }

    fn release(&mut self, fd: c_int) {
        assert!(!self.entries.contains_key(&fd));
        self.free.push(fd);
    }
}

trait EntropyBackend: Send + Sync {
    fn fill(&self, bytes: &mut [u8]) -> Result<(), ()>;
}

struct SecurityEntropy;

impl EntropyBackend for SecurityEntropy {
    fn fill(&self, bytes: &mut [u8]) -> Result<(), ()> {
        if bytes.is_empty() {
            return Ok(());
        }
        // SAFETY: Security.framework accepts this writable slice for its exact
        // length and does not retain it. The default generator is process-owned.
        let status =
            unsafe { sec_random_copy_bytes(SEC_RANDOM_DEFAULT, bytes.len(), bytes.as_mut_ptr()) };
        if status == 0 { Ok(()) } else { Err(()) }
    }
}

#[repr(C)]
struct DirectoryToken {
    opaque_id: u64,
}

struct DirectoryRecord {
    // Token and translated entry are separate allocations containing no host
    // descriptor or DIR pointer. Only their addresses cross the guest ABI.
    _token: Box<DirectoryToken>,
    host_directory: usize,
    offset: i64,
    entry: Box<AndroidDirent>,
}

impl Drop for DirectoryRecord {
    fn drop(&mut self) {
        if self.host_directory != 0 {
            let mut ignored_errno = 0;
            // SAFETY: a nonzero stream is owned by this state until this call.
            unsafe {
                darwin_art_bionic_fs_host_closedir(
                    self.host_directory as *mut c_void,
                    &mut ignored_errno,
                )
            };
            self.host_directory = 0;
        }
    }
}

struct DirectoryTable {
    // Only live streams are retained. POSIX makes DIR* use after closedir
    // undefined, so close can reclaim both facade-owned guest allocations.
    streams: BTreeMap<usize, DirectoryRecord>,
    next_id: u64,
}

impl Default for DirectoryTable {
    fn default() -> Self {
        Self {
            streams: BTreeMap::new(),
            next_id: 1,
        }
    }
}

impl DirectoryTable {
    fn insert(&mut self, host_directory: *mut c_void) -> *mut c_void {
        let opaque_id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        let token = Box::new(DirectoryToken { opaque_id });
        let token_pointer = (&*token as *const DirectoryToken).cast_mut().cast();
        let state = DirectoryRecord {
            _token: token,
            host_directory: host_directory as usize,
            offset: 0,
            entry: Box::new(AndroidDirent::default()),
        };
        self.streams.insert(token_pointer as usize, state);
        token_pointer
    }
}

pub struct Facade {
    prefix: MountTable,
    broker: ReadOnlyBroker,
    cwd: Mutex<Vec<u8>>,
    descriptors: Mutex<DescriptorTable>,
    overlay: Mutex<OverlayState>,
    private_root: Option<PathBuf>,
    directories: Mutex<DirectoryTable>,
    entropy: Arc<dyn EntropyBackend>,
    capability_failure: AtomicBool,
}

impl Facade {
    pub fn new(root: File, guest_mount: &[u8], cwd: &[u8]) -> Result<Self, &'static str> {
        Self::new_with_entropy(root, guest_mount, cwd, Arc::new(SecurityEntropy))
    }

    fn new_with_entropy(
        root: File,
        guest_mount: &[u8],
        cwd: &[u8],
        entropy: Arc<dyn EntropyBackend>,
    ) -> Result<Self, &'static str> {
        if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
            eprintln!(
                "DARWIN FS: install mount={} cwd={}",
                String::from_utf8_lossy(guest_mount),
                String::from_utf8_lossy(cwd)
            );
        }
        let mut prefix = MountTable::new();
        prefix
            .add_mount(1, MountKind::Immutable, false, guest_mount)
            .map_err(|_| "invalid guest mount")?;
        prefix
            .add_mount(2, MountKind::Private, true, b"/data")
            .map_err(|_| "invalid private data mount")?;
        prefix.seal().map_err(|_| "could not seal guest mount")?;
        // Prove cwd belongs to this mount before any guest operation can use it.
        let initial_cwd = prefix
            .resolve(cwd, b".")
            .map_err(|_| "cwd is outside guest mount")?;
        if initial_cwd.mount_id != 1 || initial_cwd.writable {
            return Err("cwd is outside immutable guest mount");
        }
        let broker = ReadOnlyBroker::from_directory(root).map_err(|_| "invalid mount root")?;
        let opened_cwd = broker
            .open(&initial_cwd.relative_path)
            .map_err(|_| "cwd cannot be securely opened")?;
        if !opened_cwd.metadata().is_dir() {
            return Err("cwd is not a directory");
        }
        let private_root = std::env::var_os("DARWIN_ART_ANDROID_PRIVATE_DATA_ROOT")
            .map(PathBuf::from)
            .or_else(|| {
                std::env::var_os("DARWIN_ART_ANDROID_FILESYSTEM_ROOT")
                    .map(PathBuf::from)
                    .map(|root| root.join("data"))
            });
        if let Some(path) = private_root.as_ref()
            && (!path.is_absolute() || !path.is_dir())
        {
            return Err("invalid private data root");
        }
        Ok(Self {
            prefix,
            broker,
            cwd: Mutex::new(initial_cwd.normalized_path),
            descriptors: Mutex::new(DescriptorTable::default()),
            overlay: Mutex::new(OverlayState::default()),
            private_root,
            directories: Mutex::new(DirectoryTable::default()),
            entropy,
            capability_failure: AtomicBool::new(false),
        })
    }

    pub fn activate(self: &Arc<Self>) -> Activation {
        let previous = ACTIVE.with(|active| active.replace(Some(Arc::clone(self))));
        Activation {
            previous,
            _not_send: PhantomData,
        }
    }

    pub fn has_capability_failure(&self) -> bool {
        self.capability_failure.load(Ordering::Acquire)
    }

    fn set_android_errno(value: i32) {
        // SAFETY: the standalone Bionic errno module is linked for the facade lifetime.
        unsafe { darwin_art_bionic_errno_store(value) };
    }

    fn fail(&self, android_errno: i32) -> c_int {
        Self::set_android_errno(android_errno);
        -1
    }

    fn fail_capability(&self) -> c_int {
        self.capability_failure.store(true, Ordering::Release);
        Self::set_android_errno(ANDROID_EIO);
        -1
    }

    fn fail_io(&self, error: &std::io::Error) -> c_int {
        if let Some(raw) = error.raw_os_error() {
            // SAFETY: translation has no pointer arguments and preserves host errno.
            if unsafe { darwin_art_bionic_errno_set_from_darwin(raw) } != 0 {
                return -1;
            }
        }
        self.fail_capability()
    }

    fn fail_broker(&self, error: &BrokerError) -> c_int {
        match error {
            BrokerError::InvalidPath(_) => self.fail(ANDROID_EACCES),
            BrokerError::NotDirectoryRoot => self.fail(ANDROID_ENOTDIR),
            BrokerError::UnsupportedNodeType => self.fail(ANDROID_EOPNOTSUPP),
            BrokerError::Io { source, .. } => self.fail_io(source),
        }
    }

    fn resolve_from(&self, cwd: &[u8], path: &[u8]) -> Result<Resolution, c_int> {
        match self.prefix.resolve(cwd, path) {
            Ok(resolution)
                if (resolution.mount_id == 1 && !resolution.writable)
                    || (resolution.mount_id == 2 && resolution.writable) =>
            {
                Ok(resolution)
            }
            Ok(resolution) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!(
                        "DARWIN FS: rejected mount path={} id={} writable={}",
                        String::from_utf8_lossy(path),
                        resolution.mount_id,
                        resolution.writable
                    );
                }
                Err(ANDROID_EACCES)
            }
            Err(PrefixError::NoMount) => Err(ANDROID_EACCES),
            Err(_) => Err(ANDROID_EINVAL),
        }
    }

    fn resolve(&self, path: &[u8]) -> Result<Resolution, c_int> {
        let cwd = match self.cwd.lock() {
            Ok(cwd) => cwd.clone(),
            Err(_) => {
                self.fail_capability();
                return Err(ANDROID_EIO);
            }
        };
        self.resolve_from(&cwd, path)
    }

    fn validate_immutable_flags(&self, flags: c_int) -> Result<(), c_int> {
        if flags & O_ACCMODE == O_WRONLY
            || flags & O_ACCMODE == O_RDWR
            || flags & WRITE_FLAGS != 0
            || flags & O_TMPFILE == O_TMPFILE
        {
            return Err(ANDROID_EROFS);
        }
        if flags & !ACCEPTED_FLAGS != 0 {
            return Err(ANDROID_EOPNOTSUPP);
        }
        Ok(())
    }

    fn overlay_parent(path: &[u8]) -> &[u8] {
        path.iter()
            .rposition(|byte| *byte == b'/')
            .map_or(b"".as_slice(), |index| &path[..index])
    }

    fn validate_overlay_flags(&self, flags: c_int) -> Result<(), c_int> {
        let access = flags & O_ACCMODE;
        if access != O_RDONLY && access != O_WRONLY {
            return Err(ANDROID_EOPNOTSUPP);
        }
        if flags & (O_APPEND | O_DSYNC | O_SYNC) != 0
            || flags & O_TMPFILE == O_TMPFILE
            || flags & O_DIRECTORY != 0
        {
            return Err(ANDROID_EOPNOTSUPP);
        }
        let accepted =
            O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_NOFOLLOW | O_LARGEFILE | O_CLOEXEC;
        if flags & !accepted != 0 {
            return Err(ANDROID_EOPNOTSUPP);
        }
        if access == O_RDONLY && flags & (O_CREAT | O_EXCL | O_TRUNC) != 0 {
            return Err(ANDROID_EINVAL);
        }
        Ok(())
    }

    fn private_path(&self, relative: &[u8]) -> Result<PathBuf, c_int> {
        let root = self.private_root.as_ref().ok_or(ANDROID_EIO)?;
        if relative.contains(&0)
            || (!relative.is_empty()
                && relative.split(|byte| *byte == b'/').any(|component| {
                    component.is_empty() || component == b"." || component == b".."
                }))
        {
            return Err(ANDROID_EINVAL);
        }
        Ok(root.join(std::ffi::OsString::from_vec(relative.to_vec())))
    }

    fn resolve_private_host_path(&self, path: &[u8]) -> Result<PathBuf, c_int> {
        let resolution = self.resolve(path)?;
        if resolution.mount_id != 2 || !resolution.writable {
            return Err(ANDROID_EACCES);
        }
        self.private_path(&resolution.relative_path)
    }

    fn open_private(&self, resolution: Resolution, flags: c_int, mode: u32) -> c_int {
        let accepted = O_ACCMODE
            | O_CREAT
            | O_EXCL
            | O_TRUNC
            | O_APPEND
            | O_DSYNC
            | O_SYNC
            | O_NONBLOCK
            | O_DIRECTORY
            | O_NOFOLLOW
            | O_LARGEFILE
            | O_CLOEXEC;
        if flags & !accepted != 0 || flags & O_TMPFILE == O_TMPFILE {
            if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                eprintln!(
                    "DARWIN FS: private open unsupported relative={} flags={flags:#x}",
                    String::from_utf8_lossy(&resolution.relative_path)
                );
            }
            return self.fail(ANDROID_EOPNOTSUPP);
        }
        let path = match self.private_path(&resolution.relative_path) {
            Ok(path) => path,
            Err(error) => return self.fail(error),
        };
        if flags & O_NOFOLLOW != 0
            && matches!(fs::symlink_metadata(&path), Ok(metadata) if metadata.file_type().is_symlink())
        {
            return self.fail(ANDROID_EACCES);
        }
        let access = flags & O_ACCMODE;
        let mut options = OpenOptions::new();
        options
            .read(access != O_WRONLY)
            .write(access != O_RDONLY)
            .create(flags & O_CREAT != 0)
            .create_new(flags & O_EXCL != 0)
            .truncate(flags & O_TRUNC != 0)
            .append(flags & O_APPEND != 0);
        let file = match options.open(&path) {
            Ok(file) => file,
            Err(error) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!(
                        "DARWIN FS: private open failed relative={} flags={flags:#x} error={error}",
                        String::from_utf8_lossy(&resolution.relative_path)
                    );
                }
                return self.fail_io(&error);
            }
        };
        if flags & O_DIRECTORY != 0 && !file.metadata().is_ok_and(|metadata| metadata.is_dir()) {
            return self.fail(ANDROID_ENOTDIR);
        }
        if flags & O_CREAT != 0 {
            use std::os::unix::fs::PermissionsExt;
            let _ = fs::set_permissions(&path, fs::Permissions::from_mode(mode & 0o7777));
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.insert(Descriptor::PrivateFile(file)) {
            Ok(fd) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!(
                        "DARWIN FS: private open relative={} flags={flags:#x} fd={fd}",
                        String::from_utf8_lossy(&resolution.relative_path)
                    );
                }
                fd
            }
            Err(()) => self.fail(ANDROID_EMFILE),
        }
    }

    fn random_device(path: &[u8]) -> Option<RandomDeviceKind> {
        match path {
            b"/dev/random" => Some(RandomDeviceKind::Random),
            b"/dev/urandom" => Some(RandomDeviceKind::Urandom),
            _ => None,
        }
    }

    fn proc_self_maps(path: &[u8]) -> bool {
        if path == b"/proc/self/maps" || path == b"/proc/thread-self/maps" {
            return true;
        }
        let Some(pid) = path
            .strip_prefix(b"/proc/")
            .and_then(|path| path.strip_suffix(b"/maps"))
        else {
            return false;
        };
        // Android child services are isolated as virtual processes inside one
        // Darwin task. Chromium commonly spells its own maps path using the
        // Java/ActivityManager pid rather than the host getpid() value. Every
        // such virtual pid therefore aliases this task's synthetic image map.
        !pid.is_empty() && pid.len() <= 10 && pid.iter().all(u8::is_ascii_digit) && pid != b"0"
    }

    fn open_proc_self_maps(&self, flags: c_int) -> c_int {
        if flags & O_ACCMODE != O_RDONLY || flags & WRITE_FLAGS != 0 {
            return self.fail(ANDROID_EROFS);
        }
        if flags & !(O_ACCMODE | O_CLOEXEC | O_NONBLOCK | O_LARGEFILE) != 0 {
            return self.fail(ANDROID_EOPNOTSUPP);
        }

        unsafe extern "C" fn append_region(
            context: *mut c_void,
            start: u64,
            end: u64,
            protection: c_int,
        ) -> c_int {
            if context.is_null() || start >= end {
                return -1;
            }
            // SAFETY: host_enumerate_regions synchronously passes back the
            // String pointer supplied by this function.
            let output = unsafe { &mut *context.cast::<String>() };
            let read = if protection & 1 != 0 { 'r' } else { '-' };
            let write = if protection & 2 != 0 { 'w' } else { '-' };
            let execute = if protection & 4 != 0 { 'x' } else { '-' };
            if writeln!(
                output,
                "{start:016x}-{end:016x} {read}{write}{execute}p 00000000 00:00 0"
            )
            .is_err()
            {
                return -1;
            }
            0
        }

        let mut contents = String::new();
        // SAFETY: the callback and context remain live for this synchronous
        // enumeration of the current task's Mach VM map.
        let result = unsafe {
            darwin_art_bionic_fs_host_enumerate_regions(
                append_region,
                (&mut contents as *mut String).cast(),
            )
        };
        if result != 0 {
            return self.fail(ANDROID_EIO);
        }
        let node = Arc::new(Mutex::new(OverlayFile {
            inode: 0x7072_6f63,
            mode: ANDROID_S_IFREG | 0o444,
            data: contents.into_bytes(),
        }));
        let descriptor = Descriptor::Overlay(OverlayDescriptor {
            node,
            offset: 0,
            readable: true,
            writable: false,
        });
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.insert(descriptor) {
            Ok(fd) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!("DARWIN FS: synthetic proc maps fd={fd}");
                }
                fd
            }
            Err(()) => self.fail(ANDROID_EMFILE),
        }
    }

    fn open_random(&self, kind: RandomDeviceKind, flags: c_int) -> c_int {
        if flags & O_ACCMODE != O_RDONLY
            || flags & WRITE_FLAGS != 0
            || flags & O_TMPFILE == O_TMPFILE
        {
            return self.fail(ANDROID_EROFS);
        }
        // Android callers routinely open the kernel random devices with
        // O_CLOEXEC (Chromium's PartitionAlloc does this during startup), and
        // may also request non-blocking semantics. Neither flag changes the
        // behavior of the synthetic descriptor, but both are valid on the
        // real Android character devices.
        if flags & !(O_ACCMODE | O_CLOEXEC | O_NONBLOCK | O_LARGEFILE) != 0 {
            return self.fail(ANDROID_EOPNOTSUPP);
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.insert(Descriptor::Random(kind)) {
            Ok(fd) => fd,
            Err(()) => self.fail(ANDROID_EMFILE),
        }
    }

    #[cfg(test)]
    fn open(&self, path: &[u8], flags: c_int) -> c_int {
        self.open_with_mode(path, flags, 0o600)
    }

    fn open_with_mode(&self, path: &[u8], flags: c_int, mode: u32) -> c_int {
        if let Some(kind) = Self::random_device(path) {
            return self.open_random(kind, flags);
        }
        if Self::proc_self_maps(path) {
            return self.open_proc_self_maps(flags);
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!(
                        "DARWIN FS: open resolve failed path={} errno={error}",
                        String::from_utf8_lossy(path)
                    );
                }
                return self.fail(error);
            }
        };
        if resolution.mount_id == 2 {
            if self.private_root.is_some() {
                return self.open_private(resolution, flags, mode);
            }
            return self.open_overlay(resolution, flags, mode);
        }
        if let Err(error) = self.validate_immutable_flags(flags) {
            return self.fail(error);
        }
        let mut relative = resolution.relative_path;
        if resolution.requires_directory && !relative.is_empty() {
            relative.push(b'/');
        }
        let opened = match self.broker.open(&relative) {
            Ok(opened) => opened,
            Err(error) => {
                if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                    eprintln!(
                        "DARWIN FS: open broker failed path={} relative={} error={error}",
                        String::from_utf8_lossy(path),
                        String::from_utf8_lossy(&relative)
                    );
                }
                return self.fail_broker(&error);
            }
        };
        if flags & O_DIRECTORY != 0 && !opened.metadata().is_dir() {
            return self.fail(ANDROID_ENOTDIR);
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        let result = match descriptors.insert(Descriptor::File(opened.into_file())) {
            Ok(fd) => fd,
            Err(()) => self.fail(ANDROID_EMFILE),
        };
        if std::env::var_os("DARWIN_ART_FS_TRACE").is_some()
            && (path.windows(7).any(|part| part == b"/fonts/") || path.ends_with(b"fonts.xml"))
        {
            eprintln!(
                "DARWIN FS: open path={} fd={result}",
                String::from_utf8_lossy(path)
            );
        }
        result
    }

    fn open_overlay(&self, resolution: Resolution, flags: c_int, mode: u32) -> c_int {
        if resolution.requires_directory || resolution.relative_path.is_empty() {
            return self.fail(ANDROID_EISDIR);
        }
        if let Err(error) = self.validate_overlay_flags(flags) {
            return self.fail(error);
        }
        let access = flags & O_ACCMODE;
        let mut overlay = match self.overlay.lock() {
            Ok(overlay) => overlay,
            Err(_) => return self.fail_capability(),
        };
        let parent = Self::overlay_parent(&resolution.relative_path);
        if !matches!(
            overlay.entries.get(parent),
            Some(OverlayEntry::Directory(_))
        ) {
            return self.fail(ANDROID_ENOENT);
        }
        let existing = overlay.entries.get(&resolution.relative_path);
        if flags & O_CREAT != 0 && flags & O_EXCL != 0 && existing.is_some() {
            return self.fail(ANDROID_EEXIST);
        }
        let node = match existing {
            Some(OverlayEntry::File(node)) => Arc::clone(node),
            Some(OverlayEntry::Directory(_)) => return self.fail(ANDROID_EISDIR),
            None if flags & O_CREAT == 0 => return self.fail(ANDROID_ENOENT),
            None => {
                let inode = overlay.next_inode;
                overlay.next_inode = overlay.next_inode.saturating_add(1);
                let node = Arc::new(Mutex::new(OverlayFile {
                    inode,
                    mode: ANDROID_S_IFREG | (mode & 0o7777),
                    data: Vec::new(),
                }));
                overlay.entries.insert(
                    resolution.relative_path.clone(),
                    OverlayEntry::File(Arc::clone(&node)),
                );
                node
            }
        };
        if flags & O_TRUNC != 0 {
            let mut file = match node.lock() {
                Ok(file) => file,
                Err(_) => return self.fail_capability(),
            };
            file.data.clear();
        }
        drop(overlay);
        let descriptor = Descriptor::Overlay(OverlayDescriptor {
            node,
            offset: 0,
            readable: access == O_RDONLY,
            writable: access == O_WRONLY,
        });
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.insert(descriptor) {
            Ok(fd) => fd,
            Err(()) => self.fail(ANDROID_EMFILE),
        }
    }

    fn openat_with_mode(&self, directory_fd: c_int, path: &[u8], flags: c_int, mode: u32) -> c_int {
        if directory_fd == AT_FDCWD || path.starts_with(b"/") {
            return self.open_with_mode(path, flags, mode);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.entries.get(&directory_fd) {
            Some(Descriptor::Random(_)) => self.fail(ANDROID_ENOTDIR),
            Some(Descriptor::File(_) | Descriptor::PrivateFile(_)) => self.fail(ANDROID_EOPNOTSUPP),
            Some(Descriptor::Overlay(_)) => self.fail(ANDROID_ENOTDIR),
            None => self.fail(ANDROID_EBADF),
        }
    }

    unsafe fn read(&self, fd: c_int, buffer: *mut c_void, count: usize) -> isize {
        if count > isize::MAX as usize {
            return self.fail(ANDROID_EINVAL) as isize;
        }
        if buffer.is_null() && count != 0 {
            return self.fail(ANDROID_EFAULT) as isize;
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability() as isize,
        };
        let Some(descriptor) = descriptors.entries.get_mut(&fd) else {
            return self.fail(ANDROID_EBADF) as isize;
        };
        // SAFETY: the guest ABI requires a writable buffer for count bytes. Zero length
        // accepts a null pointer, represented here with a dangling aligned pointer.
        let pointer = if count == 0 {
            ptr::NonNull::<u8>::dangling().as_ptr()
        } else {
            buffer.cast::<u8>()
        };
        let bytes = unsafe { slice::from_raw_parts_mut(pointer, count) };
        match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => match file.read(bytes) {
                Ok(read) => read as isize,
                Err(error) => self.fail_io(&error) as isize,
            },
            Descriptor::Random(_) => match self.entropy.fill(bytes) {
                Ok(()) => count as isize,
                Err(()) => self.fail(ANDROID_EIO) as isize,
            },
            Descriptor::Overlay(descriptor) => {
                if !descriptor.readable {
                    return self.fail(ANDROID_EBADF) as isize;
                }
                let file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability() as isize,
                };
                let start = usize::try_from(descriptor.offset).unwrap_or(usize::MAX);
                let available = file.data.len().saturating_sub(start);
                let copied = available.min(bytes.len());
                if copied != 0 {
                    bytes[..copied].copy_from_slice(&file.data[start..start + copied]);
                    descriptor.offset += copied as u64;
                }
                copied as isize
            }
        }
    }

    unsafe fn write(&self, fd: c_int, buffer: *const c_void, count: usize) -> isize {
        if count > isize::MAX as usize {
            return self.fail(ANDROID_EINVAL) as isize;
        }
        if buffer.is_null() && count != 0 {
            return self.fail(ANDROID_EFAULT) as isize;
        }
        let pointer = if count == 0 {
            ptr::NonNull::<u8>::dangling().as_ptr()
        } else {
            buffer.cast::<u8>()
        };
        // SAFETY: the guest ABI requires a readable buffer for count bytes.
        let bytes = unsafe { slice::from_raw_parts(pointer, count) };
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability() as isize,
        };
        let Some(descriptor) = descriptors.entries.get_mut(&fd) else {
            return self.fail(ANDROID_EBADF) as isize;
        };
        match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => match file.write(bytes) {
                Ok(written) => written as isize,
                Err(error) => self.fail_io(&error) as isize,
            },
            Descriptor::Random(_) => self.fail(ANDROID_EBADF) as isize,
            Descriptor::Overlay(descriptor) => {
                if !descriptor.writable {
                    return self.fail(ANDROID_EBADF) as isize;
                }
                let mut file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability() as isize,
                };
                let start = match usize::try_from(descriptor.offset) {
                    Ok(start) => start,
                    Err(_) => return self.fail(ANDROID_EINVAL) as isize,
                };
                let Some(end) = start.checked_add(bytes.len()) else {
                    return self.fail(ANDROID_EINVAL) as isize;
                };
                if start > file.data.len() {
                    file.data.resize(start, 0);
                }
                if end > file.data.len() {
                    file.data.resize(end, 0);
                }
                file.data[start..end].copy_from_slice(bytes);
                descriptor.offset = end as u64;
                bytes.len() as isize
            }
        }
    }

    unsafe fn pread(&self, fd: c_int, buffer: *mut c_void, count: usize, offset: i64) -> isize {
        if count > isize::MAX as usize || offset < 0 {
            return self.fail(ANDROID_EINVAL) as isize;
        }
        if buffer.is_null() && count != 0 {
            return self.fail(ANDROID_EFAULT) as isize;
        }
        let pointer = if count == 0 {
            ptr::NonNull::<u8>::dangling().as_ptr()
        } else {
            buffer.cast::<u8>()
        };
        let bytes = unsafe { slice::from_raw_parts_mut(pointer, count) };
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability() as isize,
        };
        let Some(descriptor) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF) as isize;
        };
        match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => {
                match file.read_at(bytes, offset as u64) {
                    Ok(read) => read as isize,
                    Err(error) => self.fail_io(&error) as isize,
                }
            }
            Descriptor::Random(_) => match self.entropy.fill(bytes) {
                Ok(()) => count as isize,
                Err(()) => self.fail(ANDROID_EIO) as isize,
            },
            Descriptor::Overlay(descriptor) => {
                if !descriptor.readable {
                    return self.fail(ANDROID_EBADF) as isize;
                }
                let file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability() as isize,
                };
                let start = offset as usize;
                let copied = file.data.len().saturating_sub(start).min(bytes.len());
                if copied != 0 {
                    bytes[..copied].copy_from_slice(&file.data[start..start + copied]);
                }
                copied as isize
            }
        }
    }

    unsafe fn pwrite(&self, fd: c_int, buffer: *const c_void, count: usize, offset: i64) -> isize {
        if count > isize::MAX as usize || offset < 0 {
            return self.fail(ANDROID_EINVAL) as isize;
        }
        if buffer.is_null() && count != 0 {
            return self.fail(ANDROID_EFAULT) as isize;
        }
        let pointer = if count == 0 {
            ptr::NonNull::<u8>::dangling().as_ptr()
        } else {
            buffer.cast::<u8>()
        };
        // SAFETY: the guest ABI requires a readable buffer for count bytes.
        let bytes = unsafe { slice::from_raw_parts(pointer, count) };
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability() as isize,
        };
        let Some(descriptor) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF) as isize;
        };
        match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => {
                match file.write_at(bytes, offset as u64) {
                    Ok(written) => written as isize,
                    Err(error) => self.fail_io(&error) as isize,
                }
            }
            Descriptor::Random(_) => self.fail(ANDROID_EBADF) as isize,
            Descriptor::Overlay(descriptor) => {
                if !descriptor.writable {
                    return self.fail(ANDROID_EBADF) as isize;
                }
                let mut file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability() as isize,
                };
                let start = offset as usize;
                let Some(end) = start.checked_add(bytes.len()) else {
                    return self.fail(ANDROID_EINVAL) as isize;
                };
                if start > file.data.len() {
                    file.data.resize(start, 0);
                }
                if end > file.data.len() {
                    file.data.resize(end, 0);
                }
                file.data[start..end].copy_from_slice(bytes);
                bytes.len() as isize
            }
        }
    }

    fn lseek(&self, fd: c_int, offset: i64, whence: c_int) -> i64 {
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability() as i64,
        };
        let Some(descriptor) = descriptors.entries.get_mut(&fd) else {
            return self.fail(ANDROID_EBADF) as i64;
        };
        match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => {
                let seek = match whence {
                    0 if offset >= 0 => SeekFrom::Start(offset as u64),
                    1 => SeekFrom::Current(offset),
                    2 => SeekFrom::End(offset),
                    _ => return self.fail(ANDROID_EINVAL) as i64,
                };
                match file.seek(seek) {
                    Ok(position) => {
                        i64::try_from(position).unwrap_or_else(|_| self.fail(ANDROID_EINVAL) as i64)
                    }
                    Err(error) => self.fail_io(&error) as i64,
                }
            }
            Descriptor::Random(_) => self.fail(ANDROID_EINVAL) as i64,
            Descriptor::Overlay(descriptor) => {
                let length = match descriptor.node.lock() {
                    Ok(file) => file.data.len() as i128,
                    Err(_) => return self.fail_capability() as i64,
                };
                let base = match whence {
                    0 => 0,
                    1 => descriptor.offset as i128,
                    2 => length,
                    _ => return self.fail(ANDROID_EINVAL) as i64,
                };
                let next = base + offset as i128;
                if !(0..=i64::MAX as i128).contains(&next) {
                    return self.fail(ANDROID_EINVAL) as i64;
                }
                descriptor.offset = next as u64;
                next as i64
            }
        }
    }

    fn close(&self, fd: c_int) -> c_int {
        let descriptor = {
            let mut descriptors = match self.descriptors.lock() {
                Ok(descriptors) => descriptors,
                Err(_) => return self.fail_capability(),
            };
            let Some(descriptor) = descriptors.close_entry(fd) else {
                return self.fail(ANDROID_EBADF);
            };
            descriptor
        };
        let file = match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => file,
            _ => return 0,
        };
        let raw = file.into_raw_fd();
        // SAFETY: into_raw_fd transfers this one live descriptor to host close.
        if unsafe { host_close(raw) } == 0 {
            0
        } else {
            let error = std::io::Error::last_os_error();
            self.fail_io(&error)
        }
    }

    fn flock(&self, fd: c_int, operation: c_int) -> c_int {
        if operation & !0x0f != 0 || operation & 0x0b == 0 {
            return self.fail(ANDROID_EINVAL);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        let Some(Descriptor::PrivateFile(file)) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF);
        };
        // Android and Darwin share LOCK_SH/EX/NB/UN bit values. The virtual
        // descriptor remains leased while the host applies the advisory lock.
        if unsafe { host_flock(file.as_raw_fd(), operation) } == 0 {
            0
        } else {
            self.fail_io(&std::io::Error::last_os_error())
        }
    }

    fn fcntl(&self, fd: c_int, command: c_int, argument: isize) -> c_int {
        const F_DUPFD: c_int = 0;
        const F_GETFD: c_int = 1;
        const F_SETFD: c_int = 2;
        const F_GETFL: c_int = 3;
        const F_SETFL: c_int = 4;
        const F_DUPFD_CLOEXEC: c_int = 1030;
        if matches!(command, F_DUPFD | F_DUPFD_CLOEXEC) {
            if argument < 0 {
                return self.fail(ANDROID_EINVAL);
            }
            let mut descriptors = match self.descriptors.lock() {
                Ok(descriptors) => descriptors,
                Err(_) => return self.fail_capability(),
            };
            let duplicate = match descriptors.entries.get(&fd) {
                Some(Descriptor::File(file)) => match file.try_clone() {
                    Ok(file) => Descriptor::File(file),
                    Err(error) => return self.fail_io(&error),
                },
                Some(Descriptor::PrivateFile(file)) => match file.try_clone() {
                    Ok(file) => Descriptor::PrivateFile(file),
                    Err(error) => return self.fail_io(&error),
                },
                Some(Descriptor::Random(kind)) => Descriptor::Random(*kind),
                Some(Descriptor::Overlay(_)) => return self.fail(ANDROID_EOPNOTSUPP),
                None => return self.fail(ANDROID_EBADF),
            };
            return match descriptors.insert(duplicate) {
                Ok(duplicate_fd) if (duplicate_fd as isize) >= argument => duplicate_fd,
                Ok(duplicate_fd) => {
                    let _ = descriptors.close_entry(duplicate_fd);
                    self.fail(ANDROID_EMFILE)
                }
                Err(()) => self.fail(ANDROID_EMFILE),
            };
        }
        if matches!(command, F_GETFD | F_SETFD | F_GETFL | F_SETFL) {
            if command == F_SETFD && argument & !1 != 0 {
                return self.fail(ANDROID_EINVAL);
            }
            if command == F_SETFL && argument & !(O_APPEND | O_NONBLOCK) as isize != 0 {
                return self.fail(ANDROID_EINVAL);
            }
            let descriptors = match self.descriptors.lock() {
                Ok(descriptors) => descriptors,
                Err(_) => return self.fail_capability(),
            };
            let Some(descriptor) = descriptors.entries.get(&fd) else {
                return self.fail(ANDROID_EBADF);
            };
            if command == F_GETFD {
                // Facade descriptors never cross exec; mirror Android's
                // close-on-exec ownership contract independently of the
                // private host descriptor number.
                return 1;
            }
            if command == F_SETFD {
                return 0;
            }
            let access = match descriptor {
                Descriptor::File(_) | Descriptor::Random(_) => O_RDONLY,
                Descriptor::PrivateFile(file) => {
                    // Darwin and Android share the access-mode low bits. The
                    // remaining status flags are translated explicitly below.
                    let flags = unsafe { host_fcntl(file.as_raw_fd(), F_GETFL) };
                    if flags < 0 {
                        return self.fail_io(&std::io::Error::last_os_error());
                    }
                    flags & O_ACCMODE
                }
                Descriptor::Overlay(file) => match (file.readable, file.writable) {
                    (true, true) => O_RDWR,
                    (false, true) => O_WRONLY,
                    _ => O_RDONLY,
                },
            };
            if command == F_GETFL {
                let mut android_flags = access;
                if let Descriptor::PrivateFile(file) = descriptor {
                    let host_flags = unsafe { host_fcntl(file.as_raw_fd(), F_GETFL) };
                    if host_flags < 0 {
                        return self.fail_io(&std::io::Error::last_os_error());
                    }
                    const DARWIN_O_NONBLOCK: c_int = 0x4;
                    const DARWIN_O_APPEND: c_int = 0x8;
                    if host_flags & DARWIN_O_NONBLOCK != 0 {
                        android_flags |= O_NONBLOCK;
                    }
                    if host_flags & DARWIN_O_APPEND != 0 {
                        android_flags |= O_APPEND;
                    }
                }
                return android_flags;
            }
            let Descriptor::PrivateFile(file) = descriptor else {
                return 0;
            };
            const DARWIN_O_NONBLOCK: c_int = 0x4;
            const DARWIN_O_APPEND: c_int = 0x8;
            let current = unsafe { host_fcntl(file.as_raw_fd(), F_GETFL) };
            if current < 0 {
                return self.fail_io(&std::io::Error::last_os_error());
            }
            let mut updated = current & !(DARWIN_O_NONBLOCK | DARWIN_O_APPEND);
            if argument as c_int & O_NONBLOCK != 0 {
                updated |= DARWIN_O_NONBLOCK;
            }
            if argument as c_int & O_APPEND != 0 {
                updated |= DARWIN_O_APPEND;
            }
            let result = unsafe { host_fcntl(file.as_raw_fd(), F_SETFL, updated) };
            return if result == 0 {
                0
            } else {
                self.fail_io(&std::io::Error::last_os_error())
            };
        }
        if !matches!(command, 5..=7) {
            return self.fail(ANDROID_EINVAL);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        let Some(Descriptor::PrivateFile(file)) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF);
        };
        let mut host_errno = 0;
        // SAFETY: the Android caller owns the flock object for this synchronous
        // call; the C adapter translates its arm64 layout to Darwin's layout.
        let result = unsafe {
            darwin_art_bionic_fs_host_record_lock(
                file.as_raw_fd(),
                command,
                argument,
                &mut host_errno,
            )
        };
        if result == 0 {
            return 0;
        }
        if result == -2 {
            return self.fail(ANDROID_EINVAL);
        }
        if host_errno != 0 {
            // SAFETY: this converts one Darwin errno into Android TLS errno.
            if unsafe { darwin_art_bionic_errno_set_from_darwin(host_errno) } != 0 {
                return -1;
            }
        }
        self.fail_capability()
    }

    unsafe fn fstat(&self, fd: c_int, status: *mut AndroidStat) -> c_int {
        if status.is_null() {
            return self.fail(ANDROID_EFAULT);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        let Some(descriptor) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF);
        };
        let translated = match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => {
                let metadata = match file.metadata() {
                    Ok(metadata) => metadata,
                    Err(error) => return self.fail_io(&error),
                };
                metadata_to_android(&metadata)
            }
            Descriptor::Random(kind) => random_device_stat(*kind),
            Descriptor::Overlay(descriptor) => {
                let file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability(),
                };
                overlay_file_stat(&file)
            }
        };
        // SAFETY: the guest ABI requires a writable 128-byte Android stat object.
        unsafe { status.write(translated) };
        0
    }

    fn fsync(&self, fd: c_int) -> c_int {
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        let Some(descriptor) = descriptors.entries.get(&fd) else {
            return self.fail(ANDROID_EBADF);
        };
        let result = match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => file.sync_all(),
            // Synthetic overlay files have no host-side dirty pages. Their
            // contents are committed to the in-memory overlay by write().
            Descriptor::Overlay(_) => return 0,
            Descriptor::Random(_) => return self.fail(ANDROID_EINVAL),
        };
        match result {
            Ok(()) => 0,
            Err(error) => self.fail_io(&error),
        }
    }

    unsafe fn stat(&self, path: &[u8], status: *mut AndroidStat, no_follow: bool) -> c_int {
        if status.is_null() {
            return self.fail(ANDROID_EFAULT);
        }
        if let Some(kind) = Self::random_device(path) {
            // SAFETY: the Android ABI requires one writable 128-byte stat object.
            unsafe { status.write(random_device_stat(kind)) };
            return 0;
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id == 2 {
            if self.private_root.is_some() {
                let path = match self.private_path(&resolution.relative_path) {
                    Ok(path) => path,
                    Err(error) => return self.fail(error),
                };
                let metadata = match if no_follow {
                    fs::symlink_metadata(path)
                } else {
                    fs::metadata(path)
                } {
                    Ok(metadata) => metadata,
                    Err(error) => return self.fail_io(&error),
                };
                unsafe { status.write(metadata_to_android(&metadata)) };
                return 0;
            }
            let overlay = match self.overlay.lock() {
                Ok(overlay) => overlay,
                Err(_) => return self.fail_capability(),
            };
            let translated = match overlay.entries.get(&resolution.relative_path) {
                Some(OverlayEntry::File(node)) => {
                    let file = match node.lock() {
                        Ok(file) => file,
                        Err(_) => return self.fail_capability(),
                    };
                    overlay_file_stat(&file)
                }
                Some(OverlayEntry::Directory(directory)) => overlay_directory_stat(*directory),
                None => return self.fail(ANDROID_ENOENT),
            };
            unsafe { status.write(translated) };
            return 0;
        }
        let mut relative_path = resolution.relative_path;
        if resolution.requires_directory && !relative_path.is_empty() {
            relative_path.push(b'/');
        }
        let metadata = match self.broker.stat(&relative_path) {
            Ok(metadata) => metadata,
            Err(error) => {
                if no_follow && is_final_symlink_rejection(&error, &relative_path) {
                    return self.fail(ANDROID_EOPNOTSUPP);
                }
                return self.fail_broker(&error);
            }
        };
        let translated = metadata_to_android(&metadata);
        // SAFETY: the guest ABI requires a writable 128-byte Android stat object.
        unsafe { status.write(translated) };
        0
    }

    fn chdir(&self, path: &[u8]) -> c_int {
        // chdir is process-global in Bionic. Holding this lock through the
        // authorization walk gives concurrent calls a total update order.
        let mut cwd = match self.cwd.lock() {
            Ok(cwd) => cwd,
            Err(_) => return self.fail_capability(),
        };
        let resolution = match self.resolve_from(&cwd, path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        let opened = match self.broker.open(&resolution.relative_path) {
            Ok(opened) => opened,
            Err(error) => return self.fail_broker(&error),
        };
        if !opened.metadata().is_dir() {
            return self.fail(ANDROID_ENOTDIR);
        }
        *cwd = resolution.normalized_path;
        0
    }

    unsafe fn getcwd(&self, buffer: *mut c_char, size: usize) -> *mut c_char {
        if buffer.is_null() {
            // Bionic's allocation extension needs the coherent Bionic allocator,
            // which this isolated facade deliberately does not own.
            self.fail(ANDROID_EOPNOTSUPP);
            return ptr::null_mut();
        }
        let cwd = match self.cwd.lock() {
            Ok(cwd) => cwd,
            Err(_) => {
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        let required = match cwd.len().checked_add(1) {
            Some(required) => required,
            None => {
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        if size < required {
            self.fail(ANDROID_ERANGE);
            return ptr::null_mut();
        }
        // SAFETY: the ABI requires buffer to be writable for size bytes; the
        // checked required length is at most size and includes the trailing NUL.
        unsafe {
            ptr::copy_nonoverlapping(cwd.as_ptr(), buffer.cast::<u8>(), cwd.len());
            buffer.add(cwd.len()).write(0);
        }
        buffer
    }

    fn readlink(&self, path: &[u8], buffer: *mut c_char, size: usize) -> isize {
        if buffer.is_null() && size != 0 {
            return self.fail(ANDROID_EFAULT) as isize;
        }
        if size == 0 {
            return self.fail(ANDROID_EINVAL) as isize;
        }
        if path == b"/proc/self/exe" || path == b"/proc/thread-self/exe" {
            // Android application processes are forked from zygote's
            // app_process64 executable. Chromium uses this Linux identity to
            // derive process paths; expose the Android executable identity,
            // never the Darwin host binary or an authorized host path.
            const TARGET: &[u8] = b"/system/bin/app_process64";
            let copied = TARGET.len().min(size);
            // SAFETY: the readlink ABI guarantees a writable buffer for size
            // bytes. Like Linux readlink(2), the result is not NUL-terminated
            // and truncation returns exactly the copied byte count.
            unsafe {
                ptr::copy_nonoverlapping(TARGET.as_ptr(), buffer.cast::<u8>(), copied);
            }
            return copied as isize;
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error) as isize,
        };
        match self.broker.open(&resolution.relative_path) {
            // A securely opened regular file or directory is definitively not
            // a symlink. Match Linux/Bionic readlink with EINVAL.
            Ok(_) => self.fail(ANDROID_EINVAL) as isize,
            Err(error) if is_final_symlink_rejection(&error, &resolution.relative_path) => {
                // The broker cannot safely return the final link's bytes.
                self.fail(ANDROID_EOPNOTSUPP) as isize
            }
            Err(error) => self.fail_broker(&error) as isize,
        }
    }

    fn reject_fd_mutation(&self, fd: c_int) -> c_int {
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        if descriptors.entries.contains_key(&fd) {
            self.fail(ANDROID_EROFS)
        } else {
            self.fail(ANDROID_EBADF)
        }
    }

    fn fchmod(&self, fd: c_int, mode: u32) -> c_int {
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.entries.get_mut(&fd) {
            Some(Descriptor::PrivateFile(file)) => {
                use std::os::unix::fs::PermissionsExt;
                match file.set_permissions(fs::Permissions::from_mode(mode & 0o7777)) {
                    Ok(()) => 0,
                    Err(error) => self.fail_io(&error),
                }
            }
            Some(Descriptor::Overlay(descriptor)) if descriptor.writable => {
                let mut file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability(),
                };
                file.mode = ANDROID_S_IFREG | (mode & 0o7777);
                0
            }
            Some(Descriptor::Overlay(_)) => self.fail(ANDROID_EBADF),
            Some(_) => self.fail(ANDROID_EROFS),
            None => self.fail(ANDROID_EBADF),
        }
    }

    fn chmod(&self, path: &[u8], mode: u32) -> c_int {
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id != 2 {
            return self.fail(ANDROID_EROFS);
        }
        if self.private_root.is_some() {
            use std::os::unix::fs::PermissionsExt;
            let path = match self.private_path(&resolution.relative_path) {
                Ok(path) => path,
                Err(error) => return self.fail(error),
            };
            return match fs::set_permissions(path, fs::Permissions::from_mode(mode & 0o7777)) {
                Ok(()) => 0,
                Err(error) => self.fail_io(&error),
            };
        }
        let mut overlay = match self.overlay.lock() {
            Ok(overlay) => overlay,
            Err(_) => return self.fail_capability(),
        };
        let result = match overlay.entries.get_mut(&resolution.relative_path) {
            Some(OverlayEntry::File(node)) => {
                let mut file = match node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability(),
                };
                file.mode = ANDROID_S_IFREG | (mode & 0o7777);
                0
            }
            Some(OverlayEntry::Directory(directory)) => {
                directory.mode = ANDROID_S_IFDIR | (mode & 0o7777);
                0
            }
            None => self.fail(ANDROID_ENOENT),
        };
        if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
            eprintln!(
                "DARWIN FS: chmod path={} relative={} mode={mode:o} result={result}",
                String::from_utf8_lossy(path),
                String::from_utf8_lossy(&resolution.relative_path)
            );
        }
        result
    }

    fn fchown(&self, fd: c_int, _owner: u32, _group: u32) -> c_int {
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.entries.get(&fd) {
            // The private overlay belongs to the one Android application
            // process. Ownership changes therefore preserve the same virtual
            // owner without leaking Darwin uid/gid state.
            Some(Descriptor::Overlay(_)) => 0,
            Some(_) => self.fail(ANDROID_EROFS),
            None => self.fail(ANDROID_EBADF),
        }
    }

    fn ftruncate(&self, fd: c_int, length: i64) -> c_int {
        if length < 0 {
            return self.fail(ANDROID_EINVAL);
        }
        let Ok(length) = usize::try_from(length) else {
            return self.fail(ANDROID_EINVAL);
        };
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.entries.get_mut(&fd) {
            Some(Descriptor::PrivateFile(file)) => match file.set_len(length as u64) {
                Ok(()) => 0,
                Err(error) => self.fail_io(&error),
            },
            Some(Descriptor::Overlay(descriptor)) if descriptor.writable => {
                let mut file = match descriptor.node.lock() {
                    Ok(file) => file,
                    Err(_) => return self.fail_capability(),
                };
                file.data.resize(length, 0);
                0
            }
            Some(Descriptor::Overlay(_)) => self.fail(ANDROID_EBADF),
            Some(_) => self.fail(ANDROID_EROFS),
            None => self.fail(ANDROID_EBADF),
        }
    }

    fn mkdir(&self, path: &[u8], mode: u32) -> c_int {
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id != 2 {
            return self.fail(ANDROID_EROFS);
        }
        if resolution.relative_path.is_empty() {
            return self.fail(ANDROID_EEXIST);
        }
        if self.private_root.is_some() {
            use std::os::unix::fs::PermissionsExt;
            let path = match self.private_path(&resolution.relative_path) {
                Ok(path) => path,
                Err(error) => return self.fail(error),
            };
            return match fs::create_dir(&path) {
                Ok(()) => {
                    let _ = fs::set_permissions(path, fs::Permissions::from_mode(mode & 0o7777));
                    0
                }
                Err(error) => self.fail_io(&error),
            };
        }
        let mut overlay = match self.overlay.lock() {
            Ok(overlay) => overlay,
            Err(_) => return self.fail_capability(),
        };
        if overlay.entries.contains_key(&resolution.relative_path) {
            if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                eprintln!(
                    "DARWIN FS: mkdir exists path={}",
                    String::from_utf8_lossy(path)
                );
            }
            return self.fail(ANDROID_EEXIST);
        }
        if !matches!(
            overlay
                .entries
                .get(Self::overlay_parent(&resolution.relative_path)),
            Some(OverlayEntry::Directory(_))
        ) {
            if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
                eprintln!(
                    "DARWIN FS: mkdir missing-parent path={} relative={} parent={}",
                    String::from_utf8_lossy(path),
                    String::from_utf8_lossy(&resolution.relative_path),
                    String::from_utf8_lossy(Self::overlay_parent(&resolution.relative_path))
                );
            }
            return self.fail(ANDROID_ENOENT);
        }
        let inode = overlay.next_inode;
        overlay.next_inode = overlay.next_inode.saturating_add(1);
        overlay.entries.insert(
            resolution.relative_path,
            OverlayEntry::Directory(OverlayDirectory {
                inode,
                mode: ANDROID_S_IFDIR | (mode & 0o7777),
            }),
        );
        if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
            eprintln!(
                "DARWIN FS: mkdir path={} mode={mode:o} result=0",
                String::from_utf8_lossy(path)
            );
        }
        0
    }

    fn seed_private_directory(&self, path: &[u8]) -> c_int {
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id != 2 {
            return self.fail(ANDROID_EROFS);
        }
        if self.private_root.is_some() {
            let path = match self.private_path(&resolution.relative_path) {
                Ok(path) => path,
                Err(error) => return self.fail(error),
            };
            return match fs::create_dir_all(path) {
                Ok(()) => 0,
                Err(error) => self.fail_io(&error),
            };
        }
        let mut overlay = match self.overlay.lock() {
            Ok(overlay) => overlay,
            Err(_) => return self.fail_capability(),
        };
        let mut prefix = Vec::new();
        for component in resolution
            .relative_path
            .split(|byte| *byte == b'/')
            .filter(|component| !component.is_empty())
        {
            if !prefix.is_empty() {
                prefix.push(b'/');
            }
            prefix.extend_from_slice(component);
            match overlay.entries.get(&prefix) {
                Some(OverlayEntry::Directory(_)) => continue,
                Some(OverlayEntry::File(_)) => return self.fail(ANDROID_ENOTDIR),
                None => {}
            }
            let inode = overlay.next_inode;
            overlay.next_inode = overlay.next_inode.saturating_add(1);
            overlay.entries.insert(
                prefix.clone(),
                OverlayEntry::Directory(OverlayDirectory {
                    inode,
                    mode: ANDROID_S_IFDIR | 0o700,
                }),
            );
        }
        if std::env::var_os("DARWIN_ART_FS_TRACE").is_some() {
            eprintln!(
                "DARWIN FS: seeded private directory path={} relative={}",
                String::from_utf8_lossy(path),
                String::from_utf8_lossy(&resolution.relative_path)
            );
        }
        0
    }

    fn remove_path(&self, path: &[u8]) -> c_int {
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id != 2 || self.private_root.is_none() {
            return self.fail(ANDROID_EROFS);
        }
        let path = match self.private_path(&resolution.relative_path) {
            Ok(path) => path,
            Err(error) => return self.fail(error),
        };
        let result = match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.is_dir() => fs::remove_dir(path),
            Ok(_) => fs::remove_file(path),
            Err(error) => return self.fail_io(&error),
        };
        match result {
            Ok(()) => 0,
            Err(error) => self.fail_io(&error),
        }
    }

    fn rename_path(&self, old_path: &[u8], new_path: &[u8]) -> c_int {
        let old = match self.resolve(old_path) {
            Ok(resolution) if resolution.mount_id == 2 => resolution,
            Ok(_) => return self.fail(ANDROID_EROFS),
            Err(error) => return self.fail(error),
        };
        let new = match self.resolve(new_path) {
            Ok(resolution) if resolution.mount_id == 2 => resolution,
            Ok(_) => return self.fail(ANDROID_EROFS),
            Err(error) => return self.fail(error),
        };
        if self.private_root.is_none() {
            return self.fail(ANDROID_EROFS);
        }
        let old = match self.private_path(&old.relative_path) {
            Ok(path) => path,
            Err(error) => return self.fail(error),
        };
        let new = match self.private_path(&new.relative_path) {
            Ok(path) => path,
            Err(error) => return self.fail(error),
        };
        match fs::rename(old, new) {
            Ok(()) => 0,
            Err(error) => self.fail_io(&error),
        }
    }

    fn truncate_path(&self, path: &[u8], length: i64) -> c_int {
        if length < 0 {
            return self.fail(ANDROID_EINVAL);
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) if resolution.mount_id == 2 => resolution,
            Ok(_) => return self.fail(ANDROID_EROFS),
            Err(error) => return self.fail(error),
        };
        let path = match self.private_path(&resolution.relative_path) {
            Ok(path) => path,
            Err(error) => return self.fail(error),
        };
        match OpenOptions::new()
            .write(true)
            .open(path)
            .and_then(|file| file.set_len(length as u64))
        {
            Ok(()) => 0,
            Err(error) => self.fail_io(&error),
        }
    }

    fn resolve_at(&self, directory_fd: c_int, path: &[u8]) -> Result<Resolution, c_int> {
        if directory_fd == AT_FDCWD || path.starts_with(b"/") {
            return self.resolve(path);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => {
                self.fail_capability();
                return Err(ANDROID_EIO);
            }
        };
        match descriptors.entries.get(&directory_fd) {
            Some(Descriptor::Random(_)) => Err(ANDROID_ENOTDIR),
            Some(Descriptor::File(_) | Descriptor::PrivateFile(_)) => Err(ANDROID_EOPNOTSUPP),
            Some(Descriptor::Overlay(_)) => Err(ANDROID_ENOTDIR),
            None => Err(ANDROID_EBADF),
        }
    }

    fn reject_path_mutation(&self, path: &[u8]) -> c_int {
        match self.resolve(path) {
            Ok(_) => self.fail(ANDROID_EROFS),
            Err(error) => self.fail(error),
        }
    }

    fn reject_at_path_mutation(&self, directory_fd: c_int, path: &[u8]) -> c_int {
        match self.resolve_at(directory_fd, path) {
            Ok(_) => self.fail(ANDROID_EROFS),
            Err(error) => self.fail(error),
        }
    }

    fn reject_two_path_mutation(&self, old_path: &[u8], new_path: &[u8]) -> c_int {
        if let Err(error) = self.resolve(old_path) {
            return self.fail(error);
        }
        if let Err(error) = self.resolve(new_path) {
            return self.fail(error);
        }
        self.fail(ANDROID_EROFS)
    }

    fn isatty(&self, fd: c_int) -> c_int {
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        if descriptors.entries.contains_key(&fd) {
            // Brokered files, directories, and synthetic random devices are
            // not terminals. isatty returns zero and publishes ENOTTY.
            Self::set_android_errno(ANDROID_ENOTTY);
            0
        } else {
            Self::set_android_errno(ANDROID_EBADF);
            0
        }
    }

    fn pathconf(&self, path: &[u8], name: c_int) -> i64 {
        if !(PATHCONF_MIN..=PATHCONF_MAX).contains(&name) {
            return self.fail(ANDROID_EINVAL) as i64;
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error) as i64,
        };
        let opened = match self.broker.open(&resolution.relative_path) {
            Ok(opened) => opened,
            Err(error) => return self.fail_broker(&error) as i64,
        };
        let mut value = -1_i64;
        let mut host_errno = 0;
        // SAFETY: the opened descriptor and both output cells live through the
        // call. The helper maps the semantic Android selector explicitly.
        let result = unsafe {
            darwin_art_bionic_fs_host_fpathconf(
                opened.file().as_raw_fd(),
                name,
                &mut value,
                &mut host_errno,
            )
        };
        match result {
            1 => value,
            0 => -1,
            _ => self.fail_host_errno(host_errno) as i64,
        }
    }

    unsafe fn realpath(&self, path: &[u8], resolved: *mut c_char) -> *mut c_char {
        if resolved.is_null() {
            // Bionic's allocation extension belongs to the coherent allocator
            // provider and is not guessed by this standalone facade.
            self.fail(ANDROID_EOPNOTSUPP);
            return ptr::null_mut();
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => {
                self.fail(error);
                return ptr::null_mut();
            }
        };
        if let Err(error) = self.broker.open(&resolution.relative_path) {
            self.fail_broker(&error);
            return ptr::null_mut();
        }
        // The prefix layer pins normalized guest paths to at most 4095 bytes,
        // matching Android PATH_MAX including the NUL in the caller buffer.
        unsafe {
            ptr::copy_nonoverlapping(
                resolution.normalized_path.as_ptr(),
                resolved.cast::<u8>(),
                resolution.normalized_path.len(),
            );
            resolved.add(resolution.normalized_path.len()).write(0);
        }
        resolved
    }

    unsafe fn statvfs(&self, path: &[u8], status: *mut AndroidStatvfs) -> c_int {
        if status.is_null() {
            return self.fail(ANDROID_EFAULT);
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        let opened = match self.broker.open(&resolution.relative_path) {
            Ok(opened) => opened,
            Err(error) => return self.fail_broker(&error),
        };
        let mut host = HostStatvfs::default();
        let mut host_errno = 0;
        // SAFETY: the descriptor and fixed-layout outputs live through the call.
        if unsafe {
            darwin_art_bionic_fs_host_fstatvfs(
                opened.file().as_raw_fd(),
                &mut host,
                &mut host_errno,
            )
        } != 0
        {
            return self.fail_host_errno(host_errno);
        }
        let translated = AndroidStatvfs {
            f_bsize: host.f_bsize,
            f_frsize: host.f_frsize,
            f_blocks: host.f_blocks,
            f_bfree: host.f_bfree,
            f_bavail: host.f_bavail,
            f_files: host.f_files,
            f_ffree: host.f_ffree,
            f_favail: host.f_favail,
            f_fsid: host.f_fsid,
            // The guest mount is immutable even when its provider-local host
            // filesystem is writable. Only semantically translated bits cross.
            f_flag: host.f_flag | ANDROID_ST_RDONLY,
            f_namemax: host.f_namemax,
            reserved: [0; 6],
        };
        unsafe { status.write(translated) };
        0
    }

    fn opendir(&self, path: &[u8]) -> *mut c_void {
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => {
                self.fail(error);
                return ptr::null_mut();
            }
        };
        let opened = if resolution.mount_id == 2 && self.private_root.is_some() {
            let private = match self.private_path(&resolution.relative_path) {
                Ok(path) => path,
                Err(error) => {
                    self.fail(error);
                    return ptr::null_mut();
                }
            };
            match File::open(private) {
                Ok(file) => file,
                Err(error) => {
                    self.fail_io(&error);
                    return ptr::null_mut();
                }
            }
        } else {
            match self.broker.open(&resolution.relative_path) {
                Ok(opened) => opened.into_file(),
                Err(error) => {
                    self.fail_broker(&error);
                    return ptr::null_mut();
                }
            }
        };
        if !opened.metadata().is_ok_and(|metadata| metadata.is_dir()) {
            self.fail(ANDROID_ENOTDIR);
            return ptr::null_mut();
        }
        let raw_fd = opened.into_raw_fd();
        let mut host_errno = 0;
        // SAFETY: fdopendir takes ownership of raw_fd only on success.
        let host_directory =
            unsafe { darwin_art_bionic_fs_host_fdopendir(raw_fd, &mut host_errno) };
        if host_directory.is_null() {
            // SAFETY: ownership was not transferred when fdopendir failed.
            unsafe { host_close(raw_fd) };
            self.fail_host_errno(host_errno);
            return ptr::null_mut();
        }
        let mut directories = match self.directories.lock() {
            Ok(directories) => directories,
            Err(_) => {
                let mut ignored_errno = 0;
                // SAFETY: stream ownership has not entered a table yet.
                unsafe { darwin_art_bionic_fs_host_closedir(host_directory, &mut ignored_errno) };
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        directories.insert(host_directory)
    }

    fn fdopendir(&self, fd: c_int) -> *mut c_void {
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => {
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        let Some(descriptor) = descriptors.entries.get(&fd) else {
            self.fail(ANDROID_EBADF);
            return ptr::null_mut();
        };
        if matches!(descriptor, Descriptor::Overlay(_)) {
            self.fail(ANDROID_ENOTDIR);
            return ptr::null_mut();
        }
        let file = match descriptor {
            Descriptor::File(file) | Descriptor::PrivateFile(file) => file,
            _ => {
                self.fail(ANDROID_ENOTDIR);
                return ptr::null_mut();
            }
        };
        match file.metadata() {
            Ok(metadata) if metadata.is_dir() => {}
            Ok(_) => {
                self.fail(ANDROID_ENOTDIR);
                return ptr::null_mut();
            }
            Err(error) => {
                self.fail_io(&error);
                return ptr::null_mut();
            }
        }
        let descriptor = descriptors
            .take(fd)
            .expect("validated virtual descriptor must remain present under its lock");
        let (file, private) = match descriptor {
            Descriptor::File(file) => (file, false),
            Descriptor::PrivateFile(file) => (file, true),
            _ => unreachable!("validated descriptor kind changed under lock"),
        };
        let raw_fd = file.into_raw_fd();
        let mut host_errno = 0;
        // SAFETY: fdopendir consumes raw_fd only when it returns a stream.
        let host_directory =
            unsafe { darwin_art_bionic_fs_host_fdopendir(raw_fd, &mut host_errno) };
        if host_directory.is_null() {
            // SAFETY: failed fdopendir leaves ownership of the still-live descriptor
            // with the caller; restore it under the exact same virtual descriptor.
            let restored = unsafe { File::from_raw_fd(raw_fd) };
            descriptors.restore(
                fd,
                if private {
                    Descriptor::PrivateFile(restored)
                } else {
                    Descriptor::File(restored)
                },
            );
            self.fail_host_errno(host_errno);
            return ptr::null_mut();
        }
        descriptors.release(fd);
        drop(descriptors);

        let mut directories = match self.directories.lock() {
            Ok(directories) => directories,
            Err(_) => {
                let mut ignored_errno = 0;
                // SAFETY: the host stream owns raw_fd after successful fdopendir.
                unsafe { darwin_art_bionic_fs_host_closedir(host_directory, &mut ignored_errno) };
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        directories.insert(host_directory)
    }

    fn fail_host_errno(&self, host_errno: c_int) -> c_int {
        if host_errno != 0 {
            // SAFETY: translation is value-only and preserves Darwin errno.
            if unsafe { darwin_art_bionic_errno_set_from_darwin(host_errno) } != 0 {
                return -1;
            }
        }
        self.fail_capability()
    }

    fn readdir(&self, directory: *mut c_void) -> *mut AndroidDirent {
        if directory.is_null() {
            self.fail(ANDROID_EBADF);
            return ptr::null_mut();
        }
        // A single lock serializes readdir/closedir on every facade stream.
        // Tokens are keys only and are never dereferenced before membership.
        let mut directories = match self.directories.lock() {
            Ok(directories) => directories,
            Err(_) => {
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        let Some(state) = directories.streams.get_mut(&(directory as usize)) else {
            self.fail(ANDROID_EBADF);
            return ptr::null_mut();
        };
        if state.host_directory == 0 {
            self.fail(ANDROID_EBADF);
            return ptr::null_mut();
        }
        let mut host_entry = HostDirent::default();
        let mut host_errno = 0;
        // SAFETY: the table exclusively owns and serializes this live stream.
        let result = unsafe {
            darwin_art_bionic_fs_host_readdir(
                state.host_directory as *mut c_void,
                &mut host_entry,
                &mut host_errno,
            )
        };
        if result == 0 {
            // Bionic readdir leaves errno unchanged at end-of-directory.
            return ptr::null_mut();
        }
        if result < 0 {
            self.fail_host_errno(host_errno);
            return ptr::null_mut();
        }
        let name_length = usize::from(host_entry.d_name_length);
        if name_length >= host_entry.d_name.len() {
            self.fail_capability();
            return ptr::null_mut();
        }
        state.offset = match state.offset.checked_add(1) {
            Some(offset) => offset,
            None => {
                self.fail_capability();
                return ptr::null_mut();
            }
        };
        let record_length = (19usize + name_length + 1 + 7) & !7;
        *state.entry = AndroidDirent::default();
        state.entry.d_ino = host_entry.d_ino;
        state.entry.d_off = state.offset;
        state.entry.d_reclen = record_length as u16;
        state.entry.d_type = android_directory_type(host_entry.d_type);
        state.entry.d_name[..=name_length].copy_from_slice(&host_entry.d_name[..=name_length]);
        &raw mut *state.entry
    }

    fn closedir(&self, directory: *mut c_void) -> c_int {
        if directory.is_null() {
            return self.fail(ANDROID_EBADF);
        }
        let mut directories = match self.directories.lock() {
            Ok(directories) => directories,
            Err(_) => return self.fail_capability(),
        };
        let Some(mut state) = directories.streams.remove(&(directory as usize)) else {
            return self.fail(ANDROID_EBADF);
        };
        let host_directory = std::mem::replace(&mut state.host_directory, 0);
        let mut host_errno = 0;
        // SAFETY: table ownership is transferred exactly once to closedir.
        if unsafe {
            darwin_art_bionic_fs_host_closedir(host_directory as *mut c_void, &mut host_errno)
        } == 0
        {
            0
        } else {
            self.fail_host_errno(host_errno)
        }
    }

    fn rewinddir(&self, directory: *mut c_void) {
        if directory.is_null() {
            self.fail(ANDROID_EBADF);
            return;
        }
        let mut directories = match self.directories.lock() {
            Ok(directories) => directories,
            Err(_) => {
                self.fail_capability();
                return;
            }
        };
        let Some(state) = directories.streams.get_mut(&(directory as usize)) else {
            self.fail(ANDROID_EBADF);
            return;
        };
        unsafe { darwin_art_bionic_fs_host_rewinddir(state.host_directory as *mut c_void) };
        state.offset = 0;
    }

    fn sendfile_transfer(&self, request: &SendfileRequest, result: &mut SendfileResult) -> c_int {
        *result = SendfileResult {
            abi_version: SENDFILE_ABI_VERSION,
            android_errno: 0,
            transferred: -1,
            next_offset: request.offset,
        };
        if request.abi_version != SENDFILE_ABI_VERSION
            || request.has_explicit_offset > 1
            || (request.has_explicit_offset != 0 && request.offset < 0)
            || request.output_fd == request.input_fd
        {
            result.android_errno = ANDROID_EINVAL;
            return SENDFILE_TRANSFER_OK;
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => {
                self.capability_failure.store(true, Ordering::Release);
                return SENDFILE_TRANSFER_UNAVAILABLE;
            }
        };
        if !descriptors.entries.contains_key(&request.input_fd)
            || !descriptors.entries.contains_key(&request.output_fd)
        {
            return SENDFILE_TRANSFER_BAD_FD;
        }
        let Some(mut output) = descriptors.take(request.output_fd) else {
            return SENDFILE_TRANSFER_BAD_FD;
        };
        let transfer_result = (|| {
            let input = descriptors
                .entries
                .get_mut(&request.input_fd)
                .ok_or(ANDROID_EBADF)?;
            if let (Descriptor::Overlay(input_descriptor), Descriptor::Overlay(output_descriptor)) =
                (&*input, &output)
                && Arc::ptr_eq(&input_descriptor.node, &output_descriptor.node)
            {
                return Err(ANDROID_EINVAL);
            }
            let wanted = request.count.min(64 * 1024);
            let mut bytes = vec![0_u8; wanted];
            let read = match input {
                Descriptor::File(file) | Descriptor::PrivateFile(file)
                    if request.has_explicit_offset != 0 =>
                {
                    file.read_at(&mut bytes, request.offset as u64)
                        .map_err(|_| ANDROID_EIO)?
                }
                Descriptor::File(file) | Descriptor::PrivateFile(file) => {
                    file.read(&mut bytes).map_err(|_| ANDROID_EIO)?
                }
                Descriptor::Overlay(input_descriptor) => {
                    if !input_descriptor.readable {
                        return Err(ANDROID_EBADF);
                    }
                    let input_file = input_descriptor.node.lock().map_err(|_| ANDROID_EIO)?;
                    let offset = if request.has_explicit_offset != 0 {
                        request.offset as u64
                    } else {
                        input_descriptor.offset
                    };
                    let start = usize::try_from(offset).unwrap_or(usize::MAX);
                    let copied = input_file.data.len().saturating_sub(start).min(bytes.len());
                    if copied != 0 {
                        bytes[..copied].copy_from_slice(&input_file.data[start..start + copied]);
                    }
                    drop(input_file);
                    if request.has_explicit_offset == 0 {
                        input_descriptor.offset += copied as u64;
                    }
                    copied
                }
                Descriptor::Random(_) => return Err(ANDROID_EINVAL),
            };
            bytes.truncate(read);
            match &mut output {
                Descriptor::PrivateFile(file) => file.write(&bytes).map_err(|_| ANDROID_EIO),
                Descriptor::Overlay(output_descriptor) => {
                    if !output_descriptor.writable {
                        return Err(ANDROID_EBADF);
                    }
                    let mut output_file = output_descriptor.node.lock().map_err(|_| ANDROID_EIO)?;
                    let output_start =
                        usize::try_from(output_descriptor.offset).map_err(|_| ANDROID_EINVAL)?;
                    let output_end = output_start
                        .checked_add(bytes.len())
                        .ok_or(ANDROID_EINVAL)?;
                    if output_file.data.len() < output_end {
                        output_file.data.resize(output_end, 0);
                    }
                    output_file.data[output_start..output_end].copy_from_slice(&bytes);
                    output_descriptor.offset += bytes.len() as u64;
                    Ok(bytes.len())
                }
                Descriptor::File(_) => Err(ANDROID_EROFS),
                Descriptor::Random(_) => Err(ANDROID_EINVAL),
            }
        })();
        descriptors.restore(request.output_fd, output);
        match transfer_result {
            Ok(transferred) => {
                result.transferred = transferred as isize;
                result.next_offset = match request.offset.checked_add(transferred as i64) {
                    Some(offset) => offset,
                    None => {
                        result.transferred = -1;
                        result.android_errno = ANDROID_EINVAL;
                        request.offset
                    }
                };
                SENDFILE_TRANSFER_OK
            }
            Err(error) => {
                if error == ANDROID_EIO {
                    self.capability_failure.store(true, Ordering::Release);
                }
                result.android_errno = error;
                SENDFILE_TRANSFER_OK
            }
        }
    }
}

fn is_final_symlink_rejection(error: &BrokerError, relative_path: &[u8]) -> bool {
    if error.raw_os_error() != Some(DARWIN_ELOOP) || relative_path.is_empty() {
        return false;
    }
    let final_index = relative_path.split(|byte| *byte == b'/').count() - 1;
    matches!(
        error,
        BrokerError::Io {
            component_index: Some(index),
            ..
        } if *index == final_index
    )
}

fn android_directory_type(host_type: u8) -> u8 {
    match host_type {
        1 | 2 | 4 | 6 | 8 | 10 | 12 | 14 => host_type,
        _ => 0,
    }
}

fn metadata_to_android(metadata: &Metadata) -> AndroidStat {
    AndroidStat {
        st_dev: metadata.dev(),
        st_ino: metadata.ino(),
        st_mode: metadata.mode(),
        st_nlink: metadata.nlink() as u32,
        st_uid: metadata.uid(),
        st_gid: metadata.gid(),
        st_rdev: metadata.rdev(),
        pad1: 0,
        st_size: metadata.size() as i64,
        st_blksize: metadata.blksize() as i32,
        pad2: 0,
        st_blocks: metadata.blocks() as i64,
        st_atim: AndroidTimespec {
            tv_sec: metadata.atime(),
            tv_nsec: metadata.atime_nsec(),
        },
        st_mtim: AndroidTimespec {
            tv_sec: metadata.mtime(),
            tv_nsec: metadata.mtime_nsec(),
        },
        st_ctim: AndroidTimespec {
            tv_sec: metadata.ctime(),
            tv_nsec: metadata.ctime_nsec(),
        },
        unused4: 0,
        unused5: 0,
    }
}

fn random_device_stat(kind: RandomDeviceKind) -> AndroidStat {
    let (inode, device) = match kind {
        RandomDeviceKind::Random => (1, ANDROID_RANDOM_RDEV),
        RandomDeviceKind::Urandom => (2, ANDROID_URANDOM_RDEV),
    };
    AndroidStat {
        st_dev: 0,
        st_ino: inode,
        st_mode: ANDROID_RANDOM_MODE,
        st_nlink: 1,
        st_uid: 0,
        st_gid: 0,
        st_rdev: device,
        pad1: 0,
        st_size: 0,
        st_blksize: 4096,
        pad2: 0,
        st_blocks: 0,
        st_atim: AndroidTimespec::default(),
        st_mtim: AndroidTimespec::default(),
        st_ctim: AndroidTimespec::default(),
        unused4: 0,
        unused5: 0,
    }
}

fn overlay_file_stat(file: &OverlayFile) -> AndroidStat {
    let size = file.data.len() as i64;
    AndroidStat {
        st_dev: 2,
        st_ino: file.inode,
        st_mode: file.mode,
        st_nlink: 1,
        st_uid: 0,
        st_gid: 0,
        st_rdev: 0,
        pad1: 0,
        st_size: size,
        st_blksize: 4096,
        pad2: 0,
        st_blocks: (size + 511) / 512,
        st_atim: AndroidTimespec::default(),
        st_mtim: AndroidTimespec::default(),
        st_ctim: AndroidTimespec::default(),
        unused4: 0,
        unused5: 0,
    }
}

fn overlay_directory_stat(directory: OverlayDirectory) -> AndroidStat {
    AndroidStat {
        st_dev: 2,
        st_ino: directory.inode,
        st_mode: directory.mode,
        st_nlink: 1,
        st_uid: 0,
        st_gid: 0,
        st_rdev: 0,
        pad1: 0,
        st_size: 0,
        st_blksize: 4096,
        pad2: 0,
        st_blocks: 0,
        st_atim: AndroidTimespec::default(),
        st_mtim: AndroidTimespec::default(),
        st_ctim: AndroidTimespec::default(),
        unused4: 0,
        unused5: 0,
    }
}

thread_local! {
    static ACTIVE: RefCell<Option<Arc<Facade>>> = const { RefCell::new(None) };
}

pub const PROCESS_OWNER_OK: c_int = 0;
pub const PROCESS_OWNER_INVALID_ARGUMENT: c_int = 1;
pub const PROCESS_OWNER_ALREADY_INSTALLED: c_int = 2;
pub const PROCESS_OWNER_CREATE_FAILED: c_int = 3;
pub const PROCESS_OWNER_NOT_INSTALLED: c_int = 4;
pub const PROCESS_OWNER_BUSY: c_int = 5;

// The process-wide owner, activation leases, and exported C ABI form one
// safety boundary. Keep it in the parent module's scope so the internal
// filesystem model stays private while the ABI lifecycle remains reviewable.
include!("process_api.rs");
