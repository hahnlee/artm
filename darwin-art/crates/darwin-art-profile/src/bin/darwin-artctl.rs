use darwin_art_profile::{
    ProfileLease, ProfilePaths, daemon_status, ensure_daemon, list_packages, list_processes,
    register_package, resolve_package, shutdown_daemon,
};
use std::env;
use std::error::Error;
use std::fs;
use std::io::{self, Write};
use std::os::unix::process::CommandExt;
use std::process::Command;
use std::thread;
use std::time::Duration;

fn main() {
    if let Err(error) = main_result() {
        eprintln!("darwin-artctl: {error}");
        std::process::exit(1);
    }
}

fn main_result() -> Result<(), Box<dyn Error>> {
    let paths = ProfilePaths::from_environment()?;
    match env::args().nth(1).as_deref() {
        Some("ensure") => {
            let path = ensure_daemon(&paths)?;
            io::stdout().write_all(path.as_os_str().as_encoded_bytes())?;
            println!();
        }
        Some("socket") => {
            io::stdout().write_all(paths.socket.as_os_str().as_encoded_bytes())?;
            println!();
        }
        Some("status") => println!("{}", daemon_status(&paths)?),
        Some("shutdown") => shutdown_daemon(&paths)?,
        Some("register") => {
            let package = env::args().nth(2).ok_or("register requires package")?;
            let record = env::args_os()
                .nth(3)
                .ok_or("register requires record file")?;
            register_package(&paths, &package, &fs::read(record)?)?;
        }
        Some("resolve") => {
            let package = env::args().nth(2).ok_or("resolve requires package")?;
            io::stdout().write_all(&resolve_package(&paths, &package)?)?;
        }
        Some("list") => print!("{}", list_packages(&paths)?),
        Some("ps") => print!("{}", list_processes(&paths)?),
        Some("hold") => {
            let seconds = env::args().nth(2).ok_or("hold requires seconds")?.parse()?;
            let _lease = ProfileLease::connect(&paths.socket)?;
            thread::sleep(Duration::from_secs_f64(seconds));
        }
        Some("supervise") => {
            let mut arguments = env::args_os().skip(2);
            let package = arguments
                .next()
                .ok_or("supervise requires package")?
                .into_string()
                .map_err(|_| "package is not UTF-8")?;
            let program = arguments.next().ok_or("supervise requires command")?;
            let mut child = Command::new(program).args(arguments).spawn()?;
            let lease = match ProfileLease::connect_process_pid(&paths.socket, child.id(), &package)
            {
                Ok(lease) => lease,
                Err(error) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(error.into());
                }
            };
            let status = child.wait()?;
            drop(lease);
            if !status.success() {
                std::process::exit(status.code().unwrap_or(1));
            }
        }
        Some("exec") => {
            let mut arguments = env::args_os().skip(2);
            let package = arguments
                .next()
                .ok_or("exec requires package")?
                .into_string()
                .map_err(|_| "package is not UTF-8")?;
            let program = arguments.next().ok_or("exec requires command")?;
            let lease = ProfileLease::connect_process(&paths.socket, &package)?;
            let lease_fd = lease.preserve_for_exec()?;
            return Err(Command::new(program)
                .args(arguments)
                .env("DARWIN_ART_PROFILE_LEASE_FD", lease_fd.to_string())
                .exec()
                .into());
        }
        _ => {
            return Err("usage: darwin-artctl {ensure|socket|status|shutdown|register PACKAGE RECORD|resolve PACKAGE|list|ps|hold SECONDS|supervise PACKAGE COMMAND [ARGS...]|exec PACKAGE COMMAND [ARGS...]}".into());
        }
    }
    Ok(())
}
