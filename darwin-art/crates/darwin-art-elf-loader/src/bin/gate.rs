use darwin_art_elf_loader::{Capability, LoadError, LoadedElf};
use std::env;
use std::fs;
use std::path::Path;

const PT_LOAD: u32 = 1;
const PF_X: u32 = 1;
const PF_W: u32 = 2;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments: Vec<_> = env::args_os().collect();
    if arguments.len() != 4 {
        return Err("usage: elf-loader-gate POSITIVE.so IMPORT.so TLS.so".into());
    }
    let positive = fs::read(&arguments[1])?;
    let import = fs::read(&arguments[2])?;
    let tls = fs::read(&arguments[3])?;

    run_positive(&positive)?;
    expect_capability(&import, |capability| {
        matches!(
            capability,
            Capability::NeededLibrary
                | Capability::PltRelocations
                | Capability::UndefinedSymbol(_)
                | Capability::UnsupportedRelocation { .. }
        )
    })?;
    expect_capability(&tls, |capability| matches!(capability, Capability::Tls))?;
    run_malformed_matrix(&positive)?;

    println!(
        "elf-loader-gate: positive=constructor-order+export relocations=relative-only \
         imports=capability-error tls=capability-error wx=reject overflow=reject overlap=reject \
         bounds=reject cleanup=drop"
    );
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

    let headers = load_program_headers(original)?;
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

fn load_program_headers(bytes: &[u8]) -> Result<Vec<usize>, Box<dyn std::error::Error>> {
    let offset = usize::try_from(read_u64(bytes, 32)?)?;
    let entry_size = read_u16(bytes, 54)? as usize;
    let count = read_u16(bytes, 56)? as usize;
    let mut result = Vec::new();
    for index in 0..count {
        let entry = offset
            .checked_add(index.checked_mul(entry_size).ok_or("phdr overflow")?)
            .ok_or("phdr overflow")?;
        if read_u32(bytes, entry)? == PT_LOAD {
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

#[allow(dead_code)]
fn _assert_path(_: &Path) {}
