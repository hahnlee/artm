//! Focused build phases for the ART native bootstrap.
//!
//! Keeping ARM64 assembly generation, interpreter objects, and the small
//! runtime/foundation probes in separate Rust modules prevents a change in one
//! phase from invalidating the command implementation for the others.

use super::*;

mod arm64;
mod foundation;
mod interpreter;

pub(crate) use arm64::*;
pub(crate) use foundation::*;
pub(crate) use interpreter::*;
