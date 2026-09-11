use std::ptr;

#[cfg(target_os = "macos")]
use crate::bootstrap::attach_runtime;
use crate::config::{HostError, HostOutcome, RunOptions, build_process_request};
use crate::frame::{FrameHost, receive_frame};
#[cfg(target_os = "macos")]
use crate::gpu_loop::run as run_gpu_loop;
#[cfg(target_os = "macos")]
use crate::host_services::ServiceProcessManager;
use crate::runtime::HostRuntime;
#[cfg(target_os = "macos")]
use crate::teardown::RuntimeShutdownGuard;
#[cfg(target_os = "macos")]
use darwin_art_engine::EngineSession;
use darwin_art_engine_sys::AppKitPumpEventsFn;
use darwin_art_runtime::{ProviderBridge, ProviderKind, Subsystem};
use std::sync::mpsc::{self, SyncSender, TryRecvError};
use std::thread;
use std::time::Instant;

#[cfg(target_os = "macos")]
use std::fs::{File, OpenOptions};
#[cfg(target_os = "macos")]
use std::os::fd::AsRawFd;
#[cfg(target_os = "macos")]
use std::os::unix::fs::OpenOptionsExt;
#[cfg(target_os = "macos")]
use std::path::PathBuf;

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn _exit(status: i32) -> !;
}

#[cfg(target_os = "macos")]
fn exit_android_process(status: i32) -> ! {
    // Android application processes are disposed as one OS lifetime. libc
    // `exit` would run host C++ static destructors while Chromium task runners
    // are still live, which is neither Android behavior nor race-free.
    // Flush stdio explicitly because _exit intentionally skips libc teardown.
    unsafe { libc::fflush(std::ptr::null_mut()) };
    unsafe { _exit(status) }
}

#[cfg(target_os = "macos")]
fn open_filesystem_authority() -> Result<Option<File>, HostError> {
    let root = std::env::var_os("DARWIN_ART_ANDROID_FILESYSTEM_ROOT")
        .filter(|value| !value.is_empty())
        .or_else(|| {
            std::env::var_os("DARWIN_ART_ANDROID_SYSTEM_ROOT").filter(|value| !value.is_empty())
        });
    let Some(root) = root else {
        return Ok(None);
    };
    let root = PathBuf::from(root);
    if !root.is_absolute() {
        return Err(HostError::HostService(format!(
            "Android filesystem authority must be absolute: {}",
            root.display()
        )));
    }
    OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_DIRECTORY | libc::O_CLOEXEC | libc::O_NOFOLLOW)
        .open(&root)
        .map(Some)
        .map_err(|error| {
            HostError::HostService(format!(
                "open Android filesystem authority {}: {error}",
                root.display()
            ))
        })
}

pub fn run(options: &RunOptions) -> Result<HostOutcome, HostError> {
    #[cfg(target_os = "macos")]
    {
        return run_with_appkit_actor(options.clone());
    }
    #[cfg(not(target_os = "macos"))]
    run_owner(options, None)
}

#[cfg(target_os = "macos")]
enum WorkerMessage {
    AppKitPump(AppKitPumpEventsFn),
    Finished(Result<HostOutcome, HostError>),
}

#[cfg(target_os = "macos")]
// Let AppKit sleep for one display interval while idle. nextEventMatchingMask:
// returns immediately when an NSEvent arrives, so this is event-driven rather
// than a 2 ms polling loop; the ART owner is still woken through the surface
// mailbox and Android input remains on its original sequence.
const APPKIT_PUMP_QUANTUM_SECONDS: f64 = 0.016;

