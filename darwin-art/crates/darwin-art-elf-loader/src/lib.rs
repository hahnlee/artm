use std::collections::{HashMap, HashSet};
use std::error::Error as StdError;
use std::ffi::{c_int, c_void};
use std::fmt;
use std::num::NonZeroUsize;
use std::ops::Range;
use std::ptr::{self, NonNull};
use std::sync::Arc;

mod ffi;
mod mapping;
mod namespace;
mod parser;
mod tls;

use parser::{
    parse_dynamic, parse_dynamic_with_policy, parse_image, parse_image_with_policy,
    validate_dynamic_capabilities,
};

use mapping::Mapping;
use tls::{
    MAX_TLS_ALIGNMENT, MAX_TLS_SIZE, TlsDescriptor, TlsDescriptorContext, TlsModule,
    darwin_art_tlsdesc_resolver, register_tls_descriptor, release_current_thread_tls,
    unregister_tls_descriptors,
};

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
// AArch64 ELF uses this zero-valued presence tag to state that PLT entries are
// compatible with Branch Target Identification. It does not request the
// loader to enable a process feature or rewrite the PLT.
const DT_AARCH64_BTI_PLT: i64 = 0x7000_0001;

const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;
const R_AARCH64_TLSDESC: u32 = 1031;
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
    TlsInUse {
        active_threads: usize,
    },
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
            Self::TlsInUse { active_threads } => write!(
                formatter,
                "ELF TLS module still has {active_threads} live thread allocation(s)"
            ),
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
    aarch64_bti_plt: Option<u64>,
}

