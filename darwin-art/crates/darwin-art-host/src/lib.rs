use std::error::Error;
use std::ffi::{CString, c_void};
use std::fmt;
use std::fs;
use std::mem::size_of;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr;

mod provider;

#[cfg(target_os = "macos")]
use darwin_art_engine::{EngineSession, SurfaceSession};

pub use darwin_art_engine_sys::{FrameCallback, ProcessConfig, ProcessResult};
use darwin_art_runtime::{RuntimeError, RuntimeOwners, RuntimeSession, Subsystem};
use provider::ProviderBridge;
const MAX_FRAME_DIMENSION: u32 = 4096;
const MAX_VISIBLE_SECONDS: f64 = 86_400.0;

use darwin_art_engine_sys::{
    DispatchPointerFn, PointerEvent, PumpFrameworkFrameFn, SurfaceCreateInfo,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OwnedFrame {
    pub width: u32,
    pub height: u32,
    pub argb_pixels: Vec<u32>,
}

#[derive(Debug)]
pub struct HostOutcome {
    pub process: ProcessResult,
    pub frames_presented: u64,
    pub last_frame: Option<OwnedFrame>,
}

#[derive(Clone, Debug)]
pub struct RunOptions {
    pub library: PathBuf,
    pub core_oj_jar: PathBuf,
    pub core_libart_jar: PathBuf,
    pub framework_jar: PathBuf,
    pub core_icu4j_jar: PathBuf,
    pub app_dex: PathBuf,
    pub heap_initial_bytes: u64,
    pub heap_maximum_bytes: u64,
    pub visible_seconds: f64,
}

#[derive(Debug)]
pub enum HostError {
    UnsupportedPlatform,
    InteriorNul(PathBuf),
    DynamicLoader(String),
    InvalidVisibleSeconds(f64),
    RuntimeFailed(i32),
    ShutdownFailed(i32),
    SurfaceFailed {
        operation: &'static str,
        status: i32,
    },
}

impl fmt::Display for HostError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedPlatform => write!(formatter, "darwin-art-host requires macOS"),
            Self::InteriorNul(path) => {
                write!(
                    formatter,
                    "path contains an interior NUL: {}",
                    path.display()
                )
            }
            Self::DynamicLoader(message) => write!(formatter, "dynamic loader: {message}"),
            Self::InvalidVisibleSeconds(seconds) => {
                write!(
                    formatter,
                    "visible seconds must be finite and in the range 0..=86400: {seconds}"
                )
            }
            Self::RuntimeFailed(status) => {
                write!(
                    formatter,
                    "darwin_art_run_process failed with status {status}"
                )
            }
            Self::ShutdownFailed(status) => {
                write!(
                    formatter,
                    "darwin_art_shutdown_process failed with status {status}"
                )
            }
            Self::SurfaceFailed { operation, status } => {
                write!(formatter, "surface {operation} failed with status {status}")
            }
        }
    }
}

impl Error for HostError {}

struct FrameHost {
    frames_received: u64,
    last_frame: Option<OwnedFrame>,
}

impl FrameHost {
    unsafe fn receive(
        &mut self,
        pixels: *const u32,
        width: u32,
        height: u32,
        stride_bytes: usize,
    ) -> bool {
        let Some(row_bytes) = (width as usize).checked_mul(size_of::<u32>()) else {
            return false;
        };
        let Some(pixel_count) = (width as usize).checked_mul(height as usize) else {
            return false;
        };
        if pixels.is_null()
            || width == 0
            || height == 0
            || width > MAX_FRAME_DIMENSION
            || height > MAX_FRAME_DIMENSION
            || stride_bytes < row_bytes
        {
            return false;
        }
        let Some(last_row_offset) = (height as usize - 1).checked_mul(stride_bytes) else {
            return false;
        };
        if last_row_offset.checked_add(row_bytes).is_none() {
            return false;
        }

        // The C ABI borrows the source only for this callback. Keep an owned,
        // tightly packed copy and return immediately; presentation happens
        // after ART has released the managed execution boundary.
        let mut owned = vec![0_u32; pixel_count];
        let source = pixels.cast::<u8>();
        let destination = owned.as_mut_ptr().cast::<u8>();
        for row in 0..height as usize {
            // SAFETY: The producer guarantees stride_bytes of readable frame
            // storage per row for the callback duration. Bounds and arithmetic
            // were checked above, and the destination is a packed Vec row.
            unsafe {
                ptr::copy_nonoverlapping(
                    source.add(row * stride_bytes),
                    destination.add(row * row_bytes),
                    row_bytes,
                );
            }
        }

        self.last_frame = Some(OwnedFrame {
            width,
            height,
            argb_pixels: owned,
        });
        self.frames_received += 1;
        true
    }
}

