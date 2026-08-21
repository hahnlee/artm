//! Thin unsafe provider callback adapter.
//!
//! Lease accounting stays in `provider.rs`; this module only owns the fixed
//! C ABI function pointers and turns them into the Rust lease table callbacks.

use core::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

use darwin_art_engine_sys::{
    ProviderAcquireFn, ProviderClearHooksFn, ProviderNativeAcquireFn, ProviderNativeReleaseFn,
    ProviderReleaseFn,
};

use super::{ProviderKind, ProviderLeaseError, ProviderLeaseTable};

/// Rust-owned bridge for the process-global provider callbacks.
pub struct ProviderBridge {
    leases: ProviderLeaseTable,
}

impl ProviderBridge {
    pub fn from_callbacks(
        acquire: ProviderNativeAcquireFn,
        release: ProviderNativeReleaseFn,
        clear: ProviderClearHooksFn,
    ) -> Self {
        Self {
            leases: ProviderLeaseTable::new(
                move |kind, authority_fd| {
                    // SAFETY: the function pointer belongs to the live engine
                    // image held by RuntimeSession.
                    unsafe { acquire(kind.raw(), authority_fd) }
                },
                move |kind| {
                    // SAFETY: same image-lifetime invariant as acquire.
                    unsafe { release(kind.raw()) }
                },
                move || {
                    // SAFETY: clear runs only after the lease table is
                    // quiescent and before the engine owner is dropped.
                    unsafe { clear() }
                },
            ),
        }
    }

    pub fn context(&self) -> *mut c_void {
        (self as *const Self).cast_mut().cast()
    }

    pub fn acquire_callback() -> ProviderAcquireFn {
        acquire_provider
    }

    pub fn release_callback() -> ProviderReleaseFn {
        release_provider
    }

    pub fn clear(&self) -> Result<(), ProviderLeaseError> {
        self.leases.clear()
    }
}

impl crate::NativeResource for ProviderBridge {
    fn close(&mut self) -> i32 {
        0
    }

    fn clear(&mut self) -> i32 {
        ProviderBridge::clear(self).map_or(-1, |_| 0)
    }
}

impl crate::NativeResource for Box<ProviderBridge> {
    fn close(&mut self) -> i32 {
        0
    }

    fn clear(&mut self) -> i32 {
        ProviderBridge::clear(self).map_or(-1, |_| 0)
    }
}

unsafe extern "C" fn acquire_provider(context: *mut c_void, kind: u32, authority_fd: i32) -> i32 {
    if context.is_null() {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(kind) = ProviderKind::try_from(kind) else {
            return -1;
        };
        // SAFETY: RuntimeSession owns the bridge until provider hooks are
        // cleared, and no callback is retained after that boundary.
        unsafe { &*context.cast::<ProviderBridge>() }
            .leases
            .acquire(kind, authority_fd)
    }))
    .unwrap_or(-1)
}

unsafe extern "C" fn release_provider(context: *mut c_void, kind: u32) -> i32 {
    if context.is_null() {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(kind) = ProviderKind::try_from(kind) else {
            return -1;
        };
        // SAFETY: same lifetime invariant as acquire_provider.
        unsafe { &*context.cast::<ProviderBridge>() }
            .leases
            .release(kind)
    }))
    .unwrap_or(-1)
}
