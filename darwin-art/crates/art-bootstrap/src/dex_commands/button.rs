use super::*;

pub(crate) fn build_button_dex_probe(root: &Path) -> Result<()> {
    // Rebuild the baseline first: the Button flavor intentionally reuses the
    // launcher/context/resource test classes, but replaces only Activity/View
    // and adds the real SystemFonts bootstrap. Keeping a separate DEX prevents
    // widget dependencies from weakening the small baseline regression gate.
    build_dex_probe(root)?;

    let android_platform_jar = find_android_platform_jar()?;
    let android_mock_jar = android_platform_jar
        .parent()
        .ok_or("Android platform jar has no parent")?
        .join("optional/android.test.mock.jar");
    if !android_mock_jar.is_file() {
        return Err(format!(
            "Android mock library is missing: {}",
            android_mock_jar.display()
        )
        .into());
    }

    let baseline_classes = root.join("_build/dex-probe/classes");
    let build_dir = root.join("_build/button-dex");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;

    let javac_classpath = env::join_paths([&android_platform_jar, &android_mock_jar])?;
    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg("-classpath")
            .arg(&javac_classpath)
            .arg(root.join("probes/button/FontBootstrap.java"))
            .arg(root.join("probes/button/ProbeAnimationHost.java"))
            .arg(root.join("probes/button/ProbeActivity.java"))
            .arg(root.join("probes/button/ProbeView.java"))
            .arg(root.join("tools/android-apk-app-runtime/fixture/DarwinServiceBridge.java")),
    )?;

    let baseline = |relative: &str| baseline_classes.join(relative);
    let button = |relative: &str| class_dir.join(relative);
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(&android_platform_jar)
            .arg("--classpath")
            .arg(&baseline_classes)
            .arg("--classpath")
            .arg(&android_mock_jar)
            .arg("--output")
            .arg(&dex_dir)
            .arg(baseline("android/test/mock/MockPackageManager.class"))
            .arg(baseline("dev/darwinart/probe/Hello.class"))
            .arg(baseline("dev/darwinart/probe/ProbeCanvas.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContentResolver.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContentRoot.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContext.class"))
            .arg(baseline("dev/darwinart/probe/ProbePackageManager.class"))
            .arg(baseline("dev/darwinart/probe/ProbeResources.class"))
            .arg(baseline("dev/darwinart/probe/ProbeXmlResourceParser.class"))
            .arg(button("dev/darwinart/probe/FontBootstrap.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost$1.class"))
            .arg(button("dev/darwinart/probe/ProbeActivity.class"))
            .arg(button("dev/darwinart/probe/ProbeView.class"))
            .arg(button("dev/darwinart/simple/DarwinServiceBridge.class"))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$DisplayHandler.class",
            )),
    )?;

    let classes_dex = dex_dir.join("classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    let expected = "AOSP DEX: verified=yes version=35 classes=18 methods=370 \
                    class[0]=Landroid/test/mock/MockPackageManager; \
                    class[1]=Ldev/darwinart/probe/FontBootstrap; \
                    class[2]=Ldev/darwinart/probe/Hello; \
                    class[3]=Ldev/darwinart/probe/ProbeActivity; \
                    class[4]=Ldev/darwinart/probe/ProbeAnimationHost$1; \
                    class[5]=Ldev/darwinart/probe/ProbeAnimationHost; \
                    class[6]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[7]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[8]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[9]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[10]=Ldev/darwinart/probe/ProbeContext; \
                    class[11]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[12]=Ldev/darwinart/probe/ProbeResources; \
                    class[13]=Ldev/darwinart/probe/ProbeView; \
                    class[14]=Ldev/darwinart/probe/ProbeXmlResourceParser; \
                    class[15]=Ldev/darwinart/simple/DarwinServiceBridge$DisplayHandler; \
                    class[16]=Ldev/darwinart/simple/DarwinServiceBridge$ManagerHandler; \
                    class[17]=Ldev/darwinart/simple/DarwinServiceBridge;";
    if output.trim() != expected {
        return Err(format!("unexpected Button DEX probe output: {output:?}").into());
    }

    println!("build-button-dex: {}", output.trim());
    Ok(())
}
