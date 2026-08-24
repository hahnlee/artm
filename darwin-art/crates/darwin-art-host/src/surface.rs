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
#[allow(dead_code)]
pub fn owned_surface_next_pointer_event(
    runtime: &HostRuntime,
    event: &mut darwin_art_engine_sys::PointerEvent,
) -> bool {
    owned_surface(runtime).is_some_and(|surface| surface.next_pointer_event(event))
}

#[cfg(target_os = "macos")]
pub fn owned_surface_next_pointer_event_v2(
    runtime: &HostRuntime,
    event: &mut darwin_art_engine_sys::PointerEventV2,
) -> bool {
    let Some(surface) = owned_surface(runtime) else {
        return false;
    };
    if surface.next_pointer_event_v2(event) {
        return true;
    }
    let mut legacy = darwin_art_engine_sys::PointerEvent::default();
    if !surface.next_pointer_event(&mut legacy) {
        return false;
    }
    event.version = 2;
    event.size = std::mem::size_of::<darwin_art_engine_sys::PointerEventV2>() as u32;
    event.action = legacy.action;
    event.x = legacy.x;
    event.y = legacy.y;
    event.raw_x = legacy.x;
    event.raw_y = legacy.y;
    event.pointer_id = 0;
    event.pointer_count = 1;
    event.pressure = 1.0;
    event.size_value = 1.0;
    true
}

#[cfg(target_os = "macos")]
pub fn owned_surface_next_key_event_v1(
    runtime: &HostRuntime,
    event: &mut darwin_art_engine_sys::KeyEventV1,
) -> bool {
    owned_surface(runtime).is_some_and(|surface| surface.next_key_event_v1(event))
}