#[cfg(target_os = "macos")]
fn run_with_appkit_actor(options: RunOptions) -> Result<HostOutcome, HostError> {
    let (sender, receiver) = mpsc::sync_channel::<WorkerMessage>(2);
    let worker = thread::Builder::new()
        .name("darwin-art-ui-owner".to_owned())
        .spawn(move || {
            // Android's main/UI Looper runs at an interactive scheduling
            // priority. Keep the ART owner from being deprioritized behind
            // Chromium helper work while AppKit remains on its own main
            // actor. This changes scheduling priority only; JNI/Looper
            // sequence affinity is unchanged.
            let qos_status = unsafe {
                libc::pthread_set_qos_class_self_np(
                    libc::qos_class_t::QOS_CLASS_USER_INTERACTIVE,
                    0,
                )
            };
            if qos_status != 0 && std::env::var_os("DARWIN_ART_DEBUG_FRAME_TIMING").is_some() {
                eprintln!("DARWIN_ART owner QoS setup failed status={qos_status}");
            }
            let result = run_owner(&options, Some(&sender));
            let _ = sender.send(WorkerMessage::Finished(result));
        })
        .map_err(|error| HostError::HostService(format!("spawn ART UI owner: {error}")))?;

    let mut appkit_pump: Option<AppKitPumpEventsFn> = None;
    let mut appkit_pump_count = 0_u64;
    let mut appkit_pump_total_us = 0_u64;
    let mut appkit_pump_max_us = 0_u64;
    let result = loop {
        match receiver.try_recv() {
            Ok(WorkerMessage::AppKitPump(callback)) => appkit_pump = Some(callback),
            Ok(WorkerMessage::Finished(result)) => break result,
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => {
                break Err(HostError::HostService(
                    "ART UI owner disconnected before completion".to_owned(),
                ));
            }
        }

        if let Some(callback) = appkit_pump {
            // Keep NSApplication responsive while the ART worker is blocked
            // in framework work or waiting for a marshalled Surface call.
            // The callback itself is main-thread-only and never invokes JNI.
            let pump_started = Instant::now();
            let status = unsafe { callback(APPKIT_PUMP_QUANTUM_SECONDS) };
            let elapsed_us = pump_started.elapsed().as_micros() as u64;
            appkit_pump_count += 1;
            appkit_pump_total_us = appkit_pump_total_us.saturating_add(elapsed_us);
            appkit_pump_max_us = appkit_pump_max_us.max(elapsed_us);
            // APK runs intentionally terminate the Android process with
            // _exit after the owner loop, so the outer actor never reaches
            // its final summary. Emit bounded snapshots while the actor is
            // alive to keep the split-path metric observable in those runs.
            if std::env::var_os("DARWIN_ART_DEBUG_FRAME_TIMING").is_some()
                && appkit_pump_count % 1024 == 0
            {
                eprintln!(
                    "DARWIN_ART appkit-pump count={} avg_us={} max_us={} quantum_us={}",
                    appkit_pump_count,
                    appkit_pump_total_us / appkit_pump_count,
                    appkit_pump_max_us,
                    (APPKIT_PUMP_QUANTUM_SECONDS * 1_000_000.0) as u64,
                );
            }
            if status != 0 {
                break Err(HostError::SurfaceFailed {
                    operation: "appkit_main_actor_pump",
                    status,
                });
            }
        } else {
            thread::sleep(std::time::Duration::from_millis(1));
        }
    };
    if std::env::var_os("DARWIN_ART_DEBUG_FRAME_TIMING").is_some() {
        let average_us = appkit_pump_total_us.checked_div(appkit_pump_count.max(1));
        eprintln!(
            "DARWIN_ART appkit-pump count={} avg_us={} max_us={} quantum_us={}",
            appkit_pump_count,
            average_us.unwrap_or(0),
            appkit_pump_max_us,
            (APPKIT_PUMP_QUANTUM_SECONDS * 1_000_000.0) as u64,
        );
    }
    let _ = worker.join();
    result
}

