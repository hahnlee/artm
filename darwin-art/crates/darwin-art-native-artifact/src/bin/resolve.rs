use darwin_art_native_artifact::{GraphSelection, resolve_directory_graph};
use std::env;
use std::path::Path;
use std::process::ExitCode;

fn run() -> Result<(), String> {
    let arguments = env::args_os().collect::<Vec<_>>();
    if arguments.len() != 5 {
        return Err(
            "usage: darwin-art-native-resolve APK_SHA256 RUNTIME_ABI ELF_DIRECTORY DARWIN_DIRECTORY"
                .to_owned(),
        );
    }
    let apk_sha256 = arguments[1]
        .to_str()
        .ok_or_else(|| "APK SHA-256 is not UTF-8".to_owned())?;
    let runtime_abi = arguments[2]
        .to_str()
        .ok_or_else(|| "runtime ABI is not UTF-8".to_owned())?;
    let elf_directory = Path::new(&arguments[3]);
    let darwin_directory = Path::new(&arguments[4]);
    match resolve_directory_graph(apk_sha256, runtime_abi, elf_directory, darwin_directory)
        .map_err(|error| error.to_string())?
    {
        GraphSelection::CompleteDarwin(paths) => println!(
            "native-resolve: PASS backend=darwin libraries={} directory={}",
            paths.len(),
            darwin_directory.display()
        ),
        GraphSelection::AndroidElf(paths) => println!(
            "native-resolve: PASS backend=elf libraries={} directory={}",
            paths.len(),
            elf_directory.display()
        ),
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("native-resolve: {error}");
            ExitCode::from(2)
        }
    }
}
