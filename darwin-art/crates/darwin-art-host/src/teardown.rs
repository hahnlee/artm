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
    process_exit_on_drop: bool,
}

impl<'a> RuntimeShutdownGuard<'a> {
    pub(super) fn new(runtime: &'a mut HostRuntime, process_exit_on_drop: bool) -> Self {
        Self {
            inner: Some(RuntimeOwnerGuard::new(runtime)),
            process_exit_on_drop,
        }
    }

    pub(super) fn runtime(&mut self) -> &mut HostRuntime {
        self.inner
            .as_mut()
            .expect("shutdown guard already consumed")
            .session()
    }

    pub(super) fn shutdown(mut self) -> Result<(), HostError> {
        if self.process_exit_on_drop {
            // APK processes must never enter DestroyJavaVM/ELF teardown,
            // including explicit error cleanup paths. The caller has already
            // recorded the failure; preserve it at the OS process boundary.
            self.inner.take();
            unsafe {
                libc::fflush(std::ptr::null_mut());
                libc::_exit(1);
            }
        }
        self.inner
            .take()
            .expect("shutdown guard already consumed")
            .shutdown()
            .map_err(map_shutdown_error)
    }
}

impl Drop for RuntimeShutdownGuard<'_> {
    fn drop(&mut self) {
        if self.process_exit_on_drop && self.inner.is_some() {
            // An Android APK process is an OS lifetime boundary.  If an
            // in-process error reaches this guard, unloading live Chromium
            // DSOs/DestroyJavaVM is unsafe and unlike AOSP.  Let the kernel
            // reclaim the process instead of running the host teardown path.
            unsafe {
                libc::fflush(std::ptr::null_mut());
                libc::_exit(1);
            }
        }
    }
}

fn map_shutdown_error(error: RuntimeError) -> HostError {
    match error {
        RuntimeError::EngineFailure { status } => HostError::ShutdownFailed(status),
        other => HostError::RuntimeFailed(other.status() as i32),
    }
}
