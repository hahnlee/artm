#![allow(non_camel_case_types)]
#![deny(unsafe_op_in_unsafe_fn)]

//! The sole raw FFI boundary for the native ART/HWUI engine.
//!
//! This crate declares opaque pointers and POD function tables only. Safe
//! ownership wrappers belong in the higher-level runtime crate; no Rust
//! slice, STL object, or borrowed string crosses this boundary.

use core::ffi::{c_char, c_void};
use darwin_art_abi::{AbiHeader, StatusCode};

pub type FrameCallback = unsafe extern "C" fn(
    context: *mut c_void,
    argb_pixels: *const u32,
    width: u32,
    height: u32,
    stride_bytes: usize,
) -> i32;

pub type ProviderAcquireFn =
    unsafe extern "C" fn(context: *mut c_void, provider_kind: u32, authority_fd: i32) -> i32;
pub type ProviderReleaseFn = unsafe extern "C" fn(context: *mut c_void, provider_kind: u32) -> i32;
pub type LifecycleBeginFn = unsafe extern "C" fn(context: *mut c_void) -> i32;
pub type LifecycleFinishFn =
    unsafe extern "C" fn(context: *mut c_void, runtime_created: i32) -> i32;
pub type LifecycleFailedFn = unsafe extern "C" fn(context: *mut c_void, status: i32);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LifecycleHooks {
    pub struct_size: u32,
    pub abi_version: u32,
    pub context: *mut c_void,
    pub begin_run: Option<LifecycleBeginFn>,
    pub finish_run: Option<LifecycleFinishFn>,
    pub begin_shutdown: Option<LifecycleBeginFn>,
    pub mark_failed: Option<LifecycleFailedFn>,
}

#[repr(C)]
pub struct ProcessConfig {
    pub header: AbiHeader,
    pub core_oj_jar: *const c_char,
    pub core_libart_jar: *const c_char,
    pub framework_jar: *const c_char,
    pub core_icu4j_jar: *const c_char,
    pub app_dex: *const c_char,
    pub heap_initial_bytes: u64,
    pub heap_maximum_bytes: u64,
    pub host_context: *mut c_void,
    pub frame_callback: Option<FrameCallback>,
    pub provider_context: *mut c_void,
    pub provider_acquire: Option<ProviderAcquireFn>,
    pub provider_release: Option<ProviderReleaseFn>,
    pub graphics_session_context: *mut c_void,
    pub lifecycle_hooks: *const LifecycleHooks,
}

impl ProcessConfig {
    #[allow(clippy::too_many_arguments)]
    pub const fn new(
        core_oj_jar: *const c_char,
        core_libart_jar: *const c_char,
        framework_jar: *const c_char,
        core_icu4j_jar: *const c_char,
        app_dex: *const c_char,
        heap_initial_bytes: u64,
        heap_maximum_bytes: u64,
        host_context: *mut c_void,
        frame_callback: Option<FrameCallback>,
        provider_context: *mut c_void,
        provider_acquire: Option<ProviderAcquireFn>,
        provider_release: Option<ProviderReleaseFn>,
    ) -> Self {
        Self {
            header: AbiHeader::new(core::mem::size_of::<Self>()),
            core_oj_jar,
            core_libart_jar,
            framework_jar,
            core_icu4j_jar,
            app_dex,
            heap_initial_bytes,
            heap_maximum_bytes,
            host_context,
            frame_callback,
            provider_context,
            provider_acquire,
            provider_release,
            graphics_session_context: core::ptr::null_mut(),
            lifecycle_hooks: core::ptr::null(),
        }
    }

    pub const fn is_compatible(&self) -> bool {
        self.header.accepts(core::mem::size_of::<Self>())
    }

    pub const fn with_graphics_session(mut self, context: *mut c_void) -> Self {
        self.graphics_session_context = context;
        self
    }

