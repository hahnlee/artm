//! Owner transfer for the process-scoped engine/provider/graphics resources.
//!
//! This phase performs the only unsafe provider-hook installation in the host
//! frontend. Once it returns successfully, every long-lived value is owned by
//! `HostRuntime` and the caller only keeps the opaque provider context and a
//! graphics-present capability bit.

use std::path::Path;

use crate::config::HostError;
use crate::runtime::HostRuntime;
use darwin_art_engine::EngineSession;
use darwin_art_runtime::ProviderBridge;

pub(super) struct RuntimeBootstrap {
    pub(super) graphics_attached: bool,
}

/// Open the native engine and transfer all process-scoped resources into the
/// Rust runtime owner. Every failed transfer clears hooks before the callback
/// context or engine image can be dropped.
#[cfg(target_os = "macos")]
pub(super) fn attach_runtime(
    runtime: &mut HostRuntime,
    library: &Path,
) -> Result<RuntimeBootstrap, HostError> {
    let engine = EngineSession::open(library).map_err(HostError::DynamicLoader)?;
    let provider_bridge = Box::new(engine.provider_bridge());
    // SAFETY: provider_bridge is transferred into HostRuntime immediately
    // below and remains alive until the engine hooks are cleared in teardown.
    unsafe {
        engine.install_provider_hooks(
            provider_bridge.context(),
            Some(ProviderBridge::acquire_callback()),
            Some(ProviderBridge::release_callback()),
        );
    }
    if let Err(engine) = runtime.attach_engine(engine) {
        // Hooks were installed before the ownership transfer. Clear the
        // process-global table while both callback bridge and image are live.
        let _ = provider_bridge.clear();
        drop(engine);
        return Err(HostError::RuntimeFailed(-1));
    }
    if let Err(provider_bridge) = runtime.attach_provider(provider_bridge) {
        let _ = provider_bridge.clear();
        return Err(HostError::RuntimeFailed(-1));
    }

    let graphics_attached = if let Some(graphics) = runtime
        .engine()
        .and_then(|engine| engine.create_graphics_session().ok())
    {
        if runtime.attach_graphics(graphics).is_err() {
            return Err(HostError::RuntimeFailed(-1));
        }
        true
    } else {
        false
    };

    Ok(RuntimeBootstrap { graphics_attached })
}
