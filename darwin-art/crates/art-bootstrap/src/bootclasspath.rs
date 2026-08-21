use super::*;

pub(crate) fn prepare_icu_bootclasspath(root: &Path) -> Result<()> {
    let source_lock = read_lock(root)?;
    let source = root.join("_prebuilt/android-16/bootclasspath/core-icu4j.jar");
    if !source.is_file() {
        return Err(format!(
            "{} is missing; run `art-bootstrap sync` first",
            source.display()
        )
        .into());
    }
    verify_sha256(&source, lock_value(&source_lock, "CORE_ICU4J_SHA256")?)?;

    let build_dir = root.join("_build/bootclasspath/core-icu4j");
    let output = root.join("_build/bootclasspath/core-icu4j.jar");
    fs::create_dir_all(&build_dir)?;
    if !output.is_file() {
        run_command(
            Command::new(find_d8()?)
                .args(["--min-api", "36", "--output"])
                .arg(&output)
                .arg(&source),
        )?;
    }
    run_command(
        Command::new("unzip")
            .args(["-o", "-q"])
            .arg(&output)
            .arg("classes.dex")
            .arg("-d")
            .arg(&build_dir),
    )?;
    let probe = root.join("_build/dex-probe/dex-probe");
    if !probe.is_file() {
        return Err("DEX probe is missing; run `build-dex` first".into());
    }
    let summary = command_output(
        Command::new(&probe)
            .arg("--summary")
            .arg(build_dir.join("classes.dex")),
    )?;
    let expected = "AOSP DEX: verified=yes version=39 classes=1596 methods=14990 \
                    class[0]=Landroid/icu/impl/Assert;";
    if summary.trim() != expected {
        return Err(format!("unexpected core-icu4j DEX summary: {summary:?}").into());
    }
    Ok(())
}

pub(crate) fn materialize_api36_core_icu4j(root: &Path) -> Result<PathBuf> {
    if let Some(value) = env::var_os("DARWIN_ART_CORE_ICU4J_JAR") {
        let source = PathBuf::from(value);
        if source.is_file() {
            return Ok(source);
        }
        return Err(format!(
            "DARWIN_ART_CORE_ICU4J_JAR does not name a file: {}",
            source.display()
        )
        .into());
    }

    let prebuilt = root.join("_prebuilt/android-16/i18n/core-icu4j-api36.jar");
    if prebuilt.is_file() {
        return Ok(prebuilt);
    }

    let sdk = android_sdk_root()?;
    let default_images = [
        sdk.join("system-images/android-36/google_apis_playstore/arm64-v8a/system.img"),
        sdk.join("system-images/android-36/google_apis_playstore_ps16k/arm64-v8a/system.img"),
    ];
    let system_image = if let Some(value) = env::var_os("DARWIN_ART_ANDROID16_SYSTEM_IMAGE") {
        let path = PathBuf::from(value);
        if !path.is_file() {
            return Err(format!(
                "DARWIN_ART_ANDROID16_SYSTEM_IMAGE does not name a file: {}",
                path.display()
            )
            .into());
        }
        path
    } else {
        default_images
            .into_iter()
            .find(|path| path.is_file())
            .ok_or_else(|| {
                format!(
                    "Android 16 ICU76 runtime JAR is missing. Set \
                     DARWIN_ART_CORE_ICU4J_JAR or install an API 36 ARM64 Play Store system \
                     image under {}",
                    sdk.join("system-images").display()
                )
            })?
    };

    let build_dir = root.join("_build/bootclasspath/api36-i18n-extract");
    let apex = build_dir.join("com.android.i18n.apex");
    let output = build_dir.join("core-icu4j.jar");
    fs::create_dir_all(&build_dir)?;
    if !apex.is_file() {
        run_command(
            Command::new("cargo")
                .args(["run", "--quiet", "--release", "--manifest-path"])
                .arg(root.join("tools/super-i18n-apex-extract/Cargo.toml"))
                .arg("--")
                .arg(&system_image)
                .arg(&apex),
        )?;
    }
    if !output.is_file() {
        run_command(
            Command::new("cargo")
                .args(["run", "--quiet", "--release", "--manifest-path"])
                .arg(root.join("tools/apex-ext2-extract/Cargo.toml"))
                .arg("--")
                .arg(&apex)
                .arg(&output),
        )?;
    }
    Ok(output)
}