    pub const fn with_lifecycle_hooks(mut self, hooks: *const LifecycleHooks) -> Self {
        self.lifecycle_hooks = hooks;
        self
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub struct ProcessResult {
    pub header: AbiHeader,
    pub hello_answer: i32,
    pub native_round_trip: i32,
    pub arraycopy_result: i32,
    pub activity_probe_result: i32,
    pub lifecycle_result: i32,
    pub frame_width: u32,
    pub frame_height: u32,
}

impl ProcessResult {
    pub const fn new() -> Self {
        Self {
            header: AbiHeader::new(core::mem::size_of::<Self>()),
            hello_answer: 0,
            native_round_trip: 0,
            arraycopy_result: 0,
            activity_probe_result: 0,
            lifecycle_result: 0,
            frame_width: 0,
            frame_height: 0,
        }
    }

    pub const fn is_compatible(&self) -> bool {
        self.header.accepts(core::mem::size_of::<Self>())
    }
}

pub type RunProcessFn = unsafe extern "C" fn(*const ProcessConfig, *mut ProcessResult) -> i32;
pub type ShutdownProcessFn = unsafe extern "C" fn() -> i32;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct SurfaceCreateInfo {
    pub width: u32,
    pub height: u32,
    pub title: *const c_char,
    pub visible: bool,
}

pub type SurfaceCreateFn = unsafe extern "C" fn(*const SurfaceCreateInfo, *mut i32) -> *mut c_void;
pub type SurfaceUpdateFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> i32;
pub type SurfacePresentFn = unsafe extern "C" fn(*mut c_void) -> i32;
pub type SurfacePumpEventsFn = unsafe extern "C" fn(*mut c_void, f64) -> i32;

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of};

    #[test]
    fn process_config_layout_is_owned_by_raw_ffi_crate() {
        assert_eq!(size_of::<ProcessConfig>(), 120);
        assert_eq!(align_of::<ProcessConfig>(), 8);
        assert_eq!(offset_of!(ProcessConfig, header), 0);
        assert_eq!(
            offset_of!(ProcessConfig, header) + offset_of!(AbiHeader, struct_size),
            0
        );
        assert_eq!(
            offset_of!(ProcessConfig, header) + offset_of!(AbiHeader, abi_version),
            4
        );
        assert_eq!(offset_of!(ProcessConfig, core_oj_jar), 8);
        assert_eq!(offset_of!(ProcessConfig, core_libart_jar), 16);
        assert_eq!(offset_of!(ProcessConfig, framework_jar), 24);
        assert_eq!(offset_of!(ProcessConfig, core_icu4j_jar), 32);
        assert_eq!(offset_of!(ProcessConfig, app_dex), 40);
        assert_eq!(offset_of!(ProcessConfig, heap_initial_bytes), 48);
        assert_eq!(offset_of!(ProcessConfig, heap_maximum_bytes), 56);
        assert_eq!(offset_of!(ProcessConfig, host_context), 64);
        assert_eq!(offset_of!(ProcessConfig, frame_callback), 72);
        assert_eq!(offset_of!(ProcessConfig, provider_context), 80);
        assert_eq!(offset_of!(ProcessConfig, provider_acquire), 88);
        assert_eq!(offset_of!(ProcessConfig, provider_release), 96);
        assert_eq!(offset_of!(ProcessConfig, graphics_session_context), 104);
        assert_eq!(offset_of!(ProcessConfig, lifecycle_hooks), 112);
    }

    #[test]
    fn lifecycle_hooks_layout_matches_native_abi() {
        assert_eq!(size_of::<LifecycleHooks>(), 48);
        assert_eq!(align_of::<LifecycleHooks>(), 8);
        assert_eq!(offset_of!(LifecycleHooks, struct_size), 0);
        assert_eq!(offset_of!(LifecycleHooks, abi_version), 4);
        assert_eq!(offset_of!(LifecycleHooks, context), 8);
        assert_eq!(offset_of!(LifecycleHooks, begin_run), 16);
        assert_eq!(offset_of!(LifecycleHooks, finish_run), 24);
        assert_eq!(offset_of!(LifecycleHooks, begin_shutdown), 32);
        assert_eq!(offset_of!(LifecycleHooks, mark_failed), 40);
    }

