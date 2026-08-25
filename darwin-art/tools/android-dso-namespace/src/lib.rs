//! A deliberately closed Android DSO namespace for the Darwin ELF loader.
//!
//! This is not a Darwin `dlsym(RTLD_DEFAULT, ...)` adapter. Every visible
//! SONAME and symbol is listed below, including its Android GNU symbol version.

use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr;
use std::sync::OnceLock;

pub const PROVIDER_LOADER_LIBDL: u32 = 1;
pub const PROVIDER_AOSP_LIBLOG: u32 = 2;

#[repr(C)]
pub struct AndroidDlExtInfo {
    pub flags: u64,
    pub reserved_addr: *mut c_void,
    pub reserved_size: usize,
    pub relro_fd: c_int,
    pub library_fd: c_int,
    pub library_fd_offset: i64,
    pub library_namespace: *mut c_void,
}

pub type OpenCallback = unsafe extern "C" fn(
    context: *mut c_void,
    filename: *const c_char,
    flags: c_int,
    extinfo: *const AndroidDlExtInfo,
    error: *mut c_char,
    error_capacity: usize,
) -> *mut c_void;
pub type LookupCallback = unsafe extern "C" fn(
    context: *mut c_void,
    handle: *mut c_void,
    symbol: *const c_char,
    version: *const c_char,
    error: *mut c_char,
    error_capacity: usize,
) -> *mut c_void;
pub type CloseCallback = unsafe extern "C" fn(
    context: *mut c_void,
    handle: *mut c_void,
    error: *mut c_char,
    error_capacity: usize,
) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LoaderCallbacks {
    pub context: *mut c_void,
    pub open: Option<OpenCallback>,
    pub lookup: Option<LookupCallback>,
    pub close: Option<CloseCallback>,
}

// The embedding loader owns the context lifetime and binds exactly once before
// any app thread can enter libdl. Access is read-only after OnceLock publication.
unsafe impl Send for LoaderCallbacks {}
unsafe impl Sync for LoaderCallbacks {}

static LOADER: OnceLock<LoaderCallbacks> = OnceLock::new();
static mut LIBANDROID_HANDLE_TOKEN: u8 = 0;

#[cfg(not(test))]
unsafe extern "C" {
    #[link_name = "darwin_art_android_ANativeWindow_fromSurface"]
    fn host_anative_window_from_surface(env: *mut c_void, surface: *mut c_void) -> *mut c_void;
    #[link_name = "darwin_art_android_ANativeWindow_release"]
    fn host_anative_window_release(window: *mut c_void);
    #[link_name = "darwin_art_android_ANativeWindow_lock"]
    fn host_anative_window_lock(
        window: *mut c_void,
        buffer: *mut c_void,
        bounds: *mut c_void,
    ) -> c_int;
    #[link_name = "darwin_art_android_ANativeWindow_unlockAndPost"]
    fn host_anative_window_unlock_and_post(window: *mut c_void) -> c_int;
    #[link_name = "darwin_art_android_ANativeWindow_setBuffersGeometry"]
    fn host_anative_window_set_buffers_geometry(
        window: *mut c_void,
        width: c_int,
        height: c_int,
        format: c_int,
    ) -> c_int;
}

fn libandroid_handle() -> *mut c_void {
    ptr::addr_of_mut!(LIBANDROID_HANDLE_TOKEN).cast()
}

unsafe fn libandroid_symbol(name: &CStr) -> *mut c_void {
    #[cfg(test)]
    {
        let _ = name;
        return 1usize as *mut c_void;
    }
    #[cfg(not(test))]
    {
        match name.to_bytes() {
            b"ANativeWindow_fromSurface" => host_anative_window_from_surface as *mut c_void,
            b"ANativeWindow_release" => host_anative_window_release as *mut c_void,
            b"ANativeWindow_lock" => host_anative_window_lock as *mut c_void,
            b"ANativeWindow_unlockAndPost" => host_anative_window_unlock_and_post as *mut c_void,
            b"ANativeWindow_setBuffersGeometry" => {
                host_anative_window_set_buffers_geometry as *mut c_void
            }
            _ => ptr::null_mut(),
        }
    }
}

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
    static ERROR_RETURN: RefCell<Option<CString>> = const { RefCell::new(None) };
}

const ERROR_CAPACITY: usize = 512;

