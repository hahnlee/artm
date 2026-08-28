use super::abi::EngineSymbols;
use darwin_art_engine_sys::{
    GraphicsSessionCloseFn, GraphicsSessionDestroyFn, GraphicsSessionDispatchKeyV1Fn,
    GraphicsSessionDispatchPointerFn, GraphicsSessionDispatchPointerV2Fn, GraphicsSessionHandle,
    GraphicsSessionPumpFrameFn, KeyEventV1, PointerEventV2,
};
use darwin_art_runtime::NativeResource;
use std::ptr::NonNull;

/// Rust owner for the opaque native HWUI session.  The C ABI deliberately
/// exposes only a void handle; JNI, RenderNode, and AnimationContext stay
/// entirely on the C++ side.  `close` must precede `destroy`, which makes
/// a use-after-close a normal status result instead of undefined behavior.
pub struct GraphicsSession {
    handle: Option<NonNull<GraphicsSessionHandle>>,
    close_fn: GraphicsSessionCloseFn,
    destroy_fn: GraphicsSessionDestroyFn,
    dispatch_fn: GraphicsSessionDispatchPointerFn,
    dispatch_v2_fn: Option<GraphicsSessionDispatchPointerV2Fn>,
    dispatch_key_v1_fn: Option<GraphicsSessionDispatchKeyV1Fn>,
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
            handle: NonNull::new(handle),
            close_fn,
            destroy_fn,
            dispatch_fn,
            dispatch_v2_fn: symbols.graphics.dispatch_pointer_v2,
            dispatch_key_v1_fn: symbols.graphics.dispatch_key_v1,
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
        let Some(handle) = self.handle else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: handle and callback belong to this live owner.
        let status = unsafe { (self.close_fn)(handle.as_ptr()) };
        if status == 0 {
            self.closed = true;
        }
        status
    }

    pub(crate) fn raw_handle(&self) -> *mut GraphicsSessionHandle {
        self.handle.map_or(std::ptr::null_mut(), NonNull::as_ptr)
    }

    pub fn dispatch_pointer(&self, action: u32, x: f32, y: f32) -> i32 {
        let Some(handle) = self.handle.filter(|_| !self.closed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: handle remains live while self is borrowed.
        unsafe { (self.dispatch_fn)(handle.as_ptr(), action, x, y) }
    }

    pub fn dispatch_pointer_v2(&self, event: &PointerEventV2) -> i32 {
        let Some(handle) = self.handle.filter(|_| !self.closed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        if let Some(dispatch) = self.dispatch_v2_fn {
            // SAFETY: event is borrowed for the synchronous foreign call.
            unsafe { dispatch(handle.as_ptr(), event) }
        } else {
            self.dispatch_pointer(event.action, event.x, event.y)
        }
    }

    pub fn dispatch_key_v1(&self, event: &KeyEventV1) -> i32 {
        let Some(handle) = self.handle.filter(|_| !self.closed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        let Some(dispatch) = self.dispatch_key_v1_fn else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: event is borrowed for the synchronous foreign call.
        unsafe { dispatch(handle.as_ptr(), event) }
    }

    pub fn pump_frame(&self, frame_time_nanos: i64) -> i32 {
        let Some(handle) = self.handle.filter(|_| !self.closed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: handle remains live while self is borrowed.
        unsafe { (self.pump_fn)(handle.as_ptr(), frame_time_nanos) }
    }

    fn destroy(&mut self) -> i32 {
        if !self.closed || self.handle.is_none() {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        }
        let handle = self.handle.expect("checked graphics handle");
        // SAFETY: close made the native handle inert and this is the one
        // matching destroy call for it.
        let status = unsafe { (self.destroy_fn)(handle.as_ptr()) };
        if status == 0 {
            self.handle = None;
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

impl NativeResource for GraphicsSession {
    fn close(&mut self) -> i32 {
        GraphicsSession::close(self)
    }

    fn finalize(&mut self) -> i32 {
        self.destroy()
    }
}

#[cfg(test)]
mod graphics_session_tests {
    use super::GraphicsSession;
    use core::ffi::c_void;
    use core::ptr::NonNull;
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
            handle: NonNull::new(handle),
            close_fn: close,
            destroy_fn: destroy,
            dispatch_fn: dispatch,
            dispatch_v2_fn: None,
            dispatch_key_v1_fn: None,
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
            handle: NonNull::new(handle),
            close_fn: failing_close,
            destroy_fn: destroy,
            dispatch_fn: dispatch,
            dispatch_v2_fn: None,
            dispatch_key_v1_fn: None,
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
