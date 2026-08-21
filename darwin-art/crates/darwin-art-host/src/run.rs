use std::ptr;

#[cfg(target_os = "macos")]
use crate::bootstrap::attach_runtime;
use crate::config::{HostError, HostOutcome, RunOptions, build_process_request};
use crate::frame::{FrameHost, receive_frame};
#[cfg(target_os = "macos")]
use crate::gpu_loop::run as run_gpu_loop;
use crate::runtime::HostRuntime;
#[cfg(target_os = "macos")]
use crate::teardown::RuntimeShutdownGuard;
#[cfg(target_os = "macos")]
use darwin_art_engine::EngineSession;
use darwin_art_runtime::{ProviderBridge, Subsystem};

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
        let provider_context = bootstrap.provider_context;
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

        let mut frame_host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        let request = match build_process_request(
            options,
            ptr::from_mut(&mut frame_host).cast(),
            Some(receive_frame),
            provider_context,
            Some(ProviderBridge::acquire_callback()),
            Some(ProviderBridge::release_callback()),
            shutdown_guard.runtime().graphics(),
        ) {
            Ok(inputs) => inputs,
            Err(error) => {
                let _ = shutdown_guard.shutdown();
                return Err(error);
            }
        };

        let process = match shutdown_guard
            .runtime()
            .engine()
            .ok_or(HostError::RuntimeFailed(-1))
            .and_then(|engine| {
                engine
                    .run_request(&request)
                    .map_err(HostError::RuntimeFailed)
            }) {
            Ok(result) => result,
            Err(error) => {
                let _ = shutdown_guard.shutdown();
                return Err(error);
            }
        };

        // The graphics engine publishes its drawable during run_process. The
        // surface value is still returned as a short-lived transfer and is
        // attached by gpu_loop, while engine/provider/graphics remain owned by
        // RuntimeSession for the entire process.
        let active_surface = shutdown_guard
            .runtime()
            .engine()
            .and_then(EngineSession::active_surface);
        shutdown_guard
            .runtime()
            .mark_running()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;

        if active_surface.is_some() {
            let outcome = run_gpu_loop(
                shutdown_guard.runtime(),
                active_surface,
                process,
                options,
                graphics_attached,
            );
            return match (outcome, shutdown_guard.shutdown()) {
                (Ok(outcome), Ok(())) => Ok(outcome),
                (Err(error), Ok(())) => Err(error),
                (_, Err(error)) => Err(error),
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
        shutdown_guard.shutdown()?;
        Ok(outcome)
    }
}
