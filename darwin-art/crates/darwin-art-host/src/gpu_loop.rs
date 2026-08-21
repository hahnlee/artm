use crate::config::{HostError, HostOutcome, RunOptions};
use crate::provider::ProviderBridge;
use crate::surface::{owned_surface_next_pointer_event, owned_surface_pump_events};
use crate::teardown::shutdown_runtime;
use darwin_art_engine::{EngineSession, SurfaceSession};
use darwin_art_engine_sys::{DispatchPointerFn, PointerEvent, ProcessResult, PumpFrameworkFrameFn};
use darwin_art_runtime::{RuntimeSession, Subsystem};

pub(super) struct GpuCallbacks {
    pub dispatch_pointer: DispatchPointerFn,
    pub pump_framework_frame: PumpFrameworkFrameFn,
}

#[cfg(target_os = "macos")]
pub(super) fn run(
    runtime: &mut RuntimeSession<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    active_surface: Option<SurfaceSession>,
    process: ProcessResult,
    options: &RunOptions,
    provider_lease: darwin_art_runtime::SubsystemLease,
    engine_lease: darwin_art_runtime::SubsystemLease,
    callbacks: GpuCallbacks,
) -> Result<HostOutcome, HostError> {
    let GpuCallbacks {
        dispatch_pointer,
        pump_framework_frame,
    } = callbacks;
    let Some(surface) = active_surface else {
        // No surface was published, but ART and the provider
        // bridge are already live.  Roll them back through the
        // same owner-thread LIFO as the normal GPU path; a bare
        // early return here would leave JavaVM/provider hooks
        // resident in the process.
        return match shutdown_runtime(runtime, Some(provider_lease), Some(engine_lease), None) {
            Ok(()) => Err(HostError::SurfaceFailed {
                operation: "gpu_active_surface",
                status: -1,
            }),
            Err(error) => Err(error),
        };
    };
    if let Err(surface) = runtime.attach_surface(surface) {
        // The failed transfer returns ownership to this scope; its Drop
        // closes the native surface. The already-attached engine/provider
        // still need the common rollback path.
        drop(surface);
        let cleanup = shutdown_runtime(runtime, Some(provider_lease), Some(engine_lease), None);
        return match cleanup {
            Ok(()) => Err(HostError::RuntimeFailed(-1)),
            Err(error) => Err(error),
        };
    }
    let surface_lease = match runtime.install_subsystem(Subsystem::Surface) {
        Ok(lease) => lease,
        Err(error) => {
            let cleanup = shutdown_runtime(runtime, Some(provider_lease), Some(engine_lease), None);
            return match cleanup {
                Ok(()) => Err(HostError::RuntimeFailed(error.status() as i32)),
                Err(cleanup_error) => Err(cleanup_error),
            };
        }
    };
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
    let test_hold_ms = std::env::var("DARWIN_ART_TEST_POINTER_HOLD_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0);
    eprintln!("DARWIN_ART gpu test pointer={test_pointer:?} hold_ms={test_hold_ms}");

    let dispatch_queued_events = || -> Result<u64, HostError> {
        let mut dispatched = 0_u64;
        let mut event = PointerEvent::default();
        while owned_surface_next_pointer_event(runtime.owners(), &mut event) {
            let dispatch_status = unsafe { dispatch_pointer(event.action, event.x, event.y) };
            if dispatch_status != 0 {
                return Err(HostError::RuntimeFailed(dispatch_status));
            }
            dispatched += 1;
        }
        Ok(dispatched)
    };

    if let Some((x, y)) = test_pointer {
        let dispatch_status = unsafe { dispatch_pointer(0, x, y) };
        if dispatch_status != 0 {
            loop_error = Some(HostError::RuntimeFailed(dispatch_status));
        } else {
            frames_presented += 1;
            let mut held_ms = 0_u64;
            while held_ms < test_hold_ms && loop_error.is_none() {
                let slice_ms = (test_hold_ms - held_ms).min(16);
                let pump_status =
                    owned_surface_pump_events(runtime.owners(), slice_ms as f64 / 1000.0);
                if pump_status == 7 {
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
                    let pulse_status = unsafe { pump_framework_frame(0) };
                    if pulse_status != 0 {
                        loop_error = Some(HostError::RuntimeFailed(pulse_status));
                        continue;
                    }
                    // ACTION_MOVE causes PresentContent to replay the
                    // Android RenderNode while RippleDrawable's
                    // pressed animation advances between pumps.
                    let dispatch_status = unsafe { dispatch_pointer(2, x, y) };
                    if dispatch_status != 0 {
                        loop_error = Some(HostError::RuntimeFailed(dispatch_status));
                    } else {
                        frames_presented += 1;
                    }
                }
                held_ms += slice_ms;
            }
            if loop_error.is_none() {
                let dispatch_status = unsafe { dispatch_pointer(1, x, y) };
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
        let pump_status = owned_surface_pump_events(runtime.owners(), slice);
        if pump_status == 7 {
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
        // The standalone capture gate has no Android ViewRoot to
        // request redraws after ACTION_UP. Keep the framework pulse
        // and GPU RenderNode replay alive for the test pointer so the
        // real ripple/compatibility bridge can finish on-screen.
        if loop_error.is_none()
            && let Some((x, y)) = test_pointer
        {
            let pulse_status = unsafe { pump_framework_frame(0) };
            if pulse_status != 0 {
                loop_error = Some(HostError::RuntimeFailed(pulse_status));
            } else {
                let replay_status = unsafe { dispatch_pointer(2, x, y) };
                if replay_status != 0 {
                    loop_error = Some(HostError::RuntimeFailed(replay_status));
                } else {
                    frames_presented += 1;
                }
            }
        }
        if loop_error.is_some() {
            break;
        }
        remaining -= slice;
    }
    if let Some(error) = loop_error {
        let cleanup = shutdown_runtime(
            runtime,
            Some(provider_lease),
            Some(engine_lease),
            Some(surface_lease),
        );
        return match cleanup {
            Ok(()) => Err(error),
            Err(cleanup_error) => Err(cleanup_error),
        };
    }
    shutdown_runtime(
        runtime,
        Some(provider_lease),
        Some(engine_lease),
        Some(surface_lease),
    )?;
    Ok(HostOutcome {
        process,
        frames_presented,
        last_frame: None,
    })
}
