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
pub fn owned_surface_wait_slice(runtime: &HostRuntime, seconds: f64) -> i32 {
    // AppKit's main actor owns NSApplication event delivery. The ART owner
    // only yields briefly, then observes the worker-safe close snapshot; it
    // must never synchronously dispatch to the main queue from this path.
    if owned_surface(runtime).is_none_or(SurfaceSession::close_requested) {
        return 7;
    }
    if seconds.is_finite() && seconds > 0.0 {
        std::thread::sleep(std::time::Duration::from_secs_f64(seconds.min(0.016)));
    }
    if owned_surface(runtime).is_none_or(SurfaceSession::close_requested) {
        7
    } else {
        0
    }
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
