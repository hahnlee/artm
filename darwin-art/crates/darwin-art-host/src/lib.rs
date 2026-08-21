mod config;
mod frame;
#[cfg(target_os = "macos")]
mod gpu_loop;
mod provider;
mod run;
#[cfg(target_os = "macos")]
mod surface;
#[cfg(target_os = "macos")]
mod teardown;

pub use config::{HostError, HostOutcome, RunOptions};
pub use darwin_art_engine_sys::{FrameCallback, ProcessConfig, ProcessResult};
pub use frame::OwnedFrame;
pub use run::run;

#[cfg(test)]
mod tests;