fn set_error(message: impl AsRef<str>) {
    let bytes = message.as_ref().as_bytes();
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    let value = CString::new(&bytes[..end]).expect("interior NUL removed");
    LAST_ERROR.with(|slot| *slot.borrow_mut() = Some(value));
}

fn call_error(buffer: &[c_char]) -> String {
    let bytes: Vec<u8> = buffer
        .iter()
        .take_while(|byte| **byte != 0)
        .map(|byte| *byte as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

#[no_mangle]
/// Binds the process-wide loader callbacks exactly once.
///
/// # Safety
///
/// `callbacks` must point to a readable `LoaderCallbacks`. Its context must
/// remain valid for the process lifetime and its function pointers must obey
/// their declared C ABIs.
pub unsafe extern "C" fn darwin_art_loader_bind(callbacks: *const LoaderCallbacks) -> c_int {
    if callbacks.is_null() {
        return -1;
    }
    // SAFETY: null was rejected and the value is copied immediately.
    let callbacks = unsafe { *callbacks };
    if callbacks.open.is_none() || callbacks.lookup.is_none() || callbacks.close.is_none() {
        return -1;
    }
    if LOADER.set(callbacks).is_err() {
        -1
    } else {
        0
    }
}

fn loader() -> Option<&'static LoaderCallbacks> {
    let value = LOADER.get();
    if value.is_none() {
        set_error("Android ELF loader is not bound");
    }
    value
}

#[no_mangle]
/// Opens an Android DSO through the bound loader.
///
/// # Safety
///
/// `filename` must be null or point to a valid NUL-terminated C string.
pub unsafe extern "C" fn darwin_art_bionic_dlopen(
    filename: *const c_char,
    flags: c_int,
) -> *mut c_void {
    darwin_art_bionic_android_dlopen_ext(filename, flags, ptr::null())
}

#[no_mangle]
/// Opens an Android DSO with Android extended-loader parameters.
///
/// # Safety
///
/// `filename` must be null or a valid C string. A non-null `extinfo` must
/// point to a readable `AndroidDlExtInfo` for the duration of the callback.
pub unsafe extern "C" fn darwin_art_bionic_android_dlopen_ext(
    filename: *const c_char,
    flags: c_int,
    extinfo: *const AndroidDlExtInfo,
) -> *mut c_void {
    if !filename.is_null() {
        let name = unsafe { CStr::from_ptr(filename) }.to_bytes();
        if name.rsplit(|byte| *byte == b'/').next() == Some(b"libandroid.so") {
            if std::env::var_os("DARWIN_ART_DEBUG_ANATIVEWINDOW").is_some() {
                eprintln!("ART Android libdl: opened virtual libandroid.so");
            }
            return libandroid_handle();
        }
    }
    let Some(loader) = loader() else {
        return ptr::null_mut();
    };
    let mut error = [0 as c_char; ERROR_CAPACITY];
    let result = unsafe {
        (loader.open.expect("validated"))(
            loader.context,
            filename,
            flags,
            extinfo,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if result.is_null() {
        let message = call_error(&error);
        set_error(if message.is_empty() {
            "Android dlopen failed"
        } else {
            &message
        });
    }
    result
}

#[no_mangle]
/// Looks up an exported symbol in a loader-owned handle.
///
/// # Safety
///
/// `handle` must be accepted by the bound loader and `symbol` must point to a
/// valid NUL-terminated C string.
pub unsafe extern "C" fn darwin_art_bionic_dlsym(
    handle: *mut c_void,
    symbol: *const c_char,
) -> *mut c_void {
    if symbol.is_null() {
        set_error("dlsym symbol is null");
        return ptr::null_mut();
    }
    if handle == libandroid_handle() {
        let name = unsafe { CStr::from_ptr(symbol) };
        let result = unsafe { libandroid_symbol(name) };
        if std::env::var_os("DARWIN_ART_DEBUG_ANATIVEWINDOW").is_some() {
            eprintln!(
                "ART Android libdl: libandroid dlsym {} resolved={}",
                name.to_string_lossy(),
                !result.is_null()
            );
        }
        if result.is_null() {
            set_error("libandroid.so symbol is unsupported");
        }
        return result;
    }
    let Some(loader) = loader() else {
        return ptr::null_mut();
    };
    let mut error = [0 as c_char; ERROR_CAPACITY];
    let result = unsafe {
        (loader.lookup.expect("validated"))(
            loader.context,
            handle,
            symbol,
            ptr::null(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if result.is_null() {
        let message = call_error(&error);
        set_error(if message.is_empty() {
            "Android dlsym failed"
        } else {
            &message
        });
    }
    result
}

#[no_mangle]
/// Releases a loader-owned DSO handle.
///
/// # Safety
///
/// `handle` must be null or a handle accepted by the bound loader.
pub unsafe extern "C" fn darwin_art_bionic_dlclose(handle: *mut c_void) -> c_int {
    if handle == libandroid_handle() {
        return 0;
    }
    let Some(loader) = loader() else { return -1 };
    let mut error = [0 as c_char; ERROR_CAPACITY];
    let result = unsafe {
        (loader.close.expect("validated"))(loader.context, handle, error.as_mut_ptr(), error.len())
    };
    if result != 0 {
        let message = call_error(&error);
        set_error(if message.is_empty() {
            "Android dlclose failed"
        } else {
            &message
        });
        -1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn darwin_art_bionic_dlerror() -> *mut c_char {
    let pending = LAST_ERROR.with(|slot| slot.borrow_mut().take());
    ERROR_RETURN.with(|slot| {
        *slot.borrow_mut() = pending;
        slot.borrow()
            .as_ref()
            .map_or(ptr::null_mut(), |message| message.as_ptr() as *mut c_char)
    })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Resolution {
    pub provider: u32,
    pub ordinal: u32,
    pub address: usize,
}

const LIBDL: &[(&str, &str)] = &[
    ("android_dlopen_ext", "LIBC"),
    ("dlclose", "LIBC"),
    ("dlerror", "LIBC"),
    ("dlopen", "LIBC"),
    ("dlsym", "LIBC"),
];

// Exact NDK r28 API 35 liblog stub surface. An address of zero means that the
// ELF loader must obtain the address from the linked AOSP liblog provider table,
// never from Darwin's process-global namespace.
const LIBLOG: &[&str] = &[
    "__android_log_assert",
    "__android_log_buf_print",
    "__android_log_buf_write",
    "__android_log_call_aborter",
    "__android_log_default_aborter",
    "__android_log_get_minimum_priority",
    "__android_log_is_loggable",
    "__android_log_is_loggable_len",
    "__android_log_logd_logger",
    "__android_log_print",
    "__android_log_set_aborter",
    "__android_log_set_default_tag",
    "__android_log_set_logger",
    "__android_log_set_minimum_priority",
    "__android_log_stderr_logger",
    "__android_log_vprint",
    "__android_log_write",
    "__android_log_write_log_message",
];

pub fn resolve(soname: &str, symbol: &str, version: Option<&str>) -> Result<Resolution, u32> {
    match soname {
        "libdl.so" => {
            let Some((ordinal, (_, expected))) = LIBDL
                .iter()
                .enumerate()
                .find(|(_, entry)| entry.0 == symbol)
            else {
                return Err(2);
            };
            if version.is_some_and(|actual| actual != *expected) {
                return Err(3);
            }
            let address = match ordinal {
                0 => darwin_art_bionic_android_dlopen_ext as usize,
                1 => darwin_art_bionic_dlclose as usize,
                2 => darwin_art_bionic_dlerror as usize,
                3 => darwin_art_bionic_dlopen as usize,
                4 => darwin_art_bionic_dlsym as usize,
                _ => unreachable!(),
            };
            Ok(Resolution {
                provider: PROVIDER_LOADER_LIBDL,
                ordinal: ordinal as u32,
                address,
            })
        }
        "liblog.so" => {
            let Some(ordinal) = LIBLOG.iter().position(|candidate| *candidate == symbol) else {
                return Err(2);
            };
            // The API 35 NDK stub deliberately has no GNU version definitions.
            if version.is_some() {
                return Err(3);
            }
            Ok(Resolution {
                provider: PROVIDER_AOSP_LIBLOG,
                ordinal: ordinal as u32,
                address: 0,
            })
        }
        _ => Err(1),
    }
}

#[repr(C)]
pub struct CResolution {
    pub provider: u32,
    pub ordinal: u32,
    pub address: usize,
}

#[no_mangle]
/// Resolves an allowlisted virtual Android DSO symbol.
///
/// # Safety
///
/// `soname` and `symbol` must be valid C strings, `version` must be null or a
/// valid C string, and `output` must point to writable `CResolution` storage.
pub unsafe extern "C" fn darwin_art_dso_resolve(
    soname: *const c_char,
    symbol: *const c_char,
    version: *const c_char,
    output: *mut CResolution,
) -> c_int {
    if soname.is_null() || symbol.is_null() || output.is_null() {
        return 4;
    }
    let Ok(soname) = unsafe { CStr::from_ptr(soname) }.to_str() else {
        return 4;
    };
    let Ok(symbol) = unsafe { CStr::from_ptr(symbol) }.to_str() else {
        return 4;
    };
    let version = if version.is_null() {
        None
    } else {
        let Ok(value) = unsafe { CStr::from_ptr(version) }.to_str() else {
            return 4;
        };
        Some(value)
    };
    match resolve(soname, symbol, version) {
        Ok(result) => {
            unsafe {
                *output = CResolution {
                    provider: result.provider,
                    ordinal: result.ordinal,
                    address: result.address,
                }
            };
            0
        }
        Err(code) => code as c_int,
    }
}

pub fn manifest() -> String {
    let mut output = String::new();
    for (symbol, version) in LIBDL {
        output.push_str(&format!("libdl.so\t{symbol}\t{version}\tloader\n"));
    }
    for symbol in LIBLOG {
        output.push_str(&format!("liblog.so\t{symbol}\t-\taosp-liblog\n"));
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn namespace_is_closed_and_versioned() {
        unsafe extern "C" fn open(
            _: *mut c_void,
            filename: *const c_char,
            _: c_int,
            _: *const AndroidDlExtInfo,
            error: *mut c_char,
            _: usize,
        ) -> *mut c_void {
            if !filename.is_null() && unsafe { CStr::from_ptr(filename) }.to_bytes() == b"bad.so" {
                let message = b"virtual SONAME denied\0";
                unsafe {
                    ptr::copy_nonoverlapping(
                        message.as_ptr() as *const c_char,
                        error,
                        message.len(),
                    )
                };
                ptr::null_mut()
            } else {
                0x1234usize as *mut c_void
            }
        }
        unsafe extern "C" fn lookup(
            _: *mut c_void,
            _: *mut c_void,
            _: *const c_char,
            _: *const c_char,
            _: *mut c_char,
            _: usize,
        ) -> *mut c_void {
            0x5678usize as *mut c_void
        }
        unsafe extern "C" fn close(
            _: *mut c_void,
            _: *mut c_void,
            _: *mut c_char,
            _: usize,
        ) -> c_int {
            0
        }

        assert_eq!(
            resolve("libdl.so", "dlopen", Some("LIBC"))
                .unwrap()
                .provider,
            1
        );
        assert_eq!(resolve("libdl.so", "dlopen", Some("GLIBC_2.0")), Err(3));
        assert_eq!(resolve("libc.so", "malloc", Some("LIBC")), Err(1));
        assert_eq!(
            resolve("liblog.so", "__android_log_print", None)
                .unwrap()
                .provider,
            2
        );
        assert_eq!(
            resolve("liblog.so", "__android_log_print", Some("LIBLOG")),
            Err(3)
        );
        assert_eq!(resolve("liblog.so", "android_log_private", None), Err(2));
        assert_eq!(std::mem::size_of::<AndroidDlExtInfo>(), 48);

        let callbacks = LoaderCallbacks {
            context: ptr::null_mut(),
            open: Some(open),
            lookup: Some(lookup),
            close: Some(close),
        };
        assert_eq!(unsafe { darwin_art_loader_bind(&callbacks) }, 0);
        let good = CString::new("good.so").unwrap();
        let symbol = CString::new("fixture_symbol").unwrap();
        let handle = unsafe { darwin_art_bionic_dlopen(good.as_ptr(), 2) };
        assert_eq!(handle as usize, 0x1234);
        assert_eq!(
            unsafe { darwin_art_bionic_dlsym(handle, symbol.as_ptr()) } as usize,
            0x5678
        );
        assert_eq!(unsafe { darwin_art_bionic_dlclose(handle) }, 0);
        assert!(darwin_art_bionic_dlerror().is_null());

        let bad = CString::new("bad.so").unwrap();
        assert!(unsafe { darwin_art_bionic_dlopen(bad.as_ptr(), 2) }.is_null());
        let error = darwin_art_bionic_dlerror();
        assert_eq!(
            unsafe { CStr::from_ptr(error) }.to_bytes(),
            b"virtual SONAME denied"
        );
        assert!(darwin_art_bionic_dlerror().is_null());
    }
}
