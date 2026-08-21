use super::*;

#[path = "dex_commands/baseline.rs"]
mod baseline;
#[path = "dex_commands/button.rs"]
mod button;
#[path = "dex_commands/elf_jni.rs"]
mod elf_jni;
#[path = "dex_commands/network.rs"]
mod network;

pub(crate) use baseline::build_dex_probe;
pub(crate) use button::build_button_dex_probe;
pub(crate) use elf_jni::build_elf_jni_dex_probe;
pub(crate) use network::build_network_dex_probe;

pub(crate) fn find_d8() -> Result<PathBuf> {
    let sdk_root = android_sdk_root()?;
    let build_tools = sdk_root.join("build-tools");
    let mut candidates = fs::read_dir(&build_tools)?
        .filter_map(std::result::Result::ok)
        .map(|entry| entry.path().join("d8"))
        .filter(|path| path.is_file())
        .collect::<Vec<_>>();
    candidates.sort();
    candidates
        .pop()
        .ok_or_else(|| format!("d8 was not found under {}", build_tools.display()).into())
}

pub(crate) fn android_sdk_root() -> Result<PathBuf> {
    env::var_os("ANDROID_SDK_ROOT")
        .or_else(|| env::var_os("ANDROID_HOME"))
        .map(PathBuf::from)
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .map(|home| home.join("Library/Android/sdk"))
        })
        .ok_or_else(|| "could not determine the Android SDK directory".into())
}

pub(crate) fn find_android_platform_jar() -> Result<PathBuf> {
    let jar = android_sdk_root()?.join("platforms/android-36/android.jar");
    if !jar.is_file() {
        return Err(format!("Android API 36 platform JAR is missing: {}", jar.display()).into());
    }
    Ok(jar)
}
