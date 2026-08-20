//! Thin unsafe adapter from the native hook ABI to Rust runtime ownership.

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

use darwin_art_engine::EngineSymbols;
use darwin_art_engine_sys::{ProviderAcquireFn, ProviderReleaseFn};
use darwin_art_runtime::ProviderLeaseTable;

pub(super) struct ProviderBridge {
    leases: ProviderLeaseTable,
}

impl ProviderBridge {
    pub(super) fn new(symbols: EngineSymbols) -> Self {
        let acquire_symbols = symbols;
        let release_symbols = symbols;
        let clear_symbols = symbols;
        Self {
            leases: ProviderLeaseTable::new(
                move |kind, authority_fd| {
                    // SAFETY: EngineSession keeps the image mapped while this
                    // table is owned by RuntimeSession.
                    unsafe { (acquire_symbols.provider_native_acquire)(kind, authority_fd) }
                },
                move |kind| {
                    // SAFETY: same image-lifetime invariant as acquire.
                    unsafe { (release_symbols.provider_native_release)(kind) }
                },
                move || {
                    // SAFETY: hooks are cleared only after the Rust table has
                    // observed zero active leases.
                    unsafe { (clear_symbols.provider_clear_hooks)() }
                },
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
        // SAFETY: same invariant as acquire_provider.
        unsafe { &*context.cast::<ProviderBridge>() }
            .leases
            .release(kind)
    }))
    .unwrap_or(-1)
}
