use std::ffi::{CStr, CString, c_char, c_void};
use std::mem::{size_of, transmute_copy};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

use darwin_art_engine_sys::{
    GraphicsSessionCloseFn, GraphicsSessionCreateFn, GraphicsSessionDestroyFn,
    GraphicsSessionDispatchPointerFn, GraphicsSessionPumpFrameFn, ProviderClearHooksFn,
    ProviderInstallHooksFn, ProviderNativeAcquireFn, ProviderNativeReleaseFn, RunProcessFn,
    ShutdownProcessFn, SurfaceActiveFn, SurfaceCreateFn, SurfaceDestroyFn,
    SurfaceNextPointerEventFn, SurfacePresentFn, SurfacePumpEventsFn, SurfaceUpdateFn,
};

#[derive(Clone, Copy)]
pub(crate) struct EngineSymbols {
    pub run_process: RunProcessFn,
    pub shutdown_process: ShutdownProcessFn,
    pub surface_create: SurfaceCreateFn,
    pub surface_update: SurfaceUpdateFn,
    pub surface_present: SurfacePresentFn,
    pub surface_pump_events: SurfacePumpEventsFn,
    pub surface_next_pointer_event: SurfaceNextPointerEventFn,
    pub surface_destroy: SurfaceDestroyFn,
    pub surface_active: SurfaceActiveFn,
    pub graphics_session_create: Option<GraphicsSessionCreateFn>,
    pub graphics_session_close: Option<GraphicsSessionCloseFn>,
    pub graphics_session_destroy: Option<GraphicsSessionDestroyFn>,
    pub graphics_session_dispatch_pointer: Option<GraphicsSessionDispatchPointerFn>,
    pub graphics_session_pump_frame: Option<GraphicsSessionPumpFrameFn>,
    pub provider_install_hooks: ProviderInstallHooksFn,
    pub provider_clear_hooks: ProviderClearHooksFn,
    pub provider_native_acquire: ProviderNativeAcquireFn,
    pub provider_native_release: ProviderNativeReleaseFn,
}

pub(crate) struct LoadedEngine {
    _library: DynamicLibrary,
    symbols: EngineSymbols,
}

impl LoadedEngine {
    pub(crate) fn open(path: &Path) -> Result<Self, String> {
        let library = DynamicLibrary::open(path)?;
        // SAFETY: Every name and type is fixed by the Darwin ART v1 ABI.
        let symbols = unsafe {
            EngineSymbols {
                run_process: library.symbol(b"darwin_art_run_process\0")?,
                shutdown_process: library.symbol(b"darwin_art_shutdown_process\0")?,
                surface_create: library.symbol(b"darwin_art_surface_create\0")?,
                surface_update: library.symbol(b"darwin_art_surface_update\0")?,
                surface_present: library.symbol(b"darwin_art_surface_present\0")?,
                surface_pump_events: library.symbol(b"darwin_art_surface_pump_events\0")?,
                surface_next_pointer_event: library
                    .symbol(b"darwin_art_surface_next_pointer_event\0")?,
                surface_destroy: library.symbol(b"darwin_art_surface_destroy\0")?,
                surface_active: library.symbol(b"darwin_art_surface_active_gpu\0")?,
                graphics_session_create: library
                    .symbol(b"darwin_art_graphics_session_create\0")
                    .ok(),
                graphics_session_close: library.symbol(b"darwin_art_graphics_session_close\0").ok(),
                graphics_session_destroy: library
                    .symbol(b"darwin_art_graphics_session_destroy\0")
                    .ok(),
                graphics_session_dispatch_pointer: library
                    .symbol(b"darwin_art_graphics_session_dispatch_pointer\0")
                    .ok(),
                graphics_session_pump_frame: library
                    .symbol(b"darwin_art_graphics_session_pump_frame\0")
                    .ok(),
                provider_install_hooks: library.symbol(b"darwin_art_provider_install_hooks\0")?,
                provider_clear_hooks: library.symbol(b"darwin_art_provider_clear_hooks\0")?,
                provider_native_acquire: library.symbol(b"darwin_art_provider_native_acquire\0")?,
                provider_native_release: library.symbol(b"darwin_art_provider_native_release\0")?,
            }
        };
        Ok(Self {
            _library: library,
            symbols,
        })
    }

    pub(crate) fn symbols(&self) -> EngineSymbols {
        self.symbols
    }
}

struct DynamicLibrary(*mut c_void);

impl DynamicLibrary {
    fn open(path: &Path) -> Result<Self, String> {
        let path = CString::new(path.as_os_str().as_bytes())
            .map_err(|_| "dynamic-library path contains an interior NUL".to_owned())?;
        // SAFETY: path is NUL terminated and flags are valid Darwin flags.
        let handle = unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        if handle.is_null() {
            Err(loader_error())
        } else {
            Ok(Self(handle))
        }
    }

    unsafe fn symbol<T: Copy>(&self, name: &'static [u8]) -> Result<T, String> {
        debug_assert_eq!(name.last(), Some(&0));
        // SAFETY: clearing and reading the loader error is required by dlsym.
        unsafe { dlerror() };
        let symbol = unsafe { dlsym(self.0, name.as_ptr().cast()) };
        let error = unsafe { dlerror() };
        if !error.is_null() {
            return Err(unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() });
        }
        if size_of::<T>() != size_of::<*mut c_void>() {
            return Err("function pointer has an unexpected size".to_owned());
        }
        // SAFETY: T is the fixed function-pointer type associated with name.
        Ok(unsafe { transmute_copy(&symbol) })
    }
}

fn loader_error() -> String {
    // SAFETY: dlerror returns a process-owned C string.
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
