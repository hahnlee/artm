//! Safe owner for the process-scoped Darwin engine image.
//!
//! Raw dynamic loading and typed symbol conversion live here. Callers receive
//! a copyable symbol table whose lifetime is tied to this owner. The image is
//! intentionally not unloaded: ART installs process-global callbacks while it
//! is resident and the current ABI has not proven that all callbacks are
//! restored before DestroyJavaVM.

#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(target_os = "macos")]
mod platform {
    use std::ffi::{CStr, CString, c_char, c_void};
    use std::mem::{size_of, transmute_copy};
    use std::os::unix::ffi::OsStrExt;
    use std::path::Path;

    use darwin_art_engine_sys::{
        DispatchPointerFn, ProviderAcquireFn, ProviderClearHooksFn, ProviderInstallHooksFn,
        ProviderNativeAcquireFn, ProviderNativeReleaseFn, ProviderReleaseFn, PumpFrameworkFrameFn,
        RunProcessFn, ShutdownProcessFn, SurfaceActiveFn, SurfaceCreateFn, SurfaceDestroyFn,
        SurfaceNextPointerEventFn, SurfacePresentFn, SurfacePumpEventsFn, SurfaceUpdateFn,
    };

    #[derive(Clone, Copy)]
    pub struct EngineSymbols {
        pub run_process: RunProcessFn,
        pub shutdown_process: ShutdownProcessFn,
        pub surface_create: SurfaceCreateFn,
        pub surface_update: SurfaceUpdateFn,
        pub surface_present: SurfacePresentFn,
        pub surface_pump_events: SurfacePumpEventsFn,
        pub surface_next_pointer_event: SurfaceNextPointerEventFn,
        pub surface_destroy: SurfaceDestroyFn,
        pub surface_active: SurfaceActiveFn,
        pub dispatch_pointer: DispatchPointerFn,
        pub pump_framework_frame: PumpFrameworkFrameFn,
        pub provider_install_hooks: ProviderInstallHooksFn,
        pub provider_clear_hooks: ProviderClearHooksFn,
        pub provider_native_acquire: ProviderNativeAcquireFn,
        pub provider_native_release: ProviderNativeReleaseFn,
    }

    struct LoadedEngine {
        _library: DynamicLibrary,
        symbols: EngineSymbols,
    }

    impl LoadedEngine {
        fn open(path: &Path) -> Result<Self, String> {
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
                    dispatch_pointer: library.symbol(b"darwin_art_dispatch_pointer\0")?,
                    pump_framework_frame: library.symbol(b"darwin_art_pump_framework_frame\0")?,
                    provider_install_hooks: library
                        .symbol(b"darwin_art_provider_install_hooks\0")?,
                    provider_clear_hooks: library.symbol(b"darwin_art_provider_clear_hooks\0")?,
                    provider_native_acquire: library
                        .symbol(b"darwin_art_provider_native_acquire\0")?,
                    provider_native_release: library
                        .symbol(b"darwin_art_provider_native_release\0")?,
                }
            };
            Ok(Self {
                _library: library,
                symbols,
            })
        }

        pub fn symbols(&self) -> EngineSymbols {
            self.symbols
        }
    }

    /// Process-scoped engine owner.  The dynamic library and its shutdown
    /// callback share one Rust lifetime, so callers cannot accidentally drop
    /// the symbol image before ART has been shut down.
    pub struct EngineSession {
        engine: LoadedEngine,
        shutdown_taken: bool,
    }

    impl EngineSession {
        pub fn open(path: &Path) -> Result<Self, String> {
            Ok(Self {
                engine: LoadedEngine::open(path)?,
                shutdown_taken: false,
            })
        }

        pub fn symbols(&self) -> EngineSymbols {
            self.engine.symbols()
        }

        /// # Safety
        ///
        /// `context` and both callbacks must remain valid until
        /// `clear_provider_hooks` is called. The callbacks may run on an ART
        /// thread during native-library graph loading.
        pub unsafe fn install_provider_hooks(
            &self,
            context: *mut c_void,
            acquire: Option<ProviderAcquireFn>,
            release: Option<ProviderReleaseFn>,
        ) {
            // SAFETY: the callback context is owned by the caller for the
            // entire engine session, and the function pointer table belongs
            // to this live dynamic image.
            unsafe { (self.engine.symbols.provider_install_hooks)(context, acquire, release) }
        }

        pub fn clear_provider_hooks(&self) {
            // SAFETY: the hook table is process-global and this owner is the
            // same image that installed it.
            unsafe { (self.engine.symbols.provider_clear_hooks)() }
        }

        /// Invoke the process shutdown callback at most once.  The callback
        /// is kept behind this owner so its code image remains mapped for the
        /// entire call and until the owner is dropped afterward.
        pub fn shutdown_once(&mut self) -> i32 {
            if self.shutdown_taken {
                return 0;
            }
            self.shutdown_taken = true;
            // SAFETY: the function pointer was resolved from this live,
            // version-checked engine image and takes no arguments.
            unsafe { (self.engine.symbols.shutdown_process)() }
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
}

#[cfg(target_os = "macos")]
pub use platform::{EngineSession, EngineSymbols};
