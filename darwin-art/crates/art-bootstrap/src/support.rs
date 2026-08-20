//! Small process and diagnostics boundary shared by build command modules.

use std::ffi::OsStr;
use std::process::{Command, Output};

use crate::Result;

pub(crate) fn run_command(command: &mut Command) -> Result<()> {
    let description = describe_command(command);
    let status = command.status()?;
    if !status.success() {
        return Err(format!("command failed ({status}): {description}").into());
    }
    Ok(())
}

pub(crate) fn command_output(command: &mut Command) -> Result<String> {
    let description = describe_command(command);
    let Output {
        status,
        stdout,
        stderr,
    } = command.output()?;
    if std::env::var_os("DARWIN_ART_DEBUG_CHILD_STDERR").is_some() && !stderr.is_empty() {
        eprint!("{}", String::from_utf8_lossy(&stderr));
    }
    if !status.success() {
        return Err(format!(
            "command failed ({status}): {description}\n{}",
            String::from_utf8_lossy(&stderr)
        )
        .into());
    }
    Ok(String::from_utf8(stdout)?)
}

pub(crate) fn describe_command(command: &Command) -> String {
    let program = command.get_program().to_string_lossy();
    let args = command
        .get_args()
        .map(OsStr::to_string_lossy)
        .collect::<Vec<_>>()
        .join(" ");
    format!("{program} {args}")
}
