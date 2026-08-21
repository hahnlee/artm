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
use std::ptr::NonNull;

use super::graphics::GraphicsSession;

#[derive(Debug)]
pub enum ProcessRequestError {
    InteriorNul(PathBuf),
    MissingCallbackContext(&'static str),
}

/// Validated callback/context bundle for one synchronous ART invocation.
///
/// The only raw-pointer construction lives in `from_raw`; the request and
/// engine layers carry this value by ownership and cannot accidentally omit a
/// required context or pass an unpaired provider callback.
#[derive(Clone, Copy)]
pub struct CallbackBindings {
    host_context: NonNull<c_void>,
    frame_callback: Option<FrameCallback>,
    provider_context: NonNull<c_void>,
    provider_acquire: Option<ProviderAcquireFn>,
    provider_release: Option<ProviderReleaseFn>,
    graphics_session_context: Option<NonNull<c_void>>,
}

impl CallbackBindings {
    /// # Safety
    ///
    /// Each non-null context must remain valid and synchronized, and each
    /// callback must remain callable, until the synchronous engine call
    /// returns. The native ART graph may invoke provider callbacks on
    /// attached ART threads during that call.
    pub unsafe fn from_raw(
        host_context: *mut c_void,
        frame_callback: Option<FrameCallback>,
        provider_context: *mut c_void,
        provider_acquire: Option<ProviderAcquireFn>,
        provider_release: Option<ProviderReleaseFn>,
        graphics_session_context: *mut c_void,
    ) -> Result<Self, ProcessRequestError> {
        let host_context = NonNull::new(host_context)
            .ok_or(ProcessRequestError::MissingCallbackContext("host"))?;
        let provider_context = NonNull::new(provider_context)
            .ok_or(ProcessRequestError::MissingCallbackContext("provider"))?;
        if provider_acquire.is_some() != provider_release.is_some() {
            return Err(ProcessRequestError::MissingCallbackContext(
                "provider callback pair",
            ));
        }
        Ok(Self {
            host_context,
            frame_callback,
            provider_context,
            provider_acquire,
            provider_release,
            graphics_session_context: NonNull::new(graphics_session_context),
        })
    }

    /// Attach the live graphics owner without exposing its opaque ABI pointer
    /// to host orchestration. The request borrows the session for the same
    /// synchronous call in which the wire config is materialized.
    pub fn with_graphics_session(mut self, session: Option<&GraphicsSession>) -> Self {
        self.graphics_session_context =
            session.and_then(|graphics| NonNull::new(graphics.raw_handle().cast()));
        self
    }
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
    callbacks: CallbackBindings,
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
        callbacks: CallbackBindings,
    ) -> Result<Self, ProcessRequestError> {
        Ok(Self {
            core_oj_jar: path_c_string(core_oj_jar)?,
            core_libart_jar: path_c_string(core_libart_jar)?,
            framework_jar: path_c_string(framework_jar)?,
            core_icu4j_jar: path_c_string(core_icu4j_jar)?,
            app_dex: path_c_string(app_dex)?,
            heap_initial_bytes,
            heap_maximum_bytes,
            callbacks,
        })
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
            self.callbacks.host_context.as_ptr(),
            self.callbacks.frame_callback,
            self.callbacks.provider_context.as_ptr(),
            self.callbacks.provider_acquire,
            self.callbacks.provider_release,
        )
        .with_graphics_session(
            self.callbacks
                .graphics_session_context
                .map_or(core::ptr::null_mut(), NonNull::as_ptr),
        )
    }
}

fn path_c_string(path: &Path) -> Result<CString, ProcessRequestError> {
    CString::new(path.as_os_str().as_bytes())
        .map_err(|_| ProcessRequestError::InteriorNul(path.to_owned()))
}
