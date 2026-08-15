use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::ops::Range;

const ELF_HEADER_SIZE: usize = 64;
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const EV_CURRENT: u8 = 1;
const ET_DYN: u16 = 3;
const EM_AARCH64: u16 = 183;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_TLS: u32 = 7;
const PT_GNU_STACK: u32 = 0x6474_e551;
const PT_GNU_RELRO: u32 = 0x6474_e552;
const PF_X: u32 = 1;

const SHT_NOBITS: u32 = 8;
const SHT_DYNSYM: u32 = 11;
const SHN_UNDEF: u16 = 0;

const DT_NULL: i64 = 0;
const DT_NEEDED: i64 = 1;
const DT_PLTRELSZ: i64 = 2;
const DT_HASH: i64 = 4;
const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_RELAENT: i64 = 9;
const DT_STRSZ: i64 = 10;
const DT_SYMENT: i64 = 11;
const DT_INIT: i64 = 12;
const DT_SONAME: i64 = 14;
const DT_RPATH: i64 = 15;
const DT_REL: i64 = 17;
const DT_RELSZ: i64 = 18;
const DT_PLTREL: i64 = 20;
const DT_TEXTREL: i64 = 22;
const DT_JMPREL: i64 = 23;
const DT_INIT_ARRAY: i64 = 25;
const DT_INIT_ARRAYSZ: i64 = 27;
const DT_RUNPATH: i64 = 29;
const DT_FLAGS: i64 = 30;
const DT_RELRSZ: i64 = 35;
const DT_RELR: i64 = 36;
const DT_RELRENT: i64 = 37;
const DT_GNU_HASH: i64 = 0x6fff_fef5;
const DT_FLAGS_1: i64 = 0x6fff_fffb;
const DT_ANDROID_REL: i64 = 0x6000_000f;
const DT_ANDROID_RELSZ: i64 = 0x6000_0010;
const DT_ANDROID_RELA: i64 = 0x6000_0011;
const DT_ANDROID_RELASZ: i64 = 0x6000_0012;
const DF_TEXTREL: u64 = 0x4;

const ELF64_PHDR_SIZE: u16 = 56;
const ELF64_SHDR_SIZE: u16 = 64;
const ELF64_SYM_SIZE: u64 = 24;
const ELF64_RELA_SIZE: u64 = 24;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InspectError {
    pub code: &'static str,
    pub message: String,
}

impl InspectError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

impl fmt::Display for InspectError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}: {}", self.code, self.message)
    }
}

impl std::error::Error for InspectError {}

