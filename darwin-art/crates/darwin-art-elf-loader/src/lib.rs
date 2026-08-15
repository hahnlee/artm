use std::error::Error as StdError;
use std::ffi::{c_int, c_void};
use std::fmt;
use std::ptr::{self, NonNull};

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

const R_AARCH64_RELATIVE: u32 = 1027;
const SHN_UNDEF: u16 = 0;
const STB_GLOBAL: u8 = 1;
const STB_WEAK: u8 = 2;
const STT_NOTYPE: u8 = 0;
const STT_FUNC: u8 = 2;
const STT_TLS: u8 = 6;

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
    NeededLibrary,
    Tls,
    PltRelocations,
    RelRelocations,
    RelrRelocations,
    UnsupportedRelocation { relocation_type: u32, symbol: u32 },
    UndefinedSymbol(String),
    DynamicInitializer,
    Finalizers,
    PreinitArray,
    TextRelocations,
    Rpath,
    UnknownDynamicTag(i64),
    MissingSysvHash,
}

#[derive(Debug)]
pub enum LoadError {
    Format(&'static str),
    Bounds(&'static str),
    Capability(Capability),
    Protection(&'static str),
    SymbolNotFound(String),
    InvalidSymbol(String),
    InitializersAlreadyRun,
    InitializersNotRun,
    System { operation: &'static str, code: i32 },
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
    hash: Option<u64>,
    string_table: Option<u64>,
    string_size: Option<u64>,
    symbol_table: Option<u64>,
    symbol_entry_size: Option<u64>,
    rela: Option<u64>,
    rela_size: Option<u64>,
    rela_entry_size: Option<u64>,
    relative_relocation_count: Option<u64>,
    init_array: Option<u64>,
    init_array_size: Option<u64>,
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
    initializers_run: bool,
}

impl LoadedElf {
    pub fn load(bytes: &[u8]) -> Result<Self, LoadError> {
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
            initializers_run: false,
        };
        std::mem::forget(mapping);

        loaded.validate_symbols_and_apply_relocations()?;
        loaded.apply_final_protections(parsed.page_size, &parsed.page_protections)?;
        Ok(loaded)
    }

    pub fn run_initializers(&mut self) -> Result<(), LoadError> {
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
        Ok(())
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

    fn validate_symbols_and_apply_relocations(&mut self) -> Result<(), LoadError> {
        let symbol_count = self.symbol_count()?;
        for index in 1..symbol_count {
            let symbol = self.dynamic_symbol(index)?;
            let name = self.dynamic_string(symbol.name_offset)?;
            if symbol.kind == STT_TLS {
                return Err(LoadError::Capability(Capability::Tls));
            }
            if symbol.section_index == SHN_UNDEF && !name.is_empty() {
                return Err(LoadError::Capability(Capability::UndefinedSymbol(name)));
            }
        }

        let rela_address = self.dynamic.rela.unwrap_or(0);
        let rela_size = self.dynamic.rela_size.unwrap_or(0);
        if (rela_address == 0) != (rela_size == 0) {
            return Err(LoadError::Format("incomplete DT_RELA metadata"));
        }
        if rela_size == 0 {
            return Ok(());
        }
        let entry_size = self
            .dynamic
            .rela_entry_size
            .unwrap_or(ELF64_RELA_SIZE as u64);
        if entry_size != ELF64_RELA_SIZE as u64 || rela_size % entry_size != 0 {
            return Err(LoadError::Format("invalid DT_RELAENT/DT_RELASZ"));
        }
        self.require_loaded_range(rela_address, rela_size, Some(PF_R), "DT_RELA")?;
        for index in 0..(rela_size / entry_size) {
            let address = rela_address
                .checked_add(index * entry_size)
                .ok_or(LoadError::Bounds("DT_RELA entry overflow"))?;
            let offset = self.read_loaded_u64(address)?;
            let info = self.read_loaded_u64(address + 8)?;
            let addend = self.read_loaded_i64(address + 16)?;
            let relocation_type = info as u32;
            let symbol = (info >> 32) as u32;
            if relocation_type != R_AARCH64_RELATIVE || symbol != 0 {
                return Err(LoadError::Capability(Capability::UnsupportedRelocation {
                    relocation_type,
                    symbol,
                }));
            }
            self.require_loaded_range(offset, 8, Some(PF_W), "R_AARCH64_RELATIVE target")?;
            let value = (self.mapping.as_ptr() as i128)
                .checked_add(addend as i128 - self.minimum_page as i128)
                .and_then(|value| usize::try_from(value).ok())
                .ok_or(LoadError::Bounds("R_AARCH64_RELATIVE value overflow"))?;
            let destination = self.loaded_pointer(offset, 8)?;
            // SAFETY: require_loaded_range proves an aligned-size writable destination.
            unsafe { ptr::write_unaligned(destination.cast::<usize>(), value) };
        }
        Ok(())
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
                || symbol.visibility != 0
                || symbol.value == 0
            {
                return Err(LoadError::InvalidSymbol(requested.to_owned()));
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

    fn read_loaded_u64(&self, address: u64) -> Result<u64, LoadError> {
        read_u64(self.loaded_slice(address, 8)?, 0)
    }

    fn read_loaded_i64(&self, address: u64) -> Result<i64, LoadError> {
        Ok(self.read_loaded_u64(address)? as i64)
    }
}

impl Drop for LoadedElf {
    fn drop(&mut self) {
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

fn parse_image(bytes: &[u8]) -> Result<ParsedImage, LoadError> {
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
            PT_TLS => return Err(LoadError::Capability(Capability::Tls)),
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
            DT_INIT_ARRAY => set_once(&mut info.init_array, value, "duplicate DT_INIT_ARRAY")?,
            DT_INIT_ARRAYSZ => set_once(
                &mut info.init_array_size,
                value,
                "duplicate DT_INIT_ARRAYSZ",
            )?,
            DT_NEEDED => return Err(LoadError::Capability(Capability::NeededLibrary)),
            DT_PLTRELSZ | DT_PLTGOT | DT_PLTREL | DT_JMPREL if value != 0 => {
                return Err(LoadError::Capability(Capability::PltRelocations));
            }
            DT_REL | DT_RELSZ | DT_RELENT if value != 0 => {
                return Err(LoadError::Capability(Capability::RelRelocations));
            }
            DT_RELR | DT_RELRSZ | DT_RELRENT if value != 0 => {
                return Err(LoadError::Capability(Capability::RelrRelocations));
            }
            DT_INIT if value != 0 => {
                return Err(LoadError::Capability(Capability::DynamicInitializer));
            }
            DT_FINI | DT_FINI_ARRAY | DT_FINI_ARRAYSZ if value != 0 => {
                return Err(LoadError::Capability(Capability::Finalizers));
            }
            DT_PREINIT_ARRAY | DT_PREINIT_ARRAYSZ if value != 0 => {
                return Err(LoadError::Capability(Capability::PreinitArray));
            }
            DT_TEXTREL => return Err(LoadError::Capability(Capability::TextRelocations)),
            DT_RPATH | DT_RUNPATH => return Err(LoadError::Capability(Capability::Rpath)),
            DT_GNU_HASH | DT_SONAME | DT_SYMBOLIC | DT_DEBUG | DT_BIND_NOW | DT_FLAGS
            | DT_FLAGS_1 | DT_VERSYM | DT_VERDEF | DT_VERDEFNUM | DT_VERNEED | DT_VERNEEDNUM => {}
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
