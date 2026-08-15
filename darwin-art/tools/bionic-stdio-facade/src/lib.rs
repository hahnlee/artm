#![forbid(unsafe_op_in_unsafe_fn)]
use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::ptr;
use std::slice;
use std::sync::{Arc, Mutex, OnceLock, RwLock};

const EOF: i32 = -1;
const EBADF: i32 = 9;
const EFAULT: i32 = 14;
const EINVAL: i32 = 22;
const EOVERFLOW: i32 = 75;
const EIO: i32 = 5;
const EFBIG: i32 = 27;
const MAX_STREAM_BYTES: usize = 16 * 1024 * 1024;

#[repr(C, align(8))]
pub struct AndroidFile {
    opaque: [u8; 152],
}
unsafe extern "C" {
    static mut darwin_art_bionic___sF: [AndroidFile; 3];
    fn darwin_art_bionic_errno_store(value: i32);
}

struct Stream {
    _token: Option<Box<AndroidFile>>,
    data: Vec<u8>,
    position: usize,
    readable: bool,
    writable: bool,
    pushback: Option<u8>,
    fd: i32,
}
struct Table {
    streams: BTreeMap<usize, Stream>,
    files: BTreeMap<Vec<u8>, Vec<u8>>,
    next_fd: i32,
}
pub struct Provider {
    table: Mutex<Table>,
}
static ACTIVE: OnceLock<RwLock<Option<Arc<Provider>>>> = OnceLock::new();
fn slot() -> &'static RwLock<Option<Arc<Provider>>> {
    ACTIVE.get_or_init(|| RwLock::new(None))
}

impl Provider {
    pub fn new(files: Vec<(Vec<u8>, Vec<u8>)>, stdin: Vec<u8>) -> Result<Self, &'static str> {
        if stdin.len() > MAX_STREAM_BYTES {
            return Err("invalid stdin snapshot");
        }
        let mut file_map = BTreeMap::new();
        for (path, data) in files {
            if path.is_empty()
                || path.contains(&0)
                || data.len() > MAX_STREAM_BYTES
                || file_map.insert(path, data).is_some()
            {
                return Err("invalid file snapshot");
            }
        }
        let base = ptr::addr_of_mut!(darwin_art_bionic___sF).cast::<AndroidFile>();
        let mut streams = BTreeMap::new();
        for index in 0..3 {
            // SAFETY: __sF is exactly three contiguous 152-byte aligned tokens.
            let token = unsafe { base.add(index) } as usize;
            streams.insert(
                token,
                Stream {
                    _token: None,
                    data: if index == 0 {
                        stdin.clone()
                    } else {
                        Vec::new()
                    },
                    position: 0,
                    readable: index == 0,
                    writable: index != 0,
                    pushback: None,
                    fd: index as i32,
                },
            );
        }
        Ok(Self {
            table: Mutex::new(Table {
                streams,
                files: file_map,
                next_fd: 20_000,
            }),
        })
    }
    pub fn activate(self: &Arc<Self>) -> Result<Activation, &'static str> {
        let mut active = slot().write().map_err(|_| "activation lock poisoned")?;
        if active.is_some() {
            return Err("stdio provider already active");
        }
        *active = Some(Arc::clone(self));
        Ok(Activation)
    }
    pub fn stdout_bytes(&self) -> Vec<u8> {
        let base = ptr::addr_of_mut!(darwin_art_bionic___sF).cast::<AndroidFile>() as usize;
        self.table
            .lock()
            .unwrap()
            .streams
            .get(&(base + 152))
            .map(|s| s.data.clone())
            .unwrap_or_default()
    }
}
pub struct Activation;
impl Drop for Activation {
    fn drop(&mut self) {
        if let Ok(mut active) = slot().write() {
            active.take();
        }
    }
}

