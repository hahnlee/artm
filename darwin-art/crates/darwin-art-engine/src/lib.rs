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
        DispatchPointerFn, PointerEvent, ProcessConfig, ProcessResult, ProviderAcquireFn,
        ProviderClearHooksFn, ProviderInstallHooksFn, ProviderNativeAcquireFn,
        ProviderNativeReleaseFn, ProviderReleaseFn, PumpFrameworkFrameFn, RunProcessFn,
        ShutdownProcessFn, SurfaceActiveFn, SurfaceCreateFn, SurfaceDestroyFn,
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

    /// Owner-thread surface handle. Its callback table and native handle stay
    /// paired until `close`, so RuntimeSession can drop it before EngineSession
    /// and never call into an unmapped engine image.
    pub struct SurfaceSession {
        handle: *mut c_void,
        update: SurfaceUpdateFn,
        present: SurfacePresentFn,
        pump_events: SurfacePumpEventsFn,
        next_pointer_event: SurfaceNextPointerEventFn,
        destroy: SurfaceDestroyFn,
        armed: bool,
    }

    impl SurfaceSession {
        fn from_parts(handle: *mut c_void, symbols: EngineSymbols) -> Self {
            Self {
                handle,
                update: symbols.surface_update,
                present: symbols.surface_present,
                pump_events: symbols.surface_pump_events,
                next_pointer_event: symbols.surface_next_pointer_event,
                destroy: symbols.surface_destroy,
                armed: true,
            }
        }

        pub fn handle(&self) -> *mut c_void {
            self.handle
        }

        pub fn active(symbols: EngineSymbols) -> Option<Self> {
            // SAFETY: callback belongs to the live engine image represented by
            // this symbol table.
            let handle = unsafe { (symbols.surface_active)() };
            (!handle.is_null()).then(|| Self::from_parts(handle, symbols))
        }

        pub fn create(
            symbols: EngineSymbols,
            info: &darwin_art_engine_sys::SurfaceCreateInfo,
        ) -> Result<Self, i32> {
            let mut status = -1;
            // SAFETY: info is a valid POD for the duration of this call.
            let handle = unsafe { (symbols.surface_create)(info, &mut status) };
            if handle.is_null() {
                Err(status)
            } else {
                Ok(Self::from_parts(handle, symbols))
            }
        }

        pub fn update_words(&self, pixels: &[u32]) -> i32 {
            let byte_count = pixels.len().saturating_mul(size_of::<u32>());
            // SAFETY: the callback and handle are paired and live; `pixels`
            // remains borrowed for the duration of the synchronous callback.
            unsafe { (self.update)(self.handle, pixels.as_ptr().cast(), byte_count) }
        }

        pub fn present(&self) -> i32 {
            // SAFETY: same invariant as update.
            unsafe { (self.present)(self.handle) }
        }

        pub fn pump_events(&self, visible_seconds: f64) -> i32 {
            // SAFETY: same invariant as update.
            unsafe { (self.pump_events)(self.handle, visible_seconds) }
        }

        pub fn next_pointer_event(&self, event: &mut PointerEvent) -> bool {
            // SAFETY: event is writable POD and the handle is live.
            unsafe { (self.next_pointer_event)(self.handle, event) }
        }

        pub fn close(&mut self) -> i32 {
            if !self.armed {
                return 0;
            }
            self.armed = false;
            // SAFETY: this is the one matching destroy call for the handle.
            unsafe { (self.destroy)(self.handle) }
        }
    }

    impl Drop for SurfaceSession {
        fn drop(&mut self) {
            let _ = self.close();
        }
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

        /// Run one process through the versioned ABI and construct its result
        /// in the same crate that owns the raw function pointer. The caller
        /// receives no partially initialized result on a nonzero status.
        pub fn run_process(&self, config: &ProcessConfig) -> Result<ProcessResult, i32> {
            if !config.is_compatible() {
                return Err(-1);
            }
            let mut result = ProcessResult::new();
            // SAFETY: `config` and all callback state it references are owned
            // by the caller for this synchronous invocation; the function
            // pointer belongs to this live EngineSession image.
            let status = unsafe { (self.engine.symbols.run_process)(config, &mut result) };
            if status == 0 { Ok(result) } else { Err(status) }
        }

        pub fn active_surface(&self) -> Option<SurfaceSession> {
            SurfaceSession::active(self.symbols())
        }

        /// Reports whether the graphics flavor has published a drawable
        /// surface without taking ownership of it. The non-graphics probe
        /// intentionally has no active surface and uses the diagnostic path;
        /// production graphics always publishes one before the host enters
        /// its frame loop.
        pub fn has_active_surface(&self) -> bool {
            // SAFETY: this is a read-only query on the live engine image.
            unsafe { !(self.engine.symbols.surface_active)().is_null() }
        }

        pub fn create_surface(
            &self,
            info: &darwin_art_engine_sys::SurfaceCreateInfo,
        ) -> Result<SurfaceSession, i32> {
            SurfaceSession::create(self.symbols(), info)
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

    impl Drop for EngineSession {
        fn drop(&mut self) {
            // A failed ownership transfer must not leave ART resident. Normal
            // RuntimeSession teardown marks this callback consumed first, so
            // Drop is idempotent in the successful path.
            let _ = self.shutdown_once();
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
pub use platform::{EngineSession, EngineSymbols, SurfaceSession};
