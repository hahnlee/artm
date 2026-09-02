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
    // javac and d8 do not remove outputs for source types that were deleted or
    // renamed. Reusing these directories silently kept obsolete compatibility
    // classes in the product support DEX, so every build starts from an empty
    // generated-output boundary.
    if class_dir.exists() {
        fs::remove_dir_all(&class_dir)?;
    }
    if dex_dir.exists() {
        fs::remove_dir_all(&dex_dir)?;
    }
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;

    let javac_classpath =
        env::join_paths([&android_platform_jar, &android_mock_jar, &baseline_classes])?;
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
            .arg(root.join("tools/android-apk-app-runtime/fixture/DarwinServiceBridge.java"))
            .arg(root.join("tools/android-apk-app-runtime/fixture/DarwinSystemServer.java"))
            .arg(root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGL.java"))
            .arg(root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGL10.java"))
            .arg(root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGLConfig.java"))
            .arg(
                root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGLContext.java"),
            )
            .arg(
                root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGLDisplay.java"),
            )
            .arg(
                root.join("probes/apk_support/java/javax/microedition/khronos/egl/EGLSurface.java"),
            ),
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
            .arg(baseline("android/test/mock/MockContext.class"))
            .arg(baseline("android/content/pm/ProbeShortcutManager.class"))
            .arg(baseline("android/os/ProbeUserManager.class"))
            .arg(baseline("dev/darwinart/probe/Hello.class"))
            .arg(baseline("dev/darwinart/probe/ProbeCanvas.class"))
            .arg(baseline("android/media/ProbeAudioManager.class"))
            .arg(baseline("dev/darwinart/probe/ProbeCalendarProvider.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContentResolver.class"))
            .arg(baseline(
                "dev/darwinart/probe/ProbeHostDocumentProvider.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeHostDocumentProvider$Document.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeMediaStoreProvider.class",
            ))
            .arg(baseline("dev/darwinart/probe/ProbeContentRoot.class"))
            .arg(baseline("dev/darwinart/probe/ProbeContext.class"))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$BaseContext.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$LocalServiceRecord.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$BoundServiceRecord.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$RemoteServiceBinder.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$CompatibilityHandler.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$DefaultServiceHandler.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$ThermalServiceHandler.class",
            ))
            .arg(baseline(
                "dev/darwinart/probe/ProbeContext$MainExecutor.class",
            ))
            .arg(baseline("dev/darwinart/probe/ProbeSharedPreferences.class"))
            .arg(baseline(
                "dev/darwinart/probe/ProbeSharedPreferences$EditorImpl.class",
            ))
            .arg(baseline("dev/darwinart/probe/ProbePackageManager.class"))
            .arg(baseline("dev/darwinart/probe/ProbeResources.class"))
            .arg(baseline("dev/darwinart/probe/ProbeXmlResourceParser.class"))
            .arg(button("dev/darwinart/probe/FontBootstrap.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost.class"))
            .arg(button("dev/darwinart/probe/ProbeAnimationHost$1.class"))
            .arg(button("dev/darwinart/probe/ProbeActivity.class"))
            .arg(button("dev/darwinart/probe/ProbeView.class"))
            .arg(button("dev/darwinart/simple/DarwinServiceBridge.class"))
            .arg(button("dev/darwinart/system/DarwinSystemServer.class"))
            .arg(button(
                "dev/darwinart/system/DarwinSystemServer$PackageRegistryBinder.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$MediaSessionInterfaceHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$UserManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$WindowManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$WindowManagerHandler$WindowLayoutState.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$DisplayHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ActivityTaskHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ActivityClientHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ActivityRecord.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$ActivityManagerHandler.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$AudioServiceBinder.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$DevicePolicyServiceBinder.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$HostSurfaceState.class",
            ))
            .arg(button(
                "dev/darwinart/simple/DarwinServiceBridge$IntentSenderHandler.class",
            ))
            .arg(button("javax/microedition/khronos/egl/EGL.class"))
            .arg(button("javax/microedition/khronos/egl/EGL10.class"))
            .arg(button("javax/microedition/khronos/egl/EGLConfig.class"))
            .arg(button("javax/microedition/khronos/egl/EGLContext.class"))
            .arg(button("javax/microedition/khronos/egl/EGLDisplay.class"))
            .arg(button("javax/microedition/khronos/egl/EGLSurface.class"))
            .arg(button("javax/microedition/khronos/egl/DarwinEGL10.class")),
    )?;

    let classes_dex = dex_dir.join("classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    let _historical_manifest = "AOSP DEX: verified=yes version=35 classes=77 methods=1075 \
                    class[0]=Landroid/content/pm/ProbeShortcutManager; \
                    class[1]=Landroid/media/ProbeAudioManager; \
                    class[2]=Landroid/os/ProbeUserManager; \
                    class[3]=Landroid/test/mock/MockPackageManager; \
                    class[4]=Ldev/darwinart/probe/FontBootstrap; \
                    class[5]=Ldev/darwinart/probe/Hello; \
                    class[6]=Ldev/darwinart/probe/ProbeActivity; \
                    class[7]=Ldev/darwinart/probe/ProbeAnimationHost$$ExternalSyntheticLambda0; \
                    class[8]=Ldev/darwinart/probe/ProbeAnimationHost$1; \
                    class[9]=Ldev/darwinart/probe/ProbeAnimationHost; \
                    class[10]=Ldev/darwinart/probe/ProbeCalendarProvider$$ExternalSyntheticBackport0; \
                    class[11]=Ldev/darwinart/probe/ProbeCalendarProvider$$ExternalSyntheticBackport1; \
                    class[12]=Ldev/darwinart/probe/ProbeCalendarProvider; \
                    class[13]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[14]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[15]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[16]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[17]=Ldev/darwinart/probe/ProbeContext$$ExternalSyntheticLambda0; \
                    class[18]=Ldev/darwinart/probe/ProbeContext$$ExternalSyntheticLambda1; \
                    class[19]=Ldev/darwinart/probe/ProbeContext$CompatibilityHandler; \
                    class[20]=Ldev/darwinart/probe/ProbeContext$DefaultServiceHandler; \
                    class[21]=Ldev/darwinart/probe/ProbeContext$LocalServiceRecord; \
                    class[22]=Ldev/darwinart/probe/ProbeContext$MainExecutor; \
                    class[23]=Ldev/darwinart/probe/ProbeContext$ThermalServiceHandler; \
                    class[24]=Ldev/darwinart/probe/ProbeContext; \
                    class[25]=Ldev/darwinart/probe/ProbeHostDocumentProvider$Document; \
                    class[26]=Ldev/darwinart/probe/ProbeHostDocumentProvider; \
                    class[27]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[28]=Ldev/darwinart/probe/ProbeResources; \
                    class[29]=Ldev/darwinart/probe/ProbeSharedPreferences$EditorImpl; \
                    class[30]=Ldev/darwinart/probe/ProbeSharedPreferences; \
                    class[31]=Ldev/darwinart/probe/ProbeView; \
                    class[32]=Ldev/darwinart/probe/ProbeXmlResourceParser; \
                    class[33]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda0; \
                    class[34]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda10; \
                    class[35]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda11; \
                    class[36]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda12; \
                    class[37]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda13; \
                    class[38]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda14; \
                    class[39]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda15; \
                    class[40]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda1; \
                    class[41]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda2; \
                    class[42]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda3; \
                    class[43]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda4; \
                    class[44]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda5; \
                    class[45]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda6; \
                    class[46]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda7; \
                    class[47]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda8; \
                    class[48]=Ldev/darwinart/simple/DarwinServiceBridge$$ExternalSyntheticLambda9; \
                    class[49]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityClientHandler$$ExternalSyntheticLambda0; \
                    class[50]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityClientHandler; \
                    class[51]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityManagerHandler; \
                    class[52]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityRecord; \
                    class[53]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityTaskHandler$$ExternalSyntheticLambda0; \
                    class[54]=Ldev/darwinart/simple/DarwinServiceBridge$ActivityTaskHandler; \
                    class[55]=Ldev/darwinart/simple/DarwinServiceBridge$AudioServiceBinder; \
                    class[56]=Ldev/darwinart/simple/DarwinServiceBridge$DevicePolicyServiceBinder; \
                    class[57]=Ldev/darwinart/simple/DarwinServiceBridge$DisplayHandler; \
                    class[58]=Ldev/darwinart/simple/DarwinServiceBridge$HostSurfaceState; \
                    class[59]=Ldev/darwinart/simple/DarwinServiceBridge$IntentSenderHandler$$ExternalSyntheticLambda0; \
                    class[60]=Ldev/darwinart/simple/DarwinServiceBridge$IntentSenderHandler; \
                    class[61]=Ldev/darwinart/simple/DarwinServiceBridge$ManagerHandler; \
                    class[62]=Ldev/darwinart/simple/DarwinServiceBridge$MediaSessionInterfaceHandler; \
                    class[63]=Ldev/darwinart/simple/DarwinServiceBridge$UserManagerHandler; \
                    class[64]=Ldev/darwinart/simple/DarwinServiceBridge$WindowManagerHandler$$ExternalSyntheticLambda0; \
                    class[65]=Ldev/darwinart/simple/DarwinServiceBridge$WindowManagerHandler; \
                    class[66]=Ldev/darwinart/simple/DarwinServiceBridge; \
                    class[67]=Ljavax/microedition/khronos/egl/EGL; \
                    class[68]=Ljavax/microedition/khronos/egl/EGL10; \
                    class[69]=Ljavax/microedition/khronos/egl/DarwinEGL10; \
                    class[70]=Ljavax/microedition/khronos/egl/EGLConfig; \
                    class[71]=Ljavax/microedition/khronos/egl/EGLContext; \
                    class[72]=Ljavax/microedition/khronos/egl/EGLDisplay; \
                    class[73]=Ljavax/microedition/khronos/egl/EGLSurface;";
    verify_dex_contract(
        &output,
        89,
        1350,
        &[
            "Ldev/darwinart/probe/ProbeActivity;",
            "Ldev/darwinart/probe/ProbeContext$BaseContext;",
            "Ldev/darwinart/probe/ProbeContext$RemoteServiceBinder;",
            "Ldev/darwinart/simple/DarwinServiceBridge;",
            "Ldev/darwinart/system/DarwinSystemServer;",
            "Ljavax/microedition/khronos/egl/DarwinEGL10;",
        ],
    )?;

    println!("build-button-dex: {}", output.trim());
    Ok(())
}
