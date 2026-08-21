//! ELF header/program-header/dynamic-table parsing policy.
//!
//! Relocation and mapped-image lifetime remain in the parent loader module;
//! this module only validates bytes and produces the parsed load plan.

use super::*;

pub(super) fn parse_image(bytes: &[u8]) -> Result<ParsedImage, LoadError> {
    parse_image_with_policy(bytes, false)
}

pub(super) fn parse_image_with_policy(
    bytes: &[u8],
    metadata_only: bool,
) -> Result<ParsedImage, LoadError> {
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
    let mut tls = None;
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
            PT_TLS => {
                if tls.replace(header).is_some() {
                    return Err(LoadError::Format("multiple PT_TLS segments"));
                }
            }
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

    if let Some(header) = tls {
        validate_tls_segment(bytes, &header, &loads)?;
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
        tls,
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

fn validate_tls_segment(
    bytes: &[u8],
    tls: &ProgramHeader,
    loads: &[ProgramHeader],
) -> Result<(), LoadError> {
    if tls.memory_size == 0 || tls.file_size > tls.memory_size {
        return Err(LoadError::Format("invalid PT_TLS size"));
    }
    if tls.memory_size > MAX_TLS_SIZE as u64 {
        return Err(LoadError::Bounds("PT_TLS memory size cap"));
    }
    if tls.flags != PF_R {
        return Err(LoadError::Format("PT_TLS must be read-only metadata"));
    }
    if tls.alignment == 0
        || !tls.alignment.is_power_of_two()
        || tls.alignment > MAX_TLS_ALIGNMENT as u64
        || tls.offset % tls.alignment != tls.virtual_address % tls.alignment
    {
        return Err(LoadError::Format("invalid PT_TLS alignment"));
    }
    checked_slice(
        bytes,
        to_usize(tls.offset, "PT_TLS file offset")?,
        to_usize(tls.file_size, "PT_TLS file size")?,
        "PT_TLS file range",
    )?;
    let memory_end = tls
        .virtual_address
        .checked_add(tls.memory_size)
        .ok_or(LoadError::Bounds("PT_TLS memory range overflow"))?;
    let file_end = tls
        .virtual_address
        .checked_add(tls.file_size)
        .ok_or(LoadError::Bounds("PT_TLS file range overflow"))?;
    let containing = loads.iter().find(|load| {
        load.virtual_address <= tls.virtual_address
            && load
                .virtual_address
                .checked_add(load.memory_size)
                .is_some_and(|load_end| memory_end <= load_end)
    });
    let Some(load) = containing else {
        return Err(LoadError::Bounds("PT_TLS is not inside PT_LOAD"));
    };
    let file_delta = tls
        .offset
        .checked_sub(load.offset)
        .ok_or(LoadError::Bounds("PT_TLS file mapping"))?;
    let virtual_delta = tls
        .virtual_address
        .checked_sub(load.virtual_address)
        .ok_or(LoadError::Bounds("PT_TLS virtual mapping"))?;
    if file_delta != virtual_delta || file_end > load.virtual_address.saturating_add(load.file_size)
    {
        return Err(LoadError::Format("PT_TLS file/virtual mapping mismatch"));
    }
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

pub(super) fn parse_dynamic(
    bytes: &[u8],
    dynamic: &ProgramHeader,
) -> Result<DynamicInfo, LoadError> {
    parse_dynamic_with_policy(bytes, dynamic, false)
}

pub(super) fn parse_dynamic_with_policy(
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
            DT_AARCH64_BTI_PLT => set_once(
                &mut info.aarch64_bti_plt,
                value,
                "duplicate DT_AARCH64_BTI_PLT",
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

pub(super) fn validate_dynamic_capabilities(info: &DynamicInfo) -> Result<(), LoadError> {
    if let Some(value) = info.aarch64_bti_plt.filter(|value| *value != 0) {
        return Err(LoadError::Capability(Capability::DynamicFlags {
            tag: DT_AARCH64_BTI_PLT,
            value,
        }));
    }
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
