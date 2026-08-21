use std::ptr;

use crate::config::{HostError, HostOutcome, ProcessConfigInputs, RunOptions};
use crate::frame::{FrameHost, receive_frame};
#[cfg(target_os = "macos")]
use crate::gpu_loop::run as run_gpu_loop;
use crate::provider::ProviderBridge;
#[cfg(target_os = "macos")]
use crate::teardown::{process_run_failure, shutdown_runtime, unattached_engine_failure};
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
        let mut runtime = RuntimeSession::<
            EngineSession,
            Box<ProviderBridge>,
            SurfaceSession,
            GraphicsSession,
        >::new();
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
        let mut graphics_session = engine.create_graphics_session().ok();
        let config_inputs = match ProcessConfigInputs::from_options(options) {
            Ok(inputs) => inputs,
            Err(error) => {
                // The engine has installed provider hooks, but ownership has
                // not yet transferred into RuntimeSession.  Keep this
                // pre-transfer failure on the same explicit rollback seam as
                // a failed engine attach; otherwise a malformed path (for
                // example an interior NUL) would leak the VM/bootstrap state
                // until process exit.
                let rollback = unattached_engine_failure(
                    &mut engine,
                    graphics_session.as_mut(),
                    &provider_bridge,
                );
                return Err(match rollback {
                    HostError::RuntimeFailed(_) => error,
                    other => other,
                });
            }
        };
        let mut frame_host = FrameHost {
            frames_received: 0,
            last_frame: None,
        };
        let config = config_inputs.build(
            options,
            ptr::from_mut(&mut frame_host).cast(),
            Some(receive_frame),
            provider_context,
            Some(ProviderBridge::acquire_callback()),
            Some(ProviderBridge::release_callback()),
            graphics_session
                .as_ref()
                .map_or(ptr::null_mut(), |session| session.raw_handle().cast()),
        );
        let process = match engine.run_process(&config) {
            Ok(result) => result,
            Err(status) => {
                return Err(process_run_failure(
                    &mut runtime,
                    &mut engine,
                    graphics_session.as_mut(),
                    status,
                ));
            }
        };
        // The graphics engine publishes its drawable during run_process, so
        // capture the owner only after that bootstrap has completed.
        let active_surface = engine.active_surface();
        if let Err(mut engine) = runtime.attach_engine(engine) {
            // The resource was never transferred, so its Drop path is the
            // only owner responsible for the failed transfer. Clear hooks
            // before dropping the bridge that supplied their context.
            return Err(unattached_engine_failure(
                &mut engine,
                graphics_session.as_mut(),
                &provider_bridge,
            ));
        }
        if let Err(provider_bridge) = runtime.attach_provider(provider_bridge) {
            let _ = provider_bridge.clear();
            let _ = shutdown_runtime(&mut runtime, None, None, None, None);
            return Err(HostError::RuntimeFailed(-1));
        }
        let provider_lease = match runtime.install_subsystem(Subsystem::ElfNamespace) {
            Ok(lease) => lease,
            Err(error) => {
                let _ = shutdown_runtime(&mut runtime, None, None, None, None);
                return Err(HostError::RuntimeFailed(error.status() as i32));
            }
        };
        let engine_lease = match runtime.install_subsystem(Subsystem::Engine) {
            Ok(lease) => lease,
            Err(error) => {
                let _ = shutdown_runtime(&mut runtime, Some(provider_lease), None, None, None);
                return Err(HostError::RuntimeFailed(error.status() as i32));
            }
        };
        // Attach the graphics owner before entering ART, but install its
        // lifecycle lease only once a surface exists.  The surface is
        // published by the process bootstrap and is installed by gpu_loop;
        // keeping this order makes teardown the exact reverse:
        // Graphics -> Surface -> Engine -> Provider.
        let graphics_attached = if let Some(graphics) = graphics_session.take() {
            if let Err(graphics) = runtime.attach_graphics(graphics) {
                drop(graphics);
                let _ = shutdown_runtime(
                    &mut runtime,
                    Some(provider_lease),
                    Some(engine_lease),
                    None,
                    None,
                );
                return Err(HostError::RuntimeFailed(-1));
            }
            true
        } else {
            false
        };
        if let Err(error) = runtime.mark_running() {
            runtime.fail(error);
            let _ = shutdown_runtime(
                &mut runtime,
                Some(provider_lease),
                Some(engine_lease),
                None,
                None,
            );
            return Err(HostError::RuntimeFailed(error.status() as i32));
        }
        // Darwin's application renderer is GPU-only. A published surface is
        // always handed to the direct Metal/HWUI loop below.
        if active_surface.is_some() {
            return run_gpu_loop(
                &mut runtime,
                active_surface,
                process,
                options,
                provider_lease,
                engine_lease,
                graphics_attached,
            );
        }
        // Headless ART is a first-class mode. It never allocates a surface and
        // never uploads the callback mailbox into an IOSurface. Graphics runs
        // exclusively through `gpu_loop::run` above.
        let graphics_lease = if graphics_attached {
            match runtime.install_subsystem(Subsystem::Graphics) {
                Ok(lease) => Some(lease),
                Err(error) => {
                    let cleanup = shutdown_runtime(
                        &mut runtime,
                        Some(provider_lease),
                        Some(engine_lease),
                        None,
                        None,
                    );
                    return match cleanup {
                        Ok(()) => Err(HostError::RuntimeFailed(error.status() as i32)),
                        Err(cleanup_error) => Err(cleanup_error),
                    };
                }
            }
        } else {
            None
        };
        shutdown_runtime(
            &mut runtime,
            Some(provider_lease),
            Some(engine_lease),
            None,
            graphics_lease,
        )?;
        Ok(HostOutcome {
            process,
            frames_presented: 0,
            last_frame: frame_host.last_frame,
        })
    }
}
