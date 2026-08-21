use super::*;

pub(crate) fn finalize(
    staged: &RuntimeBootstrapStaging,
    real_graphics: bool,
    compiled: RuntimeBootstrapCompiled,
) -> Result<()> {
    let archive = staged.build_dir.join(if real_graphics {
        "libart-runtime-graphics-bootstrap-darwin.a"
    } else {
        "libart-runtime-bootstrap-darwin.a"
    });
    create_archive(&archive, &compiled.objects)?;
    println!(
        "{}: ART runtime initialization spine Mach-O objects={} compiled={} cached={} archive={}",
        if real_graphics {
            "build-runtime-graphics-bootstrap"
        } else {
            "build-runtime-bootstrap"
        },
        compiled.objects.len(),
        compiled.compiled_objects,
        compiled.cached_objects,
        archive.display()
    );
    Ok(())
}
