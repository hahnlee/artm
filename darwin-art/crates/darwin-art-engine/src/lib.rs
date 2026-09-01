//! Safe owner for the process-scoped Darwin engine image.
//!
//! The public owner types are split by responsibility while the platform
//! module keeps the macOS-only ABI surface private.

#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(target_os = "macos")]
mod platform {
    pub(crate) mod abi;
    pub(crate) mod engine;
    pub(crate) mod graphics;
    pub(crate) mod process;
    pub(crate) mod surface;

    pub use engine::EngineSession;
    pub use graphics::{GraphicsSession, LooperWakeToken};
    pub use process::{CallbackBindings, ProcessRequest, ProcessRequestError};
    pub use surface::{ScanoutToken, SurfaceSession};
}

#[cfg(target_os = "macos")]
pub use platform::{
    CallbackBindings, EngineSession, GraphicsSession, LooperWakeToken, ProcessRequest,
    ProcessRequestError, ScanoutToken, SurfaceSession,
};
