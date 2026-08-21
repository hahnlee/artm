//! Stable host configuration and result types.

use std::error::Error;
use std::ffi::{CString, c_void};
use std::fmt;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::path::PathBuf;

use darwin_art_engine_sys::{
    FrameCallback, ProcessConfig, ProcessResult, ProviderAcquireFn, ProviderReleaseFn,
};

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

/// Owns the NUL-terminated process paths for the synchronous engine call.
/// Keeping these strings together with the builder makes the pointer lifetime
/// in `ProcessConfig` explicit at the orchestration boundary.
pub(crate) struct ProcessConfigInputs {
    core_oj_jar: CString,
    core_libart_jar: CString,
    framework_jar: CString,
    core_icu4j_jar: CString,
    app_dex: CString,
}

impl ProcessConfigInputs {
    pub(crate) fn from_options(options: &RunOptions) -> Result<Self, HostError> {
        Ok(Self {
            core_oj_jar: path_c_string(&options.core_oj_jar)?,
            core_libart_jar: path_c_string(&options.core_libart_jar)?,
            framework_jar: path_c_string(&options.framework_jar)?,
            core_icu4j_jar: path_c_string(&options.core_icu4j_jar)?,
            app_dex: path_c_string(&options.app_dex)?,
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn build(
        &self,
        options: &RunOptions,
        host_context: *mut c_void,
        frame_callback: Option<FrameCallback>,
        provider_context: *mut c_void,
        provider_acquire: Option<ProviderAcquireFn>,
        provider_release: Option<ProviderReleaseFn>,
        graphics_session_context: *mut c_void,
    ) -> ProcessConfig {
        let mut config = ProcessConfig::new(
            self.core_oj_jar.as_ptr(),
            self.core_libart_jar.as_ptr(),
            self.framework_jar.as_ptr(),
            self.core_icu4j_jar.as_ptr(),
            self.app_dex.as_ptr(),
            options.heap_initial_bytes,
            options.heap_maximum_bytes,
            host_context,
            frame_callback,
            provider_context,
            provider_acquire,
            provider_release,
        );
        config.graphics_session_context = graphics_session_context;
        config
    }
}

fn path_c_string(path: &Path) -> Result<CString, HostError> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| HostError::InteriorNul(path.into()))
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