type Result<T> = std::result::Result<T, InspectError>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProgramSegment {
    pub kind: u32,
    pub flags: u32,
    pub offset: u64,
    pub virtual_address: u64,
    pub file_size: u64,
    pub memory_size: u64,
    pub alignment: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DynamicSummary {
    pub needed: Vec<String>,
    pub soname: Option<String>,
    pub rpath: Option<String>,
    pub runpath: Option<String>,
    pub sysv_hash: bool,
    pub gnu_hash: bool,
    pub symbol_count_source: Option<&'static str>,
    pub init: Option<u64>,
    pub init_array_address: Option<u64>,
    pub init_array: Vec<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Symbol {
    pub name: String,
    pub binding: u8,
    pub symbol_type: u8,
    pub visibility: u8,
    pub section_index: u16,
    pub value: u64,
    pub size: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RelocationType {
    pub number: u32,
    pub name: &'static str,
    pub count: usize,
    pub supported_by_policy: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SvcInstruction {
    pub file_offset: u64,
    pub virtual_address: u64,
    pub immediate: u16,
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct UnsupportedCapability {
    pub code: &'static str,
    pub detail: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Inspection {
    pub file_size: usize,
    pub elf_type: u16,
    pub entry: u64,
    pub load_segments: Vec<ProgramSegment>,
    pub dynamic_segments: Vec<ProgramSegment>,
    pub tls_segments: Vec<ProgramSegment>,
    pub gnu_relro_segments: Vec<ProgramSegment>,
    pub dynamic: DynamicSummary,
    pub dynamic_symbol_count: usize,
    pub imports: Vec<Symbol>,
    pub exports: Vec<Symbol>,
    pub rela_count: usize,
    pub relocation_types: Vec<RelocationType>,
    pub svc_instructions: Vec<SvcInstruction>,
    pub requirements: Vec<&'static str>,
    pub unsupported_capabilities: Vec<UnsupportedCapability>,
}

#[derive(Clone, Copy, Debug)]
struct ElfHeader {
    elf_type: u16,
    entry: u64,
    program_offset: u64,
    section_offset: u64,
    program_entry_size: u16,
    program_count: u16,
    section_entry_size: u16,
    section_count: u16,
}

#[derive(Clone, Copy, Debug)]
struct SectionHeader {
    kind: u32,
    offset: u64,
    size: u64,
    entry_size: u64,
}

#[derive(Clone, Copy, Debug)]
struct DynamicEntry {
    tag: i64,
    value: u64,
}

#[derive(Clone, Copy, Debug)]
struct RawRela {
    relocation_type: u32,
    symbol_index: u32,
}

struct Reader<'a> {
    bytes: &'a [u8],
}

impl<'a> Reader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes }
    }

    fn range(&self, offset: u64, size: u64, what: &str) -> Result<Range<usize>> {
        let end = offset.checked_add(size).ok_or_else(|| {
            InspectError::new("bounds-overflow", format!("{what} range overflows"))
        })?;
        let start = usize::try_from(offset).map_err(|_| {
            InspectError::new(
                "bounds-overflow",
                format!("{what} offset does not fit usize"),
            )
        })?;
        let end = usize::try_from(end).map_err(|_| {
            InspectError::new("bounds-overflow", format!("{what} end does not fit usize"))
        })?;
        if end > self.bytes.len() {
            return Err(InspectError::new(
                "truncated",
                format!(
                    "{what} range {start}..{end} exceeds file size {}",
                    self.bytes.len()
                ),
            ));
        }
        Ok(start..end)
    }

    fn slice(&self, offset: u64, size: u64, what: &str) -> Result<&'a [u8]> {
        Ok(&self.bytes[self.range(offset, size, what)?])
    }

    fn u16(&self, offset: u64, what: &str) -> Result<u16> {
        let bytes = self.slice(offset, 2, what)?;
        Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
    }

    fn u32(&self, offset: u64, what: &str) -> Result<u32> {
        let bytes = self.slice(offset, 4, what)?;
        Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
    }

    fn u64(&self, offset: u64, what: &str) -> Result<u64> {
        let bytes = self.slice(offset, 8, what)?;
        Ok(u64::from_le_bytes(bytes.try_into().unwrap()))
    }

    fn i64(&self, offset: u64, what: &str) -> Result<i64> {
        let bytes = self.slice(offset, 8, what)?;
        Ok(i64::from_le_bytes(bytes.try_into().unwrap()))
    }
}

fn checked_table_offset(base: u64, index: usize, stride: u64, what: &str) -> Result<u64> {
    let index = u64::try_from(index)
        .map_err(|_| InspectError::new("bounds-overflow", format!("{what} index overflow")))?;
    base.checked_add(index.checked_mul(stride).ok_or_else(|| {
        InspectError::new(
            "bounds-overflow",
            format!("{what} index multiplication overflow"),
        )
    })?)
    .ok_or_else(|| InspectError::new("bounds-overflow", format!("{what} offset overflow")))
}

fn parse_header(reader: &Reader<'_>) -> Result<ElfHeader> {
    if reader.bytes.len() < ELF_HEADER_SIZE {
        return Err(InspectError::new(
            "truncated",
            "ELF header is shorter than 64 bytes",
        ));
    }
    if &reader.bytes[0..4] != b"\x7fELF" {
        return Err(InspectError::new("invalid-magic", "missing ELF magic"));
    }
    if reader.bytes[4] != ELFCLASS64 {
        return Err(InspectError::new(
            "unsupported-class",
            "expected ELFCLASS64",
        ));
    }
    if reader.bytes[5] != ELFDATA2LSB {
        return Err(InspectError::new(
            "unsupported-endianness",
            "expected little-endian ELF",
        ));
    }
    if reader.bytes[6] != EV_CURRENT {
        return Err(InspectError::new(
            "unsupported-ident-version",
            "unexpected ELF ident version",
        ));
    }
    let machine = reader.u16(18, "e_machine")?;
    if machine != EM_AARCH64 {
        return Err(InspectError::new(
            "unsupported-machine",
            format!("expected EM_AARCH64 ({EM_AARCH64}), got {machine}"),
        ));
    }
    let version = reader.u32(20, "e_version")?;
    if version != u32::from(EV_CURRENT) {
        return Err(InspectError::new(
            "unsupported-version",
            format!("unexpected e_version {version}"),
        ));
    }
    let header_size = reader.u16(52, "e_ehsize")?;
    if header_size != ELF_HEADER_SIZE as u16 {
        return Err(InspectError::new(
            "invalid-header-size",
            format!("expected e_ehsize 64, got {header_size}"),
        ));
    }
    let program_count = reader.u16(56, "e_phnum")?;
    if program_count == 0xffff {
        return Err(InspectError::new(
            "unsupported-extended-numbering",
            "PN_XNUM program-header numbering is not supported",
        ));
    }
    let section_offset = reader.u64(40, "e_shoff")?;
    let section_count = reader.u16(60, "e_shnum")?;
    if section_offset != 0 && section_count == 0 {
        return Err(InspectError::new(
            "unsupported-extended-numbering",
            "extended section-header numbering is not supported",
        ));
    }
    Ok(ElfHeader {
        elf_type: reader.u16(16, "e_type")?,
        entry: reader.u64(24, "e_entry")?,
        program_offset: reader.u64(32, "e_phoff")?,
        section_offset,
        program_entry_size: reader.u16(54, "e_phentsize")?,
        program_count,
        section_entry_size: reader.u16(58, "e_shentsize")?,
        section_count,
    })
}

fn parse_program_headers(reader: &Reader<'_>, header: ElfHeader) -> Result<Vec<ProgramSegment>> {
    if header.program_count == 0 {
        return Ok(Vec::new());
    }
    if header.program_entry_size < ELF64_PHDR_SIZE {
        return Err(InspectError::new(
            "invalid-program-header-size",
            format!(
                "e_phentsize {} is smaller than 56",
                header.program_entry_size
            ),
        ));
    }
    let table_size = u64::from(header.program_entry_size)
        .checked_mul(u64::from(header.program_count))
        .ok_or_else(|| {
            InspectError::new("bounds-overflow", "program-header table size overflow")
        })?;
    reader.range(header.program_offset, table_size, "program-header table")?;
    let mut segments = Vec::with_capacity(usize::from(header.program_count));
    for index in 0..usize::from(header.program_count) {
        let offset = checked_table_offset(
            header.program_offset,
            index,
            u64::from(header.program_entry_size),
            "program header",
        )?;
        let segment = ProgramSegment {
            kind: reader.u32(offset, "p_type")?,
            flags: reader.u32(offset + 4, "p_flags")?,
            offset: reader.u64(offset + 8, "p_offset")?,
            virtual_address: reader.u64(offset + 16, "p_vaddr")?,
            file_size: reader.u64(offset + 32, "p_filesz")?,
            memory_size: reader.u64(offset + 40, "p_memsz")?,
            alignment: reader.u64(offset + 48, "p_align")?,
        };
        reader.range(segment.offset, segment.file_size, "program segment")?;
        if segment.kind == PT_LOAD {
            if segment.file_size > segment.memory_size {
                return Err(InspectError::new(
                    "invalid-load-segment",
                    "PT_LOAD p_filesz exceeds p_memsz",
                ));
            }
            if segment.alignment > 1 {
                if !segment.alignment.is_power_of_two() {
                    return Err(InspectError::new(
                        "invalid-load-alignment",
                        format!(
                            "PT_LOAD alignment {} is not a power of two",
                            segment.alignment
                        ),
                    ));
                }
                if segment.offset % segment.alignment != segment.virtual_address % segment.alignment
                {
                    return Err(InspectError::new(
                        "invalid-load-congruence",
                        "PT_LOAD p_offset and p_vaddr are incongruent",
                    ));
                }
            }
        }
        if segment.kind == PT_TLS {
            if segment.file_size > segment.memory_size {
                return Err(InspectError::new(
                    "invalid-tls-segment",
                    "PT_TLS p_filesz exceeds p_memsz",
                ));
            }
            if segment.alignment > 1 && !segment.alignment.is_power_of_two() {
                return Err(InspectError::new(
                    "invalid-tls-alignment",
                    format!(
                        "PT_TLS alignment {} is not a power of two",
                        segment.alignment
                    ),
                ));
            }
        }
        segments.push(segment);
    }
    Ok(segments)
}

fn parse_section_headers(reader: &Reader<'_>, header: ElfHeader) -> Result<Vec<SectionHeader>> {
    if header.section_count == 0 {
        return Ok(Vec::new());
    }
    if header.section_entry_size < ELF64_SHDR_SIZE {
        return Err(InspectError::new(
            "invalid-section-header-size",
            format!(
                "e_shentsize {} is smaller than 64",
                header.section_entry_size
            ),
        ));
    }
    let table_size = u64::from(header.section_entry_size)
        .checked_mul(u64::from(header.section_count))
        .ok_or_else(|| {
            InspectError::new("bounds-overflow", "section-header table size overflow")
        })?;
    reader.range(header.section_offset, table_size, "section-header table")?;
    let mut sections = Vec::with_capacity(usize::from(header.section_count));
    for index in 0..usize::from(header.section_count) {
        let offset = checked_table_offset(
            header.section_offset,
            index,
            u64::from(header.section_entry_size),
            "section header",
        )?;
        let section = SectionHeader {
            kind: reader.u32(offset + 4, "sh_type")?,
            offset: reader.u64(offset + 24, "sh_offset")?,
            size: reader.u64(offset + 32, "sh_size")?,
            entry_size: reader.u64(offset + 56, "sh_entsize")?,
        };
        if section.kind != SHT_NOBITS {
            reader.range(section.offset, section.size, "section contents")?;
        }
        sections.push(section);
    }
    Ok(sections)
}

fn parse_dynamic(reader: &Reader<'_>, segment: &ProgramSegment) -> Result<Vec<DynamicEntry>> {
    if segment.file_size % 16 != 0 {
        return Err(InspectError::new(
            "invalid-dynamic-size",
            "PT_DYNAMIC p_filesz is not a multiple of Elf64_Dyn",
        ));
    }
    let count = usize::try_from(segment.file_size / 16)
        .map_err(|_| InspectError::new("bounds-overflow", "dynamic-entry count overflow"))?;
    let mut entries = Vec::new();
    let mut terminated = false;
    for index in 0..count {
        let offset = checked_table_offset(segment.offset, index, 16, "dynamic entry")?;
        let entry = DynamicEntry {
            tag: reader.i64(offset, "d_tag")?,
            value: reader.u64(offset + 8, "d_val")?,
        };
        if entry.tag == DT_NULL {
            terminated = true;
            break;
        }
        entries.push(entry);
    }
    if !terminated {
        return Err(InspectError::new(
            "unterminated-dynamic",
            "PT_DYNAMIC has no DT_NULL",
        ));
    }
    Ok(entries)
}

fn first_dynamic(entries: &[DynamicEntry], tag: i64) -> Result<Option<u64>> {
    let mut value = None;
    for entry in entries.iter().filter(|entry| entry.tag == tag) {
        if let Some(previous) = value {
            if previous != entry.value {
                return Err(InspectError::new(
                    "conflicting-dynamic-tag",
                    format!("dynamic tag {tag:#x} has conflicting values"),
                ));
            }
        }
        value = Some(entry.value);
    }
    Ok(value)
}

fn load_segments(segments: &[ProgramSegment]) -> impl Iterator<Item = &ProgramSegment> {
    segments.iter().filter(|segment| segment.kind == PT_LOAD)
}

fn virtual_to_file(
    segments: &[ProgramSegment],
    address: u64,
    size: u64,
    what: &str,
) -> Result<u64> {
    let end = address.checked_add(size).ok_or_else(|| {
        InspectError::new("bounds-overflow", format!("{what} virtual range overflow"))
    })?;
    for segment in load_segments(segments) {
        let file_end = segment
            .virtual_address
            .checked_add(segment.file_size)
            .ok_or_else(|| {
                InspectError::new("bounds-overflow", "PT_LOAD virtual file range overflow")
            })?;
        if address >= segment.virtual_address && end <= file_end {
            return segment
                .offset
                .checked_add(address - segment.virtual_address)
                .ok_or_else(|| {
                    InspectError::new("bounds-overflow", format!("{what} file offset overflow"))
                });
        }
    }
    Err(InspectError::new(
        "unmapped-virtual-address",
        format!("{what} address {address:#x} size {size} is not file-backed by PT_LOAD"),
    ))
}

fn readable_virtual_bytes(segments: &[ProgramSegment], address: u64, what: &str) -> Result<u64> {
    for segment in load_segments(segments) {
        let end = segment
            .virtual_address
            .checked_add(segment.file_size)
            .ok_or_else(|| {
                InspectError::new("bounds-overflow", "PT_LOAD virtual file range overflow")
            })?;
        if address >= segment.virtual_address && address <= end {
            return Ok(end - address);
        }
    }
    Err(InspectError::new(
        "unmapped-virtual-address",
        format!("{what} address {address:#x} is not file-backed by PT_LOAD"),
    ))
}

fn dynamic_string(table: &[u8], offset: u64, what: &str) -> Result<String> {
    let offset = usize::try_from(offset).map_err(|_| {
        InspectError::new("bounds-overflow", format!("{what} string offset overflow"))
    })?;
    if offset >= table.len() {
        return Err(InspectError::new(
            "invalid-dynamic-string",
            format!(
                "{what} string offset {offset} exceeds DT_STRSZ {}",
                table.len()
            ),
        ));
    }
    let tail = &table[offset..];
    let length = tail.iter().position(|byte| *byte == 0).ok_or_else(|| {
        InspectError::new(
            "unterminated-dynamic-string",
            format!("{what} string is not NUL terminated"),
        )
    })?;
    Ok(String::from_utf8_lossy(&tail[..length]).into_owned())
}

fn section_dynsym_count(sections: &[SectionHeader]) -> Result<Option<usize>> {
    let mut count = None;
    for section in sections.iter().filter(|section| section.kind == SHT_DYNSYM) {
        if section.entry_size != ELF64_SYM_SIZE || section.size % ELF64_SYM_SIZE != 0 {
            return Err(InspectError::new(
                "invalid-dynsym-section",
                "SHT_DYNSYM size or entry size is invalid",
            ));
        }
        let current = usize::try_from(section.size / ELF64_SYM_SIZE)
            .map_err(|_| InspectError::new("bounds-overflow", "dynsym section count overflow"))?;
        if count.replace(current).is_some() {
            return Err(InspectError::new(
                "multiple-dynsym-sections",
                "multiple SHT_DYNSYM sections",
            ));
        }
    }
    Ok(count)
}

fn sysv_hash_count(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    address: u64,
) -> Result<usize> {
    let offset = virtual_to_file(segments, address, 8, "DT_HASH")?;
    let bucket_count = u64::from(reader.u32(offset, "DT_HASH nbucket")?);
    let chain_count = u64::from(reader.u32(offset + 4, "DT_HASH nchain")?);
    let words = bucket_count
        .checked_add(chain_count)
        .ok_or_else(|| InspectError::new("bounds-overflow", "DT_HASH word count overflow"))?;
    let size = 8u64
        .checked_add(
            words.checked_mul(4).ok_or_else(|| {
                InspectError::new("bounds-overflow", "DT_HASH table size overflow")
            })?,
        )
        .ok_or_else(|| InspectError::new("bounds-overflow", "DT_HASH table size overflow"))?;
    let offset = virtual_to_file(segments, address, size, "DT_HASH")?;
    reader.range(offset, size, "DT_HASH")?;
    usize::try_from(chain_count)
        .map_err(|_| InspectError::new("bounds-overflow", "DT_HASH symbol count overflow"))
}

fn gnu_hash_count(reader: &Reader<'_>, segments: &[ProgramSegment], address: u64) -> Result<usize> {
    let offset = virtual_to_file(segments, address, 16, "DT_GNU_HASH")?;
    let bucket_count = u64::from(reader.u32(offset, "GNU hash nbuckets")?);
    let symbol_offset = u64::from(reader.u32(offset + 4, "GNU hash symoffset")?);
    let bloom_count = u64::from(reader.u32(offset + 8, "GNU hash bloom_size")?);
    if bloom_count == 0 || !bloom_count.is_power_of_two() {
        return Err(InspectError::new(
            "invalid-gnu-hash",
            format!("GNU hash bloom_size {bloom_count} is not a nonzero power of two"),
        ));
    }
    let bloom_bytes = bloom_count
        .checked_mul(8)
        .ok_or_else(|| InspectError::new("bounds-overflow", "GNU hash bloom size overflow"))?;
    let bucket_bytes = bucket_count
        .checked_mul(4)
        .ok_or_else(|| InspectError::new("bounds-overflow", "GNU hash bucket size overflow"))?;
    let bucket_address = address
        .checked_add(16)
        .and_then(|value| value.checked_add(bloom_bytes))
        .ok_or_else(|| InspectError::new("bounds-overflow", "GNU hash bucket address overflow"))?;
    let bucket_offset =
        virtual_to_file(segments, bucket_address, bucket_bytes, "GNU hash buckets")?;
    let chain_address = bucket_address
        .checked_add(bucket_bytes)
        .ok_or_else(|| InspectError::new("bounds-overflow", "GNU hash chain address overflow"))?;
    let chain_capacity = readable_virtual_bytes(segments, chain_address, "GNU hash chains")? / 4;
    let mut symbol_count = symbol_offset;
    for bucket_index in 0..bucket_count {
        let bucket = u64::from(reader.u32(bucket_offset + bucket_index * 4, "GNU hash bucket")?);
        if bucket == 0 {
            continue;
        }
        if bucket < symbol_offset {
            return Err(InspectError::new(
                "invalid-gnu-hash",
                format!("GNU hash bucket {bucket} precedes symoffset {symbol_offset}"),
            ));
        }
        let mut chain_index = bucket - symbol_offset;
        loop {
            if chain_index >= chain_capacity {
                return Err(InspectError::new(
                    "invalid-gnu-hash",
                    "GNU hash chain is not terminated inside its PT_LOAD",
                ));
            }
            let chain_offset = virtual_to_file(
                segments,
                chain_address + chain_index * 4,
                4,
                "GNU hash chain",
            )?;
            let chain = reader.u32(chain_offset, "GNU hash chain")?;
            symbol_count = symbol_count.max(symbol_offset + chain_index + 1);
            if chain & 1 != 0 {
                break;
            }
            chain_index += 1;
        }
    }
    usize::try_from(symbol_count)
        .map_err(|_| InspectError::new("bounds-overflow", "GNU hash symbol count overflow"))
}

fn determine_symbol_count(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    sections: &[SectionHeader],
    dynamic: &[DynamicEntry],
) -> Result<(usize, Option<&'static str>)> {
    let section_count = section_dynsym_count(sections)?;
    let sysv_count = first_dynamic(dynamic, DT_HASH)?
        .map(|address| sysv_hash_count(reader, segments, address))
        .transpose()?;
    let gnu_count = first_dynamic(dynamic, DT_GNU_HASH)?
        .map(|address| gnu_hash_count(reader, segments, address))
        .transpose()?;

    let mut authoritative = Vec::new();
    if let Some(count) = sysv_count {
        authoritative.push((count, "sysv-hash"));
    }
    if let Some(count) = section_count {
        authoritative.push((count, "section-dynsym"));
    }
    if let Some(count) = gnu_count {
        authoritative.push((count, "gnu-hash"));
    }
    if let Some((expected, _)) = authoritative.first() {
        if authoritative.iter().any(|(count, _)| count != expected) {
            return Err(InspectError::new(
                "inconsistent-symbol-count",
                format!("dynamic symbol count sources disagree: {authoritative:?}"),
            ));
        }
        return Ok((*expected, Some(authoritative[0].1)));
    }

    let symbol_address = first_dynamic(dynamic, DT_SYMTAB)?;
    let string_address = first_dynamic(dynamic, DT_STRTAB)?;
    if let (Some(symbol_address), Some(string_address)) = (symbol_address, string_address) {
        if string_address > symbol_address {
            let entry_size = first_dynamic(dynamic, DT_SYMENT)?.unwrap_or(ELF64_SYM_SIZE);
            if entry_size == ELF64_SYM_SIZE && (string_address - symbol_address) % entry_size == 0 {
                let count = usize::try_from((string_address - symbol_address) / entry_size)
                    .map_err(|_| {
                        InspectError::new("bounds-overflow", "inferred symbol count overflow")
                    })?;
                return Ok((count, Some("symtab-to-strtab-gap")));
            }
        }
    }
    Ok((0, None))
}

fn parse_symbols(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    dynamic: &[DynamicEntry],
    strings: &[u8],
    count: usize,
) -> Result<Vec<Symbol>> {
    if count == 0 {
        return Ok(Vec::new());
    }
    let symbol_address = first_dynamic(dynamic, DT_SYMTAB)?.ok_or_else(|| {
        InspectError::new(
            "missing-dynamic-tag",
            "symbol count exists without DT_SYMTAB",
        )
    })?;
    let entry_size = first_dynamic(dynamic, DT_SYMENT)?.unwrap_or(ELF64_SYM_SIZE);
    if entry_size != ELF64_SYM_SIZE {
        return Err(InspectError::new(
            "invalid-symbol-entry-size",
            format!("expected DT_SYMENT 24, got {entry_size}"),
        ));
    }
    let byte_count = u64::try_from(count)
        .ok()
        .and_then(|value| value.checked_mul(entry_size))
        .ok_or_else(|| {
            InspectError::new("bounds-overflow", "dynamic symbol table size overflow")
        })?;
    let symbol_offset = virtual_to_file(segments, symbol_address, byte_count, "DT_SYMTAB")?;
    let mut symbols = Vec::with_capacity(count);
    for index in 0..count {
        let offset = checked_table_offset(symbol_offset, index, entry_size, "dynamic symbol")?;
        let name_offset = u64::from(reader.u32(offset, "st_name")?);
        let info = reader.slice(offset + 4, 1, "st_info")?[0];
        let other = reader.slice(offset + 5, 1, "st_other")?[0];
        symbols.push(Symbol {
            name: dynamic_string(strings, name_offset, "dynamic symbol")?,
            binding: info >> 4,
            symbol_type: info & 0x0f,
            visibility: other & 0x03,
            section_index: reader.u16(offset + 6, "st_shndx")?,
            value: reader.u64(offset + 8, "st_value")?,
            size: reader.u64(offset + 16, "st_size")?,
        });
    }
    Ok(symbols)
}

fn parse_rela_table(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    address: u64,
    size: u64,
    entry_size: u64,
    symbol_count: usize,
    what: &str,
) -> Result<Vec<RawRela>> {
    if entry_size != ELF64_RELA_SIZE || size % entry_size != 0 {
        return Err(InspectError::new(
            "invalid-rela-table",
            format!("{what} size {size} or entry size {entry_size} is invalid"),
        ));
    }
    let offset = virtual_to_file(segments, address, size, what)?;
    let count = usize::try_from(size / entry_size)
        .map_err(|_| InspectError::new("bounds-overflow", format!("{what} count overflow")))?;
    let mut relocations = Vec::with_capacity(count);
    for index in 0..count {
        let entry_offset = checked_table_offset(offset, index, entry_size, what)?;
        let info = reader.u64(entry_offset + 8, "r_info")?;
        let relocation = RawRela {
            relocation_type: info as u32,
            symbol_index: (info >> 32) as u32,
        };
        if symbol_count != 0
            && usize::try_from(relocation.symbol_index).unwrap_or(usize::MAX) >= symbol_count
        {
            return Err(InspectError::new(
                "invalid-relocation-symbol",
                format!(
                    "{what} references symbol {} but count is {symbol_count}",
                    relocation.symbol_index
                ),
            ));
        }
        relocations.push(relocation);
    }
    Ok(relocations)
}

fn parse_relocations(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    dynamic: &[DynamicEntry],
    symbol_count: usize,
) -> Result<Vec<RawRela>> {
    let mut relocations = Vec::new();
    if let Some(address) = first_dynamic(dynamic, DT_RELA)? {
        let size = first_dynamic(dynamic, DT_RELASZ)?.ok_or_else(|| {
            InspectError::new("missing-dynamic-tag", "DT_RELA exists without DT_RELASZ")
        })?;
        let entry_size = first_dynamic(dynamic, DT_RELAENT)?.unwrap_or(ELF64_RELA_SIZE);
        relocations.extend(parse_rela_table(
            reader,
            segments,
            address,
            size,
            entry_size,
            symbol_count,
            "DT_RELA",
        )?);
    }
    if let Some(address) = first_dynamic(dynamic, DT_JMPREL)? {
        let size = first_dynamic(dynamic, DT_PLTRELSZ)?.ok_or_else(|| {
            InspectError::new(
                "missing-dynamic-tag",
                "DT_JMPREL exists without DT_PLTRELSZ",
            )
        })?;
        let format = first_dynamic(dynamic, DT_PLTREL)?.ok_or_else(|| {
            InspectError::new("missing-dynamic-tag", "DT_JMPREL exists without DT_PLTREL")
        })?;
        if format != DT_RELA as u64 {
            return Err(InspectError::new(
                "unsupported-plt-relocation-format",
                format!("AArch64 DT_PLTREL expected DT_RELA, got {format}"),
            ));
        }
        let entry_size = first_dynamic(dynamic, DT_RELAENT)?.unwrap_or(ELF64_RELA_SIZE);
        relocations.extend(parse_rela_table(
            reader,
            segments,
            address,
            size,
            entry_size,
            symbol_count,
            "DT_JMPREL",
        )?);
    }
    Ok(relocations)
}

fn parse_pointer_array(
    reader: &Reader<'_>,
    segments: &[ProgramSegment],
    address: Option<u64>,
    size: Option<u64>,
    what: &str,
) -> Result<Vec<u64>> {
    match (address, size) {
        (None, None) => Ok(Vec::new()),
        (Some(_), None) | (None, Some(_)) => Err(InspectError::new(
            "missing-dynamic-tag",
            format!("{what} address and size must appear together"),
        )),
        (Some(address), Some(size)) => {
            if size % 8 != 0 {
                return Err(InspectError::new(
                    "invalid-constructor-array",
                    format!("{what} size {size} is not pointer aligned"),
                ));
            }
            let offset = virtual_to_file(segments, address, size, what)?;
            let count = usize::try_from(size / 8).map_err(|_| {
                InspectError::new("bounds-overflow", format!("{what} count overflow"))
            })?;
            (0..count)
                .map(|index| reader.u64(offset + index as u64 * 8, what))
                .collect()
        }
    }
}

fn scan_svc(reader: &Reader<'_>, segments: &[ProgramSegment]) -> Result<Vec<SvcInstruction>> {
    let mut instructions = Vec::new();
    for segment in load_segments(segments).filter(|segment| segment.flags & PF_X != 0) {
        let mut delta = (4 - (segment.virtual_address & 3)) & 3;
        while delta
            .checked_add(4)
            .is_some_and(|end| end <= segment.file_size)
        {
            let file_offset = segment.offset.checked_add(delta).ok_or_else(|| {
                InspectError::new("bounds-overflow", "executable segment scan offset overflow")
            })?;
            let word = reader.u32(file_offset, "AArch64 instruction")?;
            if word & 0xffe0_001f == 0xd400_0001 {
                let virtual_address =
                    segment.virtual_address.checked_add(delta).ok_or_else(|| {
                        InspectError::new(
                            "bounds-overflow",
                            "executable segment virtual address overflow",
                        )
                    })?;
                instructions.push(SvcInstruction {
                    file_offset,
                    virtual_address,
                    immediate: ((word >> 5) & 0xffff) as u16,
                });
            }
            delta += 4;
        }
    }
    Ok(instructions)
}

fn relocation_name(number: u32) -> &'static str {
    match number {
        0 => "R_AARCH64_NONE",
        257 => "R_AARCH64_ABS64",
        258 => "R_AARCH64_ABS32",
        259 => "R_AARCH64_ABS16",
        260 => "R_AARCH64_PREL64",
        261 => "R_AARCH64_PREL32",
        262 => "R_AARCH64_PREL16",
        1024 => "R_AARCH64_COPY",
        1025 => "R_AARCH64_GLOB_DAT",
        1026 => "R_AARCH64_JUMP_SLOT",
        1027 => "R_AARCH64_RELATIVE",
        1028 => "R_AARCH64_TLS_DTPMOD64",
        1029 => "R_AARCH64_TLS_DTPREL64",
        1030 => "R_AARCH64_TLS_TPREL64",
        1031 => "R_AARCH64_TLSDESC",
        1032 => "R_AARCH64_IRELATIVE",
        _ => "R_AARCH64_UNKNOWN",
    }
}

fn relocation_supported(number: u32) -> bool {
    matches!(number, 0 | 257 | 1025 | 1026 | 1027 | 1028 | 1029 | 1030)
}

fn symbol_is_public(symbol: &Symbol) -> bool {
    matches!(symbol.binding, 1 | 2 | 10) && !symbol.name.is_empty()
}

fn add_unsupported(
    unsupported: &mut BTreeSet<UnsupportedCapability>,
    code: &'static str,
    detail: impl Into<String>,
) {
    unsupported.insert(UnsupportedCapability {
        code,
        detail: detail.into(),
    });
}

pub fn inspect(bytes: &[u8]) -> Result<Inspection> {
    let reader = Reader::new(bytes);
    let header = parse_header(&reader)?;
    let segments = parse_program_headers(&reader, header)?;
    let sections = parse_section_headers(&reader, header)?;
    let load: Vec<_> = load_segments(&segments).cloned().collect();
    let dynamic_segments: Vec<_> = segments
        .iter()
        .filter(|segment| segment.kind == PT_DYNAMIC)
        .cloned()
        .collect();
    let tls_segments: Vec<_> = segments
        .iter()
        .filter(|segment| segment.kind == PT_TLS)
        .cloned()
        .collect();
    let gnu_relro_segments: Vec<_> = segments
        .iter()
        .filter(|segment| segment.kind == PT_GNU_RELRO)
        .cloned()
        .collect();

    let mut unsupported = BTreeSet::new();
    if header.elf_type != ET_DYN {
        add_unsupported(
            &mut unsupported,
            "not-shared-object",
            format!(
                "Tier-1 policy requires ET_DYN, got e_type {}",
                header.elf_type
            ),
        );
    }
    if load.is_empty() {
        add_unsupported(
            &mut unsupported,
            "missing-load-segment",
            "no PT_LOAD segment",
        );
    }
    if dynamic_segments.is_empty() {
        add_unsupported(
            &mut unsupported,
            "missing-dynamic-segment",
            "no PT_DYNAMIC segment",
        );
    } else if dynamic_segments.len() > 1 {
        add_unsupported(
            &mut unsupported,
            "multiple-dynamic-segments",
            format!("found {} PT_DYNAMIC segments", dynamic_segments.len()),
        );
    }
    if tls_segments.len() > 1 {
        add_unsupported(
            &mut unsupported,
            "multiple-tls-segments",
            format!("found {} PT_TLS segments", tls_segments.len()),
        );
    }
    for stack in segments
        .iter()
        .filter(|segment| segment.kind == PT_GNU_STACK)
    {
        if stack.flags & PF_X != 0 {
            add_unsupported(
                &mut unsupported,
                "executable-stack",
                "PT_GNU_STACK requests execute permission",
            );
        }
    }

    let dynamic_entries = if let Some(segment) = dynamic_segments.first() {
        parse_dynamic(&reader, segment)?
    } else {
        Vec::new()
    };
    let string_size = first_dynamic(&dynamic_entries, DT_STRSZ)?.unwrap_or(0);
    let string_table = match first_dynamic(&dynamic_entries, DT_STRTAB)? {
        Some(address) => {
            let offset = virtual_to_file(&segments, address, string_size, "DT_STRTAB")?;
            reader.slice(offset, string_size, "DT_STRTAB")?
        }
        None if string_size == 0 => &[][..],
        None => {
            return Err(InspectError::new(
                "missing-dynamic-tag",
                "DT_STRSZ exists without DT_STRTAB",
            ))
        }
    };
    let needed = dynamic_entries
        .iter()
        .filter(|entry| entry.tag == DT_NEEDED)
        .map(|entry| dynamic_string(string_table, entry.value, "DT_NEEDED"))
        .collect::<Result<Vec<_>>>()?;
    let soname = first_dynamic(&dynamic_entries, DT_SONAME)?
        .map(|offset| dynamic_string(string_table, offset, "DT_SONAME"))
        .transpose()?;
    let rpath = first_dynamic(&dynamic_entries, DT_RPATH)?
        .map(|offset| dynamic_string(string_table, offset, "DT_RPATH"))
        .transpose()?;
    let runpath = first_dynamic(&dynamic_entries, DT_RUNPATH)?
        .map(|offset| dynamic_string(string_table, offset, "DT_RUNPATH"))
        .transpose()?;

    let (symbol_count, symbol_count_source) = if dynamic_entries.is_empty() {
        (0, None)
    } else {
        determine_symbol_count(&reader, &segments, &sections, &dynamic_entries)?
    };
    let symbols = parse_symbols(
        &reader,
        &segments,
        &dynamic_entries,
        string_table,
        symbol_count,
    )?;
    let mut imports: Vec<_> = symbols
        .iter()
        .filter(|symbol| symbol_is_public(symbol) && symbol.section_index == SHN_UNDEF)
        .cloned()
        .collect();
    let mut exports: Vec<_> = symbols
        .iter()
        .filter(|symbol| symbol_is_public(symbol) && symbol.section_index != SHN_UNDEF)
        .cloned()
        .collect();
    imports.sort_by(|left, right| (&left.name, left.value).cmp(&(&right.name, right.value)));
    exports.sort_by(|left, right| (&left.name, left.value).cmp(&(&right.name, right.value)));

    for symbol in symbols.iter().filter(|symbol| symbol.symbol_type == 10) {
        add_unsupported(
            &mut unsupported,
            "gnu-ifunc-symbol",
            format!("symbol {} uses STT_GNU_IFUNC", symbol.name),
        );
    }
    for symbol in symbols.iter().filter(|symbol| symbol.binding == 10) {
        add_unsupported(
            &mut unsupported,
            "gnu-unique-symbol",
            format!("symbol {} uses STB_GNU_UNIQUE", symbol.name),
        );
    }

    let relocations = parse_relocations(&reader, &segments, &dynamic_entries, symbol_count)?;
    let mut relocation_counts = BTreeMap::new();
    for relocation in &relocations {
        *relocation_counts
            .entry(relocation.relocation_type)
            .or_insert(0usize) += 1;
    }
    let relocation_types: Vec<_> = relocation_counts
        .into_iter()
        .map(|(number, count)| RelocationType {
            number,
            name: relocation_name(number),
            count,
            supported_by_policy: relocation_supported(number),
        })
        .collect();
    for relocation in relocation_types
        .iter()
        .filter(|relocation| !relocation.supported_by_policy)
    {
        add_unsupported(
            &mut unsupported,
            "unsupported-rela-type",
            format!(
                "{} ({}) count={}",
                relocation.name, relocation.number, relocation.count
            ),
        );
    }

    if dynamic_entries
        .iter()
        .any(|entry| entry.tag == DT_REL || entry.tag == DT_RELSZ)
    {
        add_unsupported(
            &mut unsupported,
            "elf-rel-relocations",
            "DT_REL relocations are outside the AArch64 RELA-only policy",
        );
    }
    if dynamic_entries.iter().any(|entry| {
        matches!(
            entry.tag,
            DT_RELR
                | DT_RELRSZ
                | DT_RELRENT
                | DT_ANDROID_REL
                | DT_ANDROID_RELSZ
                | DT_ANDROID_RELA
                | DT_ANDROID_RELASZ
        )
    }) {
        add_unsupported(
            &mut unsupported,
            "packed-relocations",
            "DT_RELR or Android packed relocations require a separate decoder",
        );
    }
    let flags = first_dynamic(&dynamic_entries, DT_FLAGS)?.unwrap_or(0);
    if dynamic_entries.iter().any(|entry| entry.tag == DT_TEXTREL) || flags & DF_TEXTREL != 0 {
        add_unsupported(
            &mut unsupported,
            "text-relocations",
            "DT_TEXTREL or DF_TEXTREL requests writes to executable mappings",
        );
    }

    let init = first_dynamic(&dynamic_entries, DT_INIT)?;
    let init_array_address = first_dynamic(&dynamic_entries, DT_INIT_ARRAY)?;
    let init_array = parse_pointer_array(
        &reader,
        &segments,
        init_array_address,
        first_dynamic(&dynamic_entries, DT_INIT_ARRAYSZ)?,
        "DT_INIT_ARRAY",
    )?;
    let svc_instructions = scan_svc(&reader, &segments)?;
    if !svc_instructions.is_empty() {
        add_unsupported(
            &mut unsupported,
            "raw-aarch64-svc",
            format!(
                "found {} SVC opcode candidate(s) in executable PT_LOAD bytes",
                svc_instructions.len()
            ),
        );
    }

    let mut requirements = BTreeSet::new();
    if !tls_segments.is_empty() {
        requirements.insert("tls");
    }
    if !gnu_relro_segments.is_empty() {
        requirements.insert("gnu-relro");
    }
    if first_dynamic(&dynamic_entries, DT_HASH)?.is_some() {
        requirements.insert("sysv-hash");
    }
    if first_dynamic(&dynamic_entries, DT_GNU_HASH)?.is_some() {
        requirements.insert("gnu-hash");
    }
    if init.is_some() || !init_array.is_empty() {
        requirements.insert("constructors");
    }
    if rpath.is_some() {
        requirements.insert("rpath");
    }
    if runpath.is_some() {
        requirements.insert("runpath");
    }
    if first_dynamic(&dynamic_entries, DT_FLAGS_1)?.is_some() {
        requirements.insert("dynamic-flags-1");
    }

    Ok(Inspection {
        file_size: bytes.len(),
        elf_type: header.elf_type,
        entry: header.entry,
        load_segments: load,
        dynamic_segments,
        tls_segments,
        gnu_relro_segments,
        dynamic: DynamicSummary {
            needed,
            soname,
            rpath,
            runpath,
            sysv_hash: first_dynamic(&dynamic_entries, DT_HASH)?.is_some(),
            gnu_hash: first_dynamic(&dynamic_entries, DT_GNU_HASH)?.is_some(),
            symbol_count_source,
            init,
            init_array_address,
            init_array,
        },
        dynamic_symbol_count: symbol_count,
        imports,
        exports,
        rela_count: relocations.len(),
        relocation_types,
        svc_instructions,
        requirements: requirements.into_iter().collect(),
        unsupported_capabilities: unsupported.into_iter().collect(),
    })
}

fn json_string(output: &mut String, value: &str) {
    output.push('"');
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\u{08}' => output.push_str("\\b"),
            '\u{0c}' => output.push_str("\\f"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            character if character <= '\u{1f}' => {
                output.push_str(&format!("\\u{:04x}", character as u32));
            }
            character => output.push(character),
        }
    }
    output.push('"');
}

