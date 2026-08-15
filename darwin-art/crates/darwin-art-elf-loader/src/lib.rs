use std::collections::{HashMap, HashSet};
use std::error::Error as StdError;
use std::ffi::{c_int, c_void};
use std::fmt;
use std::num::NonZeroUsize;
use std::ops::Range;
use std::ptr::{self, NonNull};
use std::sync::Arc;

mod ffi;
mod namespace;

pub use namespace::{ClosedElfNamespace, LoadedElfGraph, NamespaceError};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ElfMetadata {
    pub soname: Option<Vec<u8>>,
    pub needed_libraries: Vec<Vec<u8>>,
}

/// Parses one Android arm64 ET_DYN image without relocating it or running guest code.
///
/// This path parses file bytes only: it does not reserve address space, relocate, or run guest code.
pub fn inspect_elf_metadata(bytes: &[u8]) -> Result<ElfMetadata, LoadError> {
    let parsed = parse_image_with_policy(bytes, true)?;
    let dynamic = parse_dynamic_with_policy(bytes, &parsed.dynamic, true)?;
    let needed_libraries = dynamic
        .needed_offsets
        .iter()
        .map(|offset| dynamic_string_bytes(bytes, &parsed.loads, &dynamic, *offset))
        .collect::<Result<Vec<_>, _>>()?;
    let soname = dynamic
        .soname_offset
        .map(|offset| dynamic_string_bytes(bytes, &parsed.loads, &dynamic, offset))
        .transpose()?;
    if needed_libraries.iter().any(Vec::is_empty) {
        return Err(LoadError::Format("empty DT_NEEDED string"));
    }
    if soname.as_ref().is_some_and(Vec::is_empty) {
        return Err(LoadError::Format("empty DT_SONAME string"));
    }
    Ok(ElfMetadata {
        soname,
        needed_libraries,
    })
}

fn dynamic_string_bytes(
    bytes: &[u8],
    loads: &[ProgramHeader],
    dynamic: &DynamicInfo,
    offset: u64,
) -> Result<Vec<u8>, LoadError> {
    let table = dynamic
        .string_table
        .ok_or(LoadError::Format("missing DT_STRTAB"))?;
    let size = dynamic
        .string_size
        .ok_or(LoadError::Format("missing DT_STRSZ"))?;
    if offset >= size {
        return Err(LoadError::Bounds("dynamic string offset"));
    }
    let address = table
        .checked_add(offset)
        .ok_or(LoadError::Bounds("dynamic string address overflow"))?;
    let remaining = size - offset;
    let end = address
        .checked_add(remaining)
        .ok_or(LoadError::Bounds("dynamic string range overflow"))?;
    let load = loads
        .iter()
        .find(|load| {
            load.flags & PF_R != 0
                && address >= load.virtual_address
                && load
                    .virtual_address
                    .checked_add(load.file_size)
                    .is_some_and(|load_end| end <= load_end)
        })
        .ok_or(LoadError::Bounds(
            "dynamic string table is not file-backed readable data",
        ))?;
    let file_offset = load
        .offset
        .checked_add(
            address
                .checked_sub(load.virtual_address)
                .ok_or(LoadError::Bounds("dynamic string file mapping"))?,
        )
        .ok_or(LoadError::Bounds("dynamic string file offset overflow"))?;
    let data = checked_slice(
        bytes,
        to_usize(file_offset, "dynamic string file offset")?,
        to_usize(remaining, "dynamic string size")?,
        "dynamic string file range",
    )?;
    let terminator = data
        .iter()
        .position(|byte| *byte == 0)
        .ok_or(LoadError::Format("unterminated dynamic string"))?;
    Ok(data[..terminator].to_vec())
}

#[cfg(not(target_os = "macos"))]
compile_error!("darwin-art-elf-loader supports only macOS hosts");

const EI_NIDENT: usize = 16;
const ELF64_EHDR_SIZE: usize = 64;
const ELF64_PHDR_SIZE: usize = 56;
const ELF64_DYN_SIZE: usize = 16;
const ELF64_RELA_SIZE: usize = 24;
const ELF64_SYM_SIZE: usize = 24;

const ET_DYN: u16 = 3;
const EM_AARCH64: u16 = 183;
const EV_CURRENT: u32 = 1;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_TLS: u32 = 7;
const PT_GNU_RELRO: u32 = 0x6474_e552;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

const DT_NULL: i64 = 0;
const DT_NEEDED: i64 = 1;
const DT_PLTRELSZ: i64 = 2;
const DT_PLTGOT: i64 = 3;
const DT_HASH: i64 = 4;
const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_RELAENT: i64 = 9;
const DT_STRSZ: i64 = 10;
const DT_SYMENT: i64 = 11;
const DT_INIT: i64 = 12;
const DT_FINI: i64 = 13;
const DT_SONAME: i64 = 14;
const DT_RPATH: i64 = 15;
const DT_SYMBOLIC: i64 = 16;
const DT_REL: i64 = 17;
const DT_RELSZ: i64 = 18;
const DT_RELENT: i64 = 19;
const DT_PLTREL: i64 = 20;
const DT_DEBUG: i64 = 21;
const DT_TEXTREL: i64 = 22;
const DT_JMPREL: i64 = 23;
const DT_BIND_NOW: i64 = 24;
const DT_INIT_ARRAY: i64 = 25;
const DT_FINI_ARRAY: i64 = 26;
const DT_INIT_ARRAYSZ: i64 = 27;
const DT_FINI_ARRAYSZ: i64 = 28;
const DT_RUNPATH: i64 = 29;
const DT_FLAGS: i64 = 30;
const DT_PREINIT_ARRAY: i64 = 32;
const DT_PREINIT_ARRAYSZ: i64 = 33;
const DT_RELR: i64 = 36;
const DT_RELRSZ: i64 = 35;
const DT_RELRENT: i64 = 37;
const DT_GNU_HASH: i64 = 0x6fff_fef5;
const DT_RELACOUNT: i64 = 0x6fff_fff9;
const DT_FLAGS_1: i64 = 0x6fff_fffb;
const DT_VERSYM: i64 = 0x6fff_fff0;
const DT_VERDEF: i64 = 0x6fff_fffc;
const DT_VERDEFNUM: i64 = 0x6fff_fffd;
const DT_VERNEED: i64 = 0x6fff_fffe;
const DT_VERNEEDNUM: i64 = 0x6fff_ffff;

const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;
const DF_BIND_NOW: u64 = 0x8;
const DF_1_NOW: u64 = 0x1;
const SHN_UNDEF: u16 = 0;
const SHN_ABS: u16 = 0xfff1;
const STB_GLOBAL: u8 = 1;
const STB_WEAK: u8 = 2;
const STT_NOTYPE: u8 = 0;
const STT_OBJECT: u8 = 1;
const STT_FUNC: u8 = 2;
const STT_TLS: u8 = 6;
const STV_DEFAULT: u8 = 0;
const STV_PROTECTED: u8 = 3;
const VER_NDX_LOCAL: u16 = 0;
const VER_NDX_GLOBAL: u16 = 1;
const VERSYM_HIDDEN: u16 = 0x8000;
const VER_FLG_BASE: u16 = 0x1;
const VER_FLG_WEAK: u16 = 0x2;

const PROT_NONE: c_int = 0;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const PROT_EXEC: c_int = 4;
const MAP_PRIVATE: c_int = 0x0002;
const MAP_ANON: c_int = 0x1000;
const MAX_IMAGE_SIZE: usize = 1024 * 1024 * 1024;

unsafe extern "C" {
    fn getpagesize() -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        file_descriptor: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn mprotect(address: *mut c_void, length: usize, protection: c_int) -> c_int;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn sys_icache_invalidate(start: *mut c_void, length: usize);
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Capability {
    HostArchitecture,
    Tls,
    Relro,
    LazyBinding,
    RelRelocations,
    RelrRelocations,
    UnsupportedRelocation { relocation_type: u32, symbol: u32 },
    DynamicInitializer,
    Finalizers,
    PreinitArray,
    TextRelocations,
    Rpath,
    UnknownDynamicTag(i64),
    MissingSysvHash,
    SymbolicLookup,
    VersionDefinitions,
    AbsoluteSymbolDefinition,
    DynamicFlags { tag: i64, value: u64 },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResolveError {
    UnknownSoname(String),
    VersionMismatch {
        soname: String,
        symbol: String,
        requested: String,
    },
    Rejected(String),
}

impl fmt::Display for ResolveError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownSoname(soname) => write!(formatter, "unknown dependency SONAME: {soname}"),
            Self::VersionMismatch {
                soname,
                symbol,
                requested,
            } => write!(
                formatter,
                "version mismatch for {symbol}@{requested} from {soname}"
            ),
            Self::Rejected(message) => write!(formatter, "resolver rejected symbol: {message}"),
        }
    }
}

impl StdError for ResolveError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResolvedSymbol {
    address: NonZeroUsize,
}

impl ResolvedSymbol {
    /// Constructs a provider result from a raw host address.
    ///
    /// # Safety
    ///
    /// The address must have the ABI, object/function kind, alignment, and readable/executable
    /// lifetime required by every relocation that names it. It must remain valid until every
    /// `LoadedElf` created with the resolver has been dropped.
    pub unsafe fn new(address: NonZeroUsize) -> Self {
        Self { address }
    }

    pub fn address(self) -> usize {
        self.address.get()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct VersionRequirement<'a> {
    pub soname: &'a str,
    pub name: &'a str,
    pub hidden: bool,
    pub flags: u16,
}

#[derive(Clone, Copy, Debug)]
pub struct SymbolRequest<'a> {
    pub symbol: &'a str,
    pub needed_libraries: &'a [String],
    pub version: Option<VersionRequirement<'a>>,
}

