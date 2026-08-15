use darwin_art_elf_loader::{
    Capability, LoadError, LoadedElf, ResolveError, ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::env;
use std::fs;
use std::num::NonZeroUsize;
use std::sync::Mutex;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const DT_NULL: i64 = 0;
const DT_NEEDED: i64 = 1;
const DT_STRTAB: i64 = 5;
const DT_FINI: i64 = 13;
const DT_FINI_ARRAYSZ: i64 = 28;
const DT_VERNEED: i64 = 0x6fff_fffe;
const PROVIDER_SONAME: &str = "libdarwin_art_provider.so";
const PROVIDER_VERSION: &str = "DARWIN_ART_1";
const LIFECYCLE_SONAME: &str = "liblifecycle_sink.so";

static PROVIDER_DATA: i32 = 11;
static FINALIZER_EVENTS: Mutex<Vec<i32>> = Mutex::new(Vec::new());

unsafe extern "C" fn provider_value() -> i32 {
    77
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments: Vec<_> = env::args_os().collect();
    if arguments.len() != 8 {
        return Err(
            "usage: elf-loader-gate POSITIVE.so IMPORT.so WEAK.so LAZY.so RELRO.so TLS.so FINALIZER.so".into(),
        );
    }
    let positive = fs::read(&arguments[1])?;
    let import = fs::read(&arguments[2])?;
    let weak = fs::read(&arguments[3])?;
    let lazy = fs::read(&arguments[4])?;
    let relro = fs::read(&arguments[5])?;
    let tls = fs::read(&arguments[6])?;
    let finalizer = fs::read(&arguments[7])?;

    run_positive(&positive)?;
    run_import(&import)?;
    run_weak(&weak)?;
    run_resolver_negative_matrix(&import)?;
    expect_capability(&lazy, |capability| {
        matches!(capability, Capability::LazyBinding)
    })?;
    expect_capability(&relro, |capability| matches!(capability, Capability::Relro))?;
    expect_capability(&tls, |capability| matches!(capability, Capability::Tls))?;
    run_malformed_matrix(&positive)?;
    run_finalizer_lifecycle(&finalizer)?;

    println!(
        "elf-loader-gate: positive=constructor-order import=ABS64+GLOB_DAT+JUMP_SLOT \
         resolver=closed+versioned weak=zero NOW=required RELRO=reject TLS=reject \
         wx=reject overflow=reject overlap=reject bounds=reject \
         finalizers=array-reverse+DT_FINI exactly-once cleanup=drop"
    );
    Ok(())
}

unsafe extern "C" fn lifecycle_record(value: i32) {
    FINALIZER_EVENTS
        .lock()
        .expect("finalizer recorder poisoned")
        .push(value);
}

struct LifecycleResolver;

impl SymbolResolver for LifecycleResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if request.needed_libraries != [LIFECYCLE_SONAME]
            || request.symbol != "lifecycle_record"
            || request.version.is_some()
        {
            return Err(ResolveError::Rejected(format!(
                "unexpected lifecycle request: {}",
                request.symbol
            )));
        }
        let address = NonZeroUsize::new(lifecycle_record as usize)
            .ok_or_else(|| ResolveError::Rejected("null lifecycle recorder".to_owned()))?;
        // SAFETY: the recorder has static lifetime and the exact Android AArch64 void(int) ABI.
        Ok(Some(unsafe { ResolvedSymbol::new(address) }))
    }
}