fn run_owner(
    options: &RunOptions,
    #[cfg(target_os = "macos")] appkit_sender: Option<&SyncSender<WorkerMessage>>,
    #[cfg(not(target_os = "macos"))] _appkit_sender: Option<&()>,
) -> Result<HostOutcome, HostError> {
    options.validate()?;

    #[cfg(not(target_os = "macos"))]
    {
        let _ = options;
        Err(HostError::UnsupportedPlatform)
    }

    #[cfg(target_os = "macos")]
    {
        if options.visible_seconds > 0.0 {
            crate::process_signal::install().map_err(|error| {
                HostError::HostService(format!("install SIGTERM handler: {error}"))
            })?;
        }
        let mut runtime = HostRuntime::new();
        runtime
            .start()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
        // Arm cleanup before opening the dynamic image. Every subsequent
        // early return, including loader/provider attach failures, now drops
        // through the same owner-thread shutdown path.
        let mut shutdown_guard =
            RuntimeShutdownGuard::new(&mut runtime, options.terminate_android_process);

        let bootstrap = attach_runtime(shutdown_guard.runtime(), &options.library)?;
        let graphics_attached = bootstrap.graphics_attached;
        if let (Some(sender), Some(engine)) = (appkit_sender, shutdown_guard.runtime().engine()) {
            sender
                .send(WorkerMessage::AppKitPump(engine.appkit_pump_callback()))
                .map_err(|_| HostError::HostService("AppKit actor disconnected".to_owned()))?;
        }

        if let Err(error) = shutdown_guard
            .runtime()
            .install_subsystem(Subsystem::Engine)
        {
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        // Provider callbacks point into the live engine image. Install the
        // engine lease before the provider/ELF lease so reverse teardown
        // clears provider hooks while the image is still mapped.
        if let Err(error) = shutdown_guard
            .runtime()
            .install_subsystem(Subsystem::ElfNamespace)
        {
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        if graphics_attached
            && let Err(error) = shutdown_guard
                .runtime()
                .install_subsystem(Subsystem::Graphics)
        {
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }

        // The native entrypoint receives a Rust-owned lifecycle bridge. It is
        // kept alive through the later shutdown call, so the C++ probe only
        // reports ART-specific runtime handles and never owns the production
        // phase machine.
        let lifecycle_hooks = shutdown_guard.runtime().native_lifecycle_hooks();
        let mut service_processes =
            ServiceProcessManager::new(options.clone()).map_err(HostError::HostService)?;
        let host_services = service_processes.native_services();

        let mut frame_host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        // Install the guest filesystem before Java starts. Libcore can enter
        // UnixNativeDispatcher while constructing the boot class path (for
        // example through Charset.availableCharsets()), well before an APK
        // loads its first native library. The File stays alive through the
        // synchronous Android process invocation; the native facade owns its
        // own duplicate until the Rust process lease is released.
        let filesystem_authority = open_filesystem_authority()?;
        let process = {
            let runtime = shutdown_guard.runtime();
            let Some(provider) = runtime.provider() else {
                let _ = shutdown_guard.shutdown();
                return Err(HostError::RuntimeFailed(-1));
            };
            if let Some(authority) = filesystem_authority.as_ref() {
                provider
                    .acquire_process_lease(ProviderKind::Filesystem, authority.as_raw_fd())
                    .map_err(HostError::RuntimeFailed)?;
            }
            // Socket and pipe descriptors can arrive in the first Binder
            // transaction that starts an Android service process, before its
            // native library is loaded.  Own the network provider for the
            // whole Android process so SCM_RIGHTS import is available at that
            // bootstrap boundary.  Native-library loads take additional
            // Rust-counted leases without reinstalling the process-global
            // broker.
            provider
                .acquire_process_lease(ProviderKind::Network, -1)
                .map_err(HostError::RuntimeFailed)?;
            let request = match build_process_request(
                options,
                ptr::from_mut(&mut frame_host).cast(),
                Some(receive_frame),
                provider,
                Some(ProviderBridge::acquire_callback()),
                Some(ProviderBridge::release_callback()),
                runtime.graphics(),
                Some(&lifecycle_hooks),
                Some(&host_services),
            ) {
                Ok(inputs) => inputs,
                Err(error) => {
                    let _ = service_processes.shutdown_all();
                    let _ = shutdown_guard.shutdown();
                    return Err(error);
                }
            };
            let Some(engine) = runtime.engine() else {
                let _ = service_processes.shutdown_all();
                let _ = shutdown_guard.shutdown();
                return Err(HostError::RuntimeFailed(-1));
            };
            match engine.run_request(&request) {
                Ok(result) => result,
                Err(error) => {
                    eprintln!(
                        "ART host run_request failed status={} before cleanup; service_processes_shutdown=1",
                        error
                    );
                    let _ = service_processes.shutdown_all();
                    let _ = shutdown_guard.shutdown();
                    return Err(HostError::RuntimeFailed(error));
                }
            }
        };

        // The graphics engine publishes its drawable during run_process.
        // Transfer that handle into RuntimeSession immediately, before any
        // later host branch can fail. This keeps the surface owned by the
        // same Rust shutdown transaction as ART/graphics instead of leaving
        // a short-lived foreign owner between process return and the frame
        // loop.
        let active_surface = shutdown_guard
            .runtime()
            .engine()
            .and_then(EngineSession::active_surface);
        let has_active_surface = if let Some(surface) = active_surface {
            shutdown_guard
                .runtime()
                .attach_surface(surface)
                .map_err(|_| HostError::RuntimeFailed(-1))?;
            shutdown_guard
                .runtime()
                .install_subsystem(Subsystem::Surface)
                .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
            true
        } else {
            false
        };
        if has_active_surface {
            let debug_boundaries = std::env::var_os("DARWIN_ART_DEBUG_FRAME_TIMING").is_some();
            if debug_boundaries {
                eprintln!("DARWIN_ART host: gpu-loop call enter");
            }
            let outcome = run_gpu_loop(
                shutdown_guard.runtime(),
                process,
                options,
                graphics_attached,
            );
            if debug_boundaries {
                eprintln!(
                    "DARWIN_ART host: gpu-loop call exit result={}",
                    outcome.is_ok()
                );
            }
            if options.terminate_android_process {
                // An Android app process ends at the OS lifetime boundary.
                // AOSP does not unload the live app NativeLoader graph or
                // destroy ART while Chromium workers may still execute it;
                // reap services and use _exit instead. Embeddable callers
                // take the explicit RuntimeShutdownGuard path below.
                if debug_boundaries {
                    eprintln!("DARWIN_ART host: service cleanup enter mode=terminate");
                }
                let service_cleanup = service_processes
                    .terminate_for_process_exit()
                    .map_err(HostError::HostService);
                if debug_boundaries {
                    eprintln!(
                        "DARWIN_ART host: service cleanup exit mode=terminate ok={}",
                        service_cleanup.is_ok()
                    );
                }
                let status = match (&outcome, &service_cleanup) {
                    (Ok(_), Ok(())) => 0,
                    (Err(error), _) | (_, Err(error)) => {
                        eprintln!("darwin-art-host: {error}");
                        1
                    }
                };
                exit_android_process(status);
            }
            // Keep Android Service processes and their Binder channels alive
            // while the browser runtime stops its native/Java threads. Killing
            // renderers first makes Chromium treat an orderly host timeout as
            // an unexpected child death and race its PartitionAlloc teardown.
            if debug_boundaries {
                eprintln!("DARWIN_ART host: runtime cleanup enter");
            }
            let runtime_cleanup = shutdown_guard.shutdown();
            if debug_boundaries {
                eprintln!(
                    "DARWIN_ART host: runtime cleanup exit ok={}",
                    runtime_cleanup.is_ok()
                );
                eprintln!("DARWIN_ART host: service cleanup enter mode=shutdown");
            }
            let service_cleanup = service_processes
                .shutdown_all()
                .map_err(HostError::HostService);
            if debug_boundaries {
                eprintln!(
                    "DARWIN_ART host: service cleanup exit mode=shutdown ok={}",
                    service_cleanup.is_ok()
                );
            }
            return match (outcome, service_cleanup, runtime_cleanup) {
                (Ok(outcome), Ok(()), Ok(())) => Ok(outcome),
                (Err(error), Ok(()), Ok(())) => Err(error),
                (_, Err(error), Ok(())) => Err(error),
                (_, _, Err(error)) => Err(error),
            };
        }

        // Headless ART is a first-class mode. It never allocates a surface
        // and never uploads the callback mailbox into an IOSurface.
        if graphics_attached
            && !shutdown_guard
                .runtime()
                .subsystem_active(Subsystem::Graphics)
            && let Err(error) = shutdown_guard
                .runtime()
                .install_subsystem(Subsystem::Graphics)
        {
            let cleanup = shutdown_guard.shutdown();
            return match cleanup {
                Ok(()) => Err(HostError::RuntimeFailed(error.status() as i32)),
                Err(cleanup_error) => Err(cleanup_error),
            };
        }
        let outcome = HostOutcome {
            process,
            frames_presented: 0,
            last_frame: frame_host.last_frame,
        };
        if options.terminate_android_process {
            // See the visible process path above: app-process exit is an OS
            // boundary, not an in-process ART/NativeLoader teardown.
            service_processes
                .terminate_for_process_exit()
                .map_err(HostError::HostService)?;
            exit_android_process(0);
        }
        shutdown_guard.shutdown()?;
        service_processes
            .shutdown_all()
            .map_err(HostError::HostService)?;
        Ok(outcome)
    }
}
