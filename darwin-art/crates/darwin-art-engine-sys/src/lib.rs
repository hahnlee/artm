#![allow(non_camel_case_types)]
#![deny(unsafe_op_in_unsafe_fn)]

//! The sole raw FFI boundary for the native ART/HWUI engine.
//!
//! This crate declares opaque pointers and POD function tables only. Safe
//! ownership wrappers belong in the higher-level runtime crate; no Rust
//! slice, STL object, or borrowed string crosses this boundary.

use core::ffi::{c_char, c_void};
use darwin_art_abi::{ABI_VERSION, StatusCode};

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

#[repr(C)]
pub struct ProcessConfig {
    pub struct_size: u32,
    pub abi_version: u32,
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
            struct_size: core::mem::size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
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
        }
    }

    pub const fn is_compatible(&self) -> bool {
        self.abi_version == ABI_VERSION && self.struct_size as usize >= core::mem::size_of::<Self>()
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub struct ProcessResult {
    pub struct_size: u32,
    pub abi_version: u32,
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
            struct_size: core::mem::size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
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
        self.abi_version == ABI_VERSION && self.struct_size as usize >= core::mem::size_of::<Self>()
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