fn run_finalizer_lifecycle(bytes: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    FINALIZER_EVENTS.lock().unwrap().clear();
    {
        let mut resolver = LifecycleResolver;
        let image = LoadedElf::load_with_resolver(bytes, &mut resolver)?;
        drop(image);
    }
    if !FINALIZER_EVENTS.lock().unwrap().is_empty() {
        return Err("uninitialized image executed finalizers".into());
    }

    for explicit_close in [true, false] {
        let mut resolver = LifecycleResolver;
        let mut image = LoadedElf::load_with_resolver(bytes, &mut resolver)?;
        image.run_initializers()?;
        if image.call_exported_i32("finalizer_value")? != 42 {
            return Err("finalizer fixture export mismatch".into());
        }
        if explicit_close {
            image.close();
        } else {
            drop(image);
        }
    }
    if *FINALIZER_EVENTS.lock().unwrap() != [12, 11, 19, 12, 11, 19] {
        return Err(format!(
            "standalone finalizer order/exactly-once mismatch: {:?}",
            *FINALIZER_EVENTS.lock().unwrap()
        )
        .into());
    }

    let mut malformed_size = bytes.to_vec();
    let size_entry = dynamic_tag_entry(bytes, DT_FINI_ARRAYSZ)?;
    malformed_size[size_entry + 8..size_entry + 16].copy_from_slice(&7_u64.to_le_bytes());
    let mut resolver = LifecycleResolver;
    match LoadedElf::load_with_resolver(&malformed_size, &mut resolver) {
        Err(LoadError::Format("invalid DT_FINI_ARRAY pair")) => {}
        Err(error) => return Err(format!("bad FINI_ARRAYSZ returned wrong error: {error}").into()),
        Ok(_) => return Err("misaligned DT_FINI_ARRAYSZ loaded".into()),
    }

    let mut malformed_fini = bytes.to_vec();
    let fini_entry = dynamic_tag_entry(bytes, DT_FINI)?;
    malformed_fini[fini_entry + 8..fini_entry + 16].copy_from_slice(&u64::MAX.to_le_bytes());
    let mut resolver = LifecycleResolver;
    match LoadedElf::load_with_resolver(&malformed_fini, &mut resolver) {
        Err(LoadError::Bounds("DT_FINI function")) => {}
        Err(error) => return Err(format!("bad DT_FINI returned wrong error: {error}").into()),
        Ok(_) => return Err("out-of-range DT_FINI loaded".into()),
    }
    Ok(())
}

fn run_positive(bytes: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    for _ in 0..32 {
        let mut image = LoadedElf::load(bytes)?;
        if !matches!(
            image.call_exported_i32("fixture_value"),
            Err(LoadError::InitializersNotRun)
        ) {
            return Err("export was callable before DT_INIT_ARRAY".into());
        }
        image.run_initializers()?;
        if image.call_exported_i32("fixture_value")? != 42 {
            return Err("constructor order/export result mismatch".into());
        }
        if !matches!(
            image.run_initializers(),
            Err(LoadError::InitializersAlreadyRun)
        ) {
            return Err("DT_INIT_ARRAY ran more than once".into());
        }
    }
    Ok(())
}

fn run_import(bytes: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    if !matches!(
        LoadedElf::load(bytes),
        Err(LoadError::UnresolvedSymbol { .. })
    ) {
        return Err("default loader unexpectedly searched a host/global namespace".into());
    }
    for _ in 0..16 {
        let mut resolver = FixtureResolver::default();
        let mut image = LoadedElf::load_with_resolver(bytes, &mut resolver)?;
        if image.needed_libraries() != [PROVIDER_SONAME] {
            return Err("DT_NEEDED order/string was not preserved".into());
        }
        if image.soname() != Some("libdarwin_art_import.so") {
            return Err("DT_SONAME was not preserved".into());
        }
        image.run_initializers()?;
        if image.call_exported_i32("imported_value")? != 165 {
            return Err("resolved import execution mismatch".into());
        }
        if resolver.requests != 2 {
            return Err("resolver was not cached per dynsym".into());
        }
    }
    Ok(())
}

fn run_weak(bytes: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    let mut resolver = MissingResolver::default();
    let mut image = LoadedElf::load_with_resolver(bytes, &mut resolver)?;
    image.run_initializers()?;
    if image.call_exported_i32("weak_value")? != 19 || resolver.requests != 1 {
        return Err("unresolved weak symbol did not resolve to zero".into());
    }
    Ok(())
}

#[derive(Default)]
struct FixtureResolver {
    requests: usize,
}

