mod install;
mod publish;

use darwin_art_native_artifact::{
    ConversionOutcome, ConversionRequest, Publication, prepare_complete_darwin_graph,
};
use install::{InstallRequest, install};
use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

fn run() -> Result<(), String> {
    let arguments = env::args_os().collect::<Vec<_>>();
    if arguments.len() != 10 {
        return Err(
            "usage: darwin-art-apk-install APK INSTALL_ROOT PACKAGE VERSION_CODE NATIVE_ROOT|none EXTRACTOR|none RUNTIME_ABI NATIVE_CACHE_ROOT CONVERTER|none"
                .to_owned(),
        );
    }
    let text = |index: usize, name: &str| {
        arguments[index]
            .to_str()
            .map(str::to_owned)
            .ok_or_else(|| format!("{name} is not UTF-8"))
    };
    let native_root = text(5, "native root")?;
    let extractor = PathBuf::from(&arguments[6]);
    let request = InstallRequest {
        apk: PathBuf::from(&arguments[1]),
        install_root: PathBuf::from(&arguments[2]),
        package: text(3, "package")?,
        version_code: text(4, "version code")?,
        native_root: (native_root != "none").then_some(native_root),
        extractor: (extractor != PathBuf::from("none")).then_some(extractor),
        runtime_abi: text(7, "runtime ABI")?,
    };
    let installed = install(&request)?;
    let native_cache_root = PathBuf::from(&arguments[8]);
    let converter_argument = PathBuf::from(&arguments[9]);
    let converter =
        (converter_argument != PathBuf::from("none")).then_some(converter_argument.as_path());
    let (native_backend, conversion) = if let Some(native_root) = &installed.native_root {
        let elf_directory = native_root
            .parent()
            .ok_or_else(|| "installed native root has no graph directory".to_owned())?;
        let cache_directory = native_cache_root
            .join(&installed.apk_sha256)
            .join(&request.runtime_abi);
        match prepare_complete_darwin_graph(&ConversionRequest {
            apk_sha256: &installed.apk_sha256,
            runtime_abi: &request.runtime_abi,
            elf_directory,
            cache_directory: &cache_directory,
            converter,
        })
        .map_err(|error| error.to_string())?
        {
            ConversionOutcome::CompleteDarwin {
                publication,
                libraries,
            } => (
                "darwin",
                format!(
                    "{}:{}",
                    match publication {
                        Publication::Published => "published",
                        Publication::Existing => "existing",
                    },
                    libraries.len()
                ),
            ),
            ConversionOutcome::AndroidElf {
                libraries,
                cached_failure,
                reason,
            } => (
                "elf",
                format!(
                    "{}:{reason}:{}",
                    if cached_failure {
                        "cached"
                    } else {
                        "attempted"
                    },
                    libraries.len()
                ),
            ),
        }
    } else {
        ("none", "not-applicable:0".to_owned())
    };
    println!(
        "apk-install: PASS package={} version_code={} apk_sha256={} base_apk={} native_root={} runtime_abi={} publication={} native_backend={} conversion={}",
        request.package,
        request.version_code,
        installed.apk_sha256,
        installed.base_apk.display(),
        installed
            .native_root
            .as_ref()
            .map_or_else(|| "none".to_owned(), |path| path.display().to_string()),
        request.runtime_abi,
        if installed.existing {
            "existing"
        } else {
            "atomic"
        },
        native_backend,
        conversion,
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("apk-install: {error}");
            ExitCode::from(2)
        }
    }
}