/// A closed symbol namespace supplied by the caller.
///
/// The loader never consults `dlsym`, dyld's global namespace, or the host process. Returning
/// `Ok(None)` means the symbol is absent. An absent weak undefined symbol resolves to zero; an
/// absent global undefined symbol fails the load.
pub trait SymbolResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError>;
}

#[derive(Debug)]
pub enum LoadError {
    Format(&'static str),
    Bounds(&'static str),
    Capability(Capability),
    Protection(&'static str),
    Resolver {
        symbol: String,
        source: ResolveError,
    },
    UnresolvedSymbol {
        symbol: String,
        soname: Option<String>,
        version: Option<String>,
    },
    SymbolNotFound(String),
    InvalidSymbol(String),
    InitializersAlreadyRun,
    InitializersNotRun,
    System {
        operation: &'static str,
        code: i32,
    },
}

impl fmt::Display for LoadError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Format(message) => write!(formatter, "invalid ELF: {message}"),
            Self::Bounds(message) => write!(formatter, "ELF bounds error: {message}"),
            Self::Capability(capability) => {
                write!(formatter, "unsupported ELF capability: {capability:?}")
            }
            Self::Protection(message) => write!(formatter, "ELF protection error: {message}"),
            Self::Resolver { symbol, source } => {
                write!(formatter, "resolver failed for {symbol}: {source}")
            }
            Self::UnresolvedSymbol {
                symbol,
                soname,
                version,
            } => {
                write!(formatter, "unresolved strong symbol: {symbol}")?;
                if let Some(version) = version {
                    write!(formatter, "@{version}")?;
                }
                if let Some(soname) = soname {
                    write!(formatter, " from {soname}")?;
                }
                Ok(())
            }
            Self::SymbolNotFound(name) => write!(formatter, "dynamic symbol not found: {name}"),
            Self::InvalidSymbol(name) => write!(formatter, "invalid dynamic symbol: {name}"),
            Self::InitializersAlreadyRun => write!(formatter, "DT_INIT_ARRAY already executed"),
            Self::InitializersNotRun => write!(formatter, "DT_INIT_ARRAY has not executed"),
            Self::System { operation, code } => {
                write!(formatter, "{operation} failed with errno {code}")
            }
        }
    }
}

impl StdError for LoadError {}

#[derive(Clone, Copy, Debug)]
struct ProgramHeader {
    kind: u32,
    flags: u32,
    offset: u64,
    virtual_address: u64,
    file_size: u64,
    memory_size: u64,
    alignment: u64,
}

#[derive(Default, Debug)]
struct DynamicInfo {
    needed_offsets: Vec<u64>,
    soname_offset: Option<u64>,
    hash: Option<u64>,
    string_table: Option<u64>,
    string_size: Option<u64>,
    symbol_table: Option<u64>,
    symbol_entry_size: Option<u64>,
    rela: Option<u64>,
    rela_size: Option<u64>,
    rela_entry_size: Option<u64>,
    relative_relocation_count: Option<u64>,
    plt_rela: Option<u64>,
    plt_rela_size: Option<u64>,
    plt_relocation_kind: Option<u64>,
    bind_now_tag: bool,
    flags: Option<u64>,
    flags_1: Option<u64>,
    versym: Option<u64>,
    verneed: Option<u64>,
    verneed_count: Option<u64>,
    verdef: Option<u64>,
    verdef_count: Option<u64>,
    init_array: Option<u64>,
    init_array_size: Option<u64>,
    fini: Option<u64>,
    fini_array: Option<u64>,
    fini_array_size: Option<u64>,
}

#[derive(Debug)]
struct ParsedImage {
    loads: Vec<ProgramHeader>,
    dynamic: ProgramHeader,
    minimum_page: u64,
    image_size: usize,
    page_size: usize,
    page_protections: Vec<c_int>,
}

pub struct LoadedElf {
    mapping: NonNull<u8>,
    mapping_size: usize,
    minimum_page: u64,
    loads: Vec<ProgramHeader>,
    dynamic: DynamicInfo,
    needed_libraries: Vec<String>,
    soname: Option<String>,
    initializers_run: bool,
    finalizers_armed: bool,
    finalizers_run: bool,
    finalizers: Vec<usize>,
    dso_lifecycle: Option<Arc<dyn DsoLifecycle>>,
}

/// Per-image lifecycle boundary for Bionic `__cxa_atexit` registrations.
///
/// A graph publishes the complete, unique mmap reservation before running any
/// constructor. Teardown is called while that reservation is still executable,
/// before ELF fini entries and `munmap`. Implementations must synchronously
/// drain callbacks and stop admitting registrations before returning.
pub trait DsoLifecycle: Send + Sync {
    fn publish_image(&self, range: Range<usize>) -> Result<(), String>;
    fn finalize_image(&self, range: Range<usize>) -> Result<(), String>;
}

struct StagedElf {
    image: LoadedElf,
    page_size: usize,
    page_protections: Vec<c_int>,
}

// SAFETY: LoadedElf exclusively owns an mmap reservation with no thread-affine host resource.
// TLS and lazy binding are rejected, resolver callbacks are not retained, and mutation is
// restricted to &mut self. C consumers additionally serialize the value behind a Mutex.
unsafe impl Send for LoadedElf {}

#[derive(Default)]
struct RejectAllResolver;

impl SymbolResolver for RejectAllResolver {
    fn resolve(
        &mut self,
        _request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        Ok(None)
    }
}

impl LoadedElf {
    pub fn load(bytes: &[u8]) -> Result<Self, LoadError> {
        Self::load_with_resolver(bytes, &mut RejectAllResolver)
    }

    pub fn load_with_resolver(
        bytes: &[u8],
        resolver: &mut dyn SymbolResolver,
    ) -> Result<Self, LoadError> {
        let mut staged = Self::stage(bytes)?;
        staged
            .image
            .finish_load(resolver, staged.page_size, &staged.page_protections)?;
        Ok(staged.image)
    }

    fn stage(bytes: &[u8]) -> Result<StagedElf, LoadError> {
        if !cfg!(target_arch = "aarch64") {
            return Err(LoadError::Capability(Capability::HostArchitecture));
        }
        let parsed = parse_image(bytes)?;
        let mapping = Mapping::reserve(parsed.image_size)?;

        for (index, protection) in parsed.page_protections.iter().copied().enumerate() {
            if protection != PROT_NONE {
                mapping.protect(
                    index * parsed.page_size,
                    parsed.page_size,
                    PROT_READ | PROT_WRITE,
                )?;
            }
        }

        for load in &parsed.loads {
            let destination_offset =
                difference_to_usize(load.virtual_address, parsed.minimum_page)?;
            let file_offset = to_usize(load.offset, "PT_LOAD file offset")?;
            let file_size = to_usize(load.file_size, "PT_LOAD file size")?;
            let memory_size = to_usize(load.memory_size, "PT_LOAD memory size")?;
            let source = checked_slice(bytes, file_offset, file_size, "PT_LOAD file range")?;
            // SAFETY: parse_image proves every destination interval is within the reservation,
            // and the corresponding pages were made writable above.
            unsafe {
                ptr::copy_nonoverlapping(
                    source.as_ptr(),
                    mapping.pointer.as_ptr().add(destination_offset),
                    file_size,
                );
                ptr::write_bytes(
                    mapping.pointer.as_ptr().add(destination_offset + file_size),
                    0,
                    memory_size - file_size,
                );
            }
        }

        let dynamic = parse_dynamic(bytes, &parsed.dynamic)?;
        validate_dynamic_capabilities(&dynamic)?;

        let mut loaded = Self {
            mapping: mapping.pointer,
            mapping_size: mapping.length,
            minimum_page: parsed.minimum_page,
            loads: parsed.loads,
            dynamic,
            needed_libraries: Vec::new(),
            soname: None,
            initializers_run: false,
            finalizers_armed: false,
            finalizers_run: false,
            finalizers: Vec::new(),
            dso_lifecycle: None,
        };
        std::mem::forget(mapping);

        loaded.materialize_dynamic_names()?;
        Ok(StagedElf {
            image: loaded,
            page_size: parsed.page_size,
            page_protections: parsed.page_protections,
        })
    }

    fn finish_load(
        &mut self,
        resolver: &mut dyn SymbolResolver,
        page_size: usize,
        page_protections: &[c_int],
    ) -> Result<(), LoadError> {
        self.validate_symbols_and_apply_relocations(resolver)?;
        self.validate_initializer_entries()?;
        self.prepare_finalizers()?;
        self.apply_final_protections(page_size, page_protections)
    }

    pub fn needed_libraries(&self) -> &[String] {
        &self.needed_libraries
    }

    pub fn soname(&self) -> Option<&str> {
        self.soname.as_deref()
    }

    /// Returns the mapped address of a defined dynamic export.
    ///
    /// The address remains valid only while this `LoadedElf` is alive. Calling it is unsafe in
    /// the general case: the caller must know the exact ABI and must run required initializers
    /// first. This method performs symbol visibility/type and executable-range validation only.
    pub fn lookup_exported(&self, name: &str) -> Result<usize, LoadError> {
        self.resolve_function(name)
    }

    pub fn run_initializers(&mut self) -> Result<(), LoadError> {
        self.run_initializers_internal(true)
    }

    fn run_initializers_for_graph(&mut self) -> Result<(), LoadError> {
        self.run_initializers_internal(false)
    }