fn write_frame_ppm(frame: &OwnedFrame, path: &Path) -> Result<(), HostError> {
    let mut bytes = Vec::with_capacity(
        32usize.saturating_add((frame.width as usize).saturating_mul(frame.height as usize) * 3),
    );
    bytes.extend_from_slice(format!("P6\n{} {}\n255\n", frame.width, frame.height).as_bytes());
    for pixel in &frame.argb_pixels {
        bytes.extend_from_slice(&[
            ((pixel >> 16) & 0xff) as u8,
            ((pixel >> 8) & 0xff) as u8,
            (pixel & 0xff) as u8,
        ]);
    }
    fs::write(path, bytes).map_err(|_| HostError::SurfaceFailed {
        operation: "write_frame",
        status: -1,
    })
}

unsafe extern "C" fn receive_frame(
    context: *mut c_void,
    pixels: *const u32,
    width: u32,
    height: u32,
    stride_bytes: usize,
) -> i32 {
    if context.is_null() {
        return 0;
    }
    // No Rust panic may cross this C callback. Validation and presentation are
    // deliberately written without panicking operations for valid ABI input.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // SAFETY: run() passes a live, uniquely borrowed FrameHost for the
        // entire synchronous darwin_art_run_process call.
        unsafe { (&mut *context.cast::<FrameHost>()).receive(pixels, width, height, stride_bytes) }
    }));
    i32::from(result.unwrap_or(false))
}

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

#[cfg(target_os = "macos")]
fn owned_surface(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
) -> Option<&SurfaceSession> {
    owners.surface()
}

#[cfg(target_os = "macos")]
fn owned_surface_pump_events(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    seconds: f64,
) -> i32 {
    owned_surface(owners).map_or(-1, |surface| surface.pump_events(seconds))
}

#[cfg(target_os = "macos")]
fn owned_surface_next_pointer_event(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    event: &mut PointerEvent,
) -> bool {
    owned_surface(owners).is_some_and(|surface| surface.next_pointer_event(event))
}

#[cfg(target_os = "macos")]
fn owned_surface_update(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
    pixels: &[u32],
) -> i32 {
    owned_surface(owners).map_or(-1, |surface| surface.update_words(pixels))
}

#[cfg(target_os = "macos")]
fn owned_surface_present(
    owners: &RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
) -> i32 {
    owned_surface(owners).map_or(-1, SurfaceSession::present)
}

#[cfg(target_os = "macos")]
fn close_surface_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
) -> Result<(), i32> {
    let Some(mut surface) = owners.take_surface() else {
        return Ok(());
    };
    let status = surface.close();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
fn shutdown_engine_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
) -> Result<(), i32> {
    let Some(mut engine) = owners.take_engine() else {
        return Ok(());
    };
    let status = engine.shutdown_once();
    if status == 0 { Ok(()) } else { Err(status) }
}

#[cfg(target_os = "macos")]
fn clear_provider_owner(
    owners: &mut RuntimeOwners<EngineSession, Box<ProviderBridge>, SurfaceSession>,
) -> i32 {
    let Some(provider) = owners.take_provider() else {
        return 0;
    };
    match provider.clear() {
        Ok(()) => 0,
        Err(()) => -1,
    }
}

