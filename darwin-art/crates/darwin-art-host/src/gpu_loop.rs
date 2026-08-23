use crate::config::{HostError, HostOutcome, RunOptions};
use crate::runtime::HostRuntime;
use crate::surface::{owned_surface_next_pointer_event_v2, owned_surface_pump_events};
use darwin_art_engine_sys::{PointerEventV2, ProcessResult};
use darwin_art_runtime::Subsystem;

#[cfg(target_os = "macos")]
pub(super) fn run(
    runtime: &mut HostRuntime,
    process: ProcessResult,
    options: &RunOptions,
    graphics_attached: bool,
) -> Result<HostOutcome, HostError> {
    if runtime.surface().is_none() {
        return Err(HostError::SurfaceFailed {
            operation: "gpu_active_surface",
            status: -1,
        });
    }
    // Graphics is normally already leased before process execution. The
    // surface lease is attached afterward, making it the newest lease so
    // shutdown removes the surface before graphics in strict LIFO order.
    if graphics_attached && !runtime.subsystem_active(Subsystem::Graphics) {
        match runtime.install_subsystem(Subsystem::Graphics) {
            Ok(_) => {}
            Err(error) => return Err(HostError::RuntimeFailed(error.status() as i32)),
        }
    }
    let mut frames_presented = 1_u64;
    let mut remaining = options.visible_seconds;
    let mut loop_error: Option<HostError> = None;
    // Keep the synthetic input path on the same owner thread as ART
    // and the Metal surface.  Each 16 ms pump is the host-side frame
    // cadence: the pointer state is dispatched into Android, then
    // View.draw()/HWUI presents the updated RenderNode directly to
    // the CAMetalLayer drawable.  No CPU framebuffer is involved.
    let test_pointer = std::env::var("DARWIN_ART_TEST_POINTER_CLICK")
        .ok()
        .and_then(|sample| {
            let (x, y) = sample.split_once(',')?;
            Some((x.parse::<f32>().ok()?, y.parse::<f32>().ok()?))
        });
    let test_drag = std::env::var("DARWIN_ART_TEST_POINTER_DRAG")
        .ok()
        .map(|path| {
            path.split(';')
                .filter_map(|sample| {
                    let (x, y) = sample.split_once(',')?;
                    Some((x.parse::<f32>().ok()?, y.parse::<f32>().ok()?))
                })
                .collect::<Vec<_>>()
        })
        .filter(|points| points.len() >= 2);
    let test_pointer = test_drag
        .as_ref()
        .and_then(|points| points.first().copied())
        .or(test_pointer);
    let test_hold_ms = std::env::var("DARWIN_ART_TEST_POINTER_HOLD_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0);
    let test_cancel = std::env::var("DARWIN_ART_TEST_POINTER_CANCEL")
        .ok()
        .is_some_and(|value| value == "1" || value.eq_ignore_ascii_case("true"));
    eprintln!(
        "DARWIN_ART gpu test pointer={test_pointer:?} drag_points={} hold_ms={test_hold_ms} cancel={test_cancel}",
        test_drag.as_ref().map_or(0, Vec::len),
    );

    let dispatch_queued_events = || -> Result<u64, HostError> {
        let mut dispatched = 0_u64;
        let mut event = PointerEventV2::default();
        let mut pending_move: Option<PointerEventV2> = None;
        let mut dispatch = |event: PointerEventV2| -> Result<(), HostError> {
            let dispatch_status = runtime
                .graphics()
                .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&event));
            if dispatch_status != 0 {
                return Err(HostError::RuntimeFailed(dispatch_status));
            }
            dispatched += 1;
            Ok(())
        };
        while owned_surface_next_pointer_event_v2(runtime, &mut event) {
            if event.action == 2 {
                // MOVE is latest-wins within one host poll. DOWN/UP/CANCEL
                // are never coalesced, so gesture boundaries remain exact.
                pending_move = Some(event);
                continue;
            }
            if let Some(move_event) = pending_move.take() {
                dispatch(move_event)?;
            }
            dispatch(event)?;
        }
        if let Some(move_event) = pending_move {
            dispatch(move_event)?;
        }
        Ok(dispatched)
    };

    let synthetic_event = |action: u32, x: f32, y: f32| {
        let mut event = PointerEventV2::default();
        event.version = 2;
        event.size = std::mem::size_of::<PointerEventV2>() as u32;
        event.action = action;
        event.pointer_count = 1;
        event.x = x;
        event.y = y;
        event.raw_x = x;
        event.raw_y = y;
        event.pressure = 1.0;
        event.size_value = 1.0;
        event
    };

    if let Some((x, y)) = test_pointer {
        let down = synthetic_event(0, x, y);
        let dispatch_status = runtime
            .graphics()
            .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&down));
        if dispatch_status != 0 {
            loop_error = Some(HostError::RuntimeFailed(dispatch_status));
        } else {
            frames_presented += 1;
            let pulse_status = runtime
                .graphics()
                .map_or(-1, |graphics| graphics.pump_frame(0));
            if pulse_status != 0 {
                loop_error = Some(HostError::RuntimeFailed(pulse_status));
            }
            let mut held_ms = 0_u64;
            while held_ms < test_hold_ms && loop_error.is_none() {
                let slice_ms = (test_hold_ms - held_ms).min(16);
                let pump_status = owned_surface_pump_events(runtime, slice_ms as f64 / 1000.0);
                if pump_status == 7 {
                    // Surface shutdown enqueues a terminal CANCEL for an
                    // active pointer stream. Drain it before leaving so the
                    // Android hierarchy cannot retain pressed state.
                    let _ = dispatch_queued_events();
                    break;
                }
                if pump_status != 0 {
                    loop_error = Some(HostError::SurfaceFailed {
                        operation: "gpu_test_pointer_pump",
                        status: pump_status,
                    });
                    break;
                }
                match dispatch_queued_events() {
                    Ok(dispatched) => frames_presented += dispatched,
                    Err(error) => loop_error = Some(error),
                }
                if loop_error.is_none() {
                    let pulse_status = runtime
                        .graphics()
                        .map_or(-1, |graphics| graphics.pump_frame(0));
                    if pulse_status != 0 {
                        loop_error = Some(HostError::RuntimeFailed(pulse_status));
                        continue;
                    }
                    // ACTION_MOVE causes PresentContent to replay the
                    // Android RenderNode while RippleDrawable's
                    // pressed animation advances between pumps.
                    let move_index = (held_ms / 16 + 1) as usize;
                    let (move_x, move_y) = test_drag
                        .as_ref()
                        .and_then(|points| points.get(move_index).copied())
                        .unwrap_or((x, y));
                    let move_event = synthetic_event(2, move_x, move_y);
                    let dispatch_status = runtime
                        .graphics()
                        .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&move_event));
                    if dispatch_status != 0 {
                        loop_error = Some(HostError::RuntimeFailed(dispatch_status));
                    } else {
                        frames_presented += 1;
                    }
                }
                held_ms += slice_ms;
            }
            if loop_error.is_none() {
                let (up_x, up_y) = test_drag
                    .as_ref()
                    .and_then(|points| points.last().copied())
                    .unwrap_or((x, y));
                let up = synthetic_event(if test_cancel { 3 } else { 1 }, up_x, up_y);
                let dispatch_status = runtime
                    .graphics()
                    .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&up));
                if dispatch_status != 0 {
                    loop_error = Some(HostError::RuntimeFailed(dispatch_status));
                } else {
                    frames_presented += 1;
                }
            }
        }
    }
    while remaining > 0.0 {
        let slice = remaining.min(0.016);
        let pump_status = owned_surface_pump_events(runtime, slice);
        if pump_status == 7 {
            let _ = dispatch_queued_events();
            break;
        }
        if pump_status != 0 {
            loop_error = Some(HostError::SurfaceFailed {
                operation: "gpu_pump_events",
                status: pump_status,
            });
            break;
        }
        match dispatch_queued_events() {
            Ok(dispatched) => frames_presented += dispatched,
            Err(error) => loop_error = Some(error),
        }
        if loop_error.is_none() {
            let pulse_status = runtime
                .graphics()
                .map_or(-1, |graphics| graphics.pump_frame(0));
            if pulse_status != 0 {
                loop_error = Some(HostError::RuntimeFailed(pulse_status));
            }
        }
        // The standalone capture gate has no Android ViewRoot to
        // request redraws after ACTION_UP. Keep the framework pulse
        // and GPU RenderNode replay alive for the test pointer so the
        // real ripple/compatibility bridge can finish on-screen.
        if loop_error.is_none()
            && let Some((x, y)) = test_pointer
        {
            let move_event = synthetic_event(2, x, y);
            let replay_status = runtime
                .graphics()
                .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&move_event));
            if replay_status != 0 {
                loop_error = Some(HostError::RuntimeFailed(replay_status));
            } else {
                frames_presented += 1;
            }
        }
        if loop_error.is_some() {
            break;
        }
        remaining -= slice;
    }
    if let Some(error) = loop_error {
        return Err(error);
    }
    Ok(HostOutcome {
        process,
        frames_presented,
        last_frame: None,
    })
}
