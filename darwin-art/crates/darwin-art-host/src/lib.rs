mod config;
mod frame;
#[cfg(target_os = "macos")]
mod gpu_loop;
mod run;
#[cfg(target_os = "macos")]
mod runtime;
#[cfg(target_os = "macos")]
mod surface;
#[cfg(target_os = "macos")]
mod teardown;

pub use config::{HostError, HostOutcome, RunOptions};
// The host surface intentionally exports only the value result. Raw config
// structs and callback function pointers belong to `darwin-art-engine-sys` and
// are kept behind the owned `ProcessRequest` path.
pub use darwin_art_engine_sys::ProcessResult;
pub use frame::OwnedFrame;
pub use run::run;

#[cfg(test)]
mod tests;