fn json_optional_string(output: &mut String, value: Option<&str>) {
    match value {
        Some(value) => json_string(output, value),
        None => output.push_str("null"),
    }
}

fn json_string_array<'a>(output: &mut String, values: impl IntoIterator<Item = &'a str>) {
    output.push('[');
    let mut first = true;
    for value in values {
        if !first {
            output.push(',');
        }
        first = false;
        json_string(output, value);
    }
    output.push(']');
}

fn json_segments(output: &mut String, segments: &[ProgramSegment]) {
    output.push('[');
    for (index, segment) in segments.iter().enumerate() {
        if index != 0 {
            output.push(',');
        }
        output.push_str(&format!(
            "{{\"offset\":{},\"virtual_address\":{},\"file_size\":{},\"memory_size\":{},\"flags\":{},\"alignment\":{}}}",
            segment.offset,
            segment.virtual_address,
            segment.file_size,
            segment.memory_size,
            segment.flags,
            segment.alignment,
        ));
    }
    output.push(']');
}

fn json_symbols(output: &mut String, symbols: &[Symbol]) {
    output.push('[');
    for (index, symbol) in symbols.iter().enumerate() {
        if index != 0 {
            output.push(',');
        }
        output.push_str("{\"name\":");
        json_string(output, &symbol.name);
        output.push_str(&format!(
            ",\"binding\":{},\"type\":{},\"visibility\":{},\"section_index\":{},\"value\":{},\"size\":{}}}",
            symbol.binding,
            symbol.symbol_type,
            symbol.visibility,
            symbol.section_index,
            symbol.value,
            symbol.size,
        ));
    }
    output.push(']');
}

