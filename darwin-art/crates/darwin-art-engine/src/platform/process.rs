//! Owned process request for the versioned ART entrypoint.
//!
//! The host may prepare paths and callback contexts, but it must not build a
//! raw `ProcessConfig` or keep the backing C strings in a separate state
//! object. This request owns every byte referenced by the synchronous native
//! call and materializes the wire struct only inside `EngineSession`.

use core::ffi::c_void;
use darwin_art_engine_sys::{FrameCallback, ProcessConfig, ProviderAcquireFn, ProviderReleaseFn};
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub enum ProcessRequestError {
    InteriorNul(PathBuf),
}

/// Owns the complete input lifetime for one synchronous ART process call.
pub struct ProcessRequest {
    core_oj_jar: CString,
    core_libart_jar: CString,
    framework_jar: CString,
    core_icu4j_jar: CString,
    app_dex: CString,
    heap_initial_bytes: u64,
    heap_maximum_bytes: u64,
    host_context: *mut c_void,
    frame_callback: Option<FrameCallback>,
    provider_context: *mut c_void,
    provider_acquire: Option<ProviderAcquireFn>,
    provider_release: Option<ProviderReleaseFn>,
    graphics_session_context: *mut c_void,
}

impl ProcessRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        core_oj_jar: &Path,
        core_libart_jar: &Path,
        framework_jar: &Path,
        core_icu4j_jar: &Path,
        app_dex: &Path,
        heap_initial_bytes: u64,
        heap_maximum_bytes: u64,
    ) -> Result<Self, ProcessRequestError> {
        Ok(Self {
            core_oj_jar: path_c_string(core_oj_jar)?,
            core_libart_jar: path_c_string(core_libart_jar)?,
            framework_jar: path_c_string(framework_jar)?,
            core_icu4j_jar: path_c_string(core_icu4j_jar)?,
            app_dex: path_c_string(app_dex)?,
            heap_initial_bytes,
            heap_maximum_bytes,
            host_context: core::ptr::null_mut(),
            frame_callback: None,
            provider_context: core::ptr::null_mut(),
            provider_acquire: None,
            provider_release: None,
            graphics_session_context: core::ptr::null_mut(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    /// Bind raw callback state for the synchronous ART call.
    ///
    /// # Safety
    ///
    /// Every context pointer must remain valid, and every callback must remain
    /// callable, until `EngineSession::run_request` returns. The native ART
    /// graph may invoke provider callbacks on attached ART threads during that
    /// call, so the context must also be synchronized for those callbacks.
    pub unsafe fn bind_callbacks(
        &mut self,
        host_context: *mut c_void,
        frame_callback: Option<FrameCallback>,
        provider_context: *mut c_void,
        provider_acquire: Option<ProviderAcquireFn>,
        provider_release: Option<ProviderReleaseFn>,
        graphics_session_context: *mut c_void,
    ) {
        self.host_context = host_context;
        self.frame_callback = frame_callback;
        self.provider_context = provider_context;
        self.provider_acquire = provider_acquire;
        self.provider_release = provider_release;
        self.graphics_session_context = graphics_session_context;
    }

    pub(crate) fn as_config(&self) -> ProcessConfig {
        ProcessConfig::new(
            self.core_oj_jar.as_ptr(),
            self.core_libart_jar.as_ptr(),
            self.framework_jar.as_ptr(),
            self.core_icu4j_jar.as_ptr(),
            self.app_dex.as_ptr(),
            self.heap_initial_bytes,
            self.heap_maximum_bytes,
            self.host_context,
            self.frame_callback,
            self.provider_context,
            self.provider_acquire,
            self.provider_release,
        )
        .with_graphics_session(self.graphics_session_context)
    }
}

fn path_c_string(path: &Path) -> Result<CString, ProcessRequestError> {
    CString::new(path.as_os_str().as_bytes())
        .map_err(|_| ProcessRequestError::InteriorNul(path.to_owned()))
}
