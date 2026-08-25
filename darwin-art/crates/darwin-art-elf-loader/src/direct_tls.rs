use super::{Capability, LoadError};

const MRS_TPIDR_EL0: u32 = 0xd53b_d040;
const MRS_REGISTER_MASK: u32 = 0xffff_ffe0;
const ADRP: u32 = 0x9000_0000;
const LDR_X_UNSIGNED_IMMEDIATE: u32 = 0xf940_0000;
const LDR_X_UNSIGNED_IMMEDIATE_MASK: u32 = 0xffc0_0000;
const BLR: u32 = 0xd63f_0000;
const BLR_REGISTER_MASK: u32 = 0xffff_fc1f;
const ADD_X_SHIFTED_REGISTER: u32 = 0x8b00_0000;
const ADD_X_SHIFTED_REGISTER_MASK: u32 = 0xffe0_fc00;
const ADD_X_IMMEDIATE: u32 = 0x9100_0000;
const ADD_X_IMMEDIATE_MASK: u32 = 0xffc0_0000;
const ADRP_X0: u32 = 0x9000_0000;
const ADRP_REGISTER_MASK: u32 = 0x9f00_001f;
const MOV_X_REGISTER: u32 = 0xaa00_03e0;
const MOV_X_REGISTER_MASK: u32 = 0xffe0_ffe0;
const LDR_X_REGISTER_OFFSET: u32 = 0xf860_6800;
const LDR_X_REGISTER_OFFSET_MASK: u32 = 0xffe0_fc00;
const ANDROID_TLS_SLOT_STACK_GUARD_OFFSET: u32 = 0x28;
// LLVM normally emits the guard load immediately after reading TPIDR_EL0, but
// highly optimized third-party code can schedule independent arithmetic between
// the two. Real optimized DSOs can have dozens of intervening instructions.
// Keep this bounded to a small basic-block-sized window: the second half of the
// pair must still be an exact `LDR Xt, [Xthread, #0x28]` using the same register.
const MAX_STACK_GUARD_DISTANCE_IN_INSTRUCTIONS: usize = 64;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct StackGuardRewrite {
    pub instruction_index: usize,
    pub original: u32,
}

/// Rewrites Bionic's direct stack-guard TLS sequence into a Darwin-safe equivalent.
///
/// Android arm64 code commonly reads `TLS_SLOT_STACK_GUARD` with an `MRS TPIDR_EL0`
/// followed shortly by `LDR Xt, [Xthread, #0x28]`. Darwin owns TPIDR_EL0 and neither
/// TPIDR_EL0 nor TPIDRRO_EL0 provides Android's immutable stack-guard slot. In particular,
/// Darwin may mutate the value at `TPIDRRO_EL0 + 0x28` while a guest function is active.
/// The rewrite redirects the validated Android stack-guard base to a loader-owned,
/// read-only page whose guard remains at the ABI's original `+0x28` offset:
///
/// `ADRP Xthread, loader_guard_page; ...; LDR Xt, [Xthread, #0x28]`
///
/// The load stays intact because one compiler-generated function can reuse the single
/// thread-base read for both its prologue and every epilogue guard comparison.
///
/// A local TLSDESC access also reads TPIDR_EL0, but only after calling the descriptor
/// resolver. `tls.rs` deliberately returns `guest_address - current_TPIDR_EL0`, so the
/// exact compiler sequence `BLR resolver; MRS thread; ADD x0, thread, x0` is already
/// Darwin-safe and is validated but left unchanged.
///
/// This is deliberately not a generic Android TLS emulation. Any direct TPIDR_EL0 use
/// that is neither the validated stack-guard pattern nor the exact local-TLSDESC sequence
/// fails loading instead of executing with subtly incorrect TLS semantics.
pub(crate) fn rewrite_android_stack_guard_tls(
    code: &mut [u8],
    stack_guard_address: usize,
) -> Result<Vec<StackGuardRewrite>, LoadError> {
    if stack_guard_address & 0xfff != ANDROID_TLS_SLOT_STACK_GUARD_OFFSET as usize {
        return Err(LoadError::Bounds("stack-guard page offset"));
    }
    let instruction_count = code.len() / 4;
    let mut rewrites = Vec::new();

    for instruction_index in 0..instruction_count {
        let instruction = read_instruction(code, instruction_index);
        if instruction & MRS_REGISTER_MASK != MRS_TPIDR_EL0 {
            continue;
        }

        let thread_register = instruction & 0x1f;
        let search_end = instruction_index
            .saturating_add(MAX_STACK_GUARD_DISTANCE_IN_INSTRUCTIONS + 1)
            .min(instruction_count);
        let guard_load = (instruction_index + 1..search_end).find(|candidate_index| {
            is_stack_guard_load(read_instruction(code, *candidate_index), thread_register)
        });
        let Some(guard_load) = guard_load else {
            if is_local_tlsdesc_result(code, instruction_index, thread_register) {
                continue;
            }
            return Err(LoadError::Capability(Capability::DirectThreadPointer));
        };
        rewrites.push((instruction_index, guard_load));
    }

    let mut applied = Vec::with_capacity(rewrites.len());
    for (thread_pointer_index, _) in &rewrites {
        let original_mrs = read_instruction(code, *thread_pointer_index);
        let thread_register = original_mrs & 0x1f;
        let instruction_address = code.as_ptr() as usize + *thread_pointer_index * 4;
        let instruction_page = instruction_address & !0xfff;
        let guard_page = stack_guard_address & !0xfff;
        let page_delta = (guard_page as i128 - instruction_page as i128) >> 12;
        if !(-(1_i128 << 20)..(1_i128 << 20)).contains(&page_delta) {
            return Err(LoadError::Bounds("stack-guard ADRP range"));
        }
        let immediate = (page_delta as i64 as u64) & 0x1f_ffff;
        let adrp = ADRP
            | (((immediate & 0x3) as u32) << 29)
            | ((((immediate >> 2) & 0x7_ffff) as u32) << 5)
            | thread_register;
        write_instruction(code, *thread_pointer_index, adrp);
        applied.push(StackGuardRewrite {
            instruction_index: *thread_pointer_index,
            original: original_mrs,
        });
    }

    Ok(applied)
}

