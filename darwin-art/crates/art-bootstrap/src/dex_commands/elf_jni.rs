use super::*;

pub(crate) fn build_elf_jni_dex_probe(root: &Path) -> Result<()> {
    let baseline_dex = root.join("_build/dex-probe/dex/classes.dex");
    let dex_probe = root.join("_build/dex-probe/dex-probe");
    for input in [&baseline_dex, &dex_probe] {
        if !input.is_file() {
            return Err(format!(
                "ELF JNI DEX baseline is missing: {}; run `build-dex` first",
                input.display()
            )
            .into());
        }
    }
    let build_dir = root.join("_build/elf-jni-dex");
    let class_dir = build_dir.join("classes");
    let dex_dir = build_dir.join("dex");
    fs::create_dir_all(&class_dir)?;
    fs::create_dir_all(&dex_dir)?;
    run_command(
        Command::new("javac")
            .args(["--release", "8", "-encoding", "UTF-8", "-d"])
            .arg(&class_dir)
            .arg(root.join("probes/android-elf-jni-fixture/NativeFixture.java")),
    )?;
    let classes_dex = dex_dir.join("classes.dex");
    if classes_dex.is_file() {
        fs::remove_file(&classes_dex)?;
    }
    run_command(
        Command::new(find_d8()?)
            .arg("--lib")
            .arg(find_android_platform_jar()?)
            .arg("--output")
            .arg(&dex_dir)
            .arg(&baseline_dex)
            .arg(class_dir.join("darwin/art/nativefixture/NativeFixture.class")),
    )?;
    let output = command_output(Command::new(&dex_probe).arg(&classes_dex))?;
    let expected = "AOSP DEX: verified=yes version=35 classes=13 methods=328 \
                    class[0]=Landroid/test/mock/MockPackageManager; \
                    class[1]=Ldarwin/art/nativefixture/NativeFixture; \
                    class[2]=Ldev/darwinart/probe/Hello; \
                    class[3]=Ldev/darwinart/probe/ProbeActivity; \
                    class[4]=Ldev/darwinart/probe/ProbeCanvas; \
                    class[5]=Ldev/darwinart/probe/ProbeContentResolver$$ExternalSyntheticLambda0; \
                    class[6]=Ldev/darwinart/probe/ProbeContentResolver; \
                    class[7]=Ldev/darwinart/probe/ProbeContentRoot; \
                    class[8]=Ldev/darwinart/probe/ProbeContext; \
                    class[9]=Ldev/darwinart/probe/ProbePackageManager; \
                    class[10]=Ldev/darwinart/probe/ProbeResources; \
                    class[11]=Ldev/darwinart/probe/ProbeView; \
                    class[12]=Ldev/darwinart/probe/ProbeXmlResourceParser;";
    if output.trim() != expected {
        return Err(format!("unexpected ELF JNI DEX probe output: {output:?}").into());
    }
    println!("build-elf-jni-dex: {}", output.trim());
    Ok(())
}
