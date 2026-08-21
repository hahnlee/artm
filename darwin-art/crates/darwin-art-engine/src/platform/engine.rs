use super::abi::{EngineSymbols, LoadedEngine};
use super::graphics::GraphicsSession;
use super::surface::SurfaceSession;
use core::ffi::c_void;
use darwin_art_engine_sys::{
    ProcessConfig, ProcessResult, ProviderAcquireFn, ProviderClearHooksFn, ProviderNativeAcquireFn,
    ProviderNativeReleaseFn, ProviderReleaseFn,
};
use std::path::Path;

/// Safe provider-hook view owned by the live `EngineSession` image.
///
/// The raw function-pointer table stays private to this crate. Runtime code
/// receives this narrow capability instead of a copy of every engine ABI
/// symbol. `RuntimeSession` drops the provider before the engine, preserving
/// the image-lifetime invariant for these callbacks.
#[derive(Clone, Copy)]
pub struct ProviderHooks {
    acquire: ProviderNativeAcquireFn,
    release: ProviderNativeReleaseFn,
    clear: ProviderClearHooksFn,
}

impl ProviderHooks {
    pub fn acquire(&self, kind: u32, authority_fd: i32) -> i32 {
        // SAFETY: this view is produced by a live EngineSession and the
        // RuntimeSession teardown order releases provider hooks first.
        unsafe { (self.acquire)(kind, authority_fd) }
    }

    pub fn release(&self, kind: u32) -> i32 {
        // SAFETY: same engine-image lifetime invariant as acquire.
        unsafe { (self.release)(kind) }
    }

    pub fn clear(&self) {
        // SAFETY: clear is called only after ProviderLeaseTable reaches
        // quiescence and before the owning EngineSession is dropped.
        unsafe { (self.clear)() }
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

    pub(crate) fn symbols(&self) -> EngineSymbols {
        self.engine.symbols()
    }

    pub fn provider_hooks(&self) -> ProviderHooks {
        let symbols = self.engine.symbols();
        ProviderHooks {
            acquire: symbols.provider.native_acquire,
            release: symbols.provider.native_release,
            clear: symbols.provider.clear_hooks,
        }
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
        let status = unsafe { (self.engine.symbols().process.run_process)(config, &mut result) };
        if status == 0 { Ok(result) } else { Err(status) }
    }

    pub fn active_surface(&self) -> Option<SurfaceSession> {
        SurfaceSession::active(self.symbols())
    }

    pub fn create_graphics_session(&self) -> Result<GraphicsSession, i32> {
        GraphicsSession::create(self.symbols())
    }

    /// Reports whether the graphics flavor has published a drawable
    /// surface without taking ownership of it. The non-graphics probe
    /// intentionally has no active surface and uses the diagnostic path;
    /// production graphics always publishes one before the host enters
    /// its frame loop.
    pub fn has_active_surface(&self) -> bool {
        // SAFETY: this is a read-only query on the live engine image.
        unsafe { !(self.engine.symbols().surface.active)().is_null() }
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
        unsafe { (self.engine.symbols().provider.install_hooks)(context, acquire, release) }
    }

    pub fn clear_provider_hooks(&self) {
        // SAFETY: the hook table is process-global and this owner is the
        // same image that installed it.
        unsafe { (self.engine.symbols().provider.clear_hooks)() }
    }

    /// Close the process-scoped engine at most once. The callback is kept
    /// behind this owner so its code image remains mapped for the entire
    /// call and until the owner is dropped afterward.
    pub fn close(&mut self) -> i32 {
        if self.shutdown_taken {
            return 0;
        }
        self.shutdown_taken = true;
        // SAFETY: the function pointer was resolved from this live,
        // version-checked engine image and takes no arguments.
        unsafe { (self.engine.symbols().process.shutdown_process)() }
    }

    /// Backwards-compatible name for the explicit process close contract.
    pub fn shutdown_once(&mut self) -> i32 {
        self.close()
    }
}

impl Drop for EngineSession {
    fn drop(&mut self) {
        // A failed ownership transfer must not leave ART resident. Normal
        // RuntimeSession teardown marks this callback consumed first, so
        // Drop is idempotent in the successful path.
        let _ = self.close();
    }
}

#[cfg(test)]
mod close_contract_tests {
    use super::{EngineSession, GraphicsSession, SurfaceSession};

    // Keep all three owner types on the same explicit close-shaped API.
    // This is intentionally a function-pointer check: changing a close
    // contract's receiver or status type fails at compile time here.
    fn assert_close_contracts(
        _engine: fn(&mut EngineSession) -> i32,
        _surface: fn(&mut SurfaceSession) -> i32,
        _graphics: fn(&mut GraphicsSession) -> i32,
    ) {
    }

    #[test]
    fn all_native_owners_expose_explicit_close_contracts() {
        assert_close_contracts(
            EngineSession::close,
            SurfaceSession::close,
            GraphicsSession::close,
        );
    }
}