impl Inspection {
    pub fn tier1_compatible(&self) -> bool {
        self.unsupported_capabilities.is_empty()
    }

    pub fn to_json(&self, input: Option<&str>) -> String {
        let mut output = String::with_capacity(4096);
        output.push_str("{\"schema_version\":1,\"policy\":\"darwin-art-tier1-v1\"");
        if let Some(input) = input {
            output.push_str(",\"input\":");
            json_string(&mut output, input);
        }
        output.push_str(&format!(
            ",\"file_size\":{},\"elf\":{{\"class\":\"ELF64\",\"endianness\":\"little\",\"machine\":\"AArch64\",\"type\":{},\"entry\":{}}}",
            self.file_size, self.elf_type, self.entry
        ));
        output.push_str(",\"segments\":{\"load\":");
        json_segments(&mut output, &self.load_segments);
        output.push_str(",\"dynamic\":");
        json_segments(&mut output, &self.dynamic_segments);
        output.push_str(",\"tls\":");
        json_segments(&mut output, &self.tls_segments);
        output.push_str(",\"gnu_relro\":");
        json_segments(&mut output, &self.gnu_relro_segments);
        output.push('}');

        output.push_str(",\"dynamic\":{\"needed\":");
        json_string_array(&mut output, self.dynamic.needed.iter().map(String::as_str));
        output.push_str(",\"soname\":");
        json_optional_string(&mut output, self.dynamic.soname.as_deref());
        output.push_str(",\"rpath\":");
        json_optional_string(&mut output, self.dynamic.rpath.as_deref());
        output.push_str(",\"runpath\":");
        json_optional_string(&mut output, self.dynamic.runpath.as_deref());
        output.push_str(&format!(
            ",\"hash\":{{\"sysv\":{},\"gnu\":{},\"symbol_count_source\":",
            self.dynamic.sysv_hash, self.dynamic.gnu_hash
        ));
        json_optional_string(&mut output, self.dynamic.symbol_count_source);
        output.push_str("},\"constructors\":{\"init\":");
        match self.dynamic.init {
            Some(value) => output.push_str(&value.to_string()),
            None => output.push_str("null"),
        }
        output.push_str(",\"init_array_address\":");
        match self.dynamic.init_array_address {
            Some(value) => output.push_str(&value.to_string()),
            None => output.push_str("null"),
        }
        output.push_str(",\"init_array\":[");
        for (index, value) in self.dynamic.init_array.iter().enumerate() {
            if index != 0 {
                output.push(',');
            }
            output.push_str(&value.to_string());
        }
        output.push_str("]}}");

        output.push_str(&format!(
            ",\"symbols\":{{\"dynamic_count\":{},\"imports\":",
            self.dynamic_symbol_count
        ));
        json_symbols(&mut output, &self.imports);
        output.push_str(",\"exports\":");
        json_symbols(&mut output, &self.exports);
        output.push('}');

        output.push_str(&format!(
            ",\"relocations\":{{\"rela_count\":{},\"types\":[",
            self.rela_count
        ));
        for (index, relocation) in self.relocation_types.iter().enumerate() {
            if index != 0 {
                output.push(',');
            }
            output.push_str(&format!(
                "{{\"number\":{},\"name\":\"{}\",\"count\":{},\"supported_by_policy\":{}}}",
                relocation.number,
                relocation.name,
                relocation.count,
                relocation.supported_by_policy,
            ));
        }
        output.push_str("]}");

        output.push_str(",\"svc_instructions\":[");
        for (index, instruction) in self.svc_instructions.iter().enumerate() {
            if index != 0 {
                output.push(',');
            }
            output.push_str(&format!(
                "{{\"file_offset\":{},\"virtual_address\":{},\"immediate\":{}}}",
                instruction.file_offset, instruction.virtual_address, instruction.immediate
            ));
        }
        output.push(']');
        output.push_str(",\"requirements\":");
        json_string_array(&mut output, self.requirements.iter().copied());
        output.push_str(&format!(
            ",\"tier1_compatible\":{}",
            self.tier1_compatible()
        ));
        output.push_str(",\"unsupported_capabilities\":[");
        for (index, unsupported) in self.unsupported_capabilities.iter().enumerate() {
            if index != 0 {
                output.push(',');
            }
            output.push_str("{\"code\":");
            json_string(&mut output, unsupported.code);
            output.push_str(",\"detail\":");
            json_string(&mut output, &unsupported.detail);
            output.push('}');
        }
        output.push_str("]}");
        output
    }
}

