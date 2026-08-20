//! Native lifetime guards owned by the Rust runtime session.
//!
//! This module intentionally contains no render-loop policy.  It only wraps
//! the two foreign resources whose destruction must be ordered by
//! `RuntimeSession`: the one-shot ART process shutdown callback and the native
//! surface handle.

use std::cell::RefCell;
use std::ffi::c_void;
use std::rc::Rc;

use darwin_art_engine::{EngineSession, EngineSymbols};
use darwin_art_engine_sys::SurfaceDestroyFn;

pub(super) type SharedProcessShutdown = Rc<RefCell<Option<Rc<RefCell<EngineSession>>>>>;

pub(super) fn shutdown_process_once(shutdown: &SharedProcessShutdown) -> i32 {
    let Some(engine) = shutdown.borrow_mut().take() else {
        return 0;
    };
    engine.borrow_mut().shutdown_once()
}

pub(super) fn engine_symbols(engine: &Rc<RefCell<EngineSession>>) -> EngineSymbols {
    engine.borrow().symbols()
}

/// Owns a surface returned by either `surface_active_gpu` or `surface_create`.
/// Surface destruction is ordered before the process guard because the
/// surface may refer to ART-owned rendering state.
pub(super) struct SurfaceCleanupGuard {
    destroy: SurfaceDestroyFn,
    surface: *mut c_void,
    armed: bool,
}

pub(super) type SharedSurfaceCleanup = Rc<RefCell<SurfaceCleanupGuard>>;

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