#[derive(Debug)]
struct ParsedImage {
    loads: Vec<ProgramHeader>,
    dynamic: ProgramHeader,
    tls: Option<ProgramHeader>,
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
    tls_header: Option<ProgramHeader>,
    tls_module: Option<Arc<TlsModule>>,
    tls_descriptor_tokens: Vec<u64>,
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

// SAFETY: LoadedElf exclusively owns an mmap reservation. Guest TLS allocations retain their
// synchronized module owner and teardown aborts rather than unmapping code with live threads.
// Resolver callbacks are not retained and mutation is restricted to &mut self. C consumers
// additionally serialize the value behind a Mutex.
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
                    mapping.pointer().as_ptr().add(destination_offset),
                    file_size,
                );
                ptr::write_bytes(
                    mapping
                        .pointer()
                        .as_ptr()
                        .add(destination_offset + file_size),
                    0,
                    memory_size - file_size,
                );
            }
        }

        let dynamic = parse_dynamic(bytes, &parsed.dynamic)?;
        validate_dynamic_capabilities(&dynamic)?;
        let tls_module = parsed
            .tls
            .map(|header| TlsModule::new(header, bytes))
            .transpose()?;

        let mut loaded = Self {
            mapping: mapping.pointer(),
            mapping_size: mapping.length(),
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
            tls_header: parsed.tls,
            tls_module,
            tls_descriptor_tokens: Vec::new(),
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
        self.refresh_tls_template()?;
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
                if symbol.section_index == SHN_UNDEF {
                    // Cross-module TLS needs graph-wide module indexing. This bounded slice owns
                    // only definitions in this image and deliberately fails imported TLS closed.
                    return Err(LoadError::Capability(Capability::Tls));
                }
                self.validate_defined_tls_symbol(&symbol)?;
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
            if relocation_type == R_AARCH64_TLSDESC {
                if plt_only {
                    return Err(LoadError::Capability(Capability::UnsupportedRelocation {
                        relocation_type,
                        symbol: symbol_index,
                    }));
                }
                self.apply_tlsdesc_relocation(offset, symbol_index, symbol_count, addend)?;
                continue;
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

    fn validate_defined_tls_symbol(&self, symbol: &DynamicSymbol) -> Result<(), LoadError> {
        let module = self
            .tls_module
            .as_ref()
            .ok_or(LoadError::Format("STT_TLS definition without PT_TLS"))?;
        let start = usize::try_from(symbol.value)
            .map_err(|_| LoadError::Bounds("STT_TLS symbol offset"))?;
        let size = usize::try_from(symbol.size.max(1))
            .map_err(|_| LoadError::Bounds("STT_TLS symbol size"))?;
        if start
            .checked_add(size)
            .is_none_or(|end| end > module.memory_size)
        {
            return Err(LoadError::Bounds("STT_TLS symbol outside PT_TLS"));
        }
        Ok(())
    }

    fn apply_tlsdesc_relocation(
        &mut self,
        target: u64,
        symbol_index: u32,
        symbol_count: u32,
        addend: i64,
    ) -> Result<(), LoadError> {
        if target % 8 != 0 {
            return Err(LoadError::Format("unaligned R_AARCH64_TLSDESC target"));
        }
        self.require_loaded_range(target, 16, Some(PF_W), "R_AARCH64_TLSDESC target")?;
        let module = self
            .tls_module
            .as_ref()
            .cloned()
            .ok_or(LoadError::Format("R_AARCH64_TLSDESC without PT_TLS"))?;
        let base = if symbol_index == 0 {
            0_i128
        } else {
            if symbol_index >= symbol_count {
                return Err(LoadError::Bounds("TLS relocation symbol index"));
            }
            let symbol = self.dynamic_symbol(symbol_index)?;
            if symbol.kind != STT_TLS
                || symbol.section_index == SHN_UNDEF
                || symbol.section_index == SHN_ABS
            {
                return Err(LoadError::InvalidSymbol(format!(
                    "dynsym[{symbol_index}] is not a local TLS definition"
                )));
            }
            self.validate_defined_tls_symbol(&symbol)?;
            i128::from(symbol.value)
        };
        let offset = base
            .checked_add(i128::from(addend))
            .and_then(|value| usize::try_from(value).ok())
            .ok_or(LoadError::Bounds("R_AARCH64_TLSDESC offset overflow"))?;
        if offset >= module.memory_size {
            return Err(LoadError::Bounds("R_AARCH64_TLSDESC outside PT_TLS"));
        }
        let destination = self.loaded_pointer(target, 16)?.cast::<TlsDescriptor>();
        let token = register_tls_descriptor(TlsDescriptorContext {
            module,
            offset,
            descriptor_address: destination as usize,
        })?;
        let descriptor = TlsDescriptor {
            resolver: darwin_art_tlsdesc_resolver as usize,
            token,
        };
        // SAFETY: target is a validated writable pair of 8-byte GOT entries. The second word is
        // an opaque integer; Rust resolves it only through the checked process registry.
        unsafe { ptr::write_unaligned(destination, descriptor) };
        self.tls_descriptor_tokens.push(token);
        Ok(())
    }

    fn refresh_tls_template(&self) -> Result<(), LoadError> {
        let (Some(header), Some(module)) = (self.tls_header, self.tls_module.as_ref()) else {
            return Ok(());
        };
        let template = self.loaded_slice(
            header.virtual_address,
            to_usize(header.file_size, "PT_TLS file size")?,
        )?;
        let mut owned = module
            .template
            .lock()
            .map_err(|_| LoadError::Protection("poisoned TLS template"))?;
        owned.clear();
        owned.extend_from_slice(template);
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
        if let Some(lifecycle) = self.dso_lifecycle.take()
            && lifecycle.finalize_image(self.mapping_range()).is_err()
        {
            // Continuing could unmap code that still owns live callbacks.
            std::process::abort();
        }
        self.run_finalizers_once();
        unregister_tls_descriptors(&self.tls_descriptor_tokens);
        if let Some(module) = &self.tls_module {
            // Guest finalizers may touch this image's TLS. Release the unloading thread's block
            // synchronously, then seal allocation. Any other live thread makes unmapping unsafe.
            release_current_thread_tls(module.id);
            if module.seal_for_unload().is_err() {
                std::process::abort();
            }
        }
        // SAFETY: LoadedElf exclusively owns this complete mmap reservation.
        let result = unsafe { munmap(self.mapping.as_ptr().cast(), self.mapping_size) };
        debug_assert_eq!(result, 0, "munmap failed while dropping LoadedElf");
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
