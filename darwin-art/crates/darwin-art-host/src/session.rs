//! Native lifetime guards owned by the Rust runtime session.
//!
//! This module intentionally contains no render-loop policy.  It only wraps
//! the two foreign resources whose destruction must be ordered by
//! `RuntimeSession`: the one-shot ART process shutdown callback and the native
//! surface handle.

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Mutex;

use darwin_art_engine::{EngineSession, EngineSymbols};
use darwin_art_engine_sys::SurfaceDestroyFn;

pub(super) fn engine_symbols(engine: &EngineSession) -> EngineSymbols {
    engine.symbols()
}

/// Rust-owned provider activation state. The native provider implementations
/// remain behind the engine ABI, while this bridge owns callback validity and
/// the graph-owner counts that must survive ART's cross-thread ELF callbacks.
pub(super) struct ProviderBridge {
    symbols: EngineSymbols,
    counts: Mutex<[u32; 7]>,
}

impl ProviderBridge {
    pub(super) fn new(symbols: EngineSymbols) -> Self {
        Self {
            symbols,
            counts: Mutex::new([0; 7]),
        }
    }

    pub(super) fn context(&self) -> *mut c_void {
        (self as *const Self).cast_mut().cast()
    }

    pub(super) fn acquire_callback() -> ProviderAcquireFn {
        acquire_provider
    }

    pub(super) fn release_callback() -> ProviderReleaseFn {
        release_provider
    }

    pub(super) fn clear(&self) -> Result<(), ()> {
        let counts = self.counts.lock().map_err(|_| ())?;
        if counts.iter().any(|count| *count != 0) {
            return Err(());
        }
        // SAFETY: the provider hook table belongs to the live engine image.
        unsafe { (self.symbols.provider_clear_hooks)() };
        Ok(())
    }

    fn acquire(&self, kind: u32, authority_fd: i32) -> i32 {
        let index = kind as usize;
        if !(1..=6).contains(&index) {
            return -1;
        }
        // SAFETY: this function pointer and its arguments are supplied by the
        // live engine image associated with this bridge.
        let status = unsafe { (self.symbols.provider_native_acquire)(kind, authority_fd) };
        if status != 0 {
            return status;
        }
        let Ok(mut counts) = self.counts.lock() else {
            // The native owner has already acquired the provider. Fail-stop
            // rather than claiming a Rust lease that cannot be released.
            return -1;
        };
        counts[index] = counts[index].saturating_add(1);
        0
    }

    fn release(&self, kind: u32) -> i32 {
        let index = kind as usize;
        if !(1..=6).contains(&index) {
            return -1;
        }
        let Ok(mut counts) = self.counts.lock() else {
            return -1;
        };
        if counts[index] == 0 {
            return -1;
        }
        // SAFETY: this function pointer and its arguments are supplied by the
        // live engine image associated with this bridge.
        let status = unsafe { (self.symbols.provider_native_release)(kind) };
        if status == 0 {
            counts[index] -= 1;
        }
        status
    }
}

type ProviderAcquireFn = unsafe extern "C" fn(*mut c_void, u32, i32) -> i32;
type ProviderReleaseFn = unsafe extern "C" fn(*mut c_void, u32) -> i32;

unsafe extern "C" fn acquire_provider(context: *mut c_void, kind: u32, authority_fd: i32) -> i32 {
    if context.is_null() {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the hook context is a live ProviderBridge owned by the
        // RuntimeSession until provider hooks are cleared.
        unsafe { &*context.cast::<ProviderBridge>() }.acquire(kind, authority_fd)
    }))
    .unwrap_or(-1)
}

unsafe extern "C" fn release_provider(context: *mut c_void, kind: u32) -> i32 {
    if context.is_null() {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: same invariant as acquire_provider.
        unsafe { &*context.cast::<ProviderBridge>() }.release(kind)
    }))
    .unwrap_or(-1)
}

/// Owns a surface returned by either `surface_active_gpu` or `surface_create`.
/// Surface destruction is ordered before the process guard because the
/// surface may refer to ART-owned rendering state.
pub(super) struct SurfaceCleanupGuard {
    destroy: SurfaceDestroyFn,
    surface: *mut c_void,
    armed: bool,
}

impl SurfaceCleanupGuard {
    pub(super) fn new(destroy: SurfaceDestroyFn, surface: *mut c_void) -> Self {
        debug_assert!(!surface.is_null());
        Self {
            destroy,
            surface,
            armed: true,
        }
    }

    pub(super) fn handle(&self) -> *mut c_void {
        self.surface
    }

    pub(super) fn close(&mut self) -> i32 {
        if !self.armed {
            return 0;
        }
        self.armed = false;
        // SAFETY: the pointer was returned by the matching v1 surface API and
        // is kept live until this one-shot destruction call.
        unsafe { (self.destroy)(self.surface) }
    }
}

impl Drop for SurfaceCleanupGuard {
    fn drop(&mut self) {
        if self.armed {
            self.armed = false;
            // SAFETY: same invariant as `close`; Drop cannot report a status,
            // but it must still release the native surface on rollback.
            unsafe {
                let _ = (self.destroy)(self.surface);
            }
        }
    }
}
