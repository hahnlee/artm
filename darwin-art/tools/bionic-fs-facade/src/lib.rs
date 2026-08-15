#![forbid(unsafe_op_in_unsafe_fn)]

use darwin_art_fs_broker::{BrokerError, ReadOnlyBroker};
use darwin_art_prefix::{MountKind, MountTable, PrefixError, Resolution};
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fs::{File, Metadata};
use std::io::Read;
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, IntoRawFd};
use std::os::unix::fs::FileExt;
use std::os::unix::fs::MetadataExt;
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
    fn darwin_art_bionic_fs_host_fdopendir(fd: c_int, host_errno: *mut c_int) -> *mut c_void;
    fn darwin_art_bionic_fs_host_readdir(
        directory: *mut c_void,
        entry: *mut HostDirent,
        host_errno: *mut c_int,
    ) -> c_int;
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
            self.next = if self.next == c_int::MAX {
                10_000
            } else {
                self.next + 1
            };
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
        Ok(Self {
            prefix,
            broker,
            cwd: Mutex::new(initial_cwd.normalized_path),
            descriptors: Mutex::new(DescriptorTable::default()),
            overlay: Mutex::new(OverlayState::default()),
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
            Ok(_) | Err(PrefixError::NoMount) => Err(ANDROID_EACCES),
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

    fn random_device(path: &[u8]) -> Option<RandomDeviceKind> {
        match path {
            b"/dev/random" => Some(RandomDeviceKind::Random),
            b"/dev/urandom" => Some(RandomDeviceKind::Urandom),
            _ => None,
        }
    }

    fn open_random(&self, kind: RandomDeviceKind, flags: c_int) -> c_int {
        if flags & O_ACCMODE != O_RDONLY
            || flags & WRITE_FLAGS != 0
            || flags & O_TMPFILE == O_TMPFILE
        {
            return self.fail(ANDROID_EROFS);
        }
        if flags != O_RDONLY {
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
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
        if resolution.mount_id == 2 {
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
            Err(error) => return self.fail_broker(&error),
        };
        if flags & O_DIRECTORY != 0 && !opened.metadata().is_dir() {
            return self.fail(ANDROID_ENOTDIR);
        }
        let mut descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        match descriptors.insert(Descriptor::File(opened.into_file())) {
            Ok(fd) => fd,
            Err(()) => self.fail(ANDROID_EMFILE),
        }
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
            Some(Descriptor::File(_)) => self.fail(ANDROID_EOPNOTSUPP),
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
            Descriptor::File(file) => match file.read(bytes) {
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
        let Descriptor::File(file) = descriptor else {
            return 0;
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
            Descriptor::File(file) => {
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
        let mut overlay = match self.overlay.lock() {
            Ok(overlay) => overlay,
            Err(_) => return self.fail_capability(),
        };
        if overlay.entries.contains_key(&resolution.relative_path) {
            return self.fail(ANDROID_EEXIST);
        }
        if !matches!(
            overlay
                .entries
                .get(Self::overlay_parent(&resolution.relative_path)),
            Some(OverlayEntry::Directory(_))
        ) {
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
        0
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
            Some(Descriptor::File(_)) => Err(ANDROID_EOPNOTSUPP),
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
        let opened = match self.broker.open(&resolution.relative_path) {
            Ok(opened) => opened,
            Err(error) => {
                self.fail_broker(&error);
                return ptr::null_mut();
            }
        };
        if !opened.metadata().is_dir() {
            self.fail(ANDROID_ENOTDIR);
            return ptr::null_mut();
        }
        let raw_fd = opened.into_file().into_raw_fd();
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
        let Descriptor::File(file) = descriptor else {
            self.fail(ANDROID_ENOTDIR);
            return ptr::null_mut();
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
        let Descriptor::File(file) = descriptors
            .take(fd)
            .expect("validated virtual descriptor must remain present under its lock")
        else {
            unreachable!("validated descriptor kind changed under lock")
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
            descriptors.restore(fd, Descriptor::File(restored));
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
            let Descriptor::Overlay(output_descriptor) = &mut output else {
                return Err(ANDROID_EROFS);
            };
            if !output_descriptor.writable {
                return Err(ANDROID_EBADF);
            }
            let mut output_file = output_descriptor.node.lock().map_err(|_| ANDROID_EIO)?;
            let input = descriptors
                .entries
                .get_mut(&request.input_fd)
                .ok_or(ANDROID_EBADF)?;
            if let Descriptor::Overlay(input_descriptor) = input
                && Arc::ptr_eq(&input_descriptor.node, &output_descriptor.node)
            {
                return Err(ANDROID_EINVAL);
            }
            let wanted = request.count.min(64 * 1024);
            let mut bytes = vec![0_u8; wanted];
            let read = match input {
                Descriptor::File(file) if request.has_explicit_offset != 0 => file
                    .read_at(&mut bytes, request.offset as u64)
                    .map_err(|_| ANDROID_EIO)?,
                Descriptor::File(file) => file.read(&mut bytes).map_err(|_| ANDROID_EIO)?,
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

#[derive(Default)]
struct ProcessOwnerState {
    facade: Option<Arc<Facade>>,
    installing: bool,
    draining: bool,
    in_flight: usize,
}

struct ProcessOwner {
    state: Mutex<ProcessOwnerState>,
    quiescent: Condvar,
}

static PROCESS_OWNER: LazyLock<ProcessOwner> = LazyLock::new(|| ProcessOwner {
    state: Mutex::new(ProcessOwnerState::default()),
    quiescent: Condvar::new(),
});

fn process_owner_state() -> MutexGuard<'static, ProcessOwnerState> {
    PROCESS_OWNER.state.lock().unwrap_or_else(|_| {
        // The phase and in-flight count cannot be reconstructed after a panic
        // while holding this lifecycle lock.
        std::process::abort()
    })
}

struct ActiveFacade {
    facade: Arc<Facade>,
    process_lease: bool,
}

impl Drop for ActiveFacade {
    fn drop(&mut self) {
        if !self.process_lease {
            return;
        }
        let mut state = process_owner_state();
        assert!(state.in_flight > 0, "process owner lease underflow");
        state.in_flight -= 1;
        if state.in_flight == 0 {
            PROCESS_OWNER.quiescent.notify_all();
        }
    }
}

fn acquire_active() -> Option<ActiveFacade> {
    if let Some(facade) = ACTIVE.with(|active| active.borrow().clone()) {
        return Some(ActiveFacade {
            facade,
            process_lease: false,
        });
    }
    let mut state = process_owner_state();
    if state.installing || state.draining {
        return None;
    }
    let facade = state.facade.as_ref()?.clone();
    state.in_flight = state
        .in_flight
        .checked_add(1)
        .expect("process owner lease count overflow");
    Some(ActiveFacade {
        facade,
        process_lease: true,
    })
}

#[cfg(test)]
fn publish_process_facade(facade: Arc<Facade>) -> c_int {
    let mut state = process_owner_state();
    if state.facade.is_some() || state.installing || state.draining {
        return PROCESS_OWNER_ALREADY_INSTALLED;
    }
    state.facade = Some(facade);
    PROCESS_OWNER_OK
}

#[unsafe(no_mangle)]
/// Installs one process-wide facade from a caller-authorized directory fd.
///
/// # Safety
///
/// Each nonempty byte slice must be readable for its supplied length. The
/// caller must keep `root_fd` live until this call returns; the facade owns an
/// immediate duplicate and never consumes the caller's descriptor.
pub unsafe extern "C" fn darwin_art_bionic_fs_process_install(
    root_fd: c_int,
    guest_mount: *const u8,
    guest_mount_length: usize,
    cwd: *const u8,
    cwd_length: usize,
) -> c_int {
    if root_fd < 0
        || guest_mount.is_null()
        || guest_mount_length == 0
        || cwd.is_null()
        || cwd_length == 0
    {
        return PROCESS_OWNER_INVALID_ARGUMENT;
    }
    {
        let mut state = process_owner_state();
        if state.facade.is_some() || state.installing || state.draining {
            return PROCESS_OWNER_ALREADY_INSTALLED;
        }
        state.installing = true;
    }

    // SAFETY: the descriptor lifetime contract is stated above. Duplication
    // completes before the caller may reclaim its descriptor.
    let duplicate = unsafe { BorrowedFd::borrow_raw(root_fd) }.try_clone_to_owned();
    // SAFETY: non-null and nonempty arguments were validated above.
    let guest_mount = unsafe { slice::from_raw_parts(guest_mount, guest_mount_length) };
    // SAFETY: same byte-slice contract as guest_mount.
    let cwd = unsafe { slice::from_raw_parts(cwd, cwd_length) };
    let facade = duplicate
        .ok()
        .and_then(|fd| Facade::new(File::from(fd), guest_mount, cwd).ok())
        .map(Arc::new);

    let mut state = process_owner_state();
    assert!(state.installing, "process owner install reservation lost");
    state.installing = false;
    match facade {
        Some(facade) => {
            assert!(state.facade.is_none() && !state.draining);
            state.facade = Some(facade);
            PROCESS_OWNER.quiescent.notify_all();
            PROCESS_OWNER_OK
        }
        None => {
            PROCESS_OWNER.quiescent.notify_all();
            PROCESS_OWNER_CREATE_FAILED
        }
    }
}

#[unsafe(no_mangle)]
/// Stops new process-owner calls, drains every active guest call, then drops
/// all facade-owned descriptors and broker authority.
pub extern "C" fn darwin_art_bionic_fs_process_uninstall() -> c_int {
    let facade = {
        let mut state = process_owner_state();
        if state.installing || state.draining {
            return PROCESS_OWNER_BUSY;
        }
        if state.facade.is_none() {
            return PROCESS_OWNER_NOT_INSTALLED;
        }
        state.draining = true;
        PROCESS_OWNER.quiescent.notify_all();
        while state.in_flight != 0 {
            state = PROCESS_OWNER
                .quiescent
                .wait(state)
                .unwrap_or_else(|_| std::process::abort());
        }
        state.facade.take()
    };
    drop(facade);
    let mut state = process_owner_state();
    assert!(state.facade.is_none() && state.draining && state.in_flight == 0);
    state.draining = false;
    PROCESS_OWNER.quiescent.notify_all();
    PROCESS_OWNER_OK
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_process_has_capability_failure() -> c_int {
    acquire_active().map_or(-1, |active| {
        c_int::from(active.facade.has_capability_failure())
    })
}

pub const IOCTL_FD_INFO_ABI_VERSION: u32 = 1;
pub const IOCTL_FD_OTHER: c_int = 0;
pub const IOCTL_FD_RANDOM_DEVICE: c_int = 1;
pub const IOCTL_FD_FOUND: c_int = 0;
pub const IOCTL_FD_BAD: c_int = 1;
pub const IOCTL_FD_CAPABILITY_UNAVAILABLE: c_int = 2;

pub const SENDFILE_ABI_VERSION: u32 = 1;
pub const SENDFILE_TRANSFER_OK: c_int = 0;
pub const SENDFILE_TRANSFER_BAD_FD: c_int = 1;
pub const SENDFILE_TRANSFER_UNAVAILABLE: c_int = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SendfileRequest {
    pub abi_version: u32,
    pub output_fd: c_int,
    pub input_fd: c_int,
    pub has_explicit_offset: u32,
    pub offset: i64,
    pub count: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SendfileResult {
    pub abi_version: u32,
    pub android_errno: c_int,
    pub transferred: isize,
    pub next_offset: i64,
}

const _: () = assert!(size_of::<SendfileRequest>() == 32);
const _: () = assert!(size_of::<SendfileResult>() == 24);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct IoctlFdInfo {
    pub abi_version: u32,
    pub kind: c_int,
}

const _: () = assert!(size_of::<IoctlFdInfo>() == 8);

#[unsafe(no_mangle)]
/// # Safety
///
/// `info` must point to a writable `DarwinArtBionicIoctlFdInfo`.
pub unsafe extern "C" fn darwin_art_bionic_fs_ioctl_fd_lookup(
    _context: *mut c_void,
    fd: c_int,
    info: *mut IoctlFdInfo,
) -> c_int {
    if info.is_null() {
        return IOCTL_FD_CAPABILITY_UNAVAILABLE;
    }
    let Some(active) = acquire_active() else {
        return IOCTL_FD_CAPABILITY_UNAVAILABLE;
    };
    let descriptors = match active.facade.descriptors.lock() {
        Ok(descriptors) => descriptors,
        Err(_) => {
            active
                .facade
                .capability_failure
                .store(true, Ordering::Release);
            return IOCTL_FD_CAPABILITY_UNAVAILABLE;
        }
    };
    let kind = match descriptors.entries.get(&fd) {
        Some(Descriptor::Random(_)) => IOCTL_FD_RANDOM_DEVICE,
        Some(Descriptor::File(_) | Descriptor::Overlay(_)) => IOCTL_FD_OTHER,
        None => return IOCTL_FD_BAD,
    };
    // SAFETY: required by this callback ABI; the ioctl provider passes its
    // stack-local record and consumes it before releasing the callback lease.
    unsafe {
        info.write(IoctlFdInfo {
            abi_version: IOCTL_FD_INFO_ABI_VERSION,
            kind,
        })
    };
    IOCTL_FD_FOUND
}

#[unsafe(no_mangle)]
/// Atomic transfer callback for the standalone sendfile provider.
///
/// # Safety
///
/// `request` must be readable and `result` writable for their complete ABI
/// records. The sendfile provider retains neither pointer after this call.
pub unsafe extern "C" fn darwin_art_bionic_fs_sendfile_transfer(
    _context: *mut c_void,
    request: *const SendfileRequest,
    result: *mut SendfileResult,
) -> c_int {
    if request.is_null() || result.is_null() {
        return SENDFILE_TRANSFER_UNAVAILABLE;
    }
    let Some(active) = acquire_active() else {
        return SENDFILE_TRANSFER_UNAVAILABLE;
    };
    // SAFETY: required by the callback ABI and consumed synchronously.
    let request = unsafe { &*request };
    // SAFETY: same callback lifetime contract as request.
    let result = unsafe { &mut *result };
    active.facade.sendfile_transfer(request, result)
}

/// A thread-affine activation scope.
///
/// Forgetting a guard leaks an `Arc` into that pthread rather than leaving a
/// dangling pointer. Moving the guard to another thread is rejected:
///
/// ```compile_fail
/// use bionic_fs_facade::Activation;
/// fn require_send<T: Send>() {}
/// require_send::<Activation>();
/// ```
pub struct Activation {
    previous: Option<Arc<Facade>>,
    _not_send: PhantomData<Rc<()>>,
}

impl Drop for Activation {
    fn drop(&mut self) {
        ACTIVE.with(|active| {
            active.replace(self.previous.take());
        });
    }
}

fn with_active<T>(default: T, operation: impl FnOnce(&Facade) -> T) -> T {
    match acquire_active() {
        Some(active) => operation(&active.facade),
        None => {
            Facade::set_android_errno(ANDROID_EINVAL);
            default
        }
    }
}

unsafe fn path_bytes<'a>(path: *const c_char) -> Option<&'a [u8]> {
    if path.is_null() {
        return None;
    }
    // SAFETY: the Android libc ABI requires a readable NUL-terminated path.
    Some(unsafe { CStr::from_ptr(path) }.to_bytes())
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_open_core(
    path: *const c_char,
    flags: c_int,
    mode: u32,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.open_with_mode(path, flags, mode))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_openat_core(
    directory_fd: c_int,
    path: *const c_char,
    flags: c_int,
    mode: u32,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.openat_with_mode(directory_fd, path, flags, mode)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// For nonzero `count`, `buffer` must be writable for `count` bytes.
pub unsafe extern "C" fn darwin_art_bionic_fs_read_core(
    fd: c_int,
    buffer: *mut c_void,
    count: usize,
) -> isize {
    with_active(-1, |facade| unsafe { facade.read(fd, buffer, count) })
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_close_core(fd: c_int) -> c_int {
    with_active(-1, |facade| facade.close(fd))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `status` must point to a writable 128-byte Android arm64 `struct stat`.
pub unsafe extern "C" fn darwin_art_bionic_fs_fstat_core(
    fd: c_int,
    status: *mut AndroidStat,
) -> c_int {
    with_active(-1, |facade| unsafe { facade.fstat(fd, status) })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be a readable NUL-terminated byte path and `status` must point
/// to a writable 128-byte Android arm64 `struct stat`.
pub unsafe extern "C" fn darwin_art_bionic_fs_stat_core(
    path: *const c_char,
    status: *mut AndroidStat,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| unsafe { facade.stat(path, status, false) })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// Same pointer contract as [`darwin_art_bionic_fs_stat_core`]. Symlink
/// metadata is unsupported by the no-follow broker and fails explicitly.
pub unsafe extern "C" fn darwin_art_bionic_fs_lstat_core(
    path: *const c_char,
    status: *mut AndroidStat,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| unsafe { facade.stat(path, status, true) })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be readable and NUL-terminated. For nonzero `size`, `buffer`
/// must be writable for that many bytes; this broker currently always rejects
/// the link-content capability.
pub unsafe extern "C" fn darwin_art_bionic_fs_readlink_core(
    path: *const c_char,
    buffer: *mut c_char,
    size: usize,
) -> isize {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.readlink(path, buffer, size))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `buffer` must be writable for `size` bytes. Null allocation mode is an
/// explicit unsupported capability.
pub unsafe extern "C" fn darwin_art_bionic_fs_getcwd_core(
    buffer: *mut c_char,
    size: usize,
) -> *mut c_char {
    with_active(ptr::null_mut(), |facade| unsafe {
        facade.getcwd(buffer, size)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be a readable NUL-terminated byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_chdir_core(path: *const c_char) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.chdir(path))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be a readable NUL-terminated byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_opendir_core(path: *const c_char) -> *mut c_void {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return ptr::null_mut();
    };
    with_active(ptr::null_mut(), |facade| facade.opendir(path))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_fdopendir_core(fd: c_int) -> *mut c_void {
    with_active(ptr::null_mut(), |facade| facade.fdopendir(fd))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_readdir_core(directory: *mut c_void) -> *mut AndroidDirent {
    with_active(ptr::null_mut(), |facade| facade.readdir(directory))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_closedir_core(directory: *mut c_void) -> c_int {
    with_active(-1, |facade| facade.closedir(directory))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_fchmod_core(fd: c_int, _mode: u32) -> c_int {
    with_active(-1, |facade| facade.fchmod(fd, _mode))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_fchmodat_core(
    directory_fd: c_int,
    path: *const c_char,
    _mode: u32,
    flags: c_int,
) -> c_int {
    if flags != 0 && flags != AT_SYMLINK_NOFOLLOW {
        Facade::set_android_errno(ANDROID_EINVAL);
        return -1;
    }
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.reject_at_path_mutation(directory_fd, path)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_ftruncate_core(fd: c_int, _length: i64) -> c_int {
    with_active(-1, |facade| facade.ftruncate(fd, _length))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_isatty_core(fd: c_int) -> c_int {
    with_active(0, |facade| facade.isatty(fd))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// Both paths must point to readable NUL-terminated Android byte paths.
pub unsafe extern "C" fn darwin_art_bionic_fs_link_core(
    old_path: *const c_char,
    new_path: *const c_char,
) -> c_int {
    let Some(old_path) = (unsafe { path_bytes(old_path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    let Some(new_path) = (unsafe { path_bytes(new_path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.reject_two_path_mutation(old_path, new_path)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_mkdir_core(path: *const c_char, _mode: u32) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.mkdir(path, _mode))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_pathconf_core(
    path: *const c_char,
    name: c_int,
) -> i64 {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.pathconf(path, name))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be readable and NUL terminated. A non-null `resolved` must be
/// writable for Android `PATH_MAX` bytes.
pub unsafe extern "C" fn darwin_art_bionic_fs_realpath_core(
    path: *const c_char,
    resolved: *mut c_char,
) -> *mut c_char {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return ptr::null_mut();
    };
    with_active(ptr::null_mut(), |facade| unsafe {
        facade.realpath(path, resolved)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_remove_core(path: *const c_char) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.reject_path_mutation(path))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// Both paths must point to readable NUL-terminated Android byte paths.
pub unsafe extern "C" fn darwin_art_bionic_fs_rename_core(
    old_path: *const c_char,
    new_path: *const c_char,
) -> c_int {
    let Some(old_path) = (unsafe { path_bytes(old_path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    let Some(new_path) = (unsafe { path_bytes(new_path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.reject_two_path_mutation(old_path, new_path)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be readable and NUL terminated; `status` must point to a
/// writable 112-byte Android arm64 `struct statvfs`.
pub unsafe extern "C" fn darwin_art_bionic_fs_statvfs_core(
    path: *const c_char,
    status: *mut AndroidStatvfs,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| unsafe { facade.statvfs(path, status) })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// Both arguments must point to readable NUL-terminated byte strings. The
/// target is never interpreted because immutable mounts reject link creation.
pub unsafe extern "C" fn darwin_art_bionic_fs_symlink_core(
    target: *const c_char,
    link_path: *const c_char,
) -> c_int {
    if (unsafe { path_bytes(target) }).is_none() {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    }
    let Some(link_path) = (unsafe { path_bytes(link_path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.reject_path_mutation(link_path))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_truncate_core(
    path: *const c_char,
    _length: i64,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.reject_path_mutation(path))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_unlinkat_core(
    directory_fd: c_int,
    path: *const c_char,
    flags: c_int,
) -> c_int {
    if flags != 0 && flags != AT_REMOVEDIR {
        Facade::set_android_errno(ANDROID_EINVAL);
        return -1;
    }
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.reject_at_path_mutation(directory_fd, path)
    })
}

#[unsafe(no_mangle)]
/// # Safety
///
/// A non-null `path` must be readable and NUL terminated. `times` may be null;
/// otherwise the Android ABI requires two readable timespec records, which are
/// deliberately not consumed by the immutable-mount rejection.
pub unsafe extern "C" fn darwin_art_bionic_fs_utimensat_core(
    directory_fd: c_int,
    path: *const c_char,
    _times: *const AndroidTimespec,
    flags: c_int,
) -> c_int {
    if flags != 0 && flags != AT_SYMLINK_NOFOLLOW {
        Facade::set_android_errno(ANDROID_EINVAL);
        return -1;
    }
    if path.is_null() {
        return with_active(-1, |facade| facade.reject_fd_mutation(directory_fd));
    }
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| {
        facade.reject_at_path_mutation(directory_fd, path)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Barrier, Condvar};
    use std::thread;
    use std::time::Duration;

    static PROCESS_TEST_LOCK: Mutex<()> = Mutex::new(());

    unsafe extern "C" {
        fn __error() -> *mut i32;
        fn darwin_art_bionic_errno_load() -> i32;
    }

    #[test]
    fn unknown_darwin_errno_publishes_eio_and_marks_capability_failure() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        Facade::set_android_errno(2);
        // SAFETY: Darwin returns the current pthread's host errno cell.
        unsafe { *__error() = 33_002 };

        assert_eq!(
            facade.fail_io(&std::io::Error::from_raw_os_error(123_456)),
            -1
        );
        assert!(facade.has_capability_failure());
        // SAFETY: the standalone errno provider is linked for this test binary.
        assert_eq!(unsafe { darwin_art_bionic_errno_load() }, ANDROID_EIO);
        // SAFETY: same pthread-local host errno cell set above.
        assert_eq!(unsafe { *__error() }, 33_002);
    }

    #[test]
    fn repeated_opendir_closedir_reclaims_side_table_records() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        for _ in 0..512 {
            let directory = facade.opendir(b"/system");
            assert!(!directory.is_null());
            assert_eq!(facade.closedir(directory), 0);
        }
        assert!(facade.directories.lock().unwrap().streams.is_empty());
    }

    struct BlockingEntropy {
        state: Mutex<(bool, bool)>,
        condition: Condvar,
    }

    impl BlockingEntropy {
        fn new() -> Self {
            Self {
                state: Mutex::new((false, false)),
                condition: Condvar::new(),
            }
        }

        fn wait_until_entered(&self) {
            let state = self.state.lock().unwrap();
            drop(
                self.condition
                    .wait_while(state, |(entered, _)| !*entered)
                    .unwrap(),
            );
        }

        fn release(&self) {
            let mut state = self.state.lock().unwrap();
            state.1 = true;
            self.condition.notify_all();
        }
    }

    impl EntropyBackend for BlockingEntropy {
        fn fill(&self, bytes: &mut [u8]) -> Result<(), ()> {
            let mut state = self.state.lock().unwrap();
            state.0 = true;
            self.condition.notify_all();
            state = self
                .condition
                .wait_while(state, |(_, release)| !*release)
                .unwrap();
            drop(state);
            bytes.fill(0xa5);
            Ok(())
        }
    }

    #[test]
    fn random_read_serializes_close_and_reuses_descriptor_without_stale_kind() {
        let entropy = Arc::new(BlockingEntropy::new());
        let facade = Arc::new(
            Facade::new_with_entropy(
                File::open("/").unwrap(),
                b"/system",
                b"/system",
                entropy.clone(),
            )
            .unwrap(),
        );
        let fd = facade.open(b"/dev/random", O_RDONLY);
        assert!(fd >= 10_000);

        let reader_facade = facade.clone();
        let reader = thread::spawn(move || {
            let mut bytes = [0_u8; 32];
            // SAFETY: the local output remains writable for the whole call.
            let result = unsafe { reader_facade.read(fd, bytes.as_mut_ptr().cast(), bytes.len()) };
            (result, bytes)
        });
        entropy.wait_until_entered();

        let close_done = Arc::new(AtomicBool::new(false));
        let close_facade = facade.clone();
        let close_done_thread = close_done.clone();
        let closer = thread::spawn(move || {
            let result = close_facade.close(fd);
            close_done_thread.store(true, Ordering::Release);
            result
        });
        thread::sleep(Duration::from_millis(20));
        assert!(!close_done.load(Ordering::Acquire));
        entropy.release();
        let (read, bytes) = reader.join().unwrap();
        assert_eq!(read, 32);
        assert_eq!(bytes, [0xa5; 32]);
        assert_eq!(closer.join().unwrap(), 0);

        let reused = facade.open(b"/dev/urandom", O_RDONLY);
        assert_eq!(reused, fd);
        assert_eq!(facade.close(reused), 0);
    }

    #[test]
    fn ioctl_kind_lookup_is_atomic_with_close() {
        let facade =
            Arc::new(Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap());
        let fd = facade.open(b"/dev/random", O_RDONLY);
        let barrier = Arc::new(Barrier::new(9));
        let mut workers = Vec::new();
        for _ in 0..8 {
            let worker_facade = facade.clone();
            let worker_barrier = barrier.clone();
            workers.push(thread::spawn(move || {
                let _activation = worker_facade.activate();
                worker_barrier.wait();
                for _ in 0..2_000 {
                    let mut info = IoctlFdInfo::default();
                    // SAFETY: info remains writable through this synchronous callback.
                    let status = unsafe {
                        darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info)
                    };
                    assert!(status == IOCTL_FD_FOUND || status == IOCTL_FD_BAD);
                    if status == IOCTL_FD_FOUND {
                        assert_eq!(info.kind, IOCTL_FD_RANDOM_DEVICE);
                    }
                }
            }));
        }
        barrier.wait();
        assert_eq!(facade.close(fd), 0);
        for worker in workers {
            worker.join().unwrap();
        }
    }

    #[test]
    fn private_data_overlay_persists_and_sendfile_preserves_offset_rules() {
        let project = File::open(concat!(env!("CARGO_MANIFEST_DIR"), "/../..")).unwrap();
        let facade = Facade::new(project, b"/", b"/").unwrap();
        assert_eq!(facade.mkdir(b"/data/cache", 0o755), 0);
        let input = facade.open(b"/Cargo.toml", O_RDONLY);
        let output = facade.open(b"/data/cache/copy", O_WRONLY | O_CREAT | O_TRUNC);
        assert!(input >= 10_000 && output >= 10_000);

        let mut result = SendfileResult::default();
        let request = SendfileRequest {
            abi_version: SENDFILE_ABI_VERSION,
            output_fd: output,
            input_fd: input,
            has_explicit_offset: 0,
            offset: 0,
            count: 5,
        };
        assert_eq!(
            facade.sendfile_transfer(&request, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!(result.transferred, 5);
        assert_eq!(facade.close(output), 0);

        let persisted = facade.open(b"/data/cache/copy", O_RDONLY);
        let mut first = [0_u8; 5];
        assert_eq!(
            unsafe { facade.read(persisted, first.as_mut_ptr().cast(), 5) },
            5
        );
        assert_eq!(&first, b"[work");
        assert_eq!(facade.close(persisted), 0);

        let offset_output = facade.open(b"/data/offset", O_WRONLY | O_CREAT | O_TRUNC);
        let explicit = SendfileRequest {
            output_fd: offset_output,
            has_explicit_offset: 1,
            offset: 1,
            count: 3,
            ..request
        };
        assert_eq!(
            facade.sendfile_transfer(&explicit, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!((result.transferred, result.next_offset), (3, 4));
        let current = SendfileRequest {
            output_fd: offset_output,
            count: 1,
            ..request
        };
        assert_eq!(
            facade.sendfile_transfer(&current, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!(result.transferred, 1);
        assert_eq!(facade.close(offset_output), 0);
        let offset_copy = facade.open(b"/data/offset", O_RDONLY);
        let mut bytes = [0_u8; 4];
        assert_eq!(
            unsafe { facade.read(offset_copy, bytes.as_mut_ptr().cast(), 4) },
            4
        );
        assert_eq!(&bytes, b"wors");
        assert_eq!(facade.close(offset_copy), 0);

        let truncate = facade.open(b"/data/cache/copy", O_WRONLY | O_TRUNC);
        assert_eq!(facade.ftruncate(truncate, 2), 0);
        let mut status = AndroidStat::default();
        assert_eq!(unsafe { facade.fstat(truncate, &mut status) }, 0);
        assert_eq!(status.st_size, 2);
        assert_eq!(facade.close(truncate), 0);
        assert_eq!(facade.open(b"/Cargo.toml", O_WRONLY | O_TRUNC), -1);
    }

    #[test]
    fn process_owner_cross_thread_rollback_duplicate_and_quiescent_uninstall() {
        let _serial = PROCESS_TEST_LOCK.lock().unwrap();
        assert_eq!(
            darwin_art_bionic_fs_process_uninstall(),
            PROCESS_OWNER_NOT_INSTALLED
        );
        let root = File::open("/").unwrap();
        // SAFETY: all byte slices and the borrowed directory fd remain live
        // through each synchronous installation attempt.
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    ptr::null(),
                    0,
                )
            },
            PROCESS_OWNER_INVALID_ARGUMENT
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/darwin-art-missing-process-owner-cwd".as_ptr(),
                    b"/darwin-art-missing-process-owner-cwd".len(),
                )
            },
            PROCESS_OWNER_CREATE_FAILED
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/".as_ptr(),
                    1,
                )
            },
            PROCESS_OWNER_OK
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/".as_ptr(),
                    1,
                )
            },
            PROCESS_OWNER_ALREADY_INSTALLED
        );
        let worker = thread::spawn(|| {
            // SAFETY: static paths and local output records remain live for
            // each call. This pthread has no TLS Activation guard.
            let fd =
                unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) };
            assert!(fd >= 10_000);
            let mut bytes = [0_u8; 16];
            assert_eq!(
                unsafe {
                    darwin_art_bionic_fs_read_core(fd, bytes.as_mut_ptr().cast(), bytes.len())
                },
                bytes.len() as isize
            );
            let mut info = IoctlFdInfo::default();
            assert_eq!(
                unsafe { darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info) },
                IOCTL_FD_FOUND
            );
            assert_eq!(info.kind, IOCTL_FD_RANDOM_DEVICE);
            assert_eq!(darwin_art_bionic_fs_close_core(fd), 0);
        });
        worker.join().unwrap();
        assert_eq!(darwin_art_bionic_fs_process_has_capability_failure(), 0);
        assert_eq!(darwin_art_bionic_fs_process_uninstall(), PROCESS_OWNER_OK);

        let entropy = Arc::new(BlockingEntropy::new());
        let facade = Arc::new(
            Facade::new_with_entropy(File::open("/").unwrap(), b"/", b"/", entropy.clone())
                .unwrap(),
        );
        assert_eq!(publish_process_facade(facade), PROCESS_OWNER_OK);
        // SAFETY: static path remains live for the call.
        let fd = unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) };
        let reader = thread::spawn(move || {
            let mut bytes = [0_u8; 32];
            // SAFETY: local output remains writable through the call.
            unsafe { darwin_art_bionic_fs_read_core(fd, bytes.as_mut_ptr().cast(), bytes.len()) }
        });
        entropy.wait_until_entered();
        let uninstall_done = Arc::new(AtomicBool::new(false));
        let uninstall_done_thread = uninstall_done.clone();
        let uninstaller = thread::spawn(move || {
            let status = darwin_art_bionic_fs_process_uninstall();
            uninstall_done_thread.store(true, Ordering::Release);
            status
        });
        {
            let state = process_owner_state();
            drop(
                PROCESS_OWNER
                    .quiescent
                    .wait_while(state, |state| !state.draining)
                    .unwrap_or_else(std::sync::PoisonError::into_inner),
            );
        }
        assert!(!uninstall_done.load(Ordering::Acquire));
        assert_eq!(darwin_art_bionic_fs_process_uninstall(), PROCESS_OWNER_BUSY);
        // New calls fail closed once draining begins; they cannot extend the
        // lifetime being awaited by uninstall.
        assert_eq!(
            unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) },
            -1
        );
        entropy.release();
        assert_eq!(reader.join().unwrap(), 32);
        assert_eq!(uninstaller.join().unwrap(), PROCESS_OWNER_OK);
        assert_eq!(darwin_art_bionic_fs_process_has_capability_failure(), -1);
        let mut info = IoctlFdInfo::default();
        assert_eq!(
            unsafe { darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info) },
            IOCTL_FD_CAPABILITY_UNAVAILABLE
        );
        assert_eq!(
            darwin_art_bionic_fs_process_uninstall(),
            PROCESS_OWNER_NOT_INSTALLED
        );
    }
}