    fn run_initializers_internal(&mut self, arm_finalizers: bool) -> Result<(), LoadError> {
        if self.initializers_run {
            return Err(LoadError::InitializersAlreadyRun);
        }
        let address = self.dynamic.init_array.unwrap_or(0);
        let size = self.dynamic.init_array_size.unwrap_or(0);
        if (address == 0) != (size == 0) || size % 8 != 0 {
            return Err(LoadError::Format("invalid DT_INIT_ARRAY pair"));
        }
        if size != 0 {
            self.require_loaded_range(address, size, None, "DT_INIT_ARRAY")?;
            for index in 0..(size / 8) {
                let entry_address = address
                    .checked_add(index * 8)
                    .ok_or(LoadError::Bounds("DT_INIT_ARRAY entry overflow"))?;
                let pointer = self.read_loaded_u64(entry_address)?;
                if pointer == 0 || pointer == u64::MAX {
                    continue;
                }
                self.require_host_executable(pointer, "DT_INIT_ARRAY function")?;
                // SAFETY: the entry was relocated, lies in an executable PT_LOAD, and the
                // fixture ABI is the supported no-argument AArch64 procedure-call subset.
                let initializer: unsafe extern "C" fn() = unsafe {
                    std::mem::transmute::<usize, unsafe extern "C" fn()>(
                        usize::try_from(pointer)
                            .map_err(|_| LoadError::Bounds("initializer pointer"))?,
                    )
                };
                unsafe { initializer() };
            }
        }
        self.initializers_run = true;
        self.finalizers_armed = arm_finalizers;
        Ok(())
    }

    fn arm_finalizers(&mut self) {
        debug_assert!(self.initializers_run);
        self.finalizers_armed = true;
    }

    fn mapping_range(&self) -> Range<usize> {
        let start = self.mapping.as_ptr() as usize;
        start..start + self.mapping_size
    }

    fn publish_dso_lifecycle(&mut self, lifecycle: Arc<dyn DsoLifecycle>) -> Result<(), String> {
        lifecycle.publish_image(self.mapping_range())?;
        self.dso_lifecycle = Some(lifecycle);
        Ok(())
    }

    /// Consumes this image, running its finalizers (when initialized) before unmapping it.
    ///
    /// Dropping the image has the same lifecycle semantics. The consuming API exists so callers
    /// can make an intentional unload point explicit without introducing a second owner state.
    pub fn close(self) {}

    fn validate_initializer_entries(&self) -> Result<(), LoadError> {
        let address = self.dynamic.init_array.unwrap_or(0);
        let size = self.dynamic.init_array_size.unwrap_or(0);
        if (address == 0) != (size == 0) || size % 8 != 0 {
            return Err(LoadError::Format("invalid DT_INIT_ARRAY pair"));
        }
        if size == 0 {
            return Ok(());
        }
        self.require_loaded_range(address, size, None, "DT_INIT_ARRAY")?;
        for index in 0..(size / 8) {
            let entry_address = address
                .checked_add(index * 8)
                .ok_or(LoadError::Bounds("DT_INIT_ARRAY entry overflow"))?;
            let pointer = self.read_loaded_u64(entry_address)?;
            if pointer != 0 && pointer != u64::MAX {
                self.require_host_executable(pointer, "DT_INIT_ARRAY function")?;
            }
        }
        Ok(())
    }

    fn prepare_finalizers(&mut self) -> Result<(), LoadError> {
        let address = self.dynamic.fini_array.unwrap_or(0);
        let size = self.dynamic.fini_array_size.unwrap_or(0);
        if (address == 0) != (size == 0) || size % 8 != 0 {
            return Err(LoadError::Format("invalid DT_FINI_ARRAY pair"));
        }

        let mut finalizers = Vec::new();
        if size != 0 {
            self.require_loaded_range(address, size, None, "DT_FINI_ARRAY")?;
            for index in (0..(size / 8)).rev() {
                let entry_address = address
                    .checked_add(index * 8)
                    .ok_or(LoadError::Bounds("DT_FINI_ARRAY entry overflow"))?;
                let pointer = self.read_loaded_u64(entry_address)?;
                if pointer == 0 || pointer == u64::MAX {
                    continue;
                }
                self.require_host_executable(pointer, "DT_FINI_ARRAY function")?;
                finalizers.push(
                    usize::try_from(pointer).map_err(|_| LoadError::Bounds("finalizer pointer"))?,
                );
            }
        }

        if let Some(address) = self.dynamic.fini.filter(|address| *address != 0) {
            self.require_loaded_range(address, 1, Some(PF_X), "DT_FINI function")?;
            finalizers.push(self.loaded_pointer(address, 1)? as usize);
        }
        self.finalizers = finalizers;
        Ok(())
    }

    fn run_finalizers_once(&mut self) {
        if !self.finalizers_armed || self.finalizers_run {
            return;
        }
        // Mark first: re-entrant teardown must never execute the sequence twice.
        self.finalizers_run = true;
        for &pointer in &self.finalizers {
            // SAFETY: finish_load validated and captured each relocated function pointer while
            // the image was immutable. The mapping is still live and this loader supports the
            // no-argument AArch64 finalizer ABI used by Android ELF DSOs.
            let finalizer: unsafe extern "C" fn() = unsafe { std::mem::transmute(pointer) };
            unsafe { finalizer() };
        }
    }

