use darwin_art_profile::{
    ProfileLease, ProfilePaths, daemon_status, ensure_daemon, shutdown_daemon,
};
use std::env;
use std::error::Error;
use std::io::{self, Write};
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
        Some("hold") => {
            let seconds = env::args().nth(2).ok_or("hold requires seconds")?.parse()?;
            let _lease = ProfileLease::connect(&paths.socket)?;
            thread::sleep(Duration::from_secs_f64(seconds));
        }
        _ => {
            return Err("usage: darwin-artctl {ensure|socket|status|shutdown|hold SECONDS}".into());
        }
    }
    Ok(())
}