pub(crate) fn prepare_icu_runtime_bootclasspath(root: &Path) -> Result<PathBuf> {
    let source = materialize_api36_core_icu4j(root)?;
    run_command(
        Command::new("bash")
            .arg(root.join("tools/audit-android16-core-icu4j-runtime.sh"))
            .arg(&source),
    )?;
    let output = root.join("_build/bootclasspath/core-icu4j-api36.jar");
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    if source != output {
        fs::copy(source, &output)?;
    }
    Ok(output)
}
pub(crate) fn find_ndk_headers() -> Result<(PathBuf, PathBuf)> {
    let mut ndk_roots = Vec::new();
    for variable in ["ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"] {
        if let Some(value) = env::var_os(variable) {
            ndk_roots.push(PathBuf::from(value));
        }
    }
    if let Some(home) = env::var_os("HOME") {
        let ndk_parent = PathBuf::from(home).join("Library/Android/sdk/ndk");
        if let Ok(entries) = fs::read_dir(ndk_parent) {
            let mut installed: Vec<PathBuf> = entries
                .filter_map(std::result::Result::ok)
                .map(|entry| entry.path())
                .filter(|path| path.is_dir())
                .collect();
            installed.sort();
            installed.reverse();
            ndk_roots.extend(installed);
        }
    }

    for ndk_root in ndk_roots {
        let prebuilt = ndk_root.join("toolchains/llvm/prebuilt");
        let Ok(hosts) = fs::read_dir(prebuilt) else {
            continue;
        };
        for host in hosts.filter_map(std::result::Result::ok) {
            let include = host.path().join("sysroot/usr/include");
            let arch = include.join("aarch64-linux-android");
            if include.join("elf.h").exists() && arch.is_dir() {
                return Ok((include, arch));
            }
        }
    }
    Err("Android NDK headers are required for the Android ELF ABI (<elf.h>)".into())
}