    pub fn call_exported_i32(&self, name: &str) -> Result<i32, LoadError> {
        if !self.initializers_run {
            return Err(LoadError::InitializersNotRun);
        }
        let pointer = self.resolve_function(name)?;
        // SAFETY: resolve_function validates a defined function symbol inside executable
        // mapped memory. This API deliberately supports only the no-argument/i32 ABI slice.
        let function: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute::<usize, unsafe extern "C" fn() -> i32>(pointer) };
        Ok(unsafe { function() })
    }

    fn materialize_dynamic_names(&mut self) -> Result<(), LoadError> {
        self.needed_libraries = self
            .dynamic
            .needed_offsets
            .iter()
            .map(|offset| {
                u32::try_from(*offset)
                    .map_err(|_| LoadError::Bounds("DT_NEEDED string offset"))
                    .and_then(|offset| self.dynamic_string(offset))
            })
            .collect::<Result<Vec<_>, _>>()?;
        self.soname = self
            .dynamic
            .soname_offset
            .map(|offset| {
                u32::try_from(offset)
                    .map_err(|_| LoadError::Bounds("DT_SONAME string offset"))
                    .and_then(|offset| self.dynamic_string(offset))
            })
            .transpose()?;
        if self.needed_libraries.iter().any(String::is_empty) {
            return Err(LoadError::Format("empty DT_NEEDED string"));
        }
        if self.soname.as_ref().is_some_and(String::is_empty) {
            return Err(LoadError::Format("empty DT_SONAME string"));
        }
        Ok(())
    }

    fn validate_symbols_and_apply_relocations(
        &mut self,
        resolver: &mut dyn SymbolResolver,
    ) -> Result<(), LoadError> {
        let symbol_count = self.symbol_count()?;
        for index in 1..symbol_count {
            let symbol = self.dynamic_symbol(index)?;
            if symbol.kind == STT_TLS {
                return Err(LoadError::Capability(Capability::Tls));
            }
        }

        // Validate provider-side GNU version metadata even when this image is loaded outside a
        // graph and therefore does not need an export catalog.
        self.parse_version_definitions(symbol_count)?;
        let versions = self.parse_version_requirements(symbol_count)?;
        let mut resolved = HashMap::new();
        let rela_address = self.dynamic.rela.unwrap_or(0);
        let rela_size = self.dynamic.rela_size.unwrap_or(0);
        if (rela_address == 0) != (rela_size == 0) {
            return Err(LoadError::Format("incomplete DT_RELA metadata"));
        }
        if rela_size != 0 {
            self.apply_relocation_table(
                rela_address,
                rela_size,
                false,
                self.dynamic.relative_relocation_count.unwrap_or(0),
                symbol_count,
                &versions,
                &mut resolved,
                resolver,
            )?;
        }

        let plt_address = self.dynamic.plt_rela.unwrap_or(0);
        let plt_size = self.dynamic.plt_rela_size.unwrap_or(0);
        if (plt_address == 0) != (plt_size == 0) {
            return Err(LoadError::Format("incomplete PLT relocation metadata"));
        }
        if plt_size != 0 {
            let rela_end = rela_address
                .checked_add(rela_size)
                .ok_or(LoadError::Bounds("DT_RELA range overflow"))?;
            let plt_end = plt_address
                .checked_add(plt_size)
                .ok_or(LoadError::Bounds("DT_JMPREL range overflow"))?;
            if rela_size != 0 && rela_address < plt_end && plt_address < rela_end {
                return Err(LoadError::Format("DT_RELA and DT_JMPREL overlap"));
            }
            self.apply_relocation_table(
                plt_address,
                plt_size,
                true,
                0,
                symbol_count,
                &versions,
                &mut resolved,
                resolver,
            )?;
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn apply_relocation_table(
        &mut self,
        table_address: u64,
        table_size: u64,
        plt_only: bool,
        relative_prefix_count: u64,
        symbol_count: u32,
        versions: &HashMap<u16, OwnedVersionRequirement>,
        resolved: &mut HashMap<u32, usize>,
        resolver: &mut dyn SymbolResolver,
    ) -> Result<(), LoadError> {
        let entry_size = self
            .dynamic
            .rela_entry_size
            .unwrap_or(ELF64_RELA_SIZE as u64);
        if entry_size != ELF64_RELA_SIZE as u64 || table_size % entry_size != 0 {
            return Err(LoadError::Format(
                "invalid DT_RELAENT/relocation table size",
            ));
        }
        let count = table_size / entry_size;
        if relative_prefix_count > count {
            return Err(LoadError::Format("DT_RELACOUNT exceeds DT_RELASZ"));
        }
        self.require_loaded_range(table_address, table_size, Some(PF_R), "RELA table")?;
        for index in 0..count {
            let address = table_address
                .checked_add(index * entry_size)
                .ok_or(LoadError::Bounds("DT_RELA entry overflow"))?;
            let offset = self.read_loaded_u64(address)?;
            let info = self.read_loaded_u64(address + 8)?;
            let addend = self.read_loaded_i64(address + 16)?;
            let relocation_type = info as u32;
            let symbol_index = (info >> 32) as u32;
            if index < relative_prefix_count
                && (relocation_type != R_AARCH64_RELATIVE || symbol_index != 0)
            {
                return Err(LoadError::Format(
                    "DT_RELACOUNT prefix contains non-relative relocation",
                ));
            }
            if plt_only && relocation_type != R_AARCH64_JUMP_SLOT {
                return Err(LoadError::Capability(Capability::UnsupportedRelocation {
                    relocation_type,
                    symbol: symbol_index,
                }));
            }
            self.require_loaded_range(offset, 8, Some(PF_W), "RELA target")?;
            let value = match relocation_type {
                R_AARCH64_RELATIVE if symbol_index == 0 => self.load_bias_add(addend)?,
                R_AARCH64_ABS64 | R_AARCH64_GLOB_DAT | R_AARCH64_JUMP_SLOT => {
                    if symbol_index == 0 && relocation_type != R_AARCH64_ABS64 {
                        return Err(LoadError::Format(
                            "GLOB_DAT/JUMP_SLOT relocation has STN_UNDEF symbol index",
                        ));
                    }
                    let symbol_address = self.resolve_relocation_symbol(
                        symbol_index,
                        symbol_count,
                        versions,
                        resolved,
                        resolver,
                    )?;
                    checked_signed_add(symbol_address, addend, "symbol relocation value")?
                }
                _ => {
                    return Err(LoadError::Capability(Capability::UnsupportedRelocation {
                        relocation_type,
                        symbol: symbol_index,
                    }));
                }
            };
            let destination = self.loaded_pointer(offset, 8)?;
            // SAFETY: require_loaded_range proves an aligned-size writable destination.
            unsafe { ptr::write_unaligned(destination.cast::<usize>(), value) };
        }
        Ok(())
    }

    fn resolve_relocation_symbol(
        &self,
        index: u32,
        symbol_count: u32,
        versions: &HashMap<u16, OwnedVersionRequirement>,
        resolved: &mut HashMap<u32, usize>,
        resolver: &mut dyn SymbolResolver,
    ) -> Result<usize, LoadError> {
        if let Some(address) = resolved.get(&index) {
            return Ok(*address);
        }
        if index == 0 {
            return Ok(0);
        }
        if index >= symbol_count {
            return Err(LoadError::Bounds("relocation symbol index"));
        }
        let symbol = self.dynamic_symbol(index)?;
        if symbol.kind == STT_TLS {
            return Err(LoadError::Capability(Capability::Tls));
        }
        if !matches!(symbol.kind, STT_NOTYPE | STT_OBJECT | STT_FUNC) {
            return Err(LoadError::InvalidSymbol(format!("dynsym[{index}] type")));
        }
        if symbol.section_index != SHN_UNDEF {
            if symbol.section_index == SHN_ABS {
                return Err(LoadError::Capability(Capability::AbsoluteSymbolDefinition));
            }
            self.require_loaded_range(symbol.value, symbol.size.max(1), None, "defined symbol")?;
            let address = self.loaded_pointer(symbol.value, 1)? as usize;
            resolved.insert(index, address);
            return Ok(address);
        }
        if !matches!(symbol.binding, STB_GLOBAL | STB_WEAK) || symbol.visibility != 0 {
            return Err(LoadError::InvalidSymbol(format!("dynsym[{index}] binding")));
        }
        let name = self.dynamic_string(symbol.name_offset)?;
        if name.is_empty() {
            return Err(LoadError::InvalidSymbol(format!(
                "dynsym[{index}] empty name"
            )));
        }
        let version = self.version_for_symbol(index, versions)?;
        let request_version = version.map(|(requirement, hidden)| VersionRequirement {
            soname: &requirement.soname,
            name: &requirement.name,
            hidden,
            flags: requirement.flags,
        });
        let result = resolver
            .resolve(SymbolRequest {
                symbol: &name,
                needed_libraries: &self.needed_libraries,
                version: request_version,
            })
            .map_err(|source| LoadError::Resolver {
                symbol: name.clone(),
                source,
            })?;
        let address = match result {
            Some(symbol) => symbol.address(),
            None if symbol.binding == STB_WEAK => 0,
            None => {
                return Err(LoadError::UnresolvedSymbol {
                    symbol: name,
                    soname: version.map(|(item, _)| item.soname.clone()),
                    version: version.map(|(item, _)| item.name.clone()),
                });
            }
        };
        resolved.insert(index, address);
        Ok(address)
    }

    fn parse_version_requirements(
        &self,
        symbol_count: u32,
    ) -> Result<HashMap<u16, OwnedVersionRequirement>, LoadError> {
        let mut result = HashMap::new();
        let (Some(mut current), Some(count)) = (self.dynamic.verneed, self.dynamic.verneed_count)
        else {
            if self.dynamic.verneed.is_some() || self.dynamic.verneed_count.is_some() {
                return Err(LoadError::Format("incomplete DT_VERNEED metadata"));
            }
            if let Some(versym) = self.dynamic.versym {
                self.require_loaded_range(
                    versym,
                    u64::from(symbol_count) * 2,
                    Some(PF_R),
                    "DT_VERSYM",
                )?;
            }
            return Ok(result);
        };
        let versym = self
            .dynamic
            .versym
            .ok_or(LoadError::Format("DT_VERNEED requires DT_VERSYM"))?;
        if count == 0 {
            return Err(LoadError::Format("DT_VERNEEDNUM is zero"));
        }
        self.require_loaded_range(versym, u64::from(symbol_count) * 2, Some(PF_R), "DT_VERSYM")?;
        let mut visited = HashSet::new();
        for requirement_index in 0..count {
            if !visited.insert(current) {
                return Err(LoadError::Format("cyclic DT_VERNEED list"));
            }
            self.require_loaded_range(current, 16, Some(PF_R), "Elf64_Verneed")?;
            let version = self.read_loaded_u16(current)?;
            let auxiliary_count = self.read_loaded_u16(current + 2)?;
            let file_offset = self.read_loaded_u32(current + 4)?;
            let auxiliary_offset = self.read_loaded_u32(current + 8)?;
            let next_offset = self.read_loaded_u32(current + 12)?;
            if version != 1 || auxiliary_count == 0 || auxiliary_offset == 0 {
                return Err(LoadError::Format("invalid Elf64_Verneed header"));
            }
            let soname = self.dynamic_string(file_offset)?;
            if !self.needed_libraries.iter().any(|needed| needed == &soname) {
                return Err(LoadError::Format(
                    "Elf64_Verneed file is not present in DT_NEEDED",
                ));
            }
            let mut auxiliary = current
                .checked_add(u64::from(auxiliary_offset))
                .ok_or(LoadError::Bounds("Elf64_Vernaux address overflow"))?;
            for auxiliary_index in 0..auxiliary_count {
                self.require_loaded_range(auxiliary, 16, Some(PF_R), "Elf64_Vernaux")?;
                let expected_hash = self.read_loaded_u32(auxiliary)?;
                let flags = self.read_loaded_u16(auxiliary + 4)?;
                let raw_other = self.read_loaded_u16(auxiliary + 6)?;
                let name_offset = self.read_loaded_u32(auxiliary + 8)?;
                let next = self.read_loaded_u32(auxiliary + 12)?;
                let index = raw_other & !VERSYM_HIDDEN;
                if index <= VER_NDX_GLOBAL {
                    return Err(LoadError::Format("invalid Elf64_Vernaux version index"));
                }
                if flags & !VER_FLG_WEAK != 0 {
                    return Err(LoadError::Format("unsupported Elf64_Vernaux flags"));
                }
                let name = self.dynamic_string(name_offset)?;
                if elf_hash(name.as_bytes()) != expected_hash {
                    return Err(LoadError::Format("Elf64_Vernaux hash mismatch"));
                }
                let requirement = OwnedVersionRequirement {
                    soname: soname.clone(),
                    name,
                    flags,
                };
                if result.insert(index, requirement).is_some() {
                    return Err(LoadError::Format("duplicate version requirement index"));
                }
                if auxiliary_index + 1 == auxiliary_count {
                    if next != 0 {
                        return Err(LoadError::Format("final Elf64_Vernaux has nonzero next"));
                    }
                } else if next == 0 {
                    return Err(LoadError::Format("truncated Elf64_Vernaux list"));
                } else {
                    auxiliary = auxiliary
                        .checked_add(u64::from(next))
                        .ok_or(LoadError::Bounds("Elf64_Vernaux next overflow"))?;
                }
            }
            if requirement_index + 1 == count {
                if next_offset != 0 {
                    return Err(LoadError::Format("final Elf64_Verneed has nonzero next"));
                }
            } else if next_offset == 0 {
                return Err(LoadError::Format("truncated Elf64_Verneed list"));
            } else {
                current = current
                    .checked_add(u64::from(next_offset))
                    .ok_or(LoadError::Bounds("Elf64_Verneed next overflow"))?;
            }
        }
        Ok(result)
    }

    fn version_for_symbol<'a>(
        &self,
        symbol_index: u32,
        requirements: &'a HashMap<u16, OwnedVersionRequirement>,
    ) -> Result<Option<(&'a OwnedVersionRequirement, bool)>, LoadError> {
        let Some(table) = self.dynamic.versym else {
            return Ok(None);
        };
        let address = table
            .checked_add(u64::from(symbol_index) * 2)
            .ok_or(LoadError::Bounds("DT_VERSYM entry overflow"))?;
        let raw = self.read_loaded_u16(address)?;
        let index = raw & !VERSYM_HIDDEN;
        if matches!(index, VER_NDX_LOCAL | VER_NDX_GLOBAL) {
            return Ok(None);
        }
        let requirement = requirements
            .get(&index)
            .ok_or(LoadError::Format("DT_VERSYM index has no DT_VERNEED entry"))?;
        Ok(Some((requirement, raw & VERSYM_HIDDEN != 0)))
    }

    fn load_bias_add(&self, addend: i64) -> Result<usize, LoadError> {
        (self.mapping.as_ptr() as i128)
            .checked_add(addend as i128 - self.minimum_page as i128)
            .and_then(|value| usize::try_from(value).ok())
            .ok_or(LoadError::Bounds("R_AARCH64_RELATIVE value overflow"))
    }

    fn resolve_function(&self, requested: &str) -> Result<usize, LoadError> {
        for index in 1..self.symbol_count()? {
            let symbol = self.dynamic_symbol(index)?;
            if self.dynamic_string(symbol.name_offset)? != requested {
                continue;
            }
            if symbol.section_index == SHN_UNDEF
                || !matches!(symbol.kind, STT_FUNC | STT_NOTYPE)
                || !matches!(symbol.binding, STB_GLOBAL | STB_WEAK)
                || !matches!(symbol.visibility, STV_DEFAULT | STV_PROTECTED)
                || symbol.value == 0
            {
                return Err(LoadError::InvalidSymbol(requested.to_owned()));
            }
            if symbol.section_index == SHN_ABS {
                return Err(LoadError::Capability(Capability::AbsoluteSymbolDefinition));
            }
            self.require_loaded_range(
                symbol.value,
                symbol.size.max(1),
                Some(PF_X),
                "function symbol",
            )?;
            let pointer = self.loaded_pointer(symbol.value, 1)? as usize;
            return Ok(pointer);
        }
        Err(LoadError::SymbolNotFound(requested.to_owned()))
    }

    fn exported_symbols(&self) -> Result<Vec<ExportedSymbol>, LoadError> {
        let count = self.symbol_count()?;
        let versions = self.parse_version_definitions(count)?;
        let mut result = Vec::new();
        for index in 1..count {
            let symbol = self.dynamic_symbol(index)?;
            if symbol.section_index == SHN_UNDEF
                || !matches!(symbol.kind, STT_NOTYPE | STT_OBJECT | STT_FUNC)
                || !matches!(symbol.binding, STB_GLOBAL | STB_WEAK)
                || !matches!(symbol.visibility, STV_DEFAULT | STV_PROTECTED)
                || symbol.value == 0
            {
                continue;
            }
            if symbol.section_index == SHN_ABS {
                return Err(LoadError::Capability(Capability::AbsoluteSymbolDefinition));
            }
            self.require_loaded_range(symbol.value, symbol.size.max(1), None, "exported symbol")?;
            let name = self.dynamic_string(symbol.name_offset)?;
            if name.is_empty() {
                continue;
            }
            let (version, hidden) = self.definition_for_symbol(index, &versions)?;
            result.push(ExportedSymbol {
                name,
                address: self.loaded_pointer(symbol.value, 1)? as usize,
                binding: symbol.binding,
                version: version.map(|item| item.name.clone()),
                version_hidden: hidden,
            });
        }
        Ok(result)
    }

    fn parse_version_definitions(
        &self,
        symbol_count: u32,
    ) -> Result<HashMap<u16, OwnedVersionDefinition>, LoadError> {
        let Some(mut current) = self.dynamic.verdef else {
            if self.dynamic.verdef_count.is_some() {
                return Err(LoadError::Format("incomplete DT_VERDEF metadata"));
            }
            return Ok(HashMap::new());
        };
        let count = self
            .dynamic
            .verdef_count
            .ok_or(LoadError::Format("incomplete DT_VERDEF metadata"))?;
        let versym = self
            .dynamic
            .versym
            .ok_or(LoadError::Format("DT_VERDEF requires DT_VERSYM"))?;
        if count == 0 {
            return Err(LoadError::Format("DT_VERDEFNUM is zero"));
        }
        self.require_loaded_range(versym, u64::from(symbol_count) * 2, Some(PF_R), "DT_VERSYM")?;
        let mut result = HashMap::new();
        let mut visited = HashSet::new();
        for definition_index in 0..count {
            if !visited.insert(current) {
                return Err(LoadError::Format("cyclic DT_VERDEF list"));
            }
            self.require_loaded_range(current, 20, Some(PF_R), "Elf64_Verdef")?;
            let version = self.read_loaded_u16(current)?;
            let flags = self.read_loaded_u16(current + 2)?;
            let index = self.read_loaded_u16(current + 4)? & !VERSYM_HIDDEN;
            let auxiliary_count = self.read_loaded_u16(current + 6)?;
            let expected_hash = self.read_loaded_u32(current + 8)?;
            let auxiliary_offset = self.read_loaded_u32(current + 12)?;
            let next_offset = self.read_loaded_u32(current + 16)?;
            if version != 1
                || index == VER_NDX_LOCAL
                || (index == VER_NDX_GLOBAL && flags & VER_FLG_BASE == 0)
                || auxiliary_count == 0
                || auxiliary_offset == 0
            {
                return Err(LoadError::Format("invalid Elf64_Verdef header"));
            }
            if flags & !(VER_FLG_BASE | VER_FLG_WEAK) != 0 {
                return Err(LoadError::Format("unsupported Elf64_Verdef flags"));
            }
            let mut auxiliary = current
                .checked_add(u64::from(auxiliary_offset))
                .ok_or(LoadError::Bounds("Elf64_Verdaux address overflow"))?;
            let mut definition_name = None;
            for auxiliary_index in 0..auxiliary_count {
                self.require_loaded_range(auxiliary, 8, Some(PF_R), "Elf64_Verdaux")?;
                let name = self.dynamic_string(self.read_loaded_u32(auxiliary)?)?;
                let next = self.read_loaded_u32(auxiliary + 4)?;
                if auxiliary_index == 0 {
                    definition_name = Some(name);
                }
                if auxiliary_index + 1 == auxiliary_count {
                    if next != 0 {
                        return Err(LoadError::Format("final Elf64_Verdaux has nonzero next"));
                    }
                } else if next == 0 {
                    return Err(LoadError::Format("truncated Elf64_Verdaux list"));
                } else {
                    auxiliary = auxiliary
                        .checked_add(u64::from(next))
                        .ok_or(LoadError::Bounds("Elf64_Verdaux next overflow"))?;
                }
            }
            let name = definition_name.expect("nonzero Verdaux count");
            if elf_hash(name.as_bytes()) != expected_hash {
                return Err(LoadError::Format("Elf64_Verdef hash mismatch"));
            }
            if result
                .insert(index, OwnedVersionDefinition { name })
                .is_some()
            {
                return Err(LoadError::Format("duplicate version definition index"));
            }
            if definition_index + 1 == count {
                if next_offset != 0 {
                    return Err(LoadError::Format("final Elf64_Verdef has nonzero next"));
                }
            } else if next_offset == 0 {
                return Err(LoadError::Format("truncated Elf64_Verdef list"));
            } else {
                current = current
                    .checked_add(u64::from(next_offset))
                    .ok_or(LoadError::Bounds("Elf64_Verdef next overflow"))?;
            }
        }
        Ok(result)
    }

    fn definition_for_symbol<'a>(
        &self,
        symbol_index: u32,
        definitions: &'a HashMap<u16, OwnedVersionDefinition>,
    ) -> Result<(Option<&'a OwnedVersionDefinition>, bool), LoadError> {
        let Some(table) = self.dynamic.versym else {
            return Ok((None, false));
        };
        let address = table
            .checked_add(u64::from(symbol_index) * 2)
            .ok_or(LoadError::Bounds("DT_VERSYM entry overflow"))?;
        let raw = self.read_loaded_u16(address)?;
        let index = raw & !VERSYM_HIDDEN;
        if matches!(index, VER_NDX_LOCAL | VER_NDX_GLOBAL) {
            return Ok((None, raw & VERSYM_HIDDEN != 0));
        }
        let definition = definitions
            .get(&index)
            .ok_or(LoadError::Format("DT_VERSYM index has no DT_VERDEF entry"))?;
        Ok((Some(definition), raw & VERSYM_HIDDEN != 0))
    }

    fn symbol_count(&self) -> Result<u32, LoadError> {
        let hash = self
            .dynamic
            .hash
            .ok_or(LoadError::Capability(Capability::MissingSysvHash))?;
        self.require_loaded_range(hash, 8, Some(PF_R), "DT_HASH header")?;
        let bucket_count = self.read_loaded_u32(hash)? as u64;
        let chain_count = self.read_loaded_u32(hash + 4)?;
        let words = 2_u64
            .checked_add(bucket_count)
            .and_then(|value| value.checked_add(chain_count as u64))
            .ok_or(LoadError::Bounds("DT_HASH size overflow"))?;
        self.require_loaded_range(hash, words * 4, Some(PF_R), "DT_HASH table")?;
        Ok(chain_count)
    }

    fn dynamic_symbol(&self, index: u32) -> Result<DynamicSymbol, LoadError> {
        let table = self
            .dynamic
            .symbol_table
            .ok_or(LoadError::Format("missing DT_SYMTAB"))?;
        let entry_size = self
            .dynamic
            .symbol_entry_size
            .unwrap_or(ELF64_SYM_SIZE as u64);
        if entry_size != ELF64_SYM_SIZE as u64 {
            return Err(LoadError::Format("invalid DT_SYMENT"));
        }
        let address = table
            .checked_add(index as u64 * entry_size)
            .ok_or(LoadError::Bounds("dynamic symbol address overflow"))?;
        self.require_loaded_range(address, entry_size, Some(PF_R), "dynamic symbol")?;
        let bytes = self.loaded_slice(address, ELF64_SYM_SIZE)?;
        Ok(DynamicSymbol {
            name_offset: read_u32(bytes, 0)?,
            binding: bytes[4] >> 4,
            kind: bytes[4] & 0x0f,
            visibility: bytes[5] & 0x03,
            section_index: read_u16(bytes, 6)?,
            value: read_u64(bytes, 8)?,
            size: read_u64(bytes, 16)?,
        })
    }

    fn dynamic_string(&self, offset: u32) -> Result<String, LoadError> {
        let table = self
            .dynamic
            .string_table
            .ok_or(LoadError::Format("missing DT_STRTAB"))?;
        let size = self
            .dynamic
            .string_size
            .ok_or(LoadError::Format("missing DT_STRSZ"))?;
        if offset as u64 >= size {
            return Err(LoadError::Bounds("dynamic string offset"));
        }
        self.require_loaded_range(table, size, Some(PF_R), "dynamic string table")?;
        let bytes = self.loaded_slice(table + offset as u64, (size - offset as u64) as usize)?;
        let end = bytes
            .iter()
            .position(|byte| *byte == 0)
            .ok_or(LoadError::Format("unterminated dynamic string"))?;
        let value = std::str::from_utf8(&bytes[..end])
            .map_err(|_| LoadError::Format("non-UTF-8 dynamic symbol name"))?;
        Ok(value.to_owned())
    }

    fn apply_final_protections(
        &self,
        page_size: usize,
        page_protections: &[c_int],
    ) -> Result<(), LoadError> {
        let mut start = 0;
        while start < page_protections.len() {
            let protection = page_protections[start];
            let mut end = start + 1;
            while end < page_protections.len() && page_protections[end] == protection {
                end += 1;
            }
            let address = unsafe { self.mapping.as_ptr().add(start * page_size) };
            let length = (end - start) * page_size;
            if protection & PROT_EXEC != 0 {
                // SAFETY: this is a live portion of the loader-owned reservation. Apple
                // requires explicit instruction-cache invalidation for freshly copied code.
                unsafe { sys_icache_invalidate(address.cast(), length) };
            }
            if unsafe { mprotect(address.cast(), length, protection) } != 0 {
                return Err(system_error("mprotect(final)"));
            }
            start = end;
        }
        Ok(())
    }

    fn require_host_executable(&self, pointer: u64, what: &'static str) -> Result<(), LoadError> {
        let offset = pointer
            .checked_sub(self.mapping.as_ptr() as u64)
            .ok_or(LoadError::Bounds("host pointer below mapping"))?;
        let virtual_address = self
            .minimum_page
            .checked_add(offset)
            .ok_or(LoadError::Bounds("host pointer translation overflow"))?;
        self.require_loaded_range(virtual_address, 1, Some(PF_X), what)
    }

    fn require_loaded_range(
        &self,
        address: u64,
        size: u64,
        required_flag: Option<u32>,
        what: &'static str,
    ) -> Result<(), LoadError> {
        let end = address.checked_add(size).ok_or(LoadError::Bounds(what))?;
        let found = self.loads.iter().any(|load| {
            let Some(load_end) = load.virtual_address.checked_add(load.memory_size) else {
                return false;
            };
            address >= load.virtual_address
                && end <= load_end
                && required_flag.is_none_or(|flag| load.flags & flag != 0)
        });
        if found {
            Ok(())
        } else {
            Err(LoadError::Bounds(what))
        }
    }

    fn loaded_pointer(&self, address: u64, size: usize) -> Result<*mut u8, LoadError> {
        let offset = difference_to_usize(address, self.minimum_page)?;
        let end = offset
            .checked_add(size)
            .ok_or(LoadError::Bounds("mapped pointer overflow"))?;
        if end > self.mapping_size {
            return Err(LoadError::Bounds("mapped pointer outside reservation"));
        }
        Ok(unsafe { self.mapping.as_ptr().add(offset) })
    }

    fn loaded_slice(&self, address: u64, size: usize) -> Result<&[u8], LoadError> {
        let pointer = self.loaded_pointer(address, size)?;
        Ok(unsafe { std::slice::from_raw_parts(pointer, size) })
    }

    fn read_loaded_u32(&self, address: u64) -> Result<u32, LoadError> {
        read_u32(self.loaded_slice(address, 4)?, 0)
    }

    fn read_loaded_u16(&self, address: u64) -> Result<u16, LoadError> {
        read_u16(self.loaded_slice(address, 2)?, 0)
    }

    fn read_loaded_u64(&self, address: u64) -> Result<u64, LoadError> {
        read_u64(self.loaded_slice(address, 8)?, 0)
    }

    fn read_loaded_i64(&self, address: u64) -> Result<i64, LoadError> {
        Ok(self.read_loaded_u64(address)? as i64)
    }
}

