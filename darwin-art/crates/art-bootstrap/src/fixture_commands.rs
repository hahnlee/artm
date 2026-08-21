use super::*;

pub(crate) fn probe_runtime_dex_flavor(
    root: &Path,
    show_window: bool,
    real_graphics: bool,
    button: bool,
    elf_jni: bool,
    network: bool,
    apk_app: bool,
) -> Result<()> {
    let core_icu4j = if real_graphics {
        prepare_icu_runtime_bootclasspath(root)?
    } else {
        prepare_icu_bootclasspath(root)?;
        root.join("_build/bootclasspath/core-icu4j.jar")
    };
    let executable = root.join("target/debug/darwin-art-host");
    let runtime_library = root.join(if real_graphics {
        "_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
    } else {
        "_build/runtime-link-probe/libdarwin_art_runtime.dylib"
    });
    let core_oj = root.join("_prebuilt/android-16/bootclasspath/core-oj.jar");
    let core_libart = root.join("_prebuilt/android-16/bootclasspath/core-libart.jar");
    let framework = root.join("_prebuilt/android-16/bootclasspath/framework.jar");
    let classes_dex = root.join(if apk_app {
        "_build/android-apk-app-runtime/simple-no-native.apk"
    } else if network {
        "_build/network-runtime-probe/dex/classes.dex"
    } else if elf_jni {
        "_build/elf-jni-dex/dex/classes.dex"
    } else if button {
        "_build/button-dex/dex/classes.dex"
    } else {
        "_build/dex-probe/dex/classes.dex"
    });
    for input in [
        &executable,
        &runtime_library,
        &core_oj,
        &core_libart,
        &framework,
        &core_icu4j,
        &classes_dex,
    ] {
        if !input.is_file() {
            return Err(format!(
                "runtime DEX probe input is missing: {}; run `art-bootstrap all` first",
                input.display()
            )
            .into());
        }
    }

    let mut command = Command::new(&executable);
    if show_window {
        // Keep normal window probes short; the capture script controls its
        // own interaction hold time through DARWIN_ART_TEST_POINTER_HOLD_MS.
        command.args(["--window-seconds", "3"]);
    }
    command
        .arg(&runtime_library)
        .arg(&core_oj)
        .arg(&core_libart)
        .arg(&framework)
        .arg(&core_icu4j)
        .arg(&classes_dex);
    let mut system_root = None;
    if real_graphics {
        let icu_runtime = root.join("_build/icu-runtime-adapters/runtime");
        let i18n_root = icu_runtime.join("i18n");
        let data_root = icu_runtime.join("data");
        let tzdata_root = icu_runtime.join("tzdata");
        let icu_data = i18n_root.join("etc/icu/icudt76l.dat");
        if !icu_data.is_file() {
            return Err(format!(
                "Android ICU76 runtime data is missing: {}; run `build-icu-runtime-adapters` first",
                icu_data.display()
            )
            .into());
        }
        command
            .env("ANDROID_I18N_ROOT", &i18n_root)
            .env("ANDROID_DATA", &data_root)
            .env("ANDROID_TZDATA_ROOT", &tzdata_root);
        if button || apk_app {
            let fonts_xml = root.join("probes/button/fonts.xml");
            let roboto = root.join("_aosp/external/skia/resources/fonts/Roboto-Regular.ttf");
            for input in [&fonts_xml, &roboto] {
                if !input.is_file() {
                    return Err(format!("Button font input is missing: {}", input.display()).into());
                }
            }
            command
                .env("DARWIN_ART_TEST_FONTS_XML", fonts_xml)
                .env("DARWIN_ART_TEST_FONT", roboto);
            if button {
                let framework_res = root.join("_prebuilt/android-16/resources/framework-res.apk");
                if !framework_res.is_file() {
                    return Err(format!(
                        "Android framework resources are missing: {}",
                        framework_res.display()
                    )
                    .into());
                }
                command.env("DARWIN_ART_FRAMEWORK_RES_APK", framework_res);
            }
            let guest_root = prepare_probe_android_system_root(root)?;
            command.env("DARWIN_ART_ANDROID_SYSTEM_ROOT", &guest_root);
            system_root = Some(guest_root);
        }
    }
    if apk_app {
        let framework_res = root.join("_prebuilt/android-16/resources/framework-res.apk");
        if !framework_res.is_file() {
            return Err(format!(
                "Android framework resources are missing: {}",
                framework_res.display()
            )
            .into());
        }
        command
            .env("DARWIN_ART_APK_APP_PACKAGE", "dev.darwinart.simple")
            .env(
                "DARWIN_ART_APK_APP_ACTIVITY",
                "dev.darwinart.simple.MainActivity",
            )
            .env(
                "DARWIN_ART_APK_APP_DESCRIPTOR",
                "Ldev/darwinart/simple/MainActivity;",
            )
            .env(
                "DARWIN_ART_APK_APP_SUPPORT_DEX",
                root.join("_build/dex-probe/dex/classes.dex"),
            )
            .env("DARWIN_ART_FRAMEWORK_RES_APK", framework_res)
            .env("DARWIN_ART_APK_APP_EXPECT_WIDGETS", "1");
        if show_window {
            command.env("DARWIN_ART_WINDOW_SCALE", "2");
        } else {
            command.env("DARWIN_ART_TEST_POINTER_CLICK", "35,45");
        }
    }
    let mut network_fixture = None;
    if network {
        let fixture = prepare_private_network_fixture(root)?;
        command.env("DARWIN_ART_ANDROID_NETWORK_FIXTURE", &fixture.library);
        network_fixture = Some(fixture);
    }
    let mut apk_native_fixture = None;
    if elf_jni {
        build_shell_gate(root, "build-android-elf-jni-fixture.sh")?;
        build_shell_gate(root, "build-android35-libcxx-runtime-fixtures.sh")?;
        build_shell_gate(root, "build-android-elf-tls-runtime-fixture.sh")?;
        let fixture = root.join("_build/android-elf-jni-fixture/libdarwin-art-jni-fixture.so");
        let generic_fixture =
            root.join("_build/android-elf-jni-fixture/libdarwin-art-generic-root.so");
        let libcxx_collections = root.join(
            "_build/android35-libcxx-runtime-fixtures/collections/libdarwin_art_libcxx_consumer.so",
        );
        let libcxx_exception = root.join(
            "_build/android35-libcxx-runtime-fixtures/exception/libdarwin_art_libcxx_exception.so",
        );
        let tls_fixture =
            root.join("_build/android-elf-tls-runtime-fixture/libdarwin_art_tls_runtime.so");
        if !fixture.is_file()
            || !generic_fixture.is_file()
            || !libcxx_collections.is_file()
            || !libcxx_exception.is_file()
            || !tls_fixture.is_file()
        {
            return Err(format!(
                "Android ELF fixture is missing: {}, {}, {}, {}, or {}",
                fixture.display(),
                generic_fixture.display(),
                libcxx_collections.display(),
                libcxx_exception.display(),
                tls_fixture.display()
            )
            .into());
        }
        let extracted = prepare_private_apk_native_fixture(root)?;
        let extracted_root = extracted
            .extracted_root
            .join("libdarwin-art-generic-root.so");
        let extracted_jni = extracted
            .extracted_root
            .join("libdarwin-art-jni-fixture.so");
        command
            .env("DARWIN_ART_ANDROID_ELF_JNI_FIXTURE", extracted_jni)
            .env("DARWIN_ART_ANDROID_ELF_GENERIC_FIXTURE", &extracted_root)
            .env("DARWIN_ART_ANDROID_APK_ELF_FIXTURE", extracted_root)
            .env("DARWIN_ART_ANDROID_APK_SHA256", &extracted.apk_sha256)
            .env("DARWIN_ART_ANDROID_APK_ROOT_SHA256", &extracted.root_sha256)
            .env(
                "DARWIN_ART_ANDROID_LIBCXX_COLLECTIONS_FIXTURE",
                libcxx_collections,
            )
            .env(
                "DARWIN_ART_ANDROID_LIBCXX_EXCEPTION_FIXTURE",
                libcxx_exception,
            )
            .env("DARWIN_ART_ANDROID_TLS_FIXTURE", tls_fixture);
        apk_native_fixture = Some(extracted);
    }
    let output_result = command_output(&mut command);
    if let Some(guest_root) = system_root {
        let _ = fs::remove_dir_all(guest_root);
    }
    let output = output_result?;
    let expected = if apk_app {
        let render_scale = if show_window { 2 } else { 1 };
        let frame_width = 360 * render_scale;
        let frame_height = 640 * render_scale;
        let pixel_count = frame_width * frame_height;
        format!(
            "Hello from Darwin ART main: 안녕\n\
         ART Darwin Runtime::Create: ok\n\
         ART Darwin app ClassLoader: PathClassLoader\n\
         ART Darwin DEX interpreter: Hello.answer()=42\n\
         ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
         ART runtime native: System.arraycopy()=42\n\
         ART Android APK: package=dev.darwinart.simple launcher=dev.darwinart.simple.MainActivity classes.dex=APK native=0 pixels={pixel_count}/opaque widgets=TextView+CheckBox+RadioButton+ToggleButton+SeekBar+ProgressBar+Button colors>=8\n\
         ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
         ART Android view: Activity.setContentView()->DecorView.draw(Canvas)={frame_width}x{frame_height}\n\
         ART Android lifecycle: Activity.onCreate()=43\n\
         ART Darwin launcher: main(String[])=ok"
        )
    } else if network {
        "Hello from Darwin ART main: 안녕\n\
         ART Android network: JavaVMExt+JNI_OnLoad loopback-HTTP=42 socket+DNS=closed Internet=no\n\
         ART Darwin Runtime::Create: ok\n\
         ART Darwin app ClassLoader: PathClassLoader\n\
         ART Darwin DEX interpreter: Hello.answer()=42\n\
         ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
         ART runtime native: System.arraycopy()=42\n\
         ART Android framework: ProbeActivity().probeValue()=42\n\
         ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
         ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=360x640\n\
         ART Android lifecycle: Activity.onCreate()=43\n\
         ART Darwin launcher: main(String[])=ok"
            .to_owned()
    } else if elf_jni {
        let extracted = apk_native_fixture
            .as_ref()
            .expect("ELF JNI probe retains its APK extraction");
        format!(
            "Hello from Darwin ART main: 안녕\n\
         ART Android libc++: real-r28c collections=189 exception-cleanup=73 unload=sequential\n\
         ART Android ELF TLS: local-TLSDESC threads=4 align=64 unload=quiescent\n\
         ART Android ELF JNI: graph=child-first+relocated providers=bind_builtins+__errno+strlen+fs-random-ctor+scanf+swprintf+ioctl+strftime+sendfile load+JNI_OnLoad+RegisterNatives=generic+fixture scalar-ref=all nativeUsesEnv=current stack-repack=ok\n\
         ART Android APK ELF: apk-sha256={} root-sha256={} graph=root+child+grandchild load=JavaVMExt+NativeBridge unload=shutdown-trampolines-zero\n\
         ART Darwin Runtime::Create: ok\n\
         ART Darwin app ClassLoader: PathClassLoader\n\
         ART Darwin DEX interpreter: Hello.answer()=42\n\
         ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
         ART runtime native: System.arraycopy()=42\n\
         ART Android framework: ProbeActivity().probeValue()=42\n\
         ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
         ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=360x640\n\
         ART Android lifecycle: Activity.onCreate()=43\n\
         ART Darwin launcher: main(String[])=ok",
            extracted.apk_sha256, extracted.root_sha256
        )
    } else {
        "Hello from Darwin ART main: 안녕\n\
                    ART Darwin Runtime::Create: ok\n\
                    ART Darwin app ClassLoader: PathClassLoader\n\
                    ART Darwin DEX interpreter: Hello.answer()=42\n\
                    ART Darwin JNI: hostPageSize()=16384 nativeRoundTrip()=42\n\
                    ART runtime native: System.arraycopy()=42\n\
                    ART Android framework: ProbeActivity().probeValue()=42\n\
                    ART Android window: Activity.attach()=PhoneWindow+DecorView\n\
                    ART Android view: Activity.setContentView()->DecorView.draw(Canvas)=360x640\n\
                    ART Android lifecycle: Activity.onCreate()=43\n\
                    ART Darwin launcher: main(String[])=ok"
            .to_owned()
    };
    // The headless ELF acceptance intentionally has no GraphicsSession and
    // therefore performs managed Activity/DecorView validation without a
    // presented frame.  Keep that distinction explicit in the golden output;
    // real graphics/APK modes still require their concrete dimensions.
    let expected = if !real_graphics && !show_window {
        expected.replace(
            "DecorView.draw(Canvas)=360x640",
            "DecorView.draw(Canvas)=0x0",
        )
    } else {
        expected
    };
    if output.trim() != expected {
        return Err(format!("unexpected runtime DEX probe output: {output:?}").into());
    }
    drop(network_fixture);
    if apk_app {
        if show_window {
            println!("probe-runtime-apk-app-window: APK -> Activity.onCreate -> NSWindow");
        } else {
            println!(
                "probe-runtime-apk-app: no-native APK -> manifest Activity -> frame -> shutdown"
            );
        }
    } else if network {
        println!("probe-runtime-network: ART JNI loopback + socket/DNS quiescence PASS");
    } else if elf_jni {
        println!("probe-runtime-elf-jni: ART DT_NEEDED graph + JNI + reverse finalizers PASS");
    } else if button {
        if show_window {
            println!("probe-runtime-button-window: android.widget.Button -> AOSP HWUI -> NSWindow");
        } else {
            println!(
                "probe-runtime-button: android.widget.Button -> AOSP HWUI -> frame -> shutdown"
            );
        }
    } else if real_graphics {
        if show_window {
            println!(
                "probe-runtime-graphics-window: Bitmap-backed Canvas -> DecorView.draw() -> NSWindow"
            );
        } else {
            println!(
                "probe-runtime-graphics: Bitmap-backed Canvas -> DecorView.draw() -> frame -> shutdown"
            );
        }
    } else if show_window {
        println!("probe-window: PhoneWindow -> DecorView.draw(Canvas) -> NSWindow");
    } else {
        println!(
            "probe-runtime-dex: Activity.attach() + PhoneWindow + DecorView.draw(Canvas) -> Darwin"
        );
    }
    Ok(())
}

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
