use super::*;
use darwin_art_build_contract::RuntimeFlavor;

pub(crate) fn finalize(
    staged: &RuntimeBootstrapStaging,
    real_graphics: bool,
    compiled: RuntimeBootstrapCompiled,
) -> Result<()> {
    let flavor = if real_graphics {
        RuntimeFlavor::Graphics
    } else {
        RuntimeFlavor::Headless
    };
    let archive = staged.build_dir.join(flavor.archive_name());
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