pub(crate) fn verify_bootclasspath(root: &Path) -> Result<()> {
    let prebuilt = root.join("_prebuilt/android-16/bootclasspath");
    let probe = root.join("_build/dex-probe/dex-probe");
    let manifest = read_key_value_file(&root.join("bootclasspath.lock"))?;
    if !probe.exists() {
        return Err("DEX probe is missing; run `build-dex` first".into());
    }

    let entries = [
        (
            "core-oj",
            "CORE_OJ_SHA256",
            "CORE_OJ_SIZE",
            "AOSP DEX: verified=yes version=39 classes=4188 methods=41526 \
             class[0]=Ljava/lang/Object;",
        ),
        (
            "core-libart",
            "CORE_LIBART_SHA256",
            "CORE_LIBART_SIZE",
            "AOSP DEX: verified=yes version=39 classes=543 methods=5453 \
             class[0]=Landroid/compat/Compatibility$BehaviorChangeDelegate;",
        ),
    ];
    for (name, sha_key, size_key, expected) in entries {
        let jar = prebuilt.join(format!("{name}.jar"));
        if !jar.exists() {
            return Err(format!(
                "{} is missing; extract matching Android 16 boot JARs first",
                jar.display()
            )
            .into());
        }
        verify_sha256(&jar, lock_value(&manifest, sha_key)?)?;
        let expected_size: u64 = lock_value(&manifest, size_key)?.parse()?;
        let actual_size = fs::metadata(&jar)?.len();
        if actual_size != expected_size {
            return Err(format!(
                "size mismatch for {}: expected {expected_size}, found {actual_size}",
                jar.display()
            )
            .into());
        }
        let extract_dir = root.join(format!("_build/bootclasspath/{name}"));
        fs::create_dir_all(&extract_dir)?;
        run_command(
            Command::new("unzip")
                .args(["-o", "-q"])
                .arg(&jar)
                .arg("classes.dex")
                .arg("-d")
                .arg(&extract_dir),
        )?;
        let output = command_output(
            Command::new(&probe)
                .arg("--summary")
                .arg(extract_dir.join("classes.dex")),
        )?;
        if output.trim() != expected {
            return Err(format!("unexpected {name} DEX summary: {output:?}").into());
        }
        println!("verify-bootclasspath: {name} {}", output.trim());
    }
    let framework = prebuilt.join("framework.jar");
    if !framework.exists() {
        return Err(format!(
            "{} is missing; pull /system/framework/framework.jar from the matching Android 16 image",
            framework.display()
        )
        .into());
    }
    verify_sha256(&framework, lock_value(&manifest, "FRAMEWORK_SHA256")?)?;
    let expected_framework_size: u64 = lock_value(&manifest, "FRAMEWORK_SIZE")?.parse()?;
    if fs::metadata(&framework)?.len() != expected_framework_size {
        return Err(format!("size mismatch for {}", framework.display()).into());
    }
    let framework_summaries = [
        (
            "classes.dex",
            "AOSP DEX: verified=yes version=39 classes=6609 methods=65389 \
             class[0]=Landroid/Manifest$permission;",
        ),
        (
            "classes2.dex",
            "AOSP DEX: verified=yes version=39 classes=7041 methods=65454 \
             class[0]=Landroid/hardware/camera2/impl/CallbackProxies$SessionStateCallbackProxy$$ExternalSyntheticLambda0;",
        ),
        (
            "classes3.dex",
            "AOSP DEX: verified=yes version=39 classes=8736 methods=65454 \
             class[0]=Landroid/os/IInterface;",
        ),
        (
            "classes4.dex",
            "AOSP DEX: verified=yes version=39 classes=6855 methods=65167 \
             class[0]=Lcom/android/ims/internal/IImsServiceController;",
        ),
        (
            "classes5.dex",
            "AOSP DEX: verified=yes version=39 classes=6478 methods=56051 \
             class[0]=Lcom/android/internal/hidden_from_bootclasspath/android/app/admin/flags/CustomFeatureFlags$$ExternalSyntheticLambda0;",
        ),
    ];
    let extract_dir = root.join("_build/bootclasspath/framework");
    fs::create_dir_all(&extract_dir)?;
    for (dex_name, expected) in framework_summaries {
        run_command(
            Command::new("unzip")
                .args(["-o", "-q"])
                .arg(&framework)
                .arg(dex_name)
                .arg("-d")
                .arg(&extract_dir),
        )?;
        let output = command_output(
            Command::new(&probe)
                .arg("--summary")
                .arg(extract_dir.join(dex_name)),
        )?;
        if output.trim() != expected {
            return Err(format!("unexpected framework {dex_name} DEX summary: {output:?}").into());
        }
        println!(
            "verify-bootclasspath: framework/{dex_name} {}",
            output.trim()
        );
    }
    let icu_source = prebuilt.join("core-icu4j.jar");
    let expected_icu_size: u64 = lock_value(&manifest, "CORE_ICU4J_SOURCE_SIZE")?.parse()?;
    if fs::metadata(&icu_source)?.len() != expected_icu_size {
        return Err(format!("size mismatch for {}", icu_source.display()).into());
    }
    prepare_icu_bootclasspath(root)?;
    println!(
        "verify-bootclasspath: core-icu4j AOSP DEX: verified=yes version=39 \
         classes=1596 methods=14990 class[0]=Landroid/icu/impl/Assert;"
    );
    Ok(())
}

pub(crate) fn replace_required(source: &mut String, from: &str, to: &str) -> Result<()> {
    if !source.contains(from) {
        return Err(
            format!("locked ART source no longer contains expected fragment: {from}").into(),
        );
    }
    *source = source.replacen(from, to, 1);
    Ok(())
}

pub(crate) fn read_lock(root: &Path) -> Result<BTreeMap<String, String>> {
    read_key_value_file(&root.join("sources.lock"))
}

pub(crate) fn read_key_value_file(path: &Path) -> Result<BTreeMap<String, String>> {
    let mut values = BTreeMap::new();
    for line in fs::read_to_string(path)?.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, value) = line
            .split_once('=')
            .ok_or_else(|| format!("invalid line in {}: {line}", path.display()))?;
        values.insert(key.to_owned(), value.to_owned());
    }
    Ok(values)
}

pub(crate) fn lock_value<'a>(lock: &'a BTreeMap<String, String>, key: &str) -> Result<&'a str> {
    lock.get(key)
        .map(String::as_str)
        .ok_or_else(|| format!("missing {key} in lock file").into())
}

pub(crate) fn verify_sha256(path: &Path, expected: &str) -> Result<()> {
    let output = command_output(Command::new("shasum").args(["-a", "256"]).arg(path))?;
    let actual = output.split_whitespace().next().unwrap_or_default();
    if actual != expected {
        return Err(format!(
            "SHA-256 mismatch for {}: expected {expected}, found {actual}",
            path.display()
        )
        .into());
    }
    Ok(())
}