impl SymbolResolver for FixtureResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        self.requests += 1;
        if request.needed_libraries != [PROVIDER_SONAME] {
            return Err(ResolveError::UnknownSoname(
                request
                    .needed_libraries
                    .first()
                    .cloned()
                    .unwrap_or_else(|| "<none>".to_owned()),
            ));
        }
        let version = request
            .version
            .ok_or_else(|| ResolveError::VersionMismatch {
                soname: PROVIDER_SONAME.to_owned(),
                symbol: request.symbol.to_owned(),
                requested: "<unversioned>".to_owned(),
            })?;
        if version.soname != PROVIDER_SONAME {
            return Err(ResolveError::UnknownSoname(version.soname.to_owned()));
        }
        if version.name != PROVIDER_VERSION || version.hidden || version.flags != 0 {
            return Err(ResolveError::VersionMismatch {
                soname: version.soname.to_owned(),
                symbol: request.symbol.to_owned(),
                requested: version.name.to_owned(),
            });
        }
        let address = match request.symbol {
            "provider_value" => provider_value as usize,
            "provider_data" => (&raw const PROVIDER_DATA).cast::<u8>() as usize,
            _ => return Ok(None),
        };
        let address = NonZeroUsize::new(address).ok_or_else(|| {
            ResolveError::Rejected("fixture provider has null address".to_owned())
        })?;
        // SAFETY: both fixture providers have static process lifetime and the exact AArch64
        // function/data ABI declared by import.c.
        Ok(Some(unsafe { ResolvedSymbol::new(address) }))
    }
}

#[derive(Default)]
struct MissingResolver {
    requests: usize,
}

impl SymbolResolver for MissingResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        self.requests += 1;
        if request.symbol != "optional_provider" || request.version.is_some() {
            return Err(ResolveError::Rejected("unexpected weak request".to_owned()));
        }
        Ok(None)
    }
}

struct VersionMismatchResolver;

impl SymbolResolver for VersionMismatchResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        let version = request
            .version
            .ok_or_else(|| ResolveError::VersionMismatch {
                soname: "<none>".to_owned(),
                symbol: request.symbol.to_owned(),
                requested: "<unversioned>".to_owned(),
            })?;
        Err(ResolveError::VersionMismatch {
            soname: version.soname.to_owned(),
            symbol: request.symbol.to_owned(),
            requested: version.name.to_owned(),
        })
    }
}

fn run_resolver_negative_matrix(original: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    let mut unknown_soname = original.to_vec();
    replace_dynamic_string(
        &mut unknown_soname,
        dynamic_tag_value(original, DT_NEEDED)?,
        PROVIDER_SONAME,
        "libunknow_art_provider.so",
    )?;
    let mut resolver = FixtureResolver::default();
    match LoadedElf::load_with_resolver(&unknown_soname, &mut resolver) {
        Err(LoadError::Resolver {
            source: ResolveError::UnknownSoname(_),
            ..
        }) => {}
        Err(error) => return Err(format!("unknown SONAME returned wrong error: {error}").into()),
        Ok(_) => return Err("unknown SONAME resolved".into()),
    }

    let mut resolver = VersionMismatchResolver;
    match LoadedElf::load_with_resolver(original, &mut resolver) {
        Err(LoadError::Resolver {
            source: ResolveError::VersionMismatch { .. },
            ..
        }) => {}
        Err(error) => return Err(format!("version mismatch returned wrong error: {error}").into()),
        Ok(_) => return Err("wrong symbol version resolved".into()),
    }

    let mut malformed_hash = original.to_vec();
    let vernaux_file = first_vernaux_file(original)?;
    malformed_hash[vernaux_file..vernaux_file + 4].copy_from_slice(&0_u32.to_le_bytes());
    let mut resolver = FixtureResolver::default();
    match LoadedElf::load_with_resolver(&malformed_hash, &mut resolver) {
        Err(LoadError::Format("Elf64_Vernaux hash mismatch")) => {}
        Err(error) => {
            return Err(format!("malformed VERNEED hash returned wrong error: {error}").into());
        }
        Ok(_) => return Err("malformed VERNEED hash loaded".into()),
    }

    let mut malformed_version = original.to_vec();
    let verneed_address = dynamic_tag_value(original, DT_VERNEED)?;
    let verneed_file = virtual_to_file(original, verneed_address, 16)?;
    malformed_version[verneed_file..verneed_file + 2].copy_from_slice(&2_u16.to_le_bytes());
    let mut resolver = FixtureResolver::default();
    match LoadedElf::load_with_resolver(&malformed_version, &mut resolver) {
        Err(LoadError::Format("invalid Elf64_Verneed header")) => {}
        Err(error) => return Err(format!("malformed VERNEED returned wrong error: {error}").into()),
        Ok(_) => return Err("malformed VERNEED loaded".into()),
    }
    Ok(())
}

