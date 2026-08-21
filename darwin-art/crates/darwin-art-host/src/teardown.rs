//! Single owner-thread shutdown path for graphics and headless runs.

use crate::config::HostError;
use crate::runtime::HostRuntime;
use darwin_art_engine::{EngineSession, GraphicsSession, SurfaceSession};
use darwin_art_runtime::{ProviderBridge, RuntimeError, ShutdownGuard as RuntimeOwnerGuard};

/// Owns the final shutdown obligation after the runtime has entered its
/// running phase.  Moving this obligation into a guard makes every later
/// error path (surface attach, graphics install, or frame loop) use the same
/// reverse-order teardown, including newly added paths.
pub(super) struct RuntimeShutdownGuard<'a> {
    inner: Option<
        RuntimeOwnerGuard<'a, EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
    >,
}

impl<'a> RuntimeShutdownGuard<'a> {
    pub(super) fn new(runtime: &'a mut HostRuntime) -> Self {
        Self {
            inner: Some(RuntimeOwnerGuard::new(runtime)),
        }
    }

    pub(super) fn runtime(&mut self) -> &mut HostRuntime {
        self.inner
            .as_mut()
            .expect("shutdown guard already consumed")
            .session()
    }

    pub(super) fn shutdown(mut self) -> Result<(), HostError> {
        self.inner
            .take()
            .expect("shutdown guard already consumed")
            .shutdown()
            .map_err(map_shutdown_error)
    }
}

fn map_shutdown_error(error: RuntimeError) -> HostError {
    match error {
        RuntimeError::EngineFailure { status } => HostError::ShutdownFailed(status),
        other => HostError::RuntimeFailed(other.status() as i32),
    }
}