fn errno(value: i32) {
    unsafe { darwin_art_bionic_errno_store(value) }
}
fn provider() -> Option<Arc<Provider>> {
    let value = slot().read().ok()?.clone();
    if value.is_none() {
        errno(EIO);
    }
    value
}
unsafe fn bytes<'a>(p: *const c_char) -> Option<&'a [u8]> {
    if p.is_null() {
        None
    } else {
        Some(unsafe { CStr::from_ptr(p) }.to_bytes())
    }
}
fn parse_mode(mode: &[u8]) -> Option<(bool, bool, bool)> {
    match mode {
        b"r" | b"rb" => Some((true, false, false)),
        b"r+" | b"r+b" | b"rb+" => Some((true, true, false)),
        b"w" | b"wb" => Some((false, true, true)),
        b"w+" | b"w+b" | b"wb+" => Some((true, true, true)),
        _ => None,
    }
}

unsafe fn fopen(path: *const c_char, mode: *const c_char) -> *mut AndroidFile {
    let (Some(path), Some(mode)) = (unsafe { bytes(path) }, unsafe { bytes(mode) }) else {
        errno(EFAULT);
        return ptr::null_mut();
    };
    if path.is_empty() {
        errno(2);
        return ptr::null_mut();
    }
    let Some((readable, writable, truncate)) = parse_mode(mode) else {
        errno(EINVAL);
        return ptr::null_mut();
    };
    let Some(provider) = provider() else {
        return ptr::null_mut();
    };
    let mut table = match provider.table.lock() {
        Ok(v) => v,
        Err(_) => {
            errno(EIO);
            return ptr::null_mut();
        }
    };
    let data = if truncate {
        Vec::new()
    } else {
        match table.files.get(path) {
            Some(v) => v.clone(),
            None => {
                errno(2);
                return ptr::null_mut();
            }
        }
    };
    let token = Box::new(AndroidFile { opaque: [0; 152] });
    let pointer = (&*token as *const AndroidFile).cast_mut();
    let fd = table.next_fd;
    table.next_fd += 1;
    table.streams.insert(
        pointer as usize,
        Stream {
            _token: Some(token),
            data,
            position: 0,
            readable,
            writable,
            pushback: None,
            fd,
        },
    );
    pointer
}
fn with_stream<T>(file: *mut AndroidFile, error: T, operation: impl FnOnce(&mut Stream) -> T) -> T
where
    T: Copy,
{
    let Some(provider) = provider() else {
        return error;
    };
    let mut table = match provider.table.lock() {
        Ok(v) => v,
        Err(_) => {
            errno(EIO);
            return error;
        }
    };
    let Some(stream) = table.streams.get_mut(&(file as usize)) else {
        errno(EBADF);
        return error;
    };
    operation(stream)
}

