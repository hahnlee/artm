use super::{Capability, LoadError};

const LINUX_SVC_ZERO: u32 = 0xd400_0001;
const BL: u32 = 0x9400_0000;
const BL_MIN_BYTES: i128 = -(1_i128 << 27);
const BL_MAX_BYTES: i128 = (1_i128 << 27) - 4;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct DirectSyscallRewrite {
    pub instruction_index: usize,
    pub original: u32,
}

pub(crate) fn contains_linux_svc(code: &[u8]) -> bool {
    code.chunks_exact(4)
        .any(|bytes| u32::from_le_bytes(bytes.try_into().unwrap()) == LINUX_SVC_ZERO)
}

pub(crate) fn rewrite_linux_svc(
    code: &mut [u8],
    shim_address: usize,
) -> Result<Vec<DirectSyscallRewrite>, LoadError> {
    let mut rewrites = Vec::new();
    for instruction_index in 0..code.len() / 4 {
        if read_instruction(code, instruction_index) != LINUX_SVC_ZERO {
            continue;
        }
        let instruction_address = code.as_ptr() as usize + instruction_index * 4;
        let delta = shim_address as i128 - instruction_address as i128;
        if delta & 3 != 0 || !(BL_MIN_BYTES..=BL_MAX_BYTES).contains(&delta) {
            return Err(LoadError::Capability(Capability::DirectSyscall));
        }
        let immediate = ((delta >> 2) as i64 as u64 & 0x03ff_ffff) as u32;
        write_instruction(code, instruction_index, BL | immediate);
        rewrites.push(DirectSyscallRewrite {
            instruction_index,
            original: LINUX_SVC_ZERO,
        });
    }
    Ok(rewrites)
}

/// Writes an AArch64 veneer which converts the kernel syscall register shape
/// (`x8=number`, `x0..x5=args`) to Bionic's variadic `syscall(number, ...)` ABI.
///
/// # Safety
///
/// `destination` must name at least 16 writable, instruction-aligned words in a
/// loader-owned page which is not executable concurrently.
pub(crate) unsafe fn write_shim(
    destination: *mut u8,
    syscall_target: usize,
) -> Result<(), LoadError> {
    if destination.is_null() || destination as usize & 3 != 0 || syscall_target == 0 {
        return Err(LoadError::Bounds("direct-syscall shim address"));
    }
    let mut instructions = vec![
        0xd101_83ff, // sub sp, sp, #0x60
        0xf900_23fe, // str x30, [sp, #0x40]
        mov_x(6, 5),
        mov_x(5, 4),
        mov_x(4, 3),
        mov_x(3, 2),
        mov_x(2, 1),
        mov_x(1, 0),
        mov_x(0, 8),
    ];
    instructions.extend(load_immediate_x16(syscall_target as u64));
    instructions.extend([
        0xd63f_0200, // blr x16
        0xf940_23fe, // ldr x30, [sp, #0x40]
        0x9101_83ff, // add sp, sp, #0x60
        0xd65f_03c0, // ret
    ]);
    for (index, instruction) in instructions.into_iter().enumerate() {
        // SAFETY: guaranteed by the caller; write_unaligned avoids imposing a Rust reference.
        unsafe {
            destination
                .add(index * 4)
                .cast::<u32>()
                .write_unaligned(instruction)
        };
    }
    Ok(())
}

fn mov_x(destination: u32, source: u32) -> u32 {
    0xaa00_03e0 | (source << 16) | destination
}

fn load_immediate_x16(value: u64) -> [u32; 4] {
    [
        0xd280_0010 | (((value & 0xffff) as u32) << 5),
        0xf2a0_0010 | ((((value >> 16) & 0xffff) as u32) << 5),
        0xf2c0_0010 | ((((value >> 32) & 0xffff) as u32) << 5),
        0xf2e0_0010 | ((((value >> 48) & 0xffff) as u32) << 5),
    ]
}

fn read_instruction(code: &[u8], index: usize) -> u32 {
    let offset = index * 4;
    u32::from_le_bytes(code[offset..offset + 4].try_into().unwrap())
}

fn write_instruction(code: &mut [u8], index: usize, instruction: u32) {
    let offset = index * 4;
    code[offset..offset + 4].copy_from_slice(&instruction.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrites_only_linux_svc_zero_to_a_near_bl() {
        let mut code = [0xd503_201f_u32, LINUX_SVC_ZERO, 0xd400_0021]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        let shim = code.as_ptr() as usize + 0x100;
        let rewrites = rewrite_linux_svc(&mut code, shim).unwrap();
        assert_eq!(rewrites.len(), 1);
        assert_eq!(read_instruction(&code, 0), 0xd503_201f);
        assert_eq!(read_instruction(&code, 2), 0xd400_0021);
        assert_eq!(read_instruction(&code, 1) & 0xfc00_0000, BL);
    }

    #[test]
    fn rejects_a_shim_outside_bl_range() {
        let mut code = LINUX_SVC_ZERO.to_le_bytes().to_vec();
        let far = (code.as_ptr() as usize).wrapping_add(1 << 28);
        assert!(matches!(
            rewrite_linux_svc(&mut code, far),
            Err(LoadError::Capability(Capability::DirectSyscall))
        ));
    }

    #[test]
    fn shim_loads_the_complete_provider_address() {
        let mut storage = [0_u8; 128];
        unsafe { write_shim(storage.as_mut_ptr(), 0x1234_5678_9abc_def0).unwrap() };
        assert_eq!(read_instruction(&storage, 0), 0xd101_83ff);
        assert_eq!(read_instruction(&storage, 9) & 0xff80_001f, 0xd280_0010);
        assert_eq!(read_instruction(&storage, 13), 0xd63f_0200);
        assert_eq!(read_instruction(&storage, 16), 0xd65f_03c0);
    }
}