impl Drop for LoadedElf {
    fn drop(&mut self) {
        if let Some(lifecycle) = self.dso_lifecycle.take() {
            if lifecycle.finalize_image(self.mapping_range()).is_err() {
                // Continuing could unmap code that still owns live callbacks.
                std::process::abort();
            }
        }
        self.run_finalizers_once();
        // SAFETY: LoadedElf exclusively owns this complete mmap reservation.
        let result = unsafe { munmap(self.mapping.as_ptr().cast(), self.mapping_size) };
        debug_assert_eq!(result, 0, "munmap failed while dropping LoadedElf");
    }
}

struct Mapping {
    pointer: NonNull<u8>,
    length: usize,
}

impl Mapping {
    fn reserve(length: usize) -> Result<Self, LoadError> {
        let pointer = unsafe {
            mmap(
                ptr::null_mut(),
                length,
                PROT_NONE,
                MAP_PRIVATE | MAP_ANON,
                -1,
                0,
            )
        };
        if pointer as usize == usize::MAX {
            return Err(system_error("mmap(reserve)"));
        }
        let pointer = NonNull::new(pointer.cast::<u8>()).ok_or(LoadError::System {
            operation: "mmap(reserve)",
            code: 0,
        })?;
        Ok(Self { pointer, length })
    }

