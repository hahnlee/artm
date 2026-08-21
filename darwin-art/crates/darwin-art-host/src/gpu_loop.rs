use super::*;
use darwin_art_engine_sys::PointerEvent;

#[cfg(target_os = "macos")]
pub(super) fn run(
    runtime: &mut RuntimeSession<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    active_surface: Option<SurfaceSession>,
    process: ProcessResult,
    options: &RunOptions,
    provider_lease: darwin_art_runtime::SubsystemLease,
    engine_lease: darwin_art_runtime::SubsystemLease,
    dispatch_pointer: DispatchPointerFn,
    pump_framework_frame: PumpFrameworkFrameFn,
) -> Result<HostOutcome, HostError> {
    let Some(surface) = active_surface else {
        // No surface was published, but ART and the provider
        // bridge are already live.  Roll them back through the
        // same owner-thread LIFO as the normal GPU path; a bare
        // early return here would leave JavaVM/provider hooks
        // resident in the process.
        let cleanup_status = if runtime.begin_shutdown().is_err() {
            -1
        } else {
            match runtime.uninstall_subsystem(engine_lease) {
                Ok(()) => match shutdown_engine_owner(runtime.owners_mut()) {
                    Ok(()) => match runtime.uninstall_subsystem(provider_lease) {
                        Ok(()) => clear_provider_owner(runtime.owners_mut()),
                        Err(RuntimeError::EngineFailure { status }) => status,
                        Err(error) => error.status() as i32,
                    },
                    Err(status) => status,
                },
                Err(RuntimeError::EngineFailure { status }) => status,
                Err(error) => error.status() as i32,
            }
        };
        if cleanup_status == 0 {
            let _ = runtime.finish_shutdown();
        } else {
            runtime.fail(RuntimeError::EngineFailure {
                status: cleanup_status,
            });
        }
        return Err(HostError::SurfaceFailed {
            operation: "gpu_active_surface",
            status: if cleanup_status == 0 {
                -1
            } else {
                cleanup_status
            },
        });
    };
    runtime
        .attach_surface(surface)
        .map_err(|_| HostError::RuntimeFailed(-1))?;
    let surface_lease = runtime
        .install_subsystem(Subsystem::Surface)
        .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
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
    let mut surface_destroy_status = 0;
    let shutdown_status = if runtime.begin_shutdown().is_err() {
        -1
    } else {
        match runtime.uninstall_subsystem(surface_lease) {
            Ok(()) => match close_surface_owner(runtime.owners_mut()) {
                Ok(()) => match runtime.uninstall_subsystem(engine_lease) {
                    Ok(()) => match shutdown_engine_owner(runtime.owners_mut()) {
                        Ok(()) => match runtime.uninstall_subsystem(provider_lease) {
                            Ok(()) => clear_provider_owner(runtime.owners_mut()),
                            Err(RuntimeError::EngineFailure { status }) => status,
                            Err(error) => error.status() as i32,
                        },
                        Err(status) => status,
                    },
                    Err(RuntimeError::EngineFailure { status }) => status,
                    Err(error) => error.status() as i32,
                },
                Err(status) => status,
            },
            Err(RuntimeError::EngineFailure { status }) => {
                surface_destroy_status = status;
                status
            }
            Err(error) => error.status() as i32,
        }
    };
    if shutdown_status == 0 {
        if runtime.finish_shutdown().is_err() {
            runtime.fail(RuntimeError::InvalidTransition {
                from: runtime.phase(),
                to: darwin_art_runtime::RuntimePhase::Stopped,
            });
        }
    } else {
        runtime.fail(RuntimeError::EngineFailure {
            status: shutdown_status,
        });
    }
    if let Some(error) = loop_error {
        return Err(error);
    }
    if surface_destroy_status != 0 {
        return Err(HostError::SurfaceFailed {
            operation: "gpu_destroy",
            status: surface_destroy_status,
        });
    }
    if shutdown_status != 0 {
        return Err(HostError::ShutdownFailed(shutdown_status));
    }
    return Ok(HostOutcome {
        process,
        frames_presented,
        last_frame: None,
    });
}
