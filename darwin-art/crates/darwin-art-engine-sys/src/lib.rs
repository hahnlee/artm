#![allow(non_camel_case_types)]
#![deny(unsafe_op_in_unsafe_fn)]

//! The sole raw FFI boundary for the native ART/HWUI engine.
//!
//! This crate declares opaque pointers and POD function tables only. Safe
//! ownership wrappers belong in the higher-level runtime crate; no Rust
//! slice, STL object, or borrowed string crosses this boundary.

use core::ffi::{c_char, c_void};
use darwin_art_abi::{ABI_VERSION, AbiHeader, StatusCode};

#[repr(C)]
pub struct EngineHandle {
    _private: [u8; 0],
}

#[repr(C)]
pub struct RuntimeSessionHandle {
    _private: [u8; 0],
}

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

pub type EngineCreateFn = unsafe extern "C" fn(*const EngineApi) -> *mut EngineHandle;
pub type EngineShutdownFn = unsafe extern "C" fn(*mut EngineHandle) -> i32;
pub type EngineDestroyFn = unsafe extern "C" fn(*mut EngineHandle);

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

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineApi {
    pub header: AbiHeader,
    pub create: Option<EngineCreateFn>,
    pub shutdown: Option<EngineShutdownFn>,
    pub destroy: Option<EngineDestroyFn>,
}

impl EngineApi {
    pub const fn empty() -> Self {
        Self {
            header: AbiHeader::new(core::mem::size_of::<Self>()),
            create: None,
            shutdown: None,
            destroy: None,
        }
    }

    pub const fn is_compatible(&self) -> bool {
        self.header.abi_version == ABI_VERSION
            && self.header.struct_size as usize >= core::mem::size_of::<AbiHeader>()
    }
}

/// Sentinel used by a missing optional callback without exporting a C++ type.
pub const ENGINE_STATUS_UNAVAILABLE: i32 = StatusCode::Unsupported as i32;

/// Exported C ABI entrypoints will be added here as the runtime wrapper lands.
/// Keeping the declaration in this crate prevents raw FFI from leaking into
/// provider/runtime code while allowing the engine implementation to remain
/// C++/ObjC++.
pub type EngineUserData = *mut c_void;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_table_has_a_versioned_header_and_no_callbacks() {
        let api = EngineApi::empty();
        assert!(api.is_compatible());
        assert!(api.create.is_none());
        assert!(api.shutdown.is_none());
        assert!(api.destroy.is_none());
    }

    #[test]
    fn opaque_handles_are_not_constructible_from_safe_fields() {
        assert_eq!(
            core::mem::size_of::<*mut EngineHandle>(),
            core::mem::size_of::<usize>()
        );
        assert_eq!(
            core::mem::size_of::<*mut RuntimeSessionHandle>(),
            core::mem::size_of::<usize>()
        );
    }
}
