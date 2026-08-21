//! Owner-bound surface and provider operations.

#[cfg(target_os = "macos")]
use darwin_art_engine::{EngineSession, GraphicsSession, SurfaceSession};
use darwin_art_runtime::RuntimeOwners;

#[cfg(target_os = "macos")]
use crate::provider::ProviderBridge;

#[cfg(target_os = "macos")]
pub fn owned_surface(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
) -> Option<&SurfaceSession> {
    owners.surface()
}

#[cfg(target_os = "macos")]
pub fn owned_surface_pump_events(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
    seconds: f64,
) -> i32 {
    owned_surface(owners).map_or(-1, |surface| surface.pump_events(seconds))
}

#[cfg(target_os = "macos")]
pub fn owned_surface_next_pointer_event(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
    event: &mut darwin_art_engine_sys::PointerEvent,
) -> bool {
    owned_surface(owners).is_some_and(|surface| surface.next_pointer_event(event))
}

#[cfg(target_os = "macos")]
pub fn close_surface_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
) -> Result<(), i32> {
    let Some(mut surface) = owners.take_surface() else {
        return Ok(());
    };
    let status = surface.close();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
pub fn shutdown_engine_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
) -> Result<(), i32> {
    let Some(mut engine) = owners.take_engine() else {
        return Ok(());
    };
    let status = engine.shutdown_once();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
pub fn clear_provider_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
) -> i32 {
    let Some(provider) = owners.take_provider() else {
        return 0;
    };
    match provider.clear() {
        Ok(()) => 0,
        Err(()) => -1,
    }
}

#[cfg(target_os = "macos")]
pub fn close_graphics_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>,
) -> Result<(), i32> {
    // Keep the opaque graphics session alive until the canonical engine
    // shutdown callback has finished. The native process state borrows the
    // session's GraphicsState during shutdown; taking it here would drop and
    // destroy that state before `shutdown_engine_owner` reaches it.
    let Some(graphics) = owners.graphics_mut() else {
        return Ok(());
    };
    let status = graphics.close();
    if status == 0 { Ok(()) } else { Err(status) }
}