fn is_local_tlsdesc_result(code: &[u8], instruction_index: usize, thread_register: u32) -> bool {
    let instruction_count = code.len() / 4;
    if instruction_index < 4 || instruction_index + 1 >= instruction_count {
        return false;
    }

    let search_start = instruction_index.saturating_sub(8).max(3);
    let Some(call_index) = (search_start..instruction_index)
        .rev()
        .find(|candidate| is_local_tlsdesc_call(code, *candidate))
    else {
        return false;
    };

    let mut offset_registers = [false; 32];
    offset_registers[0] = true;
    let search_end = (instruction_index + 9).min(instruction_count);
    for candidate in call_index + 1..search_end {
        if candidate == instruction_index {
            continue;
        }
        let instruction = read_instruction(code, candidate);
        if instruction & MOV_X_REGISTER_MASK == MOV_X_REGISTER {
            let source = ((instruction >> 16) & 0x1f) as usize;
            if offset_registers[source] {
                offset_registers[(instruction & 0x1f) as usize] = true;
            }
        }
        if instruction & ADD_X_SHIFTED_REGISTER_MASK == ADD_X_SHIFTED_REGISTER {
            let left = (instruction >> 5) & 0x1f;
            let right = (instruction >> 16) & 0x1f;
            if left == thread_register && offset_registers[right as usize]
                || right == thread_register && offset_registers[left as usize]
            {
                return true;
            }
        }
        if instruction & LDR_X_REGISTER_OFFSET_MASK == LDR_X_REGISTER_OFFSET
            && (instruction >> 5) & 0x1f == thread_register
            && offset_registers[((instruction >> 16) & 0x1f) as usize]
        {
            return true;
        }
    }
    false
}

fn is_local_tlsdesc_call(code: &[u8], call_index: usize) -> bool {
    if call_index < 3 || read_instruction(code, call_index) & BLR_REGISTER_MASK != BLR {
        return false;
    }
    let page = read_instruction(code, call_index - 3);
    let descriptor_load = read_instruction(code, call_index - 2);
    let descriptor_address = read_instruction(code, call_index - 1);
    page & ADRP_REGISTER_MASK == ADRP_X0
        && descriptor_load & LDR_X_UNSIGNED_IMMEDIATE_MASK == LDR_X_UNSIGNED_IMMEDIATE
        && descriptor_load & 0x1f == 1
        && (descriptor_load >> 5) & 0x1f == 0
        && descriptor_address & ADD_X_IMMEDIATE_MASK == ADD_X_IMMEDIATE
        && descriptor_address & 0x1f == 0
        && (descriptor_address >> 5) & 0x1f == 0
}

fn is_stack_guard_load(instruction: u32, thread_register: u32) -> bool {
    instruction & LDR_X_UNSIGNED_IMMEDIATE_MASK == LDR_X_UNSIGNED_IMMEDIATE
        && (instruction >> 5) & 0x1f == thread_register
        && ((instruction >> 10) & 0xfff) * 8 == ANDROID_TLS_SLOT_STACK_GUARD_OFFSET
}

fn read_instruction(code: &[u8], index: usize) -> u32 {
    let offset = index * 4;
    u32::from_le_bytes(
        code[offset..offset + 4]
            .try_into()
            .expect("bounded instruction"),
    )
}

