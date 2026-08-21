//! Native graph driver.
//!
//! This module owns only the xtask/Ninja handoff. The large bootstrap flavor
//! builder remains in `runtime_commands` until its archive phases are split;
//! changing graph policy therefore does not touch ART source/flag setup.

use std::fs;
use std::path::Path;
use std::process::Command;

use crate::Result;
use crate::support::run_command;

pub(crate) fn build_native_graph(root: &Path, target: &str) -> Result<()> {
    if !matches!(
        target,
        "graphics-bootstrap" | "runtime-bootstrap" | "graphics-foundation" | "foundation"
    ) {
        return Err(format!("unsupported native graph target: {target}").into());
    }
    let graph_dir = root.join("_build/native-graph");
    fs::create_dir_all(&graph_dir)?;
    let graph = graph_dir.join("build.ninja");
    run_command(
        Command::new("cargo")
            .args([
                "run",
                "-q",
                "-p",
                "darwin-art-xtask",
                "--",
                "native-graph",
                "--out",
            ])
            .arg(&graph)
            .current_dir(root),
    )?;
    let ninja = root.join("_aosp/external/skia/third_party/ninja/ninja");
    if !ninja.is_file() {
        return Err(format!("pinned Ninja is missing: {}", ninja.display()).into());
    }
    run_command(
        Command::new(&ninja)
            .arg("-f")
            .arg(&graph)
            .arg(target)
            .current_dir(root),
    )
}
