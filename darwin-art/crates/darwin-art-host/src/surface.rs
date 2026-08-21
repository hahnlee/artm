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
