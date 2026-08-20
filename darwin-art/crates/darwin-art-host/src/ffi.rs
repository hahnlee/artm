use std::ffi::{c_char, c_void};

pub type FrameCallback = unsafe extern "C" fn(
    context: *mut c_void,
    argb_pixels: *const u32,
    width: u32,
    height: u32,
    stride_bytes: usize,
) -> i32;

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

pub(super) type RunProcessFn =
    unsafe extern "C" fn(*const ProcessConfig, *mut ProcessResult) -> i32;
pub(super) type ShutdownProcessFn = unsafe extern "C" fn() -> i32;

#[derive(Clone, Copy)]
#[repr(C)]
pub(super) struct SurfaceCreateInfo {
    pub(super) width: u32,
    pub(super) height: u32,
    pub(super) title: *const c_char,
    pub(super) visible: bool,
}

pub(super) type SurfaceCreateFn =
    unsafe extern "C" fn(*const SurfaceCreateInfo, *mut i32) -> *mut c_void;
pub(super) type SurfaceUpdateFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> i32;
pub(super) type SurfacePresentFn = unsafe extern "C" fn(*mut c_void) -> i32;
pub(super) type SurfacePumpEventsFn = unsafe extern "C" fn(*mut c_void, f64) -> i32;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub(super) struct PointerEvent {
    pub(super) action: u32,
    pub(super) x: f32,
    pub(super) y: f32,
}

pub(super) type SurfaceNextPointerEventFn =
    unsafe extern "C" fn(*mut c_void, *mut PointerEvent) -> bool;
pub(super) type SurfaceDestroyFn = unsafe extern "C" fn(*mut c_void) -> i32;
pub(super) type SurfaceActiveFn = unsafe extern "C" fn() -> *mut c_void;
pub(super) type DispatchPointerFn = unsafe extern "C" fn(u32, f32, f32) -> i32;
pub(super) type PumpFrameworkFrameFn = unsafe extern "C" fn(i64) -> i32;
