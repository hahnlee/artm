//! Process-scoped Darwin dynamic-library handle and typed symbol lookup.
//!
//! The host owns this boundary so the rest of the runner never performs raw
//! `dlopen`/`dlsym` calls. The handle is intentionally kept alive for the
//! process: ART installs process-global callbacks while the image is loaded.

use std::ffi::{CStr, CString, c_char, c_void};
use std::mem::{size_of, transmute_copy};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

pub(crate) struct DynamicLibrary(*mut c_void);

impl DynamicLibrary {
    pub(crate) fn open(path: &Path) -> Result<Self, String> {
        let path = CString::new(path.as_os_str().as_bytes())
            .map_err(|_| "dynamic-library path contains an interior NUL".to_owned())?;
        // SAFETY: path is NUL terminated and the flags are valid Darwin flags.
        let handle = unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        if handle.is_null() {
            Err(loader_error())
        } else {
            Ok(Self(handle))
        }
    }

    pub(crate) unsafe fn symbol<T: Copy>(&self, name: &'static [u8]) -> Result<T, String> {
        debug_assert_eq!(name.last(), Some(&0));
        // SAFETY: clearing and reading the loader error is required by dlsym's
        // contract; the symbol name is a static NUL-terminated byte string.
        unsafe { dlerror() };
        let symbol = unsafe { dlsym(self.0, name.as_ptr().cast()) };
        let error = unsafe { dlerror() };
        if !error.is_null() {
            return Err(unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() });
        }
        if size_of::<T>() != size_of::<*mut c_void>() {
            return Err("function pointer has an unexpected size".to_owned());
        }
        // SAFETY: callers choose T from the fixed ABI signature for `name`.
        Ok(unsafe { transmute_copy(&symbol) })
    }
}

fn loader_error() -> String {
    // SAFETY: dlerror returns either null or a process-owned C string.
    let error = unsafe { dlerror() };
    if error.is_null() {
        "unknown error".to_owned()
    } else {
        unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() }
    }
}

const RTLD_LOCAL: i32 = 0x4;
const RTLD_NOW: i32 = 0x2;

unsafe extern "C" {
    fn dlopen(path: *const c_char, mode: i32) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlerror() -> *const c_char;
}