fn write_instruction(code: &mut [u8], index: usize, instruction: u32) {
    let offset = index * 4;
    code[offset..offset + 4].copy_from_slice(&instruction.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    fn instructions(values: &[u32]) -> Vec<u8> {
        values
            .iter()
            .flat_map(|instruction| instruction.to_le_bytes())
            .collect()
    }

    #[test]
    fn rewrites_only_the_validated_android_stack_guard_pair() {
        let mut code = instructions(&[
            MRS_TPIDR_EL0 | 19,
            0xaa00_03f4,
            LDR_X_UNSIGNED_IMMEDIATE | (5 << 10) | (19 << 5) | 8,
        ]);

        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;
        assert_eq!(
            rewrite_android_stack_guard_tls(&mut code, guard)
                .unwrap()
                .len(),
            1
        );
        assert_eq!(read_instruction(&code, 0), ADRP | 19);
        assert_eq!(read_instruction(&code, 1), 0xaa00_03f4);
        assert_eq!(
            read_instruction(&code, 2),
            LDR_X_UNSIGNED_IMMEDIATE | (5 << 10) | (19 << 5) | 8
        );
    }

    #[test]
    fn rejects_a_general_direct_android_tls_access() {
        let mut code = instructions(&[
            MRS_TPIDR_EL0 | 20,
            LDR_X_UNSIGNED_IMMEDIATE | (7 << 10) | (20 << 5),
        ]);
        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;

        assert!(matches!(
            rewrite_android_stack_guard_tls(&mut code, guard),
            Err(LoadError::Capability(Capability::DirectThreadPointer))
        ));
    }

    #[test]
    fn accepts_a_scheduled_stack_guard_load() {
        let mut values = vec![MRS_TPIDR_EL0 | 19];
        values.extend(std::iter::repeat_n(0xd503_201f, 33));
        values.push(LDR_X_UNSIGNED_IMMEDIATE | (5 << 10) | (19 << 5) | 8);
        let mut code = instructions(&values);
        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;

        assert_eq!(
            rewrite_android_stack_guard_tls(&mut code, guard)
                .unwrap()
                .len(),
            1
        );
        assert_eq!(read_instruction(&code, 0), ADRP | 19);
    }

    #[test]
    fn accepts_the_exact_local_tlsdesc_result_sequence() {
        let original = instructions(&[
            ADRP_X0,
            LDR_X_UNSIGNED_IMMEDIATE | 1,
            ADD_X_IMMEDIATE,
            BLR | (1 << 5),
            MRS_TPIDR_EL0 | 8,
            ADD_X_SHIFTED_REGISTER | (8 << 5),
            0xd65f_03c0,
        ]);
        let mut code = original.clone();

        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;
        assert!(
            rewrite_android_stack_guard_tls(&mut code, guard)
                .unwrap()
                .is_empty()
        );
        assert_eq!(code, original);
    }

    #[test]
    fn rejects_tlsdesc_like_arithmetic_without_a_resolver_call() {
        let mut code = instructions(&[
            0xd503_201f,
            MRS_TPIDR_EL0 | 8,
            ADD_X_SHIFTED_REGISTER | (8 << 5),
        ]);
        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;

        assert!(matches!(
            rewrite_android_stack_guard_tls(&mut code, guard),
            Err(LoadError::Capability(Capability::DirectThreadPointer))
        ));
    }

    #[test]
    fn accepts_optimized_tlsdesc_indexed_load_and_moved_offset() {
        let original = instructions(&[
            ADRP_X0,
            LDR_X_UNSIGNED_IMMEDIATE | 1,
            ADD_X_IMMEDIATE,
            BLR | (1 << 5),
            MOV_X_REGISTER | 8,
            0x9b0a_2440,
            MRS_TPIDR_EL0 | 9,
            ADD_X_SHIFTED_REGISTER | (8 << 16) | (9 << 5) | 8,
            ADRP_X0,
            LDR_X_UNSIGNED_IMMEDIATE | 1,
            ADD_X_IMMEDIATE,
            BLR | (1 << 5),
            MRS_TPIDR_EL0 | 20,
            LDR_X_REGISTER_OFFSET | (20 << 5),
        ]);
        let mut code = original.clone();

        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;
        assert!(
            rewrite_android_stack_guard_tls(&mut code, guard)
                .unwrap()
                .is_empty()
        );
        assert_eq!(code, original);
    }

    #[test]
    fn leaves_code_without_direct_android_tls_untouched() {
        let original = instructions(&[0xd503_201f, 0xd65f_03c0]);
        let mut code = original.clone();

        let guard = (code.as_ptr() as usize & !0xfff) + 0x28;
        assert!(
            rewrite_android_stack_guard_tls(&mut code, guard)
                .unwrap()
                .is_empty()
        );
        assert_eq!(code, original);
    }
}