fn first_vernaux_file(bytes: &[u8]) -> Result<usize, Box<dyn std::error::Error>> {
    let address = dynamic_tag_value(bytes, DT_VERNEED)?;
    let file = virtual_to_file(bytes, address, 16)?;
    let auxiliary_offset = read_u32(bytes, file + 8)?;
    file.checked_add(usize::try_from(auxiliary_offset)?)
        .ok_or_else(|| "Vernaux offset overflow".into())
}

fn replace_dynamic_string(
    bytes: &mut [u8],
    string_offset: u64,
    expected: &str,
    replacement: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    if expected.len() != replacement.len() {
        return Err("replacement string length differs".into());
    }
    let table = dynamic_tag_value(bytes, DT_STRTAB)?;
    let address = table
        .checked_add(string_offset)
        .ok_or("dynamic string address overflow")?;
    let file = virtual_to_file(bytes, address, expected.len() + 1)?;
    if bytes.get(file..file + expected.len()) != Some(expected.as_bytes()) {
        return Err("dynamic string mutation expectation mismatch".into());
    }
    bytes[file..file + replacement.len()].copy_from_slice(replacement.as_bytes());
    Ok(())
}

fn expect_capability(
    bytes: &[u8],
    predicate: impl FnOnce(&Capability) -> bool,
) -> Result<(), Box<dyn std::error::Error>> {
    match LoadedElf::load(bytes) {
        Err(LoadError::Capability(capability)) if predicate(&capability) => Ok(()),
        Err(error) => Err(format!("wrong capability rejection: {error}").into()),
        Ok(_) => Err("unsupported ELF unexpectedly loaded".into()),
    }
}

fn run_malformed_matrix(original: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    expect_rejected(&original[..32], "truncated header")?;

    let mut wrong_machine = original.to_vec();
    wrong_machine[18..20].copy_from_slice(&0_u16.to_le_bytes());
    expect_rejected(&wrong_machine, "wrong machine")?;

    let headers = load_program_headers(original, Some(PT_LOAD))?;
    let first = *headers.first().ok_or("fixture has no PT_LOAD")?;
    let second = *headers.get(1).ok_or("fixture has only one PT_LOAD")?;

    let mut writable_executable = original.to_vec();
    let flags = read_u32(&writable_executable, first + 4)? | PF_W | PF_X;
    writable_executable[first + 4..first + 8].copy_from_slice(&flags.to_le_bytes());
    match LoadedElf::load(&writable_executable) {
        Err(LoadError::Protection(_)) => {}
        Err(error) => return Err(format!("W+X returned wrong error: {error}").into()),
        Ok(_) => return Err("W+X PT_LOAD unexpectedly loaded".into()),
    }

    let mut overlap = original.to_vec();
    let first_offset = read_u64(&overlap, first + 8)?;
    let first_address = read_u64(&overlap, first + 16)?;
    overlap[second + 8..second + 16].copy_from_slice(&first_offset.to_le_bytes());
    overlap[second + 16..second + 24].copy_from_slice(&first_address.to_le_bytes());
    expect_rejected(&overlap, "overlapping PT_LOAD")?;

    let mut overflowing = original.to_vec();
    overflowing[first + 16..first + 24].copy_from_slice(&(u64::MAX - 0x1000).to_le_bytes());
    overflowing[first + 40..first + 48].copy_from_slice(&0x4000_u64.to_le_bytes());
    overflowing[first + 48..first + 56].copy_from_slice(&1_u64.to_le_bytes());
    expect_rejected(&overflowing, "virtual range overflow")?;

    let mut out_of_bounds = original.to_vec();
    out_of_bounds[first + 8..first + 16].copy_from_slice(&u64::MAX.to_le_bytes());
    out_of_bounds[first + 48..first + 56].copy_from_slice(&1_u64.to_le_bytes());
    expect_rejected(&out_of_bounds, "file range out of bounds")?;
    Ok(())
}

