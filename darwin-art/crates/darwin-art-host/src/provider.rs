//! Thin unsafe adapter from the native hook ABI to Rust runtime ownership.

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

use darwin_art_engine::ProviderHooks;
use darwin_art_engine_sys::{ProviderAcquireFn, ProviderReleaseFn};
use darwin_art_runtime::{ProviderKind, ProviderLeaseTable};

pub(super) struct ProviderBridge {
    leases: ProviderLeaseTable,
}

impl ProviderBridge {
    pub(super) fn new(hooks: ProviderHooks) -> Self {
        let acquire_hooks = hooks;
        let release_hooks = hooks;
        let clear_hooks = hooks;
        Self {
            leases: ProviderLeaseTable::new(
                move |kind, authority_fd| acquire_hooks.acquire(kind.raw(), authority_fd),
                move |kind| release_hooks.release(kind.raw()),
                move || clear_hooks.clear(),
            ),
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
        self.leases.clear().map_err(|_| ())
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
        // SAFETY: the hook context is a live ProviderBridge owned by the
        // RuntimeSession until provider hooks are cleared.
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
        // SAFETY: same invariant as acquire_provider.
        unsafe { &*context.cast::<ProviderBridge>() }
            .leases
            .release(kind)
    }))
    .unwrap_or(-1)
}
