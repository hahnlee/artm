use crate::config::{HostError, HostOutcome, RunOptions};
use crate::runtime::HostRuntime;
use crate::surface::{owned_surface_next_pointer_event_v2, owned_surface_pump_events};
use darwin_art_engine_sys::{PointerEventV2, ProcessResult};
use darwin_art_runtime::Subsystem;
use std::time::Instant;

#[cfg(target_os = "macos")]
fn pump_frame_with_latency(
    runtime: &mut HostRuntime,
    debug_latency: bool,
    last_input_dispatch: &mut Option<Instant>,
    frame_latencies_us: &mut Vec<u64>,
) -> Result<(), HostError> {
    let pulse_status = runtime
        .graphics()
        .map_or(-1, |graphics| graphics.pump_frame(0));
    if pulse_status != 0 {
        return Err(HostError::RuntimeFailed(pulse_status));
    }
    if debug_latency && let Some(dispatched_at) = last_input_dispatch.take() {
        frame_latencies_us.push(dispatched_at.elapsed().as_micros() as u64);
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn dispatch_queued_events(runtime: &mut HostRuntime) -> Result<u64, HostError> {
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
}

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
    let debug_latency = std::env::var_os("DARWIN_ART_DEBUG_INPUT_LATENCY").is_some();
    let mut last_input_dispatch: Option<Instant> = None;
    let mut frame_latencies_us = Vec::new();
    let test_resize = std::env::var("DARWIN_ART_TEST_WINDOW_RESIZE")
        .ok()
        .and_then(|value| {
            let (width, height) = value.split_once('x').or_else(|| value.split_once(','))?;
            Some((width.parse::<u32>().ok()?, height.parse::<u32>().ok()?))
        });
    if let Some((width, height)) = test_resize {
        let status = runtime
            .surface()
            .map_or(-1, |surface| surface.resize(width, height));
        if status != 0 {
            loop_error = Some(HostError::SurfaceFailed {
                operation: "gpu_test_window_resize",
                status,
            });
        } else {
            eprintln!("DARWIN_ART gpu test resize={width}x{height}");
        }
    }
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
    // Test-only multi-tap input uses the same MotionEvent/DecorView route as
    // native mouse input. Samples after the first wait for their third field
    // (milliseconds) before dispatch: "x,y,0;x,y,500".
    let test_tap_sequence = std::env::var("DARWIN_ART_TEST_POINTER_SEQUENCE")
        .ok()
        .map(|sequence| {
            sequence
                .split(';')
                .filter_map(|sample| {
                    let mut values = sample.split(',');
                    Some((
                        values.next()?.parse::<f32>().ok()?,
                        values.next()?.parse::<f32>().ok()?,
                        values.next()?.parse::<u64>().ok()?,
                    ))
                })
                .collect::<Vec<_>>()
        })
        .filter(|samples| !samples.is_empty());
    let test_pointer = test_drag
        .as_ref()
        .and_then(|points| points.first().copied())
        .or_else(|| {
            test_tap_sequence
                .as_ref()
                .and_then(|samples| samples.first().map(|&(x, y, _)| (x, y)))
        })
        .or(test_pointer);
    let test_hold_ms = std::env::var("DARWIN_ART_TEST_POINTER_HOLD_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0);
    let test_cancel = std::env::var("DARWIN_ART_TEST_POINTER_CANCEL")
        .ok()
        .is_some_and(|value| value == "1" || value.eq_ignore_ascii_case("true"));
    let test_pointer_hz = std::env::var("DARWIN_ART_TEST_POINTER_HZ")
        .ok()
        .and_then(|value| value.parse::<u32>().ok())
        .filter(|hz| *hz > 0);
    let mut synthetic_moves = 0_u64;
    eprintln!(
        "DARWIN_ART gpu test pointer={test_pointer:?} drag_points={} hold_ms={test_hold_ms} cancel={test_cancel} hz={test_pointer_hz:?}",
        test_drag.as_ref().map_or(0, Vec::len),
    );

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

    if loop_error.is_none()
        && let Some((x, y)) = test_pointer
    {
        let down = synthetic_event(0, x, y);
        let dispatch_status = runtime
            .graphics()
            .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&down));
        if dispatch_status != 0 {
            loop_error = Some(HostError::RuntimeFailed(dispatch_status));
        } else {
            if debug_latency {
                last_input_dispatch = Some(Instant::now());
            }
            frames_presented += 1;
            if let Err(error) = pump_frame_with_latency(
                runtime,
                debug_latency,
                &mut last_input_dispatch,
                &mut frame_latencies_us,
            ) {
                loop_error = Some(error);
            }
            let mut held_ms = 0_u64;
            while held_ms < test_hold_ms && loop_error.is_none() {
                let slice_ms = (test_hold_ms - held_ms).min(16);
                let pump_status = owned_surface_pump_events(runtime, slice_ms as f64 / 1000.0);
                if pump_status == 7 {
                    // Surface shutdown enqueues a terminal CANCEL for an
                    // active pointer stream. Drain it before leaving so the
                    // Android hierarchy cannot retain pressed state.
                    let _ = dispatch_queued_events(runtime);
                    break;
                }
                if pump_status != 0 {
                    loop_error = Some(HostError::SurfaceFailed {
                        operation: "gpu_test_pointer_pump",
                        status: pump_status,
                    });
                    break;
                }
                match dispatch_queued_events(runtime) {
                    Ok(dispatched) => {
                        frames_presented += dispatched;
                        if debug_latency && dispatched > 0 {
                            last_input_dispatch = Some(Instant::now());
                        }
                    }
                    Err(error) => loop_error = Some(error),
                }
                if loop_error.is_none() {
                    // ACTION_MOVE is dispatched before the pulse so the
                    // next framework frame observes the newest pointer
                    // state, matching a native input/vsync handoff.
                    let moves_this_slice = test_pointer_hz
                        .map(|hz| ((slice_ms * u64::from(hz) + 999) / 1000).max(1))
                        .unwrap_or(1);
                    for move_offset in 0..moves_this_slice {
                        let move_index = (held_ms / 16 + move_offset + 1) as usize;
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
                            break;
                        }
                        if debug_latency {
                            last_input_dispatch = Some(Instant::now());
                        }
                        synthetic_moves += 1;
                    }
                    if loop_error.is_none()
                        && let Err(error) = pump_frame_with_latency(
                            runtime,
                            debug_latency,
                            &mut last_input_dispatch,
                            &mut frame_latencies_us,
                        )
                    {
                        loop_error = Some(error);
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
                    if debug_latency {
                        last_input_dispatch = Some(Instant::now());
                    }
                    frames_presented += 1;
                    if let Err(error) = pump_frame_with_latency(
                        runtime,
                        debug_latency,
                        &mut last_input_dispatch,
                        &mut frame_latencies_us,
                    ) {
                        loop_error = Some(error);
                    }
                }
            }
        }
    }
    if loop_error.is_none()
        && let Some(sequence) = test_tap_sequence.as_ref()
    {
        for &(x, y, delay_ms) in sequence.iter().skip(1) {
            let mut waited_ms = 0_u64;
            while waited_ms < delay_ms && loop_error.is_none() {
                let slice_ms = (delay_ms - waited_ms).min(16);
                let pump_status = owned_surface_pump_events(runtime, slice_ms as f64 / 1000.0);
                if pump_status != 0 {
                    loop_error = Some(HostError::SurfaceFailed {
                        operation: "gpu_test_pointer_sequence_wait",
                        status: pump_status,
                    });
                    break;
                }
                if let Err(error) = dispatch_queued_events(runtime) {
                    loop_error = Some(error);
                    break;
                }
                if let Err(error) = pump_frame_with_latency(
                    runtime,
                    debug_latency,
                    &mut last_input_dispatch,
                    &mut frame_latencies_us,
                ) {
                    loop_error = Some(error);
                    break;
                }
                waited_ms += slice_ms;
            }
            if loop_error.is_some() {
                break;
            }
            for action in [0_u32, 1_u32] {
                let event = synthetic_event(action, x, y);
                let status = runtime
                    .graphics()
                    .map_or(-1, |graphics| graphics.dispatch_pointer_v2(&event));
                if status != 0 {
                    loop_error = Some(HostError::RuntimeFailed(status));
                    break;
                }
                frames_presented += 1;
                if let Err(error) = pump_frame_with_latency(
                    runtime,
                    debug_latency,
                    &mut last_input_dispatch,
                    &mut frame_latencies_us,
                ) {
                    loop_error = Some(error);
                    break;
                }
            }
        }
    }
    while remaining > 0.0 {
        let slice = remaining.min(0.016);
        let pump_status = owned_surface_pump_events(runtime, slice);
        if pump_status == 7 {
            let _ = dispatch_queued_events(runtime);
            break;
        }
        if pump_status != 0 {
            loop_error = Some(HostError::SurfaceFailed {
                operation: "gpu_pump_events",
                status: pump_status,
            });
            break;
        }
        match dispatch_queued_events(runtime) {
            Ok(dispatched) => {
                frames_presented += dispatched;
                if debug_latency && dispatched > 0 {
                    last_input_dispatch = Some(Instant::now());
                }
            }
            Err(error) => loop_error = Some(error),
        }
        if loop_error.is_none() {
            if let Err(error) = pump_frame_with_latency(
                runtime,
                debug_latency,
                &mut last_input_dispatch,
                &mut frame_latencies_us,
            ) {
                loop_error = Some(error);
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
    if test_pointer.is_some() {
        eprintln!("DARWIN_ART gpu synthetic moves dispatched={synthetic_moves}");
    }
    if debug_latency && !frame_latencies_us.is_empty() {
        frame_latencies_us.sort_unstable();
        let percentile = |numerator: usize, denominator: usize| -> u64 {
            let index = (frame_latencies_us.len() * numerator)
                .div_ceil(denominator)
                .saturating_sub(1);
            frame_latencies_us[index.min(frame_latencies_us.len() - 1)]
        };
        eprintln!(
            "DARWIN_ART input->framework-pulse samples={} p50_us={} p95_us={} p99_us={} max_us={}",
            frame_latencies_us.len(),
            percentile(50, 100),
            percentile(95, 100),
            percentile(99, 100),
            frame_latencies_us.last().copied().unwrap_or(0),
        );
    }
    Ok(HostOutcome {
        process,
        frames_presented,
        last_frame: None,
    })
}