pub fn error_json(input: Option<&str>, error: &InspectError) -> String {
    let mut output = String::from("{\"schema_version\":1");
    if let Some(input) = input {
        output.push_str(",\"input\":");
        json_string(&mut output, input);
    }
    output.push_str(",\"error\":{\"code\":");
    json_string(&mut output, error.code);
    output.push_str(",\"message\":");
    json_string(&mut output, &error.message);
    output.push_str("}}");
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::panic::{catch_unwind, AssertUnwindSafe};

    const SMOKE: &[u8] = include_bytes!("../tests/fixtures/libarm64_inspector_smoke.so");
    const DEPENDENCY: &[u8] = include_bytes!("../tests/fixtures/libfixture_dep.so");

    fn dynamic_value_offset(bytes: &[u8], wanted_tag: i64) -> usize {
        let reader = Reader::new(bytes);
        let header = parse_header(&reader).unwrap();
        let segments = parse_program_headers(&reader, header).unwrap();
        let dynamic = segments
            .iter()
            .find(|segment| segment.kind == PT_DYNAMIC)
            .unwrap();
        for index in 0..dynamic.file_size as usize / 16 {
            let offset = dynamic.offset as usize + index * 16;
            let tag = i64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap());
            if tag == wanted_tag {
                return offset + 8;
            }
            if tag == DT_NULL {
                break;
            }
        }
        panic!("missing dynamic tag {wanted_tag:#x}");
    }

    #[test]
    fn pinned_aarch64_smoke_is_accepted() {
        let inspection = inspect(SMOKE).unwrap();
        assert_eq!(inspection.elf_type, ET_DYN);
        assert_eq!(
            inspection.dynamic.soname.as_deref(),
            Some("libarm64_inspector_smoke.so")
        );
        assert_eq!(inspection.dynamic.needed, ["libfixture_dep.so"]);
        assert_eq!(
            inspection.dynamic.runpath.as_deref(),
            Some("$ORIGIN/fixture")
        );
        assert!(inspection.dynamic.sysv_hash);
        assert!(inspection.dynamic.gnu_hash);
        assert_eq!(inspection.tls_segments.len(), 1);
        assert_eq!(inspection.gnu_relro_segments.len(), 1);
        assert!(inspection.dynamic.init.is_some());
        assert_eq!(inspection.dynamic.init_array.len(), 1);
        assert!(inspection
            .imports
            .iter()
            .any(|symbol| symbol.name == "external_api"));
        assert!(inspection
            .exports
            .iter()
            .any(|symbol| symbol.name == "exported_add"));
        assert!(inspection
            .exports
            .iter()
            .any(|symbol| symbol.name == "tls_counter"));
        assert!(inspection.relocation_types.iter().any(|relocation| {
            relocation.number == 1026 && relocation.name == "R_AARCH64_JUMP_SLOT"
        }));
        assert!(inspection.svc_instructions.is_empty());
        assert!(
            inspection.tier1_compatible(),
            "{:?}",
            inspection.unsupported_capabilities
        );
    }

    #[test]
    fn pinned_dependency_exports_symbol_and_has_both_hashes() {
        let inspection = inspect(DEPENDENCY).unwrap();
        assert_eq!(
            inspection.dynamic.soname.as_deref(),
            Some("libfixture_dep.so")
        );
        assert!(inspection
            .exports
            .iter()
            .any(|symbol| symbol.name == "external_api"));
        assert!(inspection.dynamic.sysv_hash && inspection.dynamic.gnu_hash);
    }

    #[test]
    fn sectionless_shared_object_uses_dynamic_hash_metadata() {
        let mut bytes = SMOKE.to_vec();
        bytes[40..48].copy_from_slice(&0u64.to_le_bytes());
        bytes[58..60].copy_from_slice(&0u16.to_le_bytes());
        bytes[60..62].copy_from_slice(&0u16.to_le_bytes());
        bytes[62..64].copy_from_slice(&0u16.to_le_bytes());
        let inspection = inspect(&bytes).unwrap();
        assert_eq!(inspection.dynamic_symbol_count, 7);
        assert_eq!(inspection.dynamic.symbol_count_source, Some("sysv-hash"));
        assert!(inspection
            .imports
            .iter()
            .any(|symbol| symbol.name == "external_api"));
    }

    #[test]
    fn rejects_wrong_class_endianness_and_machine() {
        let mut wrong_class = SMOKE.to_vec();
        wrong_class[4] = 1;
        assert_eq!(inspect(&wrong_class).unwrap_err().code, "unsupported-class");

        let mut wrong_endianness = SMOKE.to_vec();
        wrong_endianness[5] = 2;
        assert_eq!(
            inspect(&wrong_endianness).unwrap_err().code,
            "unsupported-endianness"
        );

        let mut wrong_machine = SMOKE.to_vec();
        wrong_machine[18..20].copy_from_slice(&62u16.to_le_bytes());
        assert_eq!(
            inspect(&wrong_machine).unwrap_err().code,
            "unsupported-machine"
        );
    }

    #[test]
    fn detects_svc_opcode_with_offset_and_immediate() {
        let baseline = inspect(SMOKE).unwrap();
        let export = baseline
            .exports
            .iter()
            .find(|symbol| symbol.name == "exported_add")
            .unwrap();
        let segment = baseline
            .load_segments
            .iter()
            .find(|segment| {
                segment.flags & PF_X != 0
                    && export.value >= segment.virtual_address
                    && export.value < segment.virtual_address + segment.file_size
            })
            .unwrap();
        let file_offset = (segment.offset + export.value - segment.virtual_address) as usize;
        let mut bytes = SMOKE.to_vec();
        bytes[file_offset..file_offset + 4].copy_from_slice(&0xd400_0541u32.to_le_bytes());
        let inspection = inspect(&bytes).unwrap();
        assert!(inspection.svc_instructions.iter().any(|instruction| {
            instruction.file_offset == file_offset as u64 && instruction.immediate == 42
        }));
        assert!(inspection
            .unsupported_capabilities
            .iter()
            .any(|item| item.code == "raw-aarch64-svc"));
    }

    #[test]
    fn every_truncated_prefix_returns_without_panicking() {
        for length in 0..SMOKE.len() {
            let outcome = catch_unwind(AssertUnwindSafe(|| inspect(&SMOKE[..length])));
            assert!(outcome.is_ok(), "panic at prefix length {length}");
            assert!(
                outcome.unwrap().is_err(),
                "truncated prefix unexpectedly parsed at {length}"
            );
        }
    }

    #[test]
    fn adversarial_offsets_and_sizes_are_rejected() {
        let mut bad_program_offset = SMOKE.to_vec();
        bad_program_offset[32..40].copy_from_slice(&u64::MAX.to_le_bytes());
        assert_eq!(
            inspect(&bad_program_offset).unwrap_err().code,
            "bounds-overflow"
        );

        let mut bad_gnu_hash = SMOKE.to_vec();
        let value_offset = dynamic_value_offset(&bad_gnu_hash, DT_GNU_HASH);
        bad_gnu_hash[value_offset..value_offset + 8].copy_from_slice(&u64::MAX.to_le_bytes());
        assert!(matches!(
            inspect(&bad_gnu_hash).unwrap_err().code,
            "bounds-overflow" | "unmapped-virtual-address"
        ));

        let mut bad_rela_size = SMOKE.to_vec();
        let value_offset = dynamic_value_offset(&bad_rela_size, DT_RELASZ);
        bad_rela_size[value_offset..value_offset + 8].copy_from_slice(&1u64.to_le_bytes());
        assert_eq!(
            inspect(&bad_rela_size).unwrap_err().code,
            "invalid-rela-table"
        );
    }

    #[test]
    fn deterministic_mutation_corpus_never_panics() {
        let mut state = 0x4d59_5df4_d0f3_3173u64;
        for case in 0..512usize {
            let mut bytes = SMOKE.to_vec();
            for _ in 0..=case % 8 {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                let index = (state as usize) % bytes.len();
                bytes[index] ^= (state >> 32) as u8 | 1;
            }
            let outcome = catch_unwind(AssertUnwindSafe(|| inspect(&bytes)));
            assert!(outcome.is_ok(), "panic in mutation case {case}");
        }
    }

    #[test]
    fn json_is_stable_and_escapes_input() {
        let inspection = inspect(SMOKE).unwrap();
        let first = inspection.to_json(Some("a\n\"b.so"));
        let second = inspection.to_json(Some("a\n\"b.so"));
        assert_eq!(first, second);
        assert!(first.contains("\"input\":\"a\\n\\\"b.so\""));
        assert!(first.contains("\"schema_version\":1"));
        assert!(first.contains("\"tier1_compatible\":true"));
    }
}