fn path_c_string(path: &Path) -> Result<CString, HostError> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| HostError::InteriorNul(path.into()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn process_config_matches_c_abi_v1() {
        assert_eq!(size_of::<ProcessConfig>(), 104);
        assert_eq!(align_of::<ProcessConfig>(), 8);
        assert_eq!(offset_of!(ProcessConfig, struct_size), 0);
        assert_eq!(offset_of!(ProcessConfig, abi_version), 4);
        assert_eq!(offset_of!(ProcessConfig, core_oj_jar), 8);
        assert_eq!(offset_of!(ProcessConfig, core_libart_jar), 16);
        assert_eq!(offset_of!(ProcessConfig, framework_jar), 24);
        assert_eq!(offset_of!(ProcessConfig, core_icu4j_jar), 32);
        assert_eq!(offset_of!(ProcessConfig, app_dex), 40);
        assert_eq!(offset_of!(ProcessConfig, heap_initial_bytes), 48);
        assert_eq!(offset_of!(ProcessConfig, heap_maximum_bytes), 56);
        assert_eq!(offset_of!(ProcessConfig, host_context), 64);
        assert_eq!(offset_of!(ProcessConfig, frame_callback), 72);
        assert_eq!(offset_of!(ProcessConfig, provider_context), 80);
        assert_eq!(offset_of!(ProcessConfig, provider_acquire), 88);
        assert_eq!(offset_of!(ProcessConfig, provider_release), 96);
    }

    #[test]
    fn process_result_matches_c_abi_v1() {
        assert_eq!(size_of::<ProcessResult>(), 36);
        assert_eq!(align_of::<ProcessResult>(), 4);
        assert_eq!(offset_of!(ProcessResult, struct_size), 0);
        assert_eq!(offset_of!(ProcessResult, abi_version), 4);
        assert_eq!(offset_of!(ProcessResult, hello_answer), 8);
        assert_eq!(offset_of!(ProcessResult, native_round_trip), 12);
        assert_eq!(offset_of!(ProcessResult, arraycopy_result), 16);
        assert_eq!(offset_of!(ProcessResult, activity_probe_result), 20);
        assert_eq!(offset_of!(ProcessResult, lifecycle_result), 24);
        assert_eq!(offset_of!(ProcessResult, frame_width), 28);
        assert_eq!(offset_of!(ProcessResult, frame_height), 32);
    }

    #[test]
    fn surface_create_info_matches_c_abi() {
        assert_eq!(size_of::<SurfaceCreateInfo>(), 24);
        assert_eq!(align_of::<SurfaceCreateInfo>(), 8);
        assert_eq!(offset_of!(SurfaceCreateInfo, width), 0);
        assert_eq!(offset_of!(SurfaceCreateInfo, height), 4);
        assert_eq!(offset_of!(SurfaceCreateInfo, title), 8);
        assert_eq!(offset_of!(SurfaceCreateInfo, visible), 16);
    }

    #[test]
    fn callback_copies_strided_frame_into_owned_packed_vec() {
        let source = [1_u32, 2, 99, 3, 4, 99];
        let mut host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        // SAFETY: source contains two rows of three u32 values and the callback
        // copies only the first two pixels from each row.
        assert!(unsafe { host.receive(source.as_ptr(), 2, 2, 3 * size_of::<u32>()) });
        assert_eq!(host.frames_received, 1);
        assert_eq!(
            host.last_frame,
            Some(OwnedFrame {
                width: 2,
                height: 2,
                argb_pixels: vec![1, 2, 3, 4],
            })
        );
    }

    #[test]
    fn callback_rejects_invalid_stride() {
        let source = [1_u32, 2, 3, 4];
        let mut host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        assert!(!unsafe { host.receive(source.as_ptr(), 2, 2, size_of::<u32>()) });
        assert!(host.last_frame.is_none());
    }

    #[test]
    fn callback_function_pointer_has_c_pointer_layout() {
        assert_eq!(size_of::<Option<FrameCallback>>(), size_of::<*mut c_void>());
        assert_eq!(
            align_of::<Option<FrameCallback>>(),
            align_of::<*mut c_void>()
        );
    }

    #[test]
    fn visible_duration_matches_surface_contract() {
        let mut options = RunOptions {
            library: PathBuf::new(),
            core_oj_jar: PathBuf::new(),
            core_libart_jar: PathBuf::new(),
            framework_jar: PathBuf::new(),
            core_icu4j_jar: PathBuf::new(),
            app_dex: PathBuf::new(),
            heap_initial_bytes: 0,
            heap_maximum_bytes: 0,
            visible_seconds: 86_400.001,
        };
        assert!(matches!(
            run(&options),
            Err(HostError::InvalidVisibleSeconds(_))
        ));
        options.visible_seconds = -0.001;
        assert!(matches!(
            run(&options),
            Err(HostError::InvalidVisibleSeconds(_))
        ));
    }
}
