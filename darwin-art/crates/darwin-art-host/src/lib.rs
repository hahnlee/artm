use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::ptr;

mod config;
mod frame;
#[cfg(target_os = "macos")]
mod gpu_loop;
mod provider;
#[cfg(target_os = "macos")]
mod surface;
#[cfg(target_os = "macos")]
mod teardown;

#[cfg(target_os = "macos")]
use darwin_art_engine::{EngineSession, SurfaceSession};

pub use config::{HostError, HostOutcome, RunOptions};
pub use darwin_art_engine_sys::{FrameCallback, ProcessConfig, ProcessResult};
use darwin_art_runtime::{RuntimeError, RuntimeSession, Subsystem};
pub use frame::OwnedFrame;
use frame::{FrameHost, receive_frame};
use provider::ProviderBridge;
#[cfg(target_os = "macos")]
use surface::*;
#[cfg(target_os = "macos")]
use teardown::shutdown_runtime;
const MAX_VISIBLE_SECONDS: f64 = 86_400.0;

use darwin_art_engine_sys::{DispatchPointerFn, PumpFrameworkFrameFn};

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
        // Darwin's application renderer is GPU-only. A published surface is
        // always handed to the direct Metal/HWUI loop below.
        if active_surface.is_some() {
            return gpu_loop::run(
                &mut runtime,
                active_surface,
                process,
                options,
                provider_lease,
                engine_lease,
                dispatch_pointer,
                pump_framework_frame,
            );
        }
        // Headless ART is a first-class mode. It never allocates a surface and
        // never uploads the callback mailbox into an IOSurface. Graphics runs
        // exclusively through `gpu_loop::run` above.
        shutdown_runtime(&mut runtime, provider_lease, engine_lease, None)?;
        Ok(HostOutcome {
            process,
            frames_presented: 0,
            last_frame: frame_host.last_frame,
        })
    }
}

fn path_c_string(path: &Path) -> Result<CString, HostError> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| HostError::InteriorNul(path.into()))
}

#[cfg(test)]
mod tests;
