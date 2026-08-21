//! Rust-owned native shutdown transaction.
//!
//! The session API exposes lifecycle/owner operations, while this module
//! contains the one dependency-ordered transaction that closes them. Keeping
//! the transaction separate prevents new runtime states from silently
//! acquiring a second teardown path in the host or engine crates.

use crate::{NativeResource, RuntimeError, RuntimeSession, Subsystem};

pub(crate) fn shutdown_native<E, P, S, G>(
    session: &mut RuntimeSession<E, P, S, G>,
) -> Result<(), RuntimeError>
where
    E: NativeResource,
    P: NativeResource,
    S: NativeResource,
    G: NativeResource,
{
    let mut first_status = session
        .begin_shutdown()
        .err()
        .map(|error| error.status() as i32);
    let mut remember = |status: i32| {
        if status != 0 && first_status.is_none() {
            first_status = Some(status);
        }
    };

    if session.surface().is_some() {
        if let Err(error) = session.remove_expected_subsystem(Subsystem::Surface) {
            remember(error.status() as i32);
        }
        if let Some(surface) = session.surface_mut() {
            remember(surface.close());
        }
        if let Err(error) = session.release_surface() {
            remember(error.status() as i32);
        }
    }

    if session.graphics().is_some() {
        if let Err(error) = session.remove_expected_subsystem(Subsystem::Graphics) {
            remember(error.status() as i32);
        }
        if let Ok(Some(graphics)) = session.graphics_for_shutdown_mut() {
            remember(graphics.close());
        }
    }

    if session.provider().is_some()
        && let Err(error) = session.remove_expected_subsystem(Subsystem::ElfNamespace)
    {
        remember(error.status() as i32);
    }

    if session.engine().is_some()
        && let Err(error) = session.remove_expected_subsystem(Subsystem::Engine)
    {
        remember(error.status() as i32);
    }

    // DestroyJavaVM may still execute provider-backed code, so the engine
    // closes before provider hooks are cleared and the image is dropped.
    if let Some(engine) = session.engine_mut() {
        remember(engine.close());
    }
    if let Err(error) = session.release_engine() {
        remember(error.status() as i32);
    }

    if let Some(provider) = session.provider_mut() {
        remember(provider.clear());
    }
    if let Err(error) = session.release_provider() {
        remember(error.status() as i32);
    }
    if let Err(error) = session.release_graphics()
        && session.graphics().is_some()
    {
        remember(error.status() as i32);
    }

    if first_status.is_none()
        && let Err(error) = session.finish_shutdown()
    {
        first_status = Some(error.status() as i32);
    }
    if let Some(status) = first_status {
        session.fail(RuntimeError::EngineFailure { status });
        Err(RuntimeError::EngineFailure { status })
    } else {
        Ok(())
    }
}