    fn protect(&self, offset: usize, length: usize, protection: c_int) -> Result<(), LoadError> {
        let end = offset
            .checked_add(length)
            .ok_or(LoadError::Bounds("mprotect overflow"))?;
        if end > self.length {
            return Err(LoadError::Bounds("mprotect outside reservation"));
        }
        let pointer = unsafe { self.pointer.as_ptr().add(offset) };
        if unsafe { mprotect(pointer.cast(), length, protection) } != 0 {
            return Err(system_error("mprotect(staging)"));
        }
        Ok(())
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        unsafe { munmap(self.pointer.as_ptr().cast(), self.length) };
    }
}

#[derive(Debug)]
struct DynamicSymbol {
    name_offset: u32,
    binding: u8,
    kind: u8,
    visibility: u8,
    section_index: u16,
    value: u64,
    size: u64,
}

#[derive(Clone, Debug)]
struct OwnedVersionRequirement {
    soname: String,
    name: String,
    flags: u16,
}

#[derive(Clone, Debug)]
struct OwnedVersionDefinition {
    name: String,
}

#[derive(Clone, Debug)]
struct ExportedSymbol {
    name: String,
    address: usize,
    binding: u8,
    version: Option<String>,
    version_hidden: bool,
}

fn parse_image(bytes: &[u8]) -> Result<ParsedImage, LoadError> {
    parse_image_with_policy(bytes, false)
}

