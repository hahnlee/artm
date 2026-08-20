use std::error::Error;
use std::ffi::{CStr, CString, c_char, c_void};
use std::fmt;
use std::fs;
use std::mem::size_of;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr;

pub const ABI_VERSION: u32 = 1;
const MAX_FRAME_DIMENSION: u32 = 4096;
const MAX_VISIBLE_SECONDS: f64 = 86_400.0;

pub type FrameCallback = unsafe extern "C" fn(
    context: *mut c_void,
    argb_pixels: *const u32,
    width: u32,
    height: u32,
    stride_bytes: usize,
) -> i32;

#[repr(C)]
pub struct ProcessConfig {
    pub struct_size: u32,
    pub abi_version: u32,
    pub core_oj_jar: *const c_char,
    pub core_libart_jar: *const c_char,
    pub framework_jar: *const c_char,
    pub core_icu4j_jar: *const c_char,
    pub app_dex: *const c_char,
    pub heap_initial_bytes: u64,
    pub heap_maximum_bytes: u64,
    pub host_context: *mut c_void,
    pub frame_callback: Option<FrameCallback>,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub struct ProcessResult {
    pub struct_size: u32,
    pub abi_version: u32,
    pub hello_answer: i32,
    pub native_round_trip: i32,
    pub arraycopy_result: i32,
    pub activity_probe_result: i32,
    pub lifecycle_result: i32,
    pub frame_width: u32,
    pub frame_height: u32,
}

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

type RunProcessFn = unsafe extern "C" fn(*const ProcessConfig, *mut ProcessResult) -> i32;
type ShutdownProcessFn = unsafe extern "C" fn() -> i32;

#[derive(Clone, Copy)]
#[repr(C)]
struct SurfaceCreateInfo {
    width: u32,
    height: u32,
    title: *const c_char,
    visible: bool,
}

type SurfaceCreateFn = unsafe extern "C" fn(*const SurfaceCreateInfo, *mut i32) -> *mut c_void;
type SurfaceUpdateFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> i32;
type SurfacePresentFn = unsafe extern "C" fn(*mut c_void) -> i32;
type SurfacePumpEventsFn = unsafe extern "C" fn(*mut c_void, f64) -> i32;
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
struct PointerEvent {
    action: u32,
    x: f32,
    y: f32,
}
type SurfaceNextPointerEventFn = unsafe extern "C" fn(*mut c_void, *mut PointerEvent) -> bool;
type SurfaceDestroyFn = unsafe extern "C" fn(*mut c_void) -> i32;
type SurfaceActiveFn = unsafe extern "C" fn() -> *mut c_void;
type DispatchPointerFn = unsafe extern "C" fn(u32, f32, f32) -> i32;
type PumpFrameworkFrameFn = unsafe extern "C" fn(i64) -> i32;

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
        let library = DynamicLibrary::open(&options.library)?;
        // SAFETY: Symbol names and signatures are fixed by darwin_art.h ABI v1.
        let run_process: RunProcessFn = unsafe { library.symbol(b"darwin_art_run_process\0")? };
        let shutdown_process: ShutdownProcessFn =
            unsafe { library.symbol(b"darwin_art_shutdown_process\0")? };
        // SAFETY: These signatures are fixed by darwin_surface_bridge.h.
        let surface_create: SurfaceCreateFn =
            unsafe { library.symbol(b"darwin_art_surface_create\0")? };
        let surface_update: SurfaceUpdateFn =
            unsafe { library.symbol(b"darwin_art_surface_update\0")? };
        let surface_present: SurfacePresentFn =
            unsafe { library.symbol(b"darwin_art_surface_present\0")? };
        let surface_pump_events: SurfacePumpEventsFn =
            unsafe { library.symbol(b"darwin_art_surface_pump_events\0")? };
        let surface_next_pointer_event: SurfaceNextPointerEventFn =
            unsafe { library.symbol(b"darwin_art_surface_next_pointer_event\0")? };
        let surface_destroy: SurfaceDestroyFn =
            unsafe { library.symbol(b"darwin_art_surface_destroy\0")? };
        let surface_active: SurfaceActiveFn =
            unsafe { library.symbol(b"darwin_art_surface_active_gpu\0")? };
        let dispatch_pointer: DispatchPointerFn =
            unsafe { library.symbol(b"darwin_art_dispatch_pointer\0")? };
        let pump_framework_frame: PumpFrameworkFrameFn =
            unsafe { library.symbol(b"darwin_art_pump_framework_frame\0")? };