    #[test]
    fn process_result_layout_is_owned_by_raw_ffi_crate() {
        assert_eq!(size_of::<ProcessResult>(), 36);
        assert_eq!(align_of::<ProcessResult>(), 4);
        assert_eq!(offset_of!(ProcessResult, header), 0);
        assert_eq!(
            offset_of!(ProcessResult, header) + offset_of!(AbiHeader, struct_size),
            0
        );
        assert_eq!(
            offset_of!(ProcessResult, header) + offset_of!(AbiHeader, abi_version),
            4
        );
        assert_eq!(offset_of!(ProcessResult, hello_answer), 8);
        assert_eq!(offset_of!(ProcessResult, native_round_trip), 12);
        assert_eq!(offset_of!(ProcessResult, arraycopy_result), 16);
        assert_eq!(offset_of!(ProcessResult, activity_probe_result), 20);
        assert_eq!(offset_of!(ProcessResult, lifecycle_result), 24);
        assert_eq!(offset_of!(ProcessResult, frame_width), 28);
        assert_eq!(offset_of!(ProcessResult, frame_height), 32);
    }

    #[test]
    fn surface_and_callback_layout_is_owned_by_raw_ffi_crate() {
        assert_eq!(size_of::<SurfaceCreateInfo>(), 24);
        assert_eq!(align_of::<SurfaceCreateInfo>(), 8);
        assert_eq!(offset_of!(SurfaceCreateInfo, width), 0);
        assert_eq!(offset_of!(SurfaceCreateInfo, height), 4);
        assert_eq!(offset_of!(SurfaceCreateInfo, title), 8);
        assert_eq!(offset_of!(SurfaceCreateInfo, visible), 16);
        assert_eq!(size_of::<Option<FrameCallback>>(), size_of::<*mut c_void>());
        assert_eq!(
            align_of::<Option<FrameCallback>>(),
            align_of::<*mut c_void>()
        );
    }
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct PointerEvent {
    pub action: u32,
    pub x: f32,
    pub y: f32,
}

pub type SurfaceNextPointerEventFn = unsafe extern "C" fn(*mut c_void, *mut PointerEvent) -> bool;
pub type SurfaceDestroyFn = unsafe extern "C" fn(*mut c_void) -> i32;
pub type SurfaceActiveFn = unsafe extern "C" fn() -> *mut c_void;
pub type DispatchPointerFn = unsafe extern "C" fn(u32, f32, f32) -> i32;
pub type PumpFrameworkFrameFn = unsafe extern "C" fn(i64) -> i32;
pub type GraphicsSessionHandle = c_void;
pub type GraphicsSessionCreateFn = unsafe extern "C" fn() -> *mut GraphicsSessionHandle;
pub type GraphicsSessionCloseFn = unsafe extern "C" fn(*mut GraphicsSessionHandle) -> i32;
pub type GraphicsSessionDestroyFn = unsafe extern "C" fn(*mut GraphicsSessionHandle) -> i32;
pub type GraphicsSessionDispatchPointerFn =
    unsafe extern "C" fn(*mut GraphicsSessionHandle, u32, f32, f32) -> i32;
pub type GraphicsSessionPumpFrameFn = unsafe extern "C" fn(*mut GraphicsSessionHandle, i64) -> i32;
pub type ProviderInstallHooksFn = unsafe extern "C" fn(
    context: *mut c_void,
    acquire: Option<ProviderAcquireFn>,
    release: Option<ProviderReleaseFn>,
);
pub type ProviderClearHooksFn = unsafe extern "C" fn();
pub type ProviderNativeAcquireFn = unsafe extern "C" fn(kind: u32, authority_fd: i32) -> i32;
pub type ProviderNativeReleaseFn = unsafe extern "C" fn(kind: u32) -> i32;

/// Sentinel used by a missing optional callback without exporting a C++ type.
pub const ENGINE_STATUS_UNAVAILABLE: i32 = StatusCode::Unsupported as i32;
