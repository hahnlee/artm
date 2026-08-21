//! Single owner-thread shutdown path for graphics and headless runs.

use crate::config::HostError;
use crate::provider::ProviderBridge;
use crate::surface::{
    clear_provider_owner, close_graphics_owner, close_surface_owner, shutdown_engine_owner,
};
use darwin_art_engine::{EngineSession, GraphicsSession, SurfaceSession};
use darwin_art_runtime::RuntimeSession;
use darwin_art_runtime::{RuntimeError, SubsystemLease};

/// Roll back a process call before its engine has transferred into the
/// `RuntimeSession`. The engine and optional graphics session are still local
/// owners here, so this boundary performs their close/shutdown operations and
/// preserves the historical error precedence.
pub(super) fn process_run_failure(
    runtime: &mut RuntimeSession<
        EngineSession,
        Box<ProviderBridge>,
        SurfaceSession,
        GraphicsSession,
    >,
    engine: &mut EngineSession,
    graphics_session: Option<&mut GraphicsSession>,
    status: i32,
) -> HostError {
    runtime.fail(RuntimeError::EngineFailure { status });
    if let Some(session) = graphics_session {
        let _ = session.close();
    }
    // A late run-stage failure may still have created ART. Ask the process ABI
    // to tear it down; NOT_READY means creation never completed and is the
    // only benign shutdown result here.
    let shutdown_status = engine.shutdown_once();
    const SHUTDOWN_NOT_READY: i32 = 67;
    if shutdown_status != 0 && shutdown_status != SHUTDOWN_NOT_READY {
        return HostError::ShutdownFailed(shutdown_status);
    }
    engine.clear_provider_hooks();
    HostError::RuntimeFailed(status)
}

/// Roll back an engine whose transfer into `RuntimeSession` was rejected.
/// Nothing in this path is owned by the runtime yet, so it must remain
/// separate from `shutdown_runtime`'s lease-driven path.
pub(super) fn unattached_engine_failure(
    engine: &mut EngineSession,
    graphics_session: Option<&mut GraphicsSession>,
    provider_bridge: &ProviderBridge,
) -> HostError {
    if let Some(session) = graphics_session {
        let _ = session.close();
    }
    let _ = engine.shutdown_once();
    let _ = provider_bridge.clear();
    HostError::RuntimeFailed(-1)
}

pub(super) fn shutdown_runtime(
    runtime: &mut RuntimeSession<
        EngineSession,
        Box<ProviderBridge>,
        SurfaceSession,
        GraphicsSession,
    >,
    provider_lease: Option<SubsystemLease>,
    engine_lease: Option<SubsystemLease>,
    surface_lease: Option<SubsystemLease>,
    graphics_lease: Option<SubsystemLease>,
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

    if let Some(lease) = graphics_lease
        && let Err(error) = runtime.uninstall_subsystem(lease)
    {
        remember_error(&mut first_error, map_shutdown_error("graphics", error));
    }
    if runtime.owners().graphics().is_some()
        && let Err(status) = close_graphics_owner(runtime.owners_mut())
    {
        remember_error(&mut first_error, HostError::RuntimeFailed(status));
    }

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