fn parse_image_with_policy(bytes: &[u8], metadata_only: bool) -> Result<ParsedImage, LoadError> {
    if bytes.len() < ELF64_EHDR_SIZE || bytes.len() < EI_NIDENT {
        return Err(LoadError::Format("truncated ELF header"));
    }
    if &bytes[..4] != b"\x7fELF" {
        return Err(LoadError::Format("bad magic"));
    }
    if bytes[4] != 2 || bytes[5] != 1 || bytes[6] != 1 {
        return Err(LoadError::Format("requires ELF64 little-endian v1"));
    }
    if read_u16(bytes, 16)? != ET_DYN {
        return Err(LoadError::Format("requires ET_DYN"));
    }
    if read_u16(bytes, 18)? != EM_AARCH64 {
        return Err(LoadError::Format("requires EM_AARCH64"));
    }
    if read_u32(bytes, 20)? != EV_CURRENT {
        return Err(LoadError::Format("bad ELF version"));
    }
    if read_u16(bytes, 52)? as usize != ELF64_EHDR_SIZE
        || read_u16(bytes, 54)? as usize != ELF64_PHDR_SIZE
    {
        return Err(LoadError::Format("unexpected ELF/program header size"));
    }
    let program_offset = to_usize(read_u64(bytes, 32)?, "program header offset")?;
    let program_count = read_u16(bytes, 56)? as usize;
    if program_count == 0 {
        return Err(LoadError::Format("no program headers"));
    }
    let table_size = program_count
        .checked_mul(ELF64_PHDR_SIZE)
        .ok_or(LoadError::Bounds("program header table overflow"))?;
    checked_slice(bytes, program_offset, table_size, "program header table")?;

    let mut loads = Vec::new();
    let mut dynamic = None;
    let mut relro = None;
    for index in 0..program_count {
        let offset = program_offset + index * ELF64_PHDR_SIZE;
        let header = ProgramHeader {
            kind: read_u32(bytes, offset)?,
            flags: read_u32(bytes, offset + 4)?,
            offset: read_u64(bytes, offset + 8)?,
            virtual_address: read_u64(bytes, offset + 16)?,
            file_size: read_u64(bytes, offset + 32)?,
            memory_size: read_u64(bytes, offset + 40)?,
            alignment: read_u64(bytes, offset + 48)?,
        };
        match header.kind {
            PT_LOAD => {
                validate_load(bytes, &header)?;
                loads.push(header);
            }
            PT_DYNAMIC => {
                if dynamic.replace(header).is_some() {
                    return Err(LoadError::Format("multiple PT_DYNAMIC segments"));
                }
            }
            PT_TLS if !metadata_only => return Err(LoadError::Capability(Capability::Tls)),
            PT_GNU_RELRO if !metadata_only => {
                if relro.replace(header).is_some() {
                    return Err(LoadError::Format("multiple PT_GNU_RELRO segments"));
                }
            }
            _ => {}
        }
    }
    if loads.is_empty() {
        return Err(LoadError::Format("no PT_LOAD segments"));
    }
    loads.sort_by_key(|load| load.virtual_address);
    for pair in loads.windows(2) {
        let previous_end = pair[0]
            .virtual_address
            .checked_add(pair[0].memory_size)
            .ok_or(LoadError::Bounds("PT_LOAD end overflow"))?;
        if previous_end > pair[1].virtual_address {
            return Err(LoadError::Format("overlapping PT_LOAD memory ranges"));
        }
    }

    let dynamic = dynamic.ok_or(LoadError::Format("missing PT_DYNAMIC"))?;
    validate_dynamic_segment(bytes, &dynamic, &loads)?;

    let page_size = usize::try_from(unsafe { getpagesize() })
        .ok()
        .filter(|size| size.is_power_of_two())
        .ok_or(LoadError::System {
            operation: "getpagesize",
            code: 0,
        })?;
    let page_mask = page_size as u64 - 1;
    let minimum = loads.first().unwrap().virtual_address & !page_mask;
    let maximum_unaligned = loads
        .iter()
        .map(|load| load.virtual_address.checked_add(load.memory_size))
        .collect::<Option<Vec<_>>>()
        .ok_or(LoadError::Bounds("PT_LOAD maximum overflow"))?
        .into_iter()
        .max()
        .unwrap();
    let maximum = maximum_unaligned
        .checked_add(page_mask)
        .ok_or(LoadError::Bounds("image page rounding overflow"))?
        & !page_mask;
    let image_size = difference_to_usize(maximum, minimum)?;
    if image_size == 0 || image_size > MAX_IMAGE_SIZE {
        return Err(LoadError::Bounds("image reservation size"));
    }
    let mut protections = vec![PROT_NONE; image_size / page_size];
    for load in &loads {
        let start = difference_to_usize(load.virtual_address & !page_mask, minimum)? / page_size;
        let end_address = load
            .virtual_address
            .checked_add(load.memory_size)
            .and_then(|value| value.checked_add(page_mask))
            .ok_or(LoadError::Bounds("PT_LOAD page span overflow"))?
            & !page_mask;
        let end = difference_to_usize(end_address, minimum)? / page_size;
        let protection = elf_flags_to_protection(load.flags);
        for page in protections
            .get_mut(start..end)
            .ok_or(LoadError::Bounds("PT_LOAD page plan"))?
        {
            *page |= protection;
            if *page & PROT_WRITE != 0 && *page & PROT_EXEC != 0 {
                return Err(LoadError::Protection(
                    "host page would be writable and executable",
                ));
            }
        }
    }

    if let Some(relro) = relro {
        if relro.flags != PF_R || relro.memory_size == 0 || relro.file_size > relro.memory_size {
            return Err(LoadError::Format("invalid PT_GNU_RELRO segment"));
        }
        let relro_end = relro
            .virtual_address
            .checked_add(relro.memory_size)
            .ok_or(LoadError::Bounds("PT_GNU_RELRO end overflow"))?;
        let containing_load = loads.iter().any(|load| {
            load.virtual_address <= relro.virtual_address
                && load
                    .virtual_address
                    .checked_add(load.memory_size)
                    .is_some_and(|load_end| relro_end <= load_end)
        });
        if !containing_load {
            return Err(LoadError::Format("PT_GNU_RELRO outside PT_LOAD"));
        }

        let start = difference_to_usize(relro.virtual_address & !page_mask, minimum)? / page_size;
        let end_address = relro_end
            .checked_add(page_mask)
            .ok_or(LoadError::Bounds("PT_GNU_RELRO page rounding overflow"))?
            & !page_mask;
        let end = difference_to_usize(end_address, minimum)? / page_size;
        let relro_pages = protections
            .get_mut(start..end)
            .ok_or(LoadError::Bounds("PT_GNU_RELRO page plan"))?;
        if relro_pages.is_empty()
            || relro_pages
                .iter()
                .any(|protection| protection & PROT_READ == 0 || protection & PROT_EXEC != 0)
        {
            return Err(LoadError::Protection(
                "PT_GNU_RELRO does not cover readable non-executable pages",
            ));
        }
        for protection in relro_pages {
            *protection &= !PROT_WRITE;
        }
    }

    Ok(ParsedImage {
        loads,
        dynamic,
        minimum_page: minimum,
        image_size,
        page_size,
        page_protections: protections,
    })
}

fn validate_load(bytes: &[u8], load: &ProgramHeader) -> Result<(), LoadError> {
    if load.memory_size == 0 {
        return Err(LoadError::Format("zero-sized PT_LOAD"));
    }
    if load.file_size > load.memory_size {
        return Err(LoadError::Format("PT_LOAD p_filesz exceeds p_memsz"));
    }
    if load.flags & !(PF_R | PF_W | PF_X) != 0 {
        return Err(LoadError::Format("unknown PT_LOAD flags"));
    }
    if load.flags & PF_W != 0 && load.flags & PF_X != 0 {
        return Err(LoadError::Protection("PT_LOAD requests W+X"));
    }
    if load.alignment > 1
        && (!load.alignment.is_power_of_two()
            || load.offset % load.alignment != load.virtual_address % load.alignment)
    {
        return Err(LoadError::Format("invalid PT_LOAD alignment"));
    }
    let offset = to_usize(load.offset, "PT_LOAD file offset")?;
    let size = to_usize(load.file_size, "PT_LOAD file size")?;
    checked_slice(bytes, offset, size, "PT_LOAD file range")?;
    load.virtual_address
        .checked_add(load.memory_size)
        .ok_or(LoadError::Bounds("PT_LOAD virtual range overflow"))?;
    Ok(())
}

fn validate_dynamic_segment(
    bytes: &[u8],
    dynamic: &ProgramHeader,
    loads: &[ProgramHeader],
) -> Result<(), LoadError> {
    if dynamic.file_size == 0
        || dynamic.file_size > dynamic.memory_size
        || dynamic.file_size % ELF64_DYN_SIZE as u64 != 0
    {
        return Err(LoadError::Format("invalid PT_DYNAMIC size"));
    }
    checked_slice(
        bytes,
        to_usize(dynamic.offset, "PT_DYNAMIC offset")?,
        to_usize(dynamic.file_size, "PT_DYNAMIC size")?,
        "PT_DYNAMIC file range",
    )?;
    let end = dynamic
        .virtual_address
        .checked_add(dynamic.memory_size)
        .ok_or(LoadError::Bounds("PT_DYNAMIC range overflow"))?;
    let containing_load = loads.iter().find(|load| {
        load.virtual_address <= dynamic.virtual_address
            && load
                .virtual_address
                .checked_add(load.memory_size)
                .is_some_and(|load_end| end <= load_end)
    });
    let Some(containing_load) = containing_load else {
        return Err(LoadError::Bounds("PT_DYNAMIC is not inside PT_LOAD"));
    };
    let file_delta = dynamic
        .offset
        .checked_sub(containing_load.offset)
        .ok_or(LoadError::Bounds("PT_DYNAMIC file mapping"))?;
    let virtual_delta = dynamic
        .virtual_address
        .checked_sub(containing_load.virtual_address)
        .ok_or(LoadError::Bounds("PT_DYNAMIC virtual mapping"))?;
    if file_delta != virtual_delta
        || file_delta
            .checked_add(dynamic.file_size)
            .is_none_or(|value| value > containing_load.file_size)
    {
        return Err(LoadError::Format(
            "PT_DYNAMIC file/virtual mapping mismatch",
        ));
    }
    Ok(())
}

fn parse_dynamic(bytes: &[u8], dynamic: &ProgramHeader) -> Result<DynamicInfo, LoadError> {
    parse_dynamic_with_policy(bytes, dynamic, false)
}

