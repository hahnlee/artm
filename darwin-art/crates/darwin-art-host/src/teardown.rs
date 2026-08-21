//! Single owner-thread shutdown path for graphics and headless runs.

use super::{
    HostError, ProviderBridge, RuntimeError, RuntimeSession, SurfaceSession, clear_provider_owner,
    close_surface_owner, shutdown_engine_owner,
};
use darwin_art_engine::EngineSession;
use darwin_art_runtime::SubsystemLease;

pub(super) fn shutdown_runtime(
    runtime: &mut RuntimeSession<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    provider_lease: SubsystemLease,
    engine_lease: SubsystemLease,
    surface_lease: Option<SubsystemLease>,
) -> Result<(), HostError> {
    let result = (|| {
        runtime
            .begin_shutdown()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;

        if let Some(lease) = surface_lease {
            runtime
                .uninstall_subsystem(lease)
                .map_err(|error| map_shutdown_error("destroy", error))?;
            close_surface_owner(runtime.owners_mut()).map_err(|status| {
                HostError::SurfaceFailed {
                    operation: "destroy",
                    status,
                }
            })?;
        }

        runtime
            .uninstall_subsystem(engine_lease)
            .map_err(|error| map_shutdown_error("engine", error))?;
        shutdown_engine_owner(runtime.owners_mut())
            .map_err(|status| HostError::ShutdownFailed(status))?;

        runtime
            .uninstall_subsystem(provider_lease)
            .map_err(|error| map_shutdown_error("provider", error))?;
        let provider_status = clear_provider_owner(runtime.owners_mut());
        if provider_status != 0 {
            return Err(HostError::ShutdownFailed(provider_status));
        }

        runtime
            .finish_shutdown()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))
    })();

    if let Err(error) = &result {
        runtime.fail(RuntimeError::EngineFailure {
            status: error_status(error),
        });
    }
    result
}

fn map_shutdown_error(operation: &'static str, error: RuntimeError) -> HostError {
    match error {
        RuntimeError::EngineFailure { status } if operation == "destroy" => {
            HostError::SurfaceFailed { operation, status }
        }
        RuntimeError::EngineFailure { status } => HostError::ShutdownFailed(status),
        other => HostError::RuntimeFailed(other.status() as i32),
    }
}

fn error_status(error: &HostError) -> i32 {
    match error {
        HostError::RuntimeFailed(status)
        | HostError::ShutdownFailed(status)
        | HostError::SurfaceFailed { status, .. } => *status,
        _ => -1,
    }
}