#[unsafe(no_mangle)]
/// # Safety
/// `p` and `m` must be readable NUL-terminated Android byte strings.
pub unsafe extern "C" fn darwin_art_bionic_stdio_fopen_core(
    p: *const c_char,
    m: *const c_char,
) -> *mut AndroidFile {
    unsafe { fopen(p, m) }
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_fclose_core(f: *mut AndroidFile) -> c_int {
    let Some(p) = provider() else { return EOF };
    let mut t = match p.table.lock() {
        Ok(v) => v,
        Err(_) => {
            errno(EIO);
            return EOF;
        }
    };
    if t.streams.remove(&(f as usize)).is_some() {
        0
    } else {
        errno(EBADF);
        EOF
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_fflush_core(f: *mut AndroidFile) -> c_int {
    if f.is_null() {
        if provider().is_some() { 0 } else { EOF }
    } else {
        with_stream(f, EOF, |_| 0)
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_fileno_core(f: *mut AndroidFile) -> c_int {
    with_stream(f, EOF, |s| s.fd)
}
#[unsafe(no_mangle)]
/// # Safety
/// For a nonzero byte product, `b` must be writable for `size * count` bytes.
pub unsafe extern "C" fn darwin_art_bionic_stdio_fread_core(
    b: *mut c_void,
    size: usize,
    count: usize,
    f: *mut AndroidFile,
) -> usize {
    let Some(total) = size.checked_mul(count) else {
        errno(EOVERFLOW);
        return 0;
    };
    if total == 0 {
        return 0;
    }
    if total > isize::MAX as usize {
        errno(EOVERFLOW);
        return 0;
    }
    if b.is_null() {
        errno(EFAULT);
        return 0;
    }
    with_stream(f, 0, |s| {
        if !s.readable {
            errno(EBADF);
            return 0;
        }
        let out = unsafe { slice::from_raw_parts_mut(b.cast::<u8>(), total) };
        let mut n = 0;
        if let Some(v) = s.pushback.take() {
            out[0] = v;
            n = 1
        }
        let available = s.data.len().saturating_sub(s.position);
        let take = (total - n).min(available);
        out[n..n + take].copy_from_slice(&s.data[s.position..s.position + take]);
        s.position += take;
        (n + take) / size
    })
}
#[unsafe(no_mangle)]
/// # Safety
/// For a nonzero byte product, `b` must be readable for `size * count` bytes.
pub unsafe extern "C" fn darwin_art_bionic_stdio_fwrite_core(
    b: *const c_void,
    size: usize,
    count: usize,
    f: *mut AndroidFile,
) -> usize {
    let Some(total) = size.checked_mul(count) else {
        errno(EOVERFLOW);
        return 0;
    };
    if total == 0 {
        return 0;
    }
    if total > isize::MAX as usize {
        errno(EOVERFLOW);
        return 0;
    }
    if total > MAX_STREAM_BYTES {
        errno(EFBIG);
        return 0;
    }
    if b.is_null() {
        errno(EFAULT);
        return 0;
    }
    let input = unsafe { slice::from_raw_parts(b.cast::<u8>(), total) };
    with_stream(f, 0, |s| {
        if !s.writable {
            errno(EBADF);
            return 0;
        }
        let Some(end) = s.position.checked_add(total) else {
            errno(EOVERFLOW);
            return 0;
        };
        if end > MAX_STREAM_BYTES {
            errno(EFBIG);
            return 0;
        }
        if s.position > s.data.len() {
            s.data.resize(s.position, 0)
        }
        if end > s.data.len() {
            s.data.resize(end, 0)
        }
        s.data[s.position..end].copy_from_slice(input);
        s.position = end;
        count
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_fseek_core(
    f: *mut AndroidFile,
    offset: i64,
    whence: c_int,
) -> c_int {
    with_stream(f, EOF, |s| {
        let base = match whence {
            0 => 0i128,
            1 => s.position as i128,
            2 => s.data.len() as i128,
            _ => {
                errno(EINVAL);
                return EOF;
            }
        };
        let value = base + offset as i128;
        if value < 0 || value > usize::MAX as i128 {
            errno(EINVAL);
            return EOF;
        }
        s.position = value as usize;
        s.pushback = None;
        0
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_ftello_core(f: *mut AndroidFile) -> i64 {
    with_stream(f, -1, |s| match i64::try_from(s.position) {
        Ok(v) => v,
        Err(_) => {
            errno(EOVERFLOW);
            -1
        }
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_fputc_core(c: c_int, f: *mut AndroidFile) -> c_int {
    let byte = (c & 255) as u8;
    with_stream(f, EOF, |s| {
        if !s.writable {
            errno(EBADF);
            return EOF;
        }
        let Some(end) = s.position.checked_add(1) else {
            errno(EOVERFLOW);
            return EOF;
        };
        if end > MAX_STREAM_BYTES {
            errno(EFBIG);
            return EOF;
        }
        if s.position > s.data.len() {
            s.data.resize(s.position, 0)
        }
        if s.position == s.data.len() {
            s.data.push(byte)
        } else {
            s.data[s.position] = byte
        }
        s.position = end;
        byte as i32
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_getc_core(f: *mut AndroidFile) -> c_int {
    with_stream(f, EOF, |s| {
        if !s.readable {
            errno(EBADF);
            return EOF;
        }
        if let Some(v) = s.pushback.take() {
            return v as i32;
        }
        if s.position >= s.data.len() {
            EOF
        } else {
            let v = s.data[s.position];
            s.position += 1;
            v as i32
        }
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_stdio_ungetc_core(c: c_int, f: *mut AndroidFile) -> c_int {
    if c == EOF {
        return EOF;
    }
    with_stream(f, EOF, |s| {
        if !s.readable || s.pushback.is_some() {
            return EOF;
        }
        let v = (c & 255) as u8;
        s.pushback = Some(v);
        v as i32
    })
}
