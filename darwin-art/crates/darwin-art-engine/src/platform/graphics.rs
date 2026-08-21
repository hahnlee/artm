use super::abi::EngineSymbols;
use darwin_art_engine_sys::{
    GraphicsSessionCloseFn, GraphicsSessionDestroyFn, GraphicsSessionDispatchPointerFn,
    GraphicsSessionHandle, GraphicsSessionPumpFrameFn,
};

/// Rust owner for the opaque native HWUI session.  The C ABI deliberately
/// exposes only a void handle; JNI, RenderNode, and AnimationContext stay
/// entirely on the C++ side.  `close` must precede `destroy`, which makes
/// a use-after-close a normal status result instead of undefined behavior.
pub struct GraphicsSession {
    handle: *mut GraphicsSessionHandle,
    close_fn: GraphicsSessionCloseFn,
    destroy_fn: GraphicsSessionDestroyFn,
    dispatch_fn: GraphicsSessionDispatchPointerFn,
    pump_fn: GraphicsSessionPumpFrameFn,
    closed: bool,
    close_attempted: bool,
}

impl GraphicsSession {
    pub(crate) fn create(symbols: EngineSymbols) -> Result<Self, i32> {
        // SAFETY: the function pointer belongs to the live engine image.
        let (Some(create_fn), Some(close_fn), Some(destroy_fn), Some(dispatch_fn), Some(pump_fn)) = (
            symbols.graphics.create,
            symbols.graphics.close,
            symbols.graphics.destroy,
            symbols.graphics.dispatch_pointer,
            symbols.graphics.pump_frame,
        ) else {
            return Err(darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE);
        };
        // SAFETY: the function pointer belongs to the live engine image.
        let handle = unsafe { (create_fn)() };
        if handle.is_null() {
            return Err(darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE);
        }
        Ok(Self {
            handle,
            close_fn,
            destroy_fn,
            dispatch_fn,
            pump_fn,
            closed: false,
            close_attempted: false,
        })
    }

    /// Close the native session. A failed explicit close leaves the
    /// handle owned and may be retried by the caller; Drop will not make
    /// that policy decision by re-entering the foreign callback.
    pub fn close(&mut self) -> i32 {
        if self.closed {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        }
        // Mark the explicit close boundary before entering foreign code.
        // A failed close remains observable to the caller, but Drop must
        // not accidentally call a non-idempotent callback a second time.
        self.close_attempted = true;
        // SAFETY: handle and callback belong to this live owner.
        let status = unsafe { (self.close_fn)(self.handle) };
        if status == 0 {
            self.closed = true;
        }
        status
    }

    pub fn raw_handle(&self) -> *mut GraphicsSessionHandle {
        self.handle
    }

    pub fn dispatch_pointer(&self, action: u32, x: f32, y: f32) -> i32 {
        if self.closed {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        }
        // SAFETY: handle remains live while self is borrowed.
        unsafe { (self.dispatch_fn)(self.handle, action, x, y) }
    }

    pub fn pump_frame(&self, frame_time_nanos: i64) -> i32 {
        if self.closed {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        }
        // SAFETY: handle remains live while self is borrowed.
        unsafe { (self.pump_fn)(self.handle, frame_time_nanos) }
    }

    fn destroy(&mut self) -> i32 {
        if !self.closed || self.handle.is_null() {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        }
        // SAFETY: close made the native handle inert and this is the one
        // matching destroy call for it.
        let status = unsafe { (self.destroy_fn)(self.handle) };
        if status == 0 {
            self.handle = std::ptr::null_mut();
        }
        status
    }
}

impl Drop for GraphicsSession {
    fn drop(&mut self) {
        // The native shutdown transaction may finalize the bound session
        // before DestroyJavaVM.  In that case destroy_fn returns the
        // benign INVALID handle status after erasing the opaque owner;
        // importantly it performs no ART lookup after the VM is gone.
        if !self.closed && !self.close_attempted {
            let _ = self.close();
        }
        if self.closed {
            let _ = self.destroy();
        }
    }
}

#[cfg(test)]
mod graphics_session_tests {
    use super::GraphicsSession;
    use core::ffi::c_void;
    use core::sync::atomic::{AtomicUsize, Ordering};

    static FAILED_CLOSE_CALLS: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn close(_: *mut c_void) -> i32 {
        0
    }

    unsafe extern "C" fn destroy(handle: *mut c_void) -> i32 {
        if !handle.is_null() {
            // SAFETY: the test allocated this exact boxed byte below.
            unsafe { drop(Box::from_raw(handle.cast::<u8>())) };
        }
        0
    }

    unsafe extern "C" fn dispatch(_: *mut c_void, _: u32, _: f32, _: f32) -> i32 {
        123
    }

    unsafe extern "C" fn pump(_: *mut c_void, _: i64) -> i32 {
        456
    }

    unsafe extern "C" fn failing_close(_: *mut c_void) -> i32 {
        FAILED_CLOSE_CALLS.fetch_add(1, Ordering::SeqCst);
        -99
    }

    #[test]
    fn close_makes_use_after_close_fail_closed() {
        let handle = Box::into_raw(Box::new(1_u8)).cast::<c_void>();
        let mut session = GraphicsSession {
            handle,
            close_fn: close,
            destroy_fn: destroy,
            dispatch_fn: dispatch,
            pump_fn: pump,
            closed: false,
            close_attempted: false,
        };
        assert_eq!(session.dispatch_pointer(0, 1.0, 1.0), 123);
        assert_eq!(session.pump_frame(10), 456);
        assert_eq!(session.close(), 0);
        assert_eq!(
            session.dispatch_pointer(0, 1.0, 1.0),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
        assert_eq!(
            session.pump_frame(10),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
        assert_eq!(
            session.close(),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
    }

    #[test]
    fn failed_explicit_close_is_not_reentered_by_drop() {
        FAILED_CLOSE_CALLS.store(0, Ordering::SeqCst);
        let handle = Box::into_raw(Box::new(1_u8)).cast::<c_void>();
        let mut session = GraphicsSession {
            handle,
            close_fn: failing_close,
            destroy_fn: destroy,
            dispatch_fn: dispatch,
            pump_fn: pump,
            closed: false,
            close_attempted: false,
        };
        assert_eq!(session.close(), -99);
        drop(session);
        assert_eq!(FAILED_CLOSE_CALLS.load(Ordering::SeqCst), 1);

        // A failed native close cannot be safely destroyed under the C
        // contract. Reclaim this test fixture manually because the Rust
        // owner deliberately leaves a failed handle untouched.
        unsafe { drop(Box::from_raw(handle.cast::<u8>())) };
    }
}
