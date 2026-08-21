use super::*;

mod archive;
mod compile;
mod staging;

pub(crate) use archive::finalize;
pub(crate) use compile::{RuntimeBootstrapCompiled, compile};
pub(crate) use staging::{RuntimeBootstrapStaging, prepare};

pub(crate) fn build_runtime_bootstrap_flavor(root: &Path, real_graphics: bool) -> Result<()> {
    let staged = prepare(root, real_graphics)?;
    let compiled = compile(&staged, real_graphics)?;
    finalize(&staged, real_graphics, compiled)
}