        let core_oj = path_c_string(&options.core_oj_jar)?;
        let core_libart = path_c_string(&options.core_libart_jar)?;
        let framework = path_c_string(&options.framework_jar)?;
        let core_icu4j = path_c_string(&options.core_icu4j_jar)?;
        let app_dex = path_c_string(&options.app_dex)?;
        let mut frame_host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        let config = ProcessConfig {
            struct_size: size_of::<ProcessConfig>() as u32,
            abi_version: ABI_VERSION,
            core_oj_jar: core_oj.as_ptr(),
            core_libart_jar: core_libart.as_ptr(),
            framework_jar: framework.as_ptr(),
            core_icu4j_jar: core_icu4j.as_ptr(),
            app_dex: app_dex.as_ptr(),
            heap_initial_bytes: options.heap_initial_bytes,
            heap_maximum_bytes: options.heap_maximum_bytes,
            host_context: ptr::from_mut(&mut frame_host).cast(),
            frame_callback: Some(receive_frame),
        };
        let mut process = ProcessResult {
            struct_size: size_of::<ProcessResult>() as u32,
            abi_version: ABI_VERSION,
            ..ProcessResult::default()
        };
        // SAFETY: config and result match ABI v1 and all pointed-to state stays
        // live for this synchronous call.
        let status = unsafe { run_process(&config, &mut process) };
        if status != 0 {
            // A late run-stage failure may still have created ART. Ask the
            // process ABI to tear it down; NOT_READY means creation never
            // completed and is the only benign shutdown result here.
            let shutdown_status = unsafe { shutdown_process() };
            const SHUTDOWN_NOT_READY: i32 = 67;
            if shutdown_status != 0 && shutdown_status != SHUTDOWN_NOT_READY {
                return Err(HostError::ShutdownFailed(shutdown_status));
            }
            return Err(HostError::RuntimeFailed(status));
        }
        // Darwin's application renderer is GPU-only.  The CPU surface path is
        // retained for diagnostics, never selected by the normal host.
        let gpu_mode = true;
        if gpu_mode {
            let surface = unsafe { surface_active() };
            if surface.is_null() {
                return Err(HostError::SurfaceFailed {
                    operation: "gpu_active_surface",
                    status: -1,
                });
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
            let test_hold_ms = std::env::var("DARWIN_ART_TEST_POINTER_HOLD_MS")
                .ok()
                .and_then(|value| value.parse::<u64>().ok())
                .unwrap_or(0);
            eprintln!("DARWIN_ART gpu test pointer={test_pointer:?} hold_ms={test_hold_ms}");

            let dispatch_queued_events = || -> Result<u64, HostError> {
                let mut dispatched = 0_u64;
                let mut event = PointerEvent::default();
                while unsafe { surface_next_pointer_event(surface, &mut event) } {
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
                            unsafe { surface_pump_events(surface, slice_ms as f64 / 1000.0) };
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
                let pump_status = unsafe { surface_pump_events(surface, slice) };
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
                if loop_error.is_none() {
                    if let Some((x, y)) = test_pointer {
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
                }
                if loop_error.is_some() {
                    break;
                }
                remaining -= slice;
            }
            let destroy_status = unsafe { surface_destroy(surface) };
            let shutdown_status = unsafe { shutdown_process() };
            if let Some(error) = loop_error {
                return Err(error);
            }
            if destroy_status != 0 {
                return Err(HostError::SurfaceFailed {
                    operation: "gpu_destroy",
                    status: destroy_status,
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
        // ART invokes the callback while its mutator lock is held. Present only
        // after returning from ART so AppKit's event loop never blocks a runnable
        // managed thread. The callback above merely copied into this Rust-owned
        // frame mailbox.
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
            let mut create_status = 0;
            // SAFETY: create_info and status remain live for this synchronous
            // main-thread call and match darwin_surface_bridge.h.
            let surface = unsafe { surface_create(&create_info, &mut create_status) };
            if surface.is_null() {
                return Err(HostError::SurfaceFailed {
                    operation: "create",
                    status: create_status,
                });
            }

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
                    // SAFETY: surface and the packed frame are live for this
                    // synchronous main-thread upload.
                    let update_status = unsafe {
                        surface_update(
                            surface,
                            frame.argb_pixels.as_ptr().cast(),
                            frame.width as usize * size_of::<u32>(),
                        )
                    };
                    surface_status("update", update_status, false)?;
                    let present_status = unsafe { surface_present(surface) };
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
                            let pump_status =
                                unsafe { surface_pump_events(surface, slice_ms as f64 / 1000.0) };
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
                    let pump_status = unsafe { surface_pump_events(surface, slice) };
                    if pump_status == 7 {
                        break;
                    }
                    surface_status("pump_events", pump_status, false)?;
                    let mut event = PointerEvent::default();
                    while unsafe { surface_next_pointer_event(surface, &mut event) } {
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
            // SAFETY: surface was returned by create and has not been destroyed.
            let destroy_status = unsafe { surface_destroy(surface) };
            let presentations = present_result?;
            surface_status("destroy", destroy_status, false)?;
            Ok(presentations)
        })();
        // Presentation has released every platform surface at this point.
        // Destroy ART before returning ownership to arbitrary Rust code so its
        // registered DexFile pointers and process-global handlers remain valid
        // throughout Runtime teardown.
        let shutdown_status = unsafe { shutdown_process() };
        if shutdown_status != 0 {
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

#[cfg(target_os = "macos")]
struct DynamicLibrary(*mut c_void);

#[cfg(target_os = "macos")]
impl DynamicLibrary {
    fn open(path: &Path) -> Result<Self, HostError> {
        let path = path_c_string(path)?;
        // SAFETY: path is NUL terminated and flags are valid Darwin dlopen flags.
        let handle = unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        if handle.is_null() {
            Err(HostError::DynamicLoader(dynamic_loader_error()))
        } else {
            Ok(Self(handle))
        }
    }

    unsafe fn symbol<T: Copy>(&self, name: &'static [u8]) -> Result<T, HostError> {
        debug_assert_eq!(name.last(), Some(&0));
        // Clear a prior loader error before dlsym and inspect dlerror after it;
        // a null symbol address is not by itself the portable error signal.
        unsafe { dlerror() };
        let symbol = unsafe { dlsym(self.0, name.as_ptr().cast()) };
        let error = unsafe { dlerror() };
        if !error.is_null() {
            return Err(HostError::DynamicLoader(unsafe {
                CStr::from_ptr(error).to_string_lossy().into_owned()
            }));
        }
        if size_of::<T>() != size_of::<*mut c_void>() {
            return Err(HostError::DynamicLoader(
                "function pointer has an unexpected size".to_owned(),
            ));
        }
        // SAFETY: The caller supplies the ABI signature associated with name.
        Ok(unsafe { ptr::read((&symbol as *const *mut c_void).cast::<T>()) })
    }
}

// ABI v1 intentionally keeps this handle loaded for the process lifetime even
// after DestroyJavaVM. ART installed process-global handlers while the image
// was loaded, and this gate has not yet proved that every handler is restored
// before dlclose. Leaking one process-scoped handle is safer than leaving a
// possible callback into unmapped code.

#[cfg(target_os = "macos")]
fn dynamic_loader_error() -> String {
    // SAFETY: dlerror returns either null or a process-owned NUL-terminated string.
    let error = unsafe { dlerror() };
    if error.is_null() {
        "unknown error".to_owned()
    } else {
        // SAFETY: Non-null dlerror result points to a NUL-terminated string.
        unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() }
    }
}

#[cfg(target_os = "macos")]
const RTLD_LOCAL: i32 = 0x4;
#[cfg(target_os = "macos")]
const RTLD_NOW: i32 = 0x2;

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn dlopen(path: *const c_char, mode: i32) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlerror() -> *const c_char;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn process_config_matches_c_abi_v1() {
        assert_eq!(size_of::<ProcessConfig>(), 80);
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