fn parse_dynamic_with_policy(
    bytes: &[u8],
    dynamic: &ProgramHeader,
    metadata_only: bool,
) -> Result<DynamicInfo, LoadError> {
    let data = checked_slice(
        bytes,
        to_usize(dynamic.offset, "PT_DYNAMIC offset")?,
        to_usize(dynamic.file_size, "PT_DYNAMIC size")?,
        "PT_DYNAMIC",
    )?;
    let mut info = DynamicInfo::default();
    let mut terminated = false;
    for entry in data.chunks_exact(ELF64_DYN_SIZE) {
        let tag = read_i64(entry, 0)?;
        let value = read_u64(entry, 8)?;
        if tag == DT_NULL {
            terminated = true;
            break;
        }
        match tag {
            DT_NEEDED => info.needed_offsets.push(value),
            DT_SONAME => set_once(&mut info.soname_offset, value, "duplicate DT_SONAME")?,
            DT_HASH => set_once(&mut info.hash, value, "duplicate DT_HASH")?,
            DT_STRTAB => set_once(&mut info.string_table, value, "duplicate DT_STRTAB")?,
            DT_STRSZ => set_once(&mut info.string_size, value, "duplicate DT_STRSZ")?,
            DT_SYMTAB => set_once(&mut info.symbol_table, value, "duplicate DT_SYMTAB")?,
            DT_SYMENT => set_once(&mut info.symbol_entry_size, value, "duplicate DT_SYMENT")?,
            DT_RELA => set_once(&mut info.rela, value, "duplicate DT_RELA")?,
            DT_RELASZ => set_once(&mut info.rela_size, value, "duplicate DT_RELASZ")?,
            DT_RELAENT => set_once(&mut info.rela_entry_size, value, "duplicate DT_RELAENT")?,
            DT_RELACOUNT => set_once(
                &mut info.relative_relocation_count,
                value,
                "duplicate DT_RELACOUNT",
            )?,
            DT_JMPREL => set_once(&mut info.plt_rela, value, "duplicate DT_JMPREL")?,
            DT_PLTRELSZ => set_once(&mut info.plt_rela_size, value, "duplicate DT_PLTRELSZ")?,
            DT_PLTREL => set_once(&mut info.plt_relocation_kind, value, "duplicate DT_PLTREL")?,
            DT_BIND_NOW => info.bind_now_tag = true,
            DT_FLAGS => set_once(&mut info.flags, value, "duplicate DT_FLAGS")?,
            DT_FLAGS_1 => set_once(&mut info.flags_1, value, "duplicate DT_FLAGS_1")?,
            DT_VERSYM => set_once(&mut info.versym, value, "duplicate DT_VERSYM")?,
            DT_VERNEED => set_once(&mut info.verneed, value, "duplicate DT_VERNEED")?,
            DT_VERNEEDNUM => set_once(&mut info.verneed_count, value, "duplicate DT_VERNEEDNUM")?,
            DT_VERDEF => set_once(&mut info.verdef, value, "duplicate DT_VERDEF")?,
            DT_VERDEFNUM => set_once(&mut info.verdef_count, value, "duplicate DT_VERDEFNUM")?,
            DT_INIT_ARRAY => set_once(&mut info.init_array, value, "duplicate DT_INIT_ARRAY")?,
            DT_INIT_ARRAYSZ => set_once(
                &mut info.init_array_size,
                value,
                "duplicate DT_INIT_ARRAYSZ",
            )?,
            DT_FINI => set_once(&mut info.fini, value, "duplicate DT_FINI")?,
            DT_FINI_ARRAY => set_once(&mut info.fini_array, value, "duplicate DT_FINI_ARRAY")?,
            DT_FINI_ARRAYSZ => set_once(
                &mut info.fini_array_size,
                value,
                "duplicate DT_FINI_ARRAYSZ",
            )?,
            DT_PLTGOT | DT_DEBUG | DT_GNU_HASH => {}
            DT_REL | DT_RELSZ | DT_RELENT => {
                if value != 0 && !metadata_only {
                    return Err(LoadError::Capability(Capability::RelRelocations));
                }
            }
            DT_RELR | DT_RELRSZ | DT_RELRENT => {
                if value != 0 && !metadata_only {
                    return Err(LoadError::Capability(Capability::RelrRelocations));
                }
            }
            DT_INIT => {
                if value != 0 && !metadata_only {
                    return Err(LoadError::Capability(Capability::DynamicInitializer));
                }
            }
            DT_PREINIT_ARRAY | DT_PREINIT_ARRAYSZ => {
                if value != 0 && !metadata_only {
                    return Err(LoadError::Capability(Capability::PreinitArray));
                }
            }
            DT_TEXTREL if !metadata_only => {
                return Err(LoadError::Capability(Capability::TextRelocations));
            }
            DT_RPATH | DT_RUNPATH if !metadata_only => {
                return Err(LoadError::Capability(Capability::Rpath));
            }
            DT_SYMBOLIC if !metadata_only => {
                return Err(LoadError::Capability(Capability::SymbolicLookup));
            }
            _ if metadata_only => {}
            _ => return Err(LoadError::Capability(Capability::UnknownDynamicTag(tag))),
        }
    }
    if !terminated {
        return Err(LoadError::Format("PT_DYNAMIC lacks DT_NULL"));
    }
    Ok(info)
}

fn validate_dynamic_capabilities(info: &DynamicInfo) -> Result<(), LoadError> {
    if info.hash.is_none() {
        return Err(LoadError::Capability(Capability::MissingSysvHash));
    }
    if info.string_table.is_none()
        || info.string_size.is_none()
        || info.symbol_table.is_none()
        || info.symbol_entry_size.is_none()
    {
        return Err(LoadError::Format("incomplete dynamic symbol metadata"));
    }
    if let Some(relative_count) = info.relative_relocation_count {
        let relocation_count = info.rela_size.unwrap_or(0) / ELF64_RELA_SIZE as u64;
        if relative_count > relocation_count {
            return Err(LoadError::Format("DT_RELACOUNT exceeds DT_RELASZ"));
        }
    }
    let plt_address = info.plt_rela.unwrap_or(0);
    let plt_size = info.plt_rela_size.unwrap_or(0);
    if (plt_address == 0) != (plt_size == 0) {
        return Err(LoadError::Format("incomplete DT_JMPREL/DT_PLTRELSZ"));
    }
    if plt_size != 0 {
        if info.plt_relocation_kind != Some(DT_RELA as u64) {
            return Err(LoadError::Format("DT_PLTREL is not DT_RELA"));
        }
        let now = info.bind_now_tag
            || info.flags.is_some_and(|flags| flags & DF_BIND_NOW != 0)
            || info.flags_1.is_some_and(|flags| flags & DF_1_NOW != 0);
        if !now {
            return Err(LoadError::Capability(Capability::LazyBinding));
        }
    } else if info.plt_relocation_kind.is_some() {
        return Err(LoadError::Format("DT_PLTREL without DT_JMPREL"));
    }
    if let Some(flags) = info.flags
        && flags & !DF_BIND_NOW != 0
    {
        return Err(LoadError::Capability(Capability::DynamicFlags {
            tag: DT_FLAGS,
            value: flags,
        }));
    }
    if let Some(flags) = info.flags_1
        && flags & !DF_1_NOW != 0
    {
        return Err(LoadError::Capability(Capability::DynamicFlags {
            tag: DT_FLAGS_1,
            value: flags,
        }));
    }
    Ok(())
}

fn elf_flags_to_protection(flags: u32) -> c_int {
    let mut protection = PROT_NONE;
    if flags & PF_R != 0 {
        protection |= PROT_READ;
    }
    if flags & PF_W != 0 {
        protection |= PROT_WRITE;
    }
    if flags & PF_X != 0 {
        protection |= PROT_EXEC;
    }
    protection
}

fn set_once(slot: &mut Option<u64>, value: u64, duplicate: &'static str) -> Result<(), LoadError> {
    if slot.replace(value).is_some() {
        Err(LoadError::Format(duplicate))
    } else {
        Ok(())
    }
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, LoadError> {
    let data: [u8; 2] = checked_slice(bytes, offset, 2, "u16 read")?
        .try_into()
        .unwrap();
    Ok(u16::from_le_bytes(data))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, LoadError> {
    let data: [u8; 4] = checked_slice(bytes, offset, 4, "u32 read")?
        .try_into()
        .unwrap();
    Ok(u32::from_le_bytes(data))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64, LoadError> {
    let data: [u8; 8] = checked_slice(bytes, offset, 8, "u64 read")?
        .try_into()
        .unwrap();
    Ok(u64::from_le_bytes(data))
}

fn read_i64(bytes: &[u8], offset: usize) -> Result<i64, LoadError> {
    Ok(read_u64(bytes, offset)? as i64)
}

fn checked_slice<'a>(
    bytes: &'a [u8],
    offset: usize,
    size: usize,
    what: &'static str,
) -> Result<&'a [u8], LoadError> {
    let end = offset.checked_add(size).ok_or(LoadError::Bounds(what))?;
    bytes.get(offset..end).ok_or(LoadError::Bounds(what))
}

fn to_usize(value: u64, what: &'static str) -> Result<usize, LoadError> {
    usize::try_from(value).map_err(|_| LoadError::Bounds(what))
}

fn difference_to_usize(value: u64, base: u64) -> Result<usize, LoadError> {
    let difference = value
        .checked_sub(base)
        .ok_or(LoadError::Bounds("address below image base"))?;
    to_usize(difference, "address difference")
}

fn checked_signed_add(base: usize, addend: i64, what: &'static str) -> Result<usize, LoadError> {
    (base as i128)
        .checked_add(addend as i128)
        .and_then(|value| usize::try_from(value).ok())
        .ok_or(LoadError::Bounds(what))
}

fn elf_hash(name: &[u8]) -> u32 {
    let mut hash = 0_u32;
    for byte in name {
        hash = hash.wrapping_shl(4).wrapping_add(u32::from(*byte));
        let high = hash & 0xf000_0000;
        if high != 0 {
            hash ^= high >> 24;
        }
        hash &= !high;
    }
    hash
}

fn system_error(operation: &'static str) -> LoadError {
    LoadError::System {
        operation,
        code: std::io::Error::last_os_error().raw_os_error().unwrap_or(0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_truncated_header() {
        assert!(matches!(
            LoadedElf::load(&[0; 8]),
            Err(LoadError::Format(_))
        ));
    }

    #[test]
    fn checked_slice_rejects_overflow() {
        assert!(checked_slice(&[0; 4], usize::MAX, 2, "test").is_err());
    }

    #[test]
    fn rejects_non_elf() {
        assert!(matches!(
            LoadedElf::load(&[0; ELF64_EHDR_SIZE]),
            Err(LoadError::Format("bad magic"))
        ));
    }
}
