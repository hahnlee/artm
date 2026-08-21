//! Owner-bound surface and provider operations.

#[cfg(target_os = "macos")]
use darwin_art_engine::SurfaceSession;

#[cfg(target_os = "macos")]
use crate::runtime::HostRuntime;

#[cfg(target_os = "macos")]
pub fn owned_surface(runtime: &HostRuntime) -> Option<&SurfaceSession> {
    runtime.surface()
}

#[cfg(target_os = "macos")]
pub fn owned_surface_pump_events(runtime: &HostRuntime, seconds: f64) -> i32 {
    owned_surface(runtime).map_or(-1, |surface| surface.pump_events(seconds))
}

#[cfg(target_os = "macos")]
pub fn owned_surface_next_pointer_event(
    runtime: &HostRuntime,
    event: &mut darwin_art_engine_sys::PointerEvent,
) -> bool {
    owned_surface(runtime).is_some_and(|surface| surface.next_pointer_event(event))
}

#[cfg(target_os = "macos")]
pub fn close_surface_owner(runtime: &mut HostRuntime) -> Result<(), i32> {
    let Some(mut surface) = runtime.release_surface().map_err(|_| -1)? else {
        return Ok(());
    };
    let status = surface.close();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
pub fn shutdown_engine_owner(runtime: &mut HostRuntime) -> Result<(), i32> {
    let Some(mut engine) = runtime.release_engine().map_err(|_| -1)? else {
        return Ok(());
    };
    let status = engine.shutdown_once();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
pub fn clear_provider_owner(runtime: &mut HostRuntime) -> i32 {
    let Some(provider) = (match runtime.release_provider() {
        Ok(provider) => provider,
        Err(_) => return -1,
    }) else {
        return 0;
    };
    match provider.clear() {
        Ok(()) => 0,
        Err(()) => -1,
    }
}

#[cfg(target_os = "macos")]
pub fn close_graphics_owner(runtime: &mut HostRuntime) -> Result<(), i32> {
    // Keep the opaque graphics session alive until the canonical engine
    // shutdown callback has finished. The native process state borrows the
    // session's GraphicsState during shutdown; taking it here would drop and
    // destroy that state before `shutdown_engine_owner` reaches it.
    let Some(graphics) = runtime.graphics_for_shutdown_mut().map_err(|_| -1)? else {
        return Ok(());
    };
    let status = graphics.close();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
pub fn release_graphics_owner(runtime: &mut HostRuntime) -> Result<(), i32> {
    let Some(graphics) = runtime.release_graphics().map_err(|_| -1)? else {
        return Ok(());
    };
    drop(graphics);
    Ok(())
}
