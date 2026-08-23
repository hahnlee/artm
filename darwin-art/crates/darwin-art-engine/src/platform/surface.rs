use super::abi::EngineSymbols;
use core::ffi::c_void;
use darwin_art_engine_sys::{
    PointerEvent, PointerEventV2, SurfaceDestroyFn, SurfaceNextPointerEventFn,
    SurfaceNextPointerEventV2Fn, SurfacePresentFn, SurfacePumpEventsFn, SurfaceUpdateFn,
};
use darwin_art_runtime::NativeResource;
use std::{mem::size_of, ptr::NonNull};

/// Owner-thread surface handle. Its callback table and native handle stay
/// paired until `close`, so RuntimeSession can drop it before EngineSession
/// and never call into an unmapped engine image.
pub struct SurfaceSession {
    handle: Option<NonNull<c_void>>,
    update: SurfaceUpdateFn,
    present: SurfacePresentFn,
    pump_events: SurfacePumpEventsFn,
    next_pointer_event: SurfaceNextPointerEventFn,
    next_pointer_event_v2: Option<SurfaceNextPointerEventV2Fn>,
    destroy: SurfaceDestroyFn,
    armed: bool,
    close_status: Option<i32>,
}

impl SurfaceSession {
    fn from_parts(handle: *mut c_void, symbols: EngineSymbols) -> Self {
        Self {
            handle: NonNull::new(handle),
            update: symbols.surface.update,
            present: symbols.surface.present,
            pump_events: symbols.surface.pump_events,
            next_pointer_event: symbols.surface.next_pointer_event,
            next_pointer_event_v2: symbols.surface.next_pointer_event_v2,
            destroy: symbols.surface.destroy,
            armed: true,
            close_status: None,
        }
    }

    pub fn handle(&self) -> *mut c_void {
        self.handle
            .filter(|_| self.armed)
            .map_or(std::ptr::null_mut(), NonNull::as_ptr)
    }

    pub(crate) fn active(symbols: EngineSymbols) -> Option<Self> {
        // SAFETY: callback belongs to the live engine image represented by
        // this symbol table.
        let handle = unsafe { (symbols.surface.active)() };
        (!handle.is_null()).then(|| Self::from_parts(handle, symbols))
    }

    pub(crate) fn create(
        symbols: EngineSymbols,
        info: &darwin_art_engine_sys::SurfaceCreateInfo,
    ) -> Result<Self, i32> {
        let mut status = -1;
        // SAFETY: info is a valid POD for the duration of this call.
        let handle = unsafe { (symbols.surface.create)(info, &mut status) };
        if handle.is_null() {
            Err(status)
        } else {
            Ok(Self::from_parts(handle, symbols))
        }
    }

    pub fn update_words(&self, pixels: &[u32]) -> i32 {
        let Some(handle) = self.handle.filter(|_| self.armed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        let byte_count = pixels.len().saturating_mul(size_of::<u32>());
        // SAFETY: the callback and handle are paired and live; `pixels`
        // remains borrowed for the duration of the synchronous callback.
        unsafe { (self.update)(handle.as_ptr(), pixels.as_ptr().cast(), byte_count) }
    }

    pub fn present(&self) -> i32 {
        let Some(handle) = self.handle.filter(|_| self.armed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: same invariant as update.
        unsafe { (self.present)(handle.as_ptr()) }
    }

    pub fn pump_events(&self, visible_seconds: f64) -> i32 {
        let Some(handle) = self.handle.filter(|_| self.armed) else {
            return darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE;
        };
        // SAFETY: same invariant as update.
        unsafe { (self.pump_events)(handle.as_ptr(), visible_seconds) }
    }

    pub fn next_pointer_event(&self, event: &mut PointerEvent) -> bool {
        let Some(handle) = self.handle.filter(|_| self.armed) else {
            return false;
        };
        // SAFETY: event is writable POD and the handle is live.
        unsafe { (self.next_pointer_event)(handle.as_ptr(), event) }
    }

    pub fn next_pointer_event_v2(&self, event: &mut PointerEventV2) -> bool {
        let Some(handle) = self.handle.filter(|_| self.armed) else {
            return false;
        };
        let Some(next) = self.next_pointer_event_v2 else {
            return false;
        };
        // SAFETY: event is writable POD and the handle is live.
        unsafe { next(handle.as_ptr(), event) }
    }

    pub fn close(&mut self) -> i32 {
        if !self.armed {
            return self.close_status.unwrap_or(0);
        }
        self.armed = false;
        let Some(handle) = self.handle else {
            return self
                .close_status
                .unwrap_or(darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE);
        };
        // SAFETY: this is the one matching destroy call for the handle.
        let status = unsafe { (self.destroy)(handle.as_ptr()) };
        self.close_status = Some(status);
        status
    }
}

impl Drop for SurfaceSession {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

impl NativeResource for SurfaceSession {
    fn close(&mut self) -> i32 {
        SurfaceSession::close(self)
    }
}

#[cfg(test)]
mod surface_session_tests {
    use super::SurfaceSession;
    use core::ffi::c_void;
    use core::ptr::NonNull;
    use core::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Mutex;

    static DESTROY_CALLS: AtomicUsize = AtomicUsize::new(0);
    static DESTROY_TEST_LOCK: Mutex<()> = Mutex::new(());

    unsafe extern "C" fn destroy(_: *mut c_void) -> i32 {
        DESTROY_CALLS.fetch_add(1, Ordering::SeqCst);
        -17
    }

    unsafe extern "C" fn update(_: *mut c_void, _: *const c_void, _: usize) -> i32 {
        0
    }

    unsafe extern "C" fn present(_: *mut c_void) -> i32 {
        0
    }

    unsafe extern "C" fn pump(_: *mut c_void, _: f64) -> i32 {
        0
    }

    unsafe extern "C" fn next_event(
        _: *mut c_void,
        _: *mut darwin_art_engine_sys::PointerEvent,
    ) -> bool {
        false
    }

    #[test]
    fn destroy_failure_is_reported_once_and_not_reentered_by_drop() {
        let _lock = DESTROY_TEST_LOCK.lock().unwrap();
        DESTROY_CALLS.store(0, Ordering::SeqCst);
        let mut session = SurfaceSession {
            handle: NonNull::new(std::ptr::dangling_mut::<c_void>()),
            update,
            present,
            pump_events: pump,
            next_pointer_event: next_event,
            next_pointer_event_v2: None,
            destroy,
            armed: true,
            close_status: None,
        };
        assert_eq!(session.close(), -17);
        assert_eq!(session.close(), -17);
        drop(session);
        assert_eq!(DESTROY_CALLS.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn closed_surface_fails_closed_before_foreign_operations() {
        let _lock = DESTROY_TEST_LOCK.lock().unwrap();
        let mut session = SurfaceSession {
            handle: NonNull::new(std::ptr::dangling_mut::<c_void>()),
            update,
            present,
            pump_events: pump,
            next_pointer_event: next_event,
            next_pointer_event_v2: None,
            destroy,
            armed: true,
            close_status: None,
        };
        assert_eq!(session.close(), -17);
        assert!(session.handle().is_null());
        assert_eq!(
            session.update_words(&[1, 2, 3]),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
        assert_eq!(
            session.present(),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
        assert_eq!(
            session.pump_events(0.1),
            darwin_art_engine_sys::ENGINE_STATUS_UNAVAILABLE
        );
        assert!(!session.next_pointer_event(&mut darwin_art_engine_sys::PointerEvent::default()));
    }
}
