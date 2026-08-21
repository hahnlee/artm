use super::*;
use std::ffi::c_void;
use std::mem::{align_of, offset_of, size_of};

#[test]
fn process_config_matches_c_abi_v1() {
    assert_eq!(size_of::<ProcessConfig>(), 104);
    assert_eq!(align_of::<ProcessConfig>(), 8);
    assert_eq!(offset_of!(ProcessConfig, struct_size), 0);
    assert_eq!(offset_of!(ProcessConfig, abi_version), 4);
    assert_eq!(offset_of!(ProcessConfig, core_oj_jar), 8);
    assert_eq!(offset_of!(ProcessConfig, core_libart_jar), 16);
    assert_eq!(offset_of!(ProcessConfig, framework_jar), 24);
    assert_eq!(offset_of!(ProcessConfig, core_icu4j_jar), 32);
    assert_eq!(offset_of!(ProcessConfig, app_dex), 40);
    assert_eq!(offset_of!(ProcessConfig, heap_initial_bytes), 48);
    assert_eq!(offset_of!(ProcessConfig, heap_maximum_bytes), 56);
    assert_eq!(offset_of!(ProcessConfig, host_context), 64);
    assert_eq!(offset_of!(ProcessConfig, frame_callback), 72);
    assert_eq!(offset_of!(ProcessConfig, provider_context), 80);
    assert_eq!(offset_of!(ProcessConfig, provider_acquire), 88);
    assert_eq!(offset_of!(ProcessConfig, provider_release), 96);
}

#[test]
fn process_result_matches_c_abi_v1() {
    assert_eq!(size_of::<ProcessResult>(), 36);
    assert_eq!(align_of::<ProcessResult>(), 4);
    assert_eq!(offset_of!(ProcessResult, struct_size), 0);
    assert_eq!(offset_of!(ProcessResult, abi_version), 4);
    assert_eq!(offset_of!(ProcessResult, hello_answer), 8);
    assert_eq!(offset_of!(ProcessResult, native_round_trip), 12);
    assert_eq!(offset_of!(ProcessResult, arraycopy_result), 16);
    assert_eq!(offset_of!(ProcessResult, activity_probe_result), 20);
    assert_eq!(offset_of!(ProcessResult, lifecycle_result), 24);
    assert_eq!(offset_of!(ProcessResult, frame_width), 28);
    assert_eq!(offset_of!(ProcessResult, frame_height), 32);
}

#[test]
fn surface_create_info_matches_c_abi() {
    assert_eq!(size_of::<SurfaceCreateInfo>(), 24);
    assert_eq!(align_of::<SurfaceCreateInfo>(), 8);
    assert_eq!(offset_of!(SurfaceCreateInfo, width), 0);
    assert_eq!(offset_of!(SurfaceCreateInfo, height), 4);
    assert_eq!(offset_of!(SurfaceCreateInfo, title), 8);
    assert_eq!(offset_of!(SurfaceCreateInfo, visible), 16);
}

#[test]
fn callback_copies_strided_frame_into_owned_packed_vec() {
    let source = [1_u32, 2, 99, 3, 4, 99];
    let mut host = FrameHost {
        frames_received: 0,
        last_frame: None,
    };
    // SAFETY: source contains two rows of three u32 values and the callback
    // copies only the first two pixels from each row.
    assert!(unsafe { host.receive(source.as_ptr(), 2, 2, 3 * size_of::<u32>()) });
    assert_eq!(host.frames_received, 1);
    assert_eq!(
        host.last_frame,
        Some(OwnedFrame {
            width: 2,
            height: 2,
            argb_pixels: vec![1, 2, 3, 4],
        })
    );
}

#[test]
fn callback_rejects_invalid_stride() {
    let source = [1_u32, 2, 3, 4];
    let mut host = FrameHost {
        frames_received: 0,
        last_frame: None,
    };
    assert!(!unsafe { host.receive(source.as_ptr(), 2, 2, size_of::<u32>()) });
    assert!(host.last_frame.is_none());
}

#[test]
fn callback_function_pointer_has_c_pointer_layout() {
    assert_eq!(size_of::<Option<FrameCallback>>(), size_of::<*mut c_void>());
    assert_eq!(
        align_of::<Option<FrameCallback>>(),
        align_of::<*mut c_void>()
    );
}

#[test]
fn visible_duration_matches_surface_contract() {
    let mut options = RunOptions {
        library: PathBuf::new(),
        core_oj_jar: PathBuf::new(),
        core_libart_jar: PathBuf::new(),
        framework_jar: PathBuf::new(),
        core_icu4j_jar: PathBuf::new(),
        app_dex: PathBuf::new(),
        heap_initial_bytes: 0,
        heap_maximum_bytes: 0,
        visible_seconds: 86_400.001,
    };
    assert!(matches!(
        run(&options),
        Err(HostError::InvalidVisibleSeconds(_))
    ));
    options.visible_seconds = -0.001;
    assert!(matches!(
        run(&options),
        Err(HostError::InvalidVisibleSeconds(_))
    ));
}
