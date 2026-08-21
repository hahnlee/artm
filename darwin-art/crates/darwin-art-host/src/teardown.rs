//! Single owner-thread shutdown path for graphics and headless runs.

use super::{
    HostError, ProviderBridge, RuntimeError, RuntimeSession, SurfaceSession, clear_provider_owner,
    close_surface_owner, shutdown_engine_owner,
};
use darwin_art_engine::EngineSession;
use darwin_art_runtime::SubsystemLease;

pub(super) fn shutdown_runtime(
    runtime: &mut RuntimeSession<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    provider_lease: Option<SubsystemLease>,
    engine_lease: Option<SubsystemLease>,
    surface_lease: Option<SubsystemLease>,
) -> Result<(), HostError> {
    // Cleanup is deliberately best-effort after shutdown begins. A failing
    // surface destroy must not prevent ART shutdown, and a provider-hook
    // failure must not leave the engine image resident. Each owner is taken
    // before its native close operation, so a repeated call is harmless and
    // cannot invoke a destroy/shutdown callback twice.
    let mut first_error = runtime
        .begin_shutdown()
        .err()
        .map(|error| HostError::RuntimeFailed(error.status() as i32));

    if let Some(lease) = surface_lease
        && let Err(error) = runtime.uninstall_subsystem(lease)
    {
        remember_error(&mut first_error, map_shutdown_error("destroy", error));
    }
    if runtime.owners().surface().is_some()
        && let Err(status) = close_surface_owner(runtime.owners_mut())
    {
        remember_error(
            &mut first_error,
            HostError::SurfaceFailed {
                operation: "destroy",
                status,
            },
        );
    }

    if let Some(lease) = engine_lease
        && let Err(error) = runtime.uninstall_subsystem(lease)
    {
        remember_error(&mut first_error, map_shutdown_error("engine", error));
    }
    if runtime.owners().engine().is_some()
        && let Err(status) = shutdown_engine_owner(runtime.owners_mut())
    {
        remember_error(&mut first_error, HostError::ShutdownFailed(status));
    }

    if let Some(lease) = provider_lease
        && let Err(error) = runtime.uninstall_subsystem(lease)
    {
        remember_error(&mut first_error, map_shutdown_error("provider", error));
    }
    if runtime.owners().provider().is_some() {
        let provider_status = clear_provider_owner(runtime.owners_mut());
        if provider_status != 0 {
            remember_error(&mut first_error, HostError::ShutdownFailed(provider_status));
        }
    }

    if first_error.is_none()
        && let Err(error) = runtime.finish_shutdown()
    {
        remember_error(
            &mut first_error,
            HostError::RuntimeFailed(error.status() as i32),
        );
    }

    if let Some(error) = first_error {
        runtime.fail(RuntimeError::EngineFailure {
            status: error_status(&error),
        });
        Err(error)
    } else {
        Ok(())
    }
}

fn remember_error(slot: &mut Option<HostError>, error: HostError) {
    if slot.is_none() {
        *slot = Some(error);
    }
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
