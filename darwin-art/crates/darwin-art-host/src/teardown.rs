//! Single owner-thread shutdown path for graphics and headless runs.

use crate::config::HostError;
use crate::runtime::HostRuntime;

/// Owns the final shutdown obligation after the runtime has entered its
/// running phase.  Moving this obligation into a guard makes every later
/// error path (surface attach, graphics install, or frame loop) use the same
/// reverse-order teardown, including newly added paths.
pub(super) struct RuntimeShutdownGuard<'a> {
    runtime: &'a mut HostRuntime,
    armed: bool,
}

impl<'a> RuntimeShutdownGuard<'a> {
    pub(super) fn new(runtime: &'a mut HostRuntime) -> Self {
        Self {
            runtime,
            armed: true,
        }
    }

    pub(super) fn runtime(&mut self) -> &mut HostRuntime {
        self.runtime
    }

    pub(super) fn shutdown(mut self) -> Result<(), HostError> {
        let result = shutdown_runtime(self.runtime);
        self.armed = false;
        result
    }
}

impl Drop for RuntimeShutdownGuard<'_> {
    fn drop(&mut self) {
        if self.armed {
            let _ = shutdown_runtime(self.runtime);
        }
    }
}

pub(super) fn shutdown_runtime(runtime: &mut HostRuntime) -> Result<(), HostError> {
    runtime.shutdown_native().map_err(|error| match error {
        darwin_art_runtime::RuntimeError::EngineFailure { status } => {
            HostError::ShutdownFailed(status)
        }
        other => HostError::RuntimeFailed(other.status() as i32),
    })
}
