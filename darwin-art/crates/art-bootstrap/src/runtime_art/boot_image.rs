//! AOSP-shaped Android 16 boot-image producer.
//!
//! The boot image is an installed-code artifact, not a test fixture.  Keep
//! the class-path order and dex2oat contract in one Rust-owned build phase so
//! callers cannot accidentally mix components from different generations.

use super::*;

const BOOT_IMAGE_BASE: &str = "0x70000000";
const BOOT_IMAGE_FILTER: &str = "speed";

/// The Android 16 boot class path used by the detached Darwin ART runtime.
/// Keep this order identical to the `bootclasspath` property consumed by ART.
fn boot_class_path(root: &Path) -> [PathBuf; 11] {
    [
        root.join("_build/android16-core-oj-compat/core-oj-compat.jar"),
        root.join("_prebuilt/android-16/bootclasspath/core-libart.jar"),
        root.join("_build/android16-framework-compat/framework-compat.jar"),
        root.join("_prebuilt/android-16/bootclasspath/framework-location.jar"),
        root.join("_build/android16-ps16k-r07/extracted/conscrypt/javalib/conscrypt.jar"),
        root.join("_build/android16-ps16k-r07/extracted/bt/javalib/framework-bluetooth.jar"),
        root.join(
            "_build/android16-ps16k-r07/extracted/mediaprovider/javalib/framework-mediaprovider.jar",
        ),
        root.join("_build/android16-ps16k-r07/extracted/permission/javalib/framework-permission.jar"),
        root.join(
            "_build/android16-ps16k-r07/extracted/permission/javalib/framework-permission-s.jar",
        ),
        root.join("_build/android16-ps16k-r07/extracted/art/javalib/okhttp.jar"),
        root.join("_build/bootclasspath/core-icu4j-api36.jar"),
    ]
}

fn create_staging_dir(parent: &Path) -> Result<PathBuf> {
    let pid = std::process::id();
    for attempt in 0..128u32 {
        let suffix = if attempt == 0 {
            pid.to_string()
        } else {
            format!("{pid}.{attempt}")
        };
        let candidate = parent.join(format!(".android16-boot-image-darwin.stage.{suffix}"));
        match fs::create_dir(&candidate) {
            Ok(()) => return Ok(candidate),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.into()),
        }
    }
    Err("could not allocate a unique boot-image staging directory".into())
}

fn required_component_names() -> [&'static str; 11] {
    [
        "boot",
        "boot-core-libart",
        "boot-framework-compat",
        "boot-framework-location",
        "boot-conscrypt",
        "boot-framework-bluetooth",
        "boot-framework-mediaprovider",
        "boot-framework-permission",
        "boot-framework-permission-s",
        "boot-okhttp",
        "boot-core-icu4j-api36",
    ]
}

fn verify_component_set(staging: &Path) -> Result<()> {
    for component in required_component_names() {
        for extension in ["art", "oat", "vdex"] {
            let path = staging.join(format!("{component}.{extension}"));
            if !path.is_file() {
                return Err(
                    format!("dex2oat omitted boot-image component: {}", path.display()).into(),
                );
            }
        }
    }
    let oat = fs::read(staging.join("boot.oat"))?;
    let filter = b"compiler-filter\0speed\0";
    if !oat.windows(filter.len()).any(|window| window == filter) {
        return Err("boot.oat does not carry compiler-filter=speed".into());
    }
    Ok(())
}

fn create_arm64_component_links(staging: &Path) -> Result<()> {
    let arm64 = staging.join("arm64");
    fs::create_dir(&arm64)?;
    for component in required_component_names() {
        for extension in ["art", "oat", "vdex"] {
            let name = format!("{component}.{extension}");
            std::os::unix::fs::symlink(format!("../{name}"), arm64.join(name))?;
        }
    }
    Ok(())
}

fn publish_boot_image(staging: &Path, destination: &Path) -> Result<()> {
    let parent = destination
        .parent()
        .ok_or("boot-image destination has no parent")?;
    let backup = parent.join(format!(
        ".android16-boot-image-darwin.rollback.{}",
        std::process::id()
    ));
    if backup.exists() {
        fs::remove_dir_all(&backup)?;
    }
    let had_existing = destination.exists();
    if had_existing {
        fs::rename(destination, &backup)?;
    }
    if let Err(error) = fs::rename(staging, destination) {
        if had_existing {
            let _ = fs::rename(&backup, destination);
        }
        return Err(error.into());
    }
    if had_existing {
        fs::remove_dir_all(&backup)?;
    }
    Ok(())
}

/// Build a complete speed boot image in a sibling directory and publish it as
/// one generation.  A failed dex2oat run leaves the installed image intact.
pub(crate) fn build_android16_boot_image(root: &Path) -> Result<()> {
    let runtime =
        root.join("_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib");
    let host = root.join("target/debug/darwin-art-host");
    if !runtime.is_file() {
        return Err(format!("graphics runtime dylib is missing: {}", runtime.display()).into());
    }
    if !host.is_file() {
        return Err(format!("darwin-art-host is missing: {}", host.display()).into());
    }

    let class_path = boot_class_path(root);
    for path in &class_path {
        if !path.is_file() {
            return Err(format!("boot class path component is missing: {}", path.display()).into());
        }
    }
    let destination = root.join("_build/android16-boot-image-darwin");
    let parent = destination
        .parent()
        .ok_or("boot-image destination has no parent")?;
    fs::create_dir_all(parent)?;
    let staging = create_staging_dir(parent)?;
    let result = (|| {
        let oat = staging.join("boot.oat");
        let image = staging.join("boot.art");
        let mut command = Command::new(&host);
        command
            .arg("--dex2oat")
            .arg(runtime.to_str().ok_or("runtime dylib path is not UTF-8")?);
        command.args([
            "--android-root=/",
            &format!("--base={BOOT_IMAGE_BASE}"),
            &format!(
                "--image={}",
                image.to_str().ok_or("boot image path is not UTF-8")?
            ),
            &format!(
                "--oat-file={}",
                oat.to_str().ok_or("boot oat path is not UTF-8")?
            ),
            &format!("--compiler-filter={BOOT_IMAGE_FILTER}"),
            "--image-format=lz4",
            "--instruction-set=arm64",
        ]);
        for path in &class_path {
            let dex_file = path.to_str().ok_or("boot class path is not UTF-8")?;
            let dex_location = path
                .strip_prefix(root)?
                .to_str()
                .ok_or("boot class path is not UTF-8")?;
            command.args([
                &format!("--dex-file={dex_file}"),
                &format!("--dex-location={dex_location}"),
            ]);
        }
        let mut boot_cp = String::new();
        for (index, path) in class_path.iter().enumerate() {
            if index != 0 {
                boot_cp.push(':');
            }
            boot_cp.push_str(
                path.strip_prefix(root)?
                    .to_str()
                    .ok_or("boot class path is not UTF-8")?,
            );
        }
        command.args([
            "--runtime-arg",
            &format!("-Xbootclasspath:{boot_cp}"),
            "--runtime-arg",
            &format!("-Xbootclasspath-locations:{boot_cp}"),
        ]);
        run_command(&mut command)?;
        verify_component_set(&staging)?;
        create_arm64_component_links(&staging)?;
        publish_boot_image(&staging, &destination)?;
        println!(
            "build-android16-boot-image: filter={BOOT_IMAGE_FILTER} components=11 published={}",
            destination.display()
        );
        Ok(())
    })();
    if result.is_err() && staging.exists() {
        fs::remove_dir_all(&staging)?;
    }
    result
}
