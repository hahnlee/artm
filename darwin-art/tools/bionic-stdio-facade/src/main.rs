use bionic_stdio_facade::Provider;
use darwin_art_elf_loader::{
    LoadedElf, ResolveError, ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::env;
use std::ffi::c_char;
use std::fs;
use std::num::NonZeroUsize;
use std::process::ExitCode;
use std::sync::Arc;
unsafe extern "C" {
    fn __error() -> *mut i32;
    fn darwin_art_bionic_stdio_resolve(n: *const c_char) -> Option<unsafe extern "C" fn()>;
    fn darwin_art_bionic_errno_resolve(n: *const c_char) -> Option<unsafe extern "C" fn()>;
    static mut darwin_art_bionic___sF: [u8; 456];
}
struct Resolver;
impl SymbolResolver for Resolver {
    fn resolve(&mut self, r: SymbolRequest<'_>) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if r.version.is_some() {
            return Err(ResolveError::Rejected("stdio imports unversioned".into()));
        }
        let address = if r.symbol == "__sF" {
            Some(std::ptr::addr_of_mut!(darwin_art_bionic___sF) as usize)
        } else {
            let n = match r.symbol {
                "__errno" => c"__errno",
                "fclose" => c"fclose",
                "fflush" => c"fflush",
                "fileno" => c"fileno",
                "fopen" => c"fopen",
                "fputc" => c"fputc",
                "fread" => c"fread",
                "fseek" => c"fseek",
                "fseeko" => c"fseeko",
                "ftello" => c"ftello",
                "fwrite" => c"fwrite",
                "getc" => c"getc",
                "ungetc" => c"ungetc",
                _ => return Ok(None),
            };
            unsafe {
                darwin_art_bionic_stdio_resolve(n.as_ptr())
                    .or_else(|| darwin_art_bionic_errno_resolve(n.as_ptr()))
            }
            .map(|f| f as usize)
        };
        Ok(address.map(|a| unsafe { ResolvedSymbol::new(NonZeroUsize::new(a).unwrap()) }))
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let fixture = env::args_os().nth(1).ok_or("fixture")?;
    let provider = Arc::new(Provider::new(
        vec![(b"/system/input.bin".to_vec(), b"abcdef".to_vec())],
        b"input".to_vec(),
    )?);
    let _activation = provider.activate()?;
    for rejected in [c"fprintf", c"vfprintf", c"fputwc", c"getwc", c"ungetwc"] {
        // SAFETY: resolver reads only the static NUL-terminated name.
        if unsafe { darwin_art_bionic_stdio_resolve(rejected.as_ptr()) }.is_some() {
            return Err("formatted/wide stdio escaped resolver rejection".into());
        }
    }
    let bytes = fs::read(fixture)?;
    let mut resolver = Resolver;
    let mut image = LoadedElf::load_with_resolver(&bytes, &mut resolver)?;
    image.run_initializers()?;
    // SAFETY: host errno is audit-only and never guest-resolved.
    unsafe { *__error() = 33_301 };
    if image.call_exported_i32("bionic_stdio_fixture_basic")? != 42 {
        return Err("basic fixture".into());
    }
    // SAFETY: same current pthread errno cell.
    if unsafe { *__error() } != 33_301 {
        return Err("stdio facade changed host errno".into());
    }
    if provider.stdout_bytes() != b"!" {
        return Err("stdout capture".into());
    }
    if image.call_exported_i32("bionic_stdio_fixture_race_setup")? != 42 {
        return Err("race setup".into());
    }
    let w = image.lookup_exported("bionic_stdio_fixture_race_write")?;
    let c = image.lookup_exported("bionic_stdio_fixture_race_close")?;
    let writer: unsafe extern "C" fn() -> i32 = unsafe { std::mem::transmute(w) };
    let close: unsafe extern "C" fn() -> i32 = unsafe { std::mem::transmute(c) };
    std::thread::scope(|s| {
        let mut h = Vec::new();
        for _ in 0..7 {
            h.push(s.spawn(move || {
                // SAFETY: Darwin host errno is pthread-local and audit-only.
                unsafe { *__error() = 33_350 };
                for _ in 0..1000 {
                    if unsafe { writer() } != 42 {
                        return false;
                    }
                }
                // SAFETY: same worker pthread errno cell.
                unsafe { *__error() == 33_350 }
            }))
        }
        h.push(s.spawn(move || unsafe { close() } == 42));
        for t in h {
            assert!(t.join().unwrap())
        }
    });
    if image.call_exported_i32("bionic_stdio_fixture_race_after")? != 42 {
        return Err("race lifetime".into());
    }
    println!("bionic-stdio-facade: PASS binary FILE tokens __sF concurrent-close");
    Ok(())
}
fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("bionic-stdio-facade: {e}");
            ExitCode::from(2)
        }
    }
}
