use std::ffi::CString;
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr;

mod config;
mod frame;
mod provider;
#[cfg(target_os = "macos")]
mod surface;

#[cfg(target_os = "macos")]
use darwin_art_engine::{EngineSession, SurfaceSession};

pub use config::{HostError, HostOutcome, RunOptions};
pub use darwin_art_engine_sys::{FrameCallback, ProcessConfig, ProcessResult};
use darwin_art_runtime::{RuntimeError, RuntimeSession, Subsystem};
use frame::{FrameHost, receive_frame};
pub use frame::{OwnedFrame, write_frame_ppm};
use provider::ProviderBridge;
#[cfg(target_os = "macos")]
use surface::*;
const MAX_VISIBLE_SECONDS: f64 = 86_400.0;

use darwin_art_engine_sys::{
    DispatchPointerFn, PointerEvent, PumpFrameworkFrameFn, SurfaceCreateInfo,
};

pub fn run(options: &RunOptions) -> Result<HostOutcome, HostError> {
    if !options.visible_seconds.is_finite()
        || options.visible_seconds < 0.0
        || options.visible_seconds > MAX_VISIBLE_SECONDS
    {
        return Err(HostError::InvalidVisibleSeconds(options.visible_seconds));
    }

    #[cfg(not(target_os = "macos"))]
    {
        let _ = options;
        Err(HostError::UnsupportedPlatform)
    }

    #[cfg(target_os = "macos")]
    {
        let mut runtime =
            RuntimeSession::<EngineSession, Box<ProviderBridge>, SurfaceSession>::new();
        runtime
            .start()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
        let mut engine = EngineSession::open(&options.library).map_err(HostError::DynamicLoader)?;
        let symbols = engine.symbols();
        let provider_bridge = Box::new(ProviderBridge::new(symbols));
        let provider_context = provider_bridge.context();
        // SAFETY: provider_bridge is kept alive by the local scope until it
        // is transferred into the RuntimeSession below; the callbacks are static
        // and use only that stable context pointer.
        unsafe {
            engine.install_provider_hooks(
                provider_context,
                Some(ProviderBridge::acquire_callback()),
                Some(ProviderBridge::release_callback()),
            );
        }
        let dispatch_pointer: DispatchPointerFn = symbols.dispatch_pointer;
        let pump_framework_frame: PumpFrameworkFrameFn = symbols.pump_framework_frame;

        let core_oj = path_c_string(&options.core_oj_jar)?;
        let core_libart = path_c_string(&options.core_libart_jar)?;
        let framework = path_c_string(&options.framework_jar)?;
        let core_icu4j = path_c_string(&options.core_icu4j_jar)?;
        let app_dex = path_c_string(&options.app_dex)?;
        let mut frame_host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        let config = ProcessConfig::new(
            core_oj.as_ptr(),
            core_libart.as_ptr(),
            framework.as_ptr(),
            core_icu4j.as_ptr(),
            app_dex.as_ptr(),
            options.heap_initial_bytes,
            options.heap_maximum_bytes,
            ptr::from_mut(&mut frame_host).cast(),
            Some(receive_frame),
            provider_context,
            Some(ProviderBridge::acquire_callback()),
            Some(ProviderBridge::release_callback()),
        );
        let process = match engine.run_process(&config) {
            Ok(result) => result,
            Err(status) => {
                runtime.fail(RuntimeError::EngineFailure { status });
                // A late run-stage failure may still have created ART. Ask the
                // process ABI to tear it down; NOT_READY means creation never
                // completed and is the only benign shutdown result here.
                let shutdown_status = engine.shutdown_once();
                const SHUTDOWN_NOT_READY: i32 = 67;
                if shutdown_status != 0 && shutdown_status != SHUTDOWN_NOT_READY {
                    return Err(HostError::ShutdownFailed(shutdown_status));
                }
                engine.clear_provider_hooks();
                return Err(HostError::RuntimeFailed(status));
            }
        };
        // The graphics engine publishes its drawable during run_process, so
        // capture the owner only after that bootstrap has completed.
        let active_surface = engine.active_surface();
        let gpu_mode = active_surface.is_some();
        runtime
            .attach_engine(engine)
            .map_err(|_| HostError::RuntimeFailed(-1))?;
        runtime
            .attach_provider(provider_bridge)
            .map_err(|_| HostError::RuntimeFailed(-1))?;
        let provider_lease = runtime
            .install_subsystem(Subsystem::ElfNamespace)
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
        let engine_lease = match runtime.install_subsystem(Subsystem::Engine) {
            Ok(lease) => lease,
            Err(error) => return Err(HostError::RuntimeFailed(error.status() as i32)),
        };
        if let Err(error) = runtime.mark_running() {
            runtime.fail(error);
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        // Darwin's application renderer is GPU-only.  The CPU surface path is
        // retained for diagnostics, never selected by the normal host.
        if gpu_mode {
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
                    let dispatch_status =
                        unsafe { dispatch_pointer(event.action, event.x, event.y) };
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
        // A non-graphics engine deliberately has no presentation surface.
        // Do not manufacture an IOSurface and copy the diagnostic callback
        // into it: that is a legacy CPU fallback, not part of the Android
        // GPU architecture, and it also makes a headless ART acceptance
        // depend on AppKit/IOSurface mapping details.  The callback mailbox
        // remains available for explicit diagnostic callers, while the
        // normal headless path owns only ART/provider teardown here.
        // The legacy IOSurface upload path remains available only for an
        // explicit diagnostic invocation.  It is never selected by the
        // normal host, whose non-graphics probes are headless and whose
        // graphics probes are direct Metal/Ganesh.
        if std::env::var_os("DARWIN_ART_ENABLE_CPU_SURFACE").is_some() {
            // Keep the old diagnostic presenter below reachable without
            // making it a production fallback.
        } else {
            let shutdown_status = if runtime.begin_shutdown().is_err() {
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
            if shutdown_status == 0 {
                runtime
                    .finish_shutdown()
                    .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
            } else {
                runtime.fail(RuntimeError::EngineFailure {
                    status: shutdown_status,
                });
                return Err(HostError::ShutdownFailed(shutdown_status));
            }
            return Ok(HostOutcome {
                process,
                frames_presented: 0,
                last_frame: frame_host.last_frame,
            });
        }
        // ART invokes the callback while its mutator lock is held. Present only
        // after returning from ART so AppKit's event loop never blocks a runnable
        // managed thread. The callback above merely copied into this Rust-owned
        // frame mailbox.
        let mut surface_lease = None;
        let presentation = (|| -> Result<u64, HostError> {
            let Some(initial_frame) = frame_host.last_frame.as_ref() else {
                return Ok(0);
            };
            let frame_width = initial_frame.width;
            let frame_height = initial_frame.height;
            let title = CString::new("Darwin ART · Activity.setContentView()")
                .expect("static window title contains no NUL");
            let create_info = SurfaceCreateInfo {
                width: frame_width,
                height: frame_height,
                title: title.as_ptr(),
                visible: options.visible_seconds > 0.0,
            };
            let surface = SurfaceSession::create(symbols, &create_info).map_err(|status| {
                HostError::SurfaceFailed {
                    operation: "create",
                    status,
                }
            })?;
            runtime
                .attach_surface(surface)
                .map_err(|_| HostError::RuntimeFailed(-1))?;
            surface_lease = Some(
                runtime
                    .install_subsystem(Subsystem::Surface)
                    .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?,
            );

            let present_result = (|| {
                let mut presentations = 0_u64;
                let upload_latest = |frame_host: &FrameHost| -> Result<(), HostError> {
                    let frame = frame_host
                        .last_frame
                        .as_ref()
                        .ok_or(HostError::SurfaceFailed {
                            operation: "missing_frame",
                            status: -1,
                        })?;
                    if frame.width != frame_width || frame.height != frame_height {
                        return Err(HostError::SurfaceFailed {
                            operation: "frame_size_changed",
                            status: -1,
                        });
                    }
                    let update_status = owned_surface_update(runtime.owners(), &frame.argb_pixels);
                    surface_status("update", update_status, false)?;
                    let present_status = owned_surface_present(runtime.owners());
                    surface_status("present", present_status, false)
                };
                // SAFETY: surface is live; the packed Rust frame remains live
                // through update, and all platform calls occur on main.
                upload_latest(&frame_host)?;
                presentations += 1;
                if let Ok(sample) = std::env::var("DARWIN_ART_TEST_POINTER_CLICK") {
                    let initial_pixels = frame_host
                        .last_frame
                        .as_ref()
                        .map(|frame| frame.argb_pixels.clone())
                        .ok_or(HostError::SurfaceFailed {
                            operation: "test_pointer_click",
                            status: -1,
                        })?;
                    let (x, y) = sample.split_once(',').ok_or(HostError::SurfaceFailed {
                        operation: "test_pointer_click",
                        status: -1,
                    })?;
                    let x = x.parse::<f32>().map_err(|_| HostError::SurfaceFailed {
                        operation: "test_pointer_click",
                        status: -1,
                    })?;
                    let y = y.parse::<f32>().map_err(|_| HostError::SurfaceFailed {
                        operation: "test_pointer_click",
                        status: -1,
                    })?;
                    let hold_ms = std::env::var("DARWIN_ART_TEST_POINTER_HOLD_MS")
                        .ok()
                        .and_then(|value| value.parse::<u64>().ok())
                        .unwrap_or(0);
                    let frame_dir =
                        std::env::var_os("DARWIN_ART_TEST_RIPPLE_DIR").map(PathBuf::from);
                    if let Some(directory) = &frame_dir {
                        fs::create_dir_all(directory).map_err(|_| HostError::SurfaceFailed {
                            operation: "create_ripple_dir",
                            status: -1,
                        })?;
                    }
                    let mut ripple_frame = 0_u32;
                    let save_ripple_frame =
                        |frame_host: &FrameHost, index: u32| -> Result<(), HostError> {
                            let Some(directory) = &frame_dir else {
                                return Ok(());
                            };
                            let frame =
                                frame_host
                                    .last_frame
                                    .as_ref()
                                    .ok_or(HostError::SurfaceFailed {
                                        operation: "missing_ripple_frame",
                                        status: -1,
                                    })?;
                            write_frame_ppm(frame, &directory.join(format!("frame-{index:03}.ppm")))
                        };
                    let dispatch_status = unsafe { dispatch_pointer(0, x, y) };
                    if dispatch_status != 0 {
                        return Err(HostError::RuntimeFailed(dispatch_status));
                    }
                    upload_latest(&frame_host)?;
                    save_ripple_frame(&frame_host, ripple_frame)?;
                    ripple_frame += 1;
                    if hold_ms > 0 {
                        let mut held = 0_u64;
                        while held < hold_ms {
                            let slice_ms = (hold_ms - held).min(16);
                            let pump_status = owned_surface_pump_events(
                                runtime.owners(),
                                slice_ms as f64 / 1000.0,
                            );
                            if pump_status == 7 {
                                break;
                            }
                            surface_status("pump_events", pump_status, false)?;
                            // Re-present a held pointer as a move so the
                            // retained Android tree is asked to draw each
                            // animation sample while the button stays down.
                            let dispatch_status = unsafe { dispatch_pointer(2, x, y) };
                            if dispatch_status != 0 {
                                return Err(HostError::RuntimeFailed(dispatch_status));
                            }
                            upload_latest(&frame_host)?;
                            save_ripple_frame(&frame_host, ripple_frame)?;
                            ripple_frame += 1;
                            held += slice_ms;
                        }
                    }
                    let dispatch_status = unsafe { dispatch_pointer(1, x, y) };
                    if dispatch_status != 0 {
                        return Err(HostError::RuntimeFailed(dispatch_status));
                    }
                    upload_latest(&frame_host)?;
                    save_ripple_frame(&frame_host, ripple_frame)?;
                    if frame_host.last_frame.as_ref().is_none_or(|frame| {
                        frame.argb_pixels.as_slice() == initial_pixels.as_slice()
                    }) {
                        return Err(HostError::SurfaceFailed {
                            operation: "test_pointer_click_unchanged",
                            status: -1,
                        });
                    }
                    presentations += 1;
                }
                let mut remaining = options.visible_seconds;
                while remaining > 0.0 {
                    let slice = remaining.min(0.016);
                    let pump_status = owned_surface_pump_events(runtime.owners(), slice);
                    if pump_status == 7 {
                        break;
                    }
                    surface_status("pump_events", pump_status, false)?;
                    let mut event = PointerEvent::default();
                    while owned_surface_next_pointer_event(runtime.owners(), &mut event) {
                        let dispatch_status =
                            unsafe { dispatch_pointer(event.action, event.x, event.y) };
                        if dispatch_status != 0 {
                            return Err(HostError::RuntimeFailed(dispatch_status));
                        }
                        upload_latest(&frame_host)?;
                        presentations += 1;
                    }
                    remaining -= slice;
                }
                Ok(presentations)
            })();
            let presentations = present_result?;
            Ok(presentations)
        })();
        // The surface lease is installed after creation and remains owned by
        // RuntimeLifecycle even when presentation returns an error. Teardown
        // therefore destroys the surface before releasing the engine lease.
        let mut surface_destroy_status = 0;
        let shutdown_status = match runtime.begin_shutdown() {
            Ok(()) => {
                let surface_status = surface_lease
                    .take()
                    .map(|lease| runtime.uninstall_subsystem(lease));
                match surface_status {
                    Some(Err(RuntimeError::EngineFailure { status })) => {
                        surface_destroy_status = status;
                        status
                    }
                    Some(Err(error)) => error.status() as i32,
                    Some(Ok(())) | None => match close_surface_owner(runtime.owners_mut()) {
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
                }
            }
            Err(error) => error.status() as i32,
        };
        if shutdown_status == 0 {
            if let Err(error) = runtime.finish_shutdown() {
                runtime.fail(error);
            }
        } else {
            runtime.fail(RuntimeError::EngineFailure {
                status: shutdown_status,
            });
        }
        if shutdown_status != 0 {
            if surface_destroy_status != 0 {
                return Err(HostError::SurfaceFailed {
                    operation: "destroy",
                    status: surface_destroy_status,
                });
            }
            return Err(HostError::ShutdownFailed(shutdown_status));
        }
        let frames_presented = presentation?;
        Ok(HostOutcome {
            process,
            frames_presented,
            last_frame: frame_host.last_frame,
        })
    }
}

fn surface_status(
    operation: &'static str,
    status: i32,
    allow_window_closed: bool,
) -> Result<(), HostError> {
    const SURFACE_OK: i32 = 0;
    const SURFACE_WINDOW_CLOSED: i32 = 7;
    if status == SURFACE_OK || (allow_window_closed && status == SURFACE_WINDOW_CLOSED) {
        Ok(())
    } else {
        Err(HostError::SurfaceFailed { operation, status })
    }
}

fn path_c_string(path: &Path) -> Result<CString, HostError> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| HostError::InteriorNul(path.into()))
}

#[cfg(test)]
mod tests;
