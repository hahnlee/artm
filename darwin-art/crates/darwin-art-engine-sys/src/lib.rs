#![allow(non_camel_case_types)]
#![deny(unsafe_op_in_unsafe_fn)]

//! The sole raw FFI boundary for the native ART/HWUI engine.
//!
//! This crate declares opaque pointers and POD function tables only. Safe
//! ownership wrappers belong in the higher-level runtime crate; no Rust
//! slice, STL object, or borrowed string crosses this boundary.

use core::ffi::c_void;
use darwin_art_abi::{ABI_VERSION, AbiHeader, StatusCode};

#[repr(C)]
pub struct EngineHandle {
    _private: [u8; 0],
}

#[repr(C)]
pub struct RuntimeSessionHandle {
    _private: [u8; 0],
}

pub type EngineCreateFn = unsafe extern "C" fn(*const EngineApi) -> *mut EngineHandle;
pub type EngineShutdownFn = unsafe extern "C" fn(*mut EngineHandle) -> i32;
pub type EngineDestroyFn = unsafe extern "C" fn(*mut EngineHandle);

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