fn expect_rejected(bytes: &[u8], case: &str) -> Result<(), Box<dyn std::error::Error>> {
    if LoadedElf::load(bytes).is_ok() {
        Err(format!("malformed case loaded: {case}").into())
    } else {
        Ok(())
    }
}

fn dynamic_tag_value(bytes: &[u8], requested: i64) -> Result<u64, Box<dyn std::error::Error>> {
    let entry = dynamic_tag_entry(bytes, requested)?;
    read_u64(bytes, entry + 8)
}

fn dynamic_tag_entry(bytes: &[u8], requested: i64) -> Result<usize, Box<dyn std::error::Error>> {
    let dynamic_header = *load_program_headers(bytes, Some(PT_DYNAMIC))?
        .first()
        .ok_or("missing PT_DYNAMIC")?;
    let offset = usize::try_from(read_u64(bytes, dynamic_header + 8)?)?;
    let size = usize::try_from(read_u64(bytes, dynamic_header + 32)?)?;
    for entry in (offset..offset.checked_add(size).ok_or("dynamic overflow")?).step_by(16) {
        let tag = read_u64(bytes, entry)? as i64;
        if tag == DT_NULL {
            break;
        }
        if tag == requested {
            return Ok(entry);
        }
    }
    Err(format!("dynamic tag {requested:#x} missing").into())
}

fn virtual_to_file(
    bytes: &[u8],
    address: u64,
    size: usize,
) -> Result<usize, Box<dyn std::error::Error>> {
    for header in load_program_headers(bytes, Some(PT_LOAD))? {
        let file_offset = read_u64(bytes, header + 8)?;
        let virtual_address = read_u64(bytes, header + 16)?;
        let file_size = read_u64(bytes, header + 32)?;
        let requested_end = address
            .checked_add(u64::try_from(size)?)
            .ok_or("virtual range overflow")?;
        let load_end = virtual_address
            .checked_add(file_size)
            .ok_or("load range overflow")?;
        if address >= virtual_address && requested_end <= load_end {
            return usize::try_from(
                file_offset
                    .checked_add(address - virtual_address)
                    .ok_or("file offset overflow")?,
            )
            .map_err(Into::into);
        }
    }
    Err("virtual address is not file-backed".into())
}

fn load_program_headers(
    bytes: &[u8],
    kind: Option<u32>,
) -> Result<Vec<usize>, Box<dyn std::error::Error>> {
    let offset = usize::try_from(read_u64(bytes, 32)?)?;
    let entry_size = read_u16(bytes, 54)? as usize;
    let count = read_u16(bytes, 56)? as usize;
    let mut result = Vec::new();
    for index in 0..count {
        let entry = offset
            .checked_add(index.checked_mul(entry_size).ok_or("phdr overflow")?)
            .ok_or("phdr overflow")?;
        if kind.is_none_or(|expected| read_u32(bytes, entry).ok() == Some(expected)) {
            result.push(entry);
        }
    }
    Ok(result)
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, Box<dyn std::error::Error>> {
    Ok(u16::from_le_bytes(
        bytes
            .get(offset..offset + 2)
            .ok_or("u16 bounds")?
            .try_into()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, Box<dyn std::error::Error>> {
    Ok(u32::from_le_bytes(
        bytes
            .get(offset..offset + 4)
            .ok_or("u32 bounds")?
            .try_into()?,
    ))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64, Box<dyn std::error::Error>> {
    Ok(u64::from_le_bytes(
        bytes
            .get(offset..offset + 8)
            .ok_or("u64 bounds")?
            .try_into()?,
    ))
}
