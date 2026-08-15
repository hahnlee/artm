#![forbid(unsafe_op_in_unsafe_fn)]

use darwin_art_fs_broker::{BrokerError, ReadOnlyBroker};
use darwin_art_prefix::{MountKind, MountTable, PrefixError, Resolution};
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fs::{File, Metadata};
use std::io::Read;
use std::marker::PhantomData;
use std::os::fd::IntoRawFd;
use std::os::unix::fs::MetadataExt;
use std::ptr;
use std::rc::Rc;
use std::slice;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

const AT_FDCWD: c_int = -100;
const O_ACCMODE: c_int = 3;
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
const ANDROID_ENOTDIR: i32 = 20;
const ANDROID_EINVAL: i32 = 22;
const ANDROID_EMFILE: i32 = 24;
const ANDROID_EROFS: i32 = 30;
const ANDROID_ERANGE: i32 = 34;
const ANDROID_EOPNOTSUPP: i32 = 95;
const DARWIN_ELOOP: i32 = 62;

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

struct DescriptorTable {
    next: c_int,
    files: BTreeMap<c_int, File>,
}

impl Default for DescriptorTable {
    fn default() -> Self {
        Self {
            next: 10_000,
            files: BTreeMap::new(),
        }
    }
}

impl DescriptorTable {
    fn insert(&mut self, file: File) -> Result<c_int, ()> {
        for _ in 0..100_000 {
            let candidate = self.next;
            self.next = if self.next == c_int::MAX {
                10_000
            } else {
                self.next + 1
            };
            if let std::collections::btree_map::Entry::Vacant(entry) = self.files.entry(candidate) {
                entry.insert(file);
                return Ok(candidate);
            }
        }
        Err(())
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
    directories: Mutex<DirectoryTable>,
    capability_failure: AtomicBool,
}

impl Facade {
    pub fn new(root: File, guest_mount: &[u8], cwd: &[u8]) -> Result<Self, &'static str> {
        let mut prefix = MountTable::new();
        prefix
            .add_mount(1, MountKind::Immutable, false, guest_mount)
            .map_err(|_| "invalid guest mount")?;
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
            directories: Mutex::new(DirectoryTable::default()),
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
            Ok(resolution) if resolution.mount_id == 1 && !resolution.writable => Ok(resolution),
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

    fn validate_flags(&self, flags: c_int) -> Result<(), c_int> {
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

    fn open(&self, path: &[u8], flags: c_int) -> c_int {
        if let Err(error) = self.validate_flags(flags) {
            return self.fail(error);
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
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
        match descriptors.insert(opened.into_file()) {
            Ok(fd) => fd,
            Err(()) => self.fail(ANDROID_EMFILE),
        }
    }

    fn openat(&self, directory_fd: c_int, path: &[u8], flags: c_int) -> c_int {
        if directory_fd == AT_FDCWD || path.starts_with(b"/") {
            return self.open(path, flags);
        }
        let descriptors = match self.descriptors.lock() {
            Ok(descriptors) => descriptors,
            Err(_) => return self.fail_capability(),
        };
        if descriptors.files.contains_key(&directory_fd) {
            self.fail(ANDROID_EOPNOTSUPP)
        } else {
            self.fail(ANDROID_EBADF)
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
        let Some(file) = descriptors.files.get_mut(&fd) else {
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
        match file.read(bytes) {
            Ok(read) => read as isize,
            Err(error) => self.fail_io(&error) as isize,
        }
    }

    fn close(&self, fd: c_int) -> c_int {
        let file = {
            let mut descriptors = match self.descriptors.lock() {
                Ok(descriptors) => descriptors,
                Err(_) => return self.fail_capability(),
            };
            let Some(file) = descriptors.files.remove(&fd) else {
                return self.fail(ANDROID_EBADF);
            };
            file
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
        let Some(file) = descriptors.files.get(&fd) else {
            return self.fail(ANDROID_EBADF);
        };
        let metadata = match file.metadata() {
            Ok(metadata) => metadata,
            Err(error) => return self.fail_io(&error),
        };
        let translated = metadata_to_android(&metadata);
        // SAFETY: the guest ABI requires a writable 128-byte Android stat object.
        unsafe { status.write(translated) };
        0
    }

    unsafe fn stat(&self, path: &[u8], status: *mut AndroidStat, no_follow: bool) -> c_int {
        if status.is_null() {
            return self.fail(ANDROID_EFAULT);
        }
        let resolution = match self.resolve(path) {
            Ok(resolution) => resolution,
            Err(error) => return self.fail(error),
        };
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

thread_local! {
    static ACTIVE: RefCell<Option<Arc<Facade>>> = const { RefCell::new(None) };
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
    let facade = ACTIVE.with(|active| active.borrow().clone());
    match facade {
        Some(facade) => operation(&facade),
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
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.open(path, flags))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a readable NUL-terminated Android byte path.
pub unsafe extern "C" fn darwin_art_bionic_fs_openat_core(
    directory_fd: c_int,
    path: *const c_char,
    flags: c_int,
) -> c_int {
    let Some(path) = (unsafe { path_bytes(path) }) else {
        Facade::set_android_errno(ANDROID_EFAULT);
        return -1;
    };
    with_active(-1, |facade| facade.openat(directory_fd, path, flags))
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
pub extern "C" fn darwin_art_bionic_fs_readdir_core(directory: *mut c_void) -> *mut AndroidDirent {
    with_active(ptr::null_mut(), |facade| facade.readdir(directory))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_fs_closedir_core(directory: *mut c_void) -> c_int {
    with_active(-1, |facade| facade.closedir(directory))
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
