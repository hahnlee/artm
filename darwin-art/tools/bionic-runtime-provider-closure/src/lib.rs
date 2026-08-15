#![forbid(unsafe_op_in_unsafe_fn)]

use std::ffi::{c_char, c_void};

/* Keep one production core entrypoint from each stateful Rust provider in the
 * final staticlib. The native resolver tables retain the remaining C ABI
 * entries by name when the embedding link extracts their shim objects. */
#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_rust_provider_closure_anchor() -> usize {
    (bionic_fs_facade::darwin_art_bionic_fs_close_core as usize)
        | (bionic_process_state_facade::darwin_art_bionic_process_getauxval_core as usize)
        | (bionic_stdio_facade::darwin_art_bionic_stdio_fclose_core as usize)
        | (bionic_dso_lifecycle_facade::darwin_art_bionic_dso_cxa_finalize_core as usize)
}

#[allow(dead_code)]
fn _abi_types(_: *const c_char, _: *mut c_void) {}
