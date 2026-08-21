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
    pub(crate) mod surface;

    pub use abi::EngineSymbols;
    pub use engine::EngineSession;
    pub use graphics::GraphicsSession;
    pub use surface::SurfaceSession;
}

#[cfg(target_os = "macos")]
pub use platform::{EngineSession, EngineSymbols, GraphicsSession, SurfaceSession};
