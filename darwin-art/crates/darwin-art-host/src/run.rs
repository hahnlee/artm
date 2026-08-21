use std::ptr;

use crate::config::{HostError, HostOutcome, RunOptions, build_process_request};
use crate::frame::{FrameHost, receive_frame};
#[cfg(target_os = "macos")]
use crate::gpu_loop::run as run_gpu_loop;
use crate::provider::ProviderBridge;
#[cfg(target_os = "macos")]
use crate::teardown::{RuntimeShutdownGuard, shutdown_runtime};
#[cfg(target_os = "macos")]
use darwin_art_engine::{EngineSession, GraphicsSession, SurfaceSession};
use darwin_art_runtime::{RuntimeSession, Subsystem};

pub fn run(options: &RunOptions) -> Result<HostOutcome, HostError> {
    options.validate()?;

    #[cfg(not(target_os = "macos"))]
    {
        let _ = options;
        Err(HostError::UnsupportedPlatform)
    }

    #[cfg(target_os = "macos")]
    {
        type HostRuntime =
            RuntimeSession<EngineSession, Box<ProviderBridge>, SurfaceSession, GraphicsSession>;

        let mut runtime = HostRuntime::new();
        runtime
            .start()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;

        // Transfer every long-lived native resource into RuntimeSession before
        // invoking ART. The bootstrap call below then borrows the Rust owner
        // instead of keeping a second host-side engine/provider state machine.
        let engine = EngineSession::open(&options.library).map_err(HostError::DynamicLoader)?;
        let provider_bridge = Box::new(ProviderBridge::new(engine.provider_hooks()));
        let provider_context = provider_bridge.context();
        // SAFETY: provider_bridge is transferred into RuntimeSession immediately
        // below and remains alive until the engine hooks are cleared in teardown.
        unsafe {
            engine.install_provider_hooks(
                provider_context,
                Some(ProviderBridge::acquire_callback()),
                Some(ProviderBridge::release_callback()),
            );
        }
        if let Err(_engine) = runtime.attach_engine(engine) {
            return Err(HostError::RuntimeFailed(-1));
        }
        if let Err(provider_bridge) = runtime.attach_provider(provider_bridge) {
            let _ = provider_bridge.clear();
            let _ = shutdown_runtime(&mut runtime);
            return Err(HostError::RuntimeFailed(-1));
        }

        let graphics_session = runtime
            .engine()
            .and_then(|engine| engine.create_graphics_session().ok());
        let graphics_attached = if let Some(graphics) = graphics_session {
            if runtime.attach_graphics(graphics).is_err() {
                let _ = shutdown_runtime(&mut runtime);
                return Err(HostError::RuntimeFailed(-1));
            }
            true
        } else {
            false
        };

        if let Err(error) = runtime.install_subsystem(Subsystem::ElfNamespace) {
            let _ = shutdown_runtime(&mut runtime);
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        if let Err(error) = runtime.install_subsystem(Subsystem::Engine) {
            let _ = shutdown_runtime(&mut runtime);
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        if graphics_attached && let Err(error) = runtime.install_subsystem(Subsystem::Graphics) {
            let _ = shutdown_runtime(&mut runtime);
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
            runtime.graphics(),
        ) {
            Ok(inputs) => inputs,
            Err(error) => {
                return match shutdown_runtime(&mut runtime) {
                    Ok(()) => Err(error),
                    Err(cleanup) => Err(cleanup),
                };
            }
        };

        let process = match runtime
            .engine()
            .ok_or(HostError::RuntimeFailed(-1))
            .and_then(|engine| {
                engine
                    .run_request(&request)
                    .map_err(HostError::RuntimeFailed)
            }) {
            Ok(result) => result,
            Err(error) => {
                return match shutdown_runtime(&mut runtime) {
                    Ok(()) => Err(error),
                    Err(cleanup) => Err(cleanup),
                };
            }
        };

        // The graphics engine publishes its drawable during run_process. The
        // surface value is still returned as a short-lived transfer and is
        // attached by gpu_loop, while engine/provider/graphics remain owned by
        // RuntimeSession for the entire process.
        let active_surface = runtime.engine().and_then(EngineSession::active_surface);
        runtime
            .mark_running()
            .map_err(|error| HostError::RuntimeFailed(error.status() as i32))?;
        let mut shutdown_guard = RuntimeShutdownGuard::new(&mut runtime);

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
