use super::*;
use crate::frame::FrameHost;
use std::mem::size_of;
use std::path::PathBuf;

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
        terminate_android_process: false,
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
