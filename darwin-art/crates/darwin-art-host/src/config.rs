//! Stable host configuration and result types.

use std::error::Error;
use std::ffi::c_void;
use std::fmt;
use std::path::PathBuf;

#[cfg(target_os = "macos")]
use darwin_art_engine::{GraphicsSession, ProcessRequest, ProcessRequestError};
use darwin_art_engine_sys::ProcessResult;

use crate::OwnedFrame;

#[derive(Debug)]
pub struct HostOutcome {
    pub process: ProcessResult,
    pub frames_presented: u64,
    pub last_frame: Option<OwnedFrame>,
}

#[derive(Clone, Debug)]
pub struct RunOptions {
    pub library: PathBuf,
    pub core_oj_jar: PathBuf,
    pub core_libart_jar: PathBuf,
    pub framework_jar: PathBuf,
    pub core_icu4j_jar: PathBuf,
    pub app_dex: PathBuf,
    pub heap_initial_bytes: u64,
    pub heap_maximum_bytes: u64,
    pub visible_seconds: f64,
}

const MAX_VISIBLE_SECONDS: f64 = 86_400.0;

impl RunOptions {
    pub(crate) fn validate(&self) -> Result<(), HostError> {
        if !self.visible_seconds.is_finite()
            || self.visible_seconds < 0.0
            || self.visible_seconds > MAX_VISIBLE_SECONDS
        {
            return Err(HostError::InvalidVisibleSeconds(self.visible_seconds));
        }
        Ok(())
    }
}

#[cfg(target_os = "macos")]
pub(crate) fn build_process_request(
    options: &RunOptions,
    host_context: *mut c_void,
    frame_callback: Option<darwin_art_engine_sys::FrameCallback>,
    provider_context: *mut c_void,
    provider_acquire: Option<darwin_art_engine_sys::ProviderAcquireFn>,
    provider_release: Option<darwin_art_engine_sys::ProviderReleaseFn>,
    graphics_session: Option<&GraphicsSession>,
) -> Result<ProcessRequest, HostError> {
    let mut request = ProcessRequest::new(
        &options.core_oj_jar,
        &options.core_libart_jar,
        &options.framework_jar,
        &options.core_icu4j_jar,
        &options.app_dex,
        options.heap_initial_bytes,
        options.heap_maximum_bytes,
    )
    .map_err(|error| match error {
        ProcessRequestError::InteriorNul(path) => HostError::InteriorNul(path),
    })?;
    // SAFETY: the caller keeps `FrameHost`, `ProviderBridge`, and the optional
    // graphics session alive for the complete synchronous `run_request` call;
    // the static callbacks only dereference those contexts during that call.
    unsafe {
        request.bind_callbacks(
            host_context,
            frame_callback,
            provider_context,
            provider_acquire,
            provider_release,
            core::ptr::null_mut(),
        );
    }
    request.bind_graphics_session(graphics_session);
    Ok(request)
}

#[derive(Debug)]
pub enum HostError {
    UnsupportedPlatform,
    InteriorNul(PathBuf),
    DynamicLoader(String),
    InvalidVisibleSeconds(f64),
    RuntimeFailed(i32),
    ShutdownFailed(i32),
    SurfaceFailed {
        operation: &'static str,
        status: i32,
    },
}

impl fmt::Display for HostError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedPlatform => write!(formatter, "darwin-art-host requires macOS"),
            Self::InteriorNul(path) => {
                write!(
                    formatter,
                    "path contains an interior NUL: {}",
                    path.display()
                )
            }
            Self::DynamicLoader(message) => write!(formatter, "dynamic loader: {message}"),
            Self::InvalidVisibleSeconds(seconds) => write!(
                formatter,
                "visible seconds must be finite and in the range 0..=86400: {seconds}"
            ),
            Self::RuntimeFailed(status) => {
                write!(
                    formatter,
                    "darwin_art_run_process failed with status {status}"
                )
            }
            Self::ShutdownFailed(status) => {
                write!(
                    formatter,
                    "darwin_art_shutdown_process failed with status {status}"
                )
            }
            Self::SurfaceFailed { operation, status } => {
                write!(formatter, "surface {operation} failed with status {status}")
            }
        }
    }
}

impl Error for HostError {}
