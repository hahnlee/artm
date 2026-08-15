use bionic_fs_facade::Facade;
use darwin_art_elf_loader::{
    LoadedElf, ResolveError, ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::env;
use std::ffi::{c_char, c_void};
use std::fs::{self, File};
use std::num::NonZeroUsize;
use std::process::ExitCode;
use std::sync::Arc;

unsafe extern "C" {
    fn __error() -> *mut i32;
    fn darwin_art_bionic_fs_resolve(name: *const c_char) -> Option<unsafe extern "C" fn()>;
    fn darwin_art_bionic_errno_resolve(name: *const c_char) -> Option<unsafe extern "C" fn()>;
}

struct ClosedResolver;

impl ClosedResolver {
    fn resolved(address: unsafe extern "C" fn()) -> ResolvedSymbol {
        let address = NonZeroUsize::new(address as usize).expect("function address is non-zero");
        // SAFETY: each provider is manifest-checked against its Android arm64 import ABI
        // and remains linked for the loaded image lifetime.
        unsafe { ResolvedSymbol::new(address) }
    }

    fn lookup(name: &str) -> Option<unsafe extern "C" fn()> {
        let bytes = match name {
            "__errno" => c"__errno",
            "close" => c"close",
            "fstat" => c"fstat",
            "open" => c"open",
            "openat" => c"openat",
            "read" => c"read",
            _ => return None,
        };
        // SAFETY: both closed C resolvers read only the static NUL-terminated name.
        unsafe {
            darwin_art_bionic_fs_resolve(bytes.as_ptr())
                .or_else(|| darwin_art_bionic_errno_resolve(bytes.as_ptr()))
        }
    }
}

impl SymbolResolver for ClosedResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if request.version.is_some() {
            return Err(ResolveError::Rejected(
                "filesystem fixture imports must be unversioned".to_owned(),
            ));
        }
        Ok(Self::lookup(request.symbol).map(Self::resolved))
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args_os();
    let _program = arguments.next();
    let fixture = arguments
        .next()
        .ok_or("missing Android filesystem fixture")?;
    let root = arguments.next().ok_or("missing mount root directory")?;
    if arguments.next().is_some() {
        return Err("usage: bionic-fs-facade <android-arm64-fixture.so> <mount-root>".into());
    }
    let metadata = fs::metadata(&root)?;
    if !metadata.is_dir() {
        return Err("mount root is not a directory".into());
    }
    let facade = Arc::new(Facade::new(File::open(root)?, b"/system", b"/system")?);
    let _activation = facade.activate();
    let bytes = fs::read(fixture)?;
    let mut resolver = ClosedResolver;
    let mut image = LoadedElf::load_with_resolver(&bytes, &mut resolver)?;
    image.run_initializers()?;
    // SAFETY: Darwin __error returns this host pthread's errno cell. It is never
    // exposed to the guest resolver and is used only to audit shim isolation.
    unsafe { *__error() = 33_001 };
    let result = image.call_exported_i32("bionic_fs_fixture_run")?;
    if result != 42 {
        return Err(format!("Android filesystem fixture failed at step {result}").into());
    }
    if facade.has_capability_failure() {
        return Err("facade encountered an untranslatable host capability failure".into());
    }
    // SAFETY: same host-only audit cell set immediately before guest execution.
    if unsafe { *__error() } != 33_001 {
        return Err("filesystem facade changed host errno".into());
    }
    println!(
        "bionic-fs-facade: PASS Android ELF open/openat/read/fstat/close mount-root broker errno"
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("bionic-fs-facade: {error}");
            ExitCode::from(2)
        }
    }
}

#[allow(dead_code)]
fn _function_pointer_is_data_pointer_sized() {
    const _: () = assert!(size_of::<unsafe extern "C" fn()>() == size_of::<*mut c_void>());
}
