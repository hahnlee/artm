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
use darwin_art_runtime::{ProviderBridge, ProviderKind, Subsystem};

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn _exit(status: i32) -> !;
}

#[cfg(target_os = "macos")]
fn exit_android_process(status: i32) -> ! {
    // Android application processes are disposed as one OS lifetime. libc
    // `exit` would run host C++ static destructors while Chromium task runners
    // are still live, which is neither Android behavior nor race-free.
    unsafe { _exit(status) }
}

pub fn run(options: &RunOptions) -> Result<HostOutcome, HostError> {
    options.validate()?;

    #[cfg(not(target_os = "macos"))]
    {
        let _ = options;
        Err(HostError::UnsupportedPlatform)
    }

    #[cfg(target_os = "macos")]
    {
        let mut runtime = HostRuntime::new();
        runtime
            .start()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
        // Arm cleanup before opening the dynamic image. Every subsequent
        // early return, including loader/provider attach failures, now drops
        // through the same owner-thread shutdown path.
        let mut shutdown_guard = RuntimeShutdownGuard::new(&mut runtime);

        let bootstrap = attach_runtime(shutdown_guard.runtime(), &options.library)?;
        let graphics_attached = bootstrap.graphics_attached;

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
        let process = {
            let runtime = shutdown_guard.runtime();
            let Some(provider) = runtime.provider() else {
                let _ = shutdown_guard.shutdown();
                return Err(HostError::RuntimeFailed(-1));
            };
            // Socket and pipe descriptors can arrive in the first Binder
            // transaction that starts an Android service process, before its
            // native library is loaded.  Own the network provider for the
            // whole Android process so SCM_RIGHTS import is available at that
            // bootstrap boundary.  Native-library loads take additional
            // Rust-counted leases without reinstalling the process-global
            // broker.
            let network_lease = provider
                .acquire_lease(ProviderKind::Network, -1)
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
                    drop(network_lease);
                    let _ = service_processes.shutdown_all();
                    let _ = shutdown_guard.shutdown();
                    return Err(error);
                }
            };
            let Some(engine) = runtime.engine() else {
                drop(network_lease);
                let _ = service_processes.shutdown_all();
                let _ = shutdown_guard.shutdown();
                return Err(HostError::RuntimeFailed(-1));
            };
            let result = match engine.run_request(&request) {
                Ok(result) => result,
                Err(error) => {
                    drop(network_lease);
                    let _ = service_processes.shutdown_all();
                    let _ = shutdown_guard.shutdown();
                    return Err(HostError::RuntimeFailed(error));
                }
            };
            drop(network_lease);
            result
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
            let outcome = run_gpu_loop(
                shutdown_guard.runtime(),
                process,
                options,
                graphics_attached,
            );
            if options.terminate_android_process {
                let service_cleanup = service_processes
                    .terminate_for_process_exit()
                    .map_err(HostError::HostService);
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
            let runtime_cleanup = shutdown_guard.shutdown();
            let service_cleanup = service_processes
                .shutdown_all()
                .map_err(HostError::HostService);
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
