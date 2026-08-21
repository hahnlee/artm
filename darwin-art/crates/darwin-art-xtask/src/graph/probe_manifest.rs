//! Input and content-stamp manifest for independently cached native probes.
//!
//! Probe invalidation is deliberately separate from the broad runtime graph
//! digest. This module owns the phase input closure and stable content stamps;
//! graph string emission consumes the resulting value-only manifest.

use std::io;
use std::path::{Path, PathBuf};

use super::inputs::{probe_content_stamp, probe_inputs};

pub(crate) struct ProbeGraphInputs {
    pub(crate) filesystem_probe_inputs: String,
    pub(crate) network_probe_inputs: String,
    pub(crate) hwui_probe_inputs: String,
    pub(crate) graphics_probe_inputs: String,
    pub(crate) graphics_phase_inputs: String,
    pub(crate) graphics_input_inputs: String,
    pub(crate) graphics_state_inputs: String,
    pub(crate) graphics_session_inputs: String,
    pub(crate) jni_acceptance_inputs: String,
    pub(crate) app_bootstrap_inputs: String,
    pub(crate) app_resources_inputs: String,
    pub(crate) app_presentation_inputs: String,
    pub(crate) filesystem_probe_stamp: PathBuf,
    pub(crate) network_probe_stamp: PathBuf,
    pub(crate) hwui_probe_stamp: PathBuf,
    pub(crate) graphics_probe_stamp: PathBuf,
    pub(crate) graphics_phase_stamp: PathBuf,
    pub(crate) graphics_input_stamp: PathBuf,
    pub(crate) graphics_state_stamp: PathBuf,
    pub(crate) graphics_session_stamp: PathBuf,
    pub(crate) jni_acceptance_stamp: PathBuf,
    pub(crate) app_bootstrap_stamp: PathBuf,
    pub(crate) app_resources_stamp: PathBuf,
    pub(crate) app_presentation_stamp: PathBuf,
    pub(crate) runtime_entry_stamp: PathBuf,
}

pub(crate) fn collect(root: &Path) -> io::Result<ProbeGraphInputs> {
    let filesystem_probe_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_filesystem_probe.cc",
            "probes/runtime_filesystem_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-ioctl-facade/include/darwin_art_bionic_ioctl.h",
        ],
    );
    let network_probe_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_network_probe.cc",
            "probes/runtime_network_probe.h",
        ],
    );
    let hwui_probe_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_hwui_probe.cc",
            "probes/runtime_hwui_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-socket-broker-adapter/include/darwin_art_bionic_socket_broker.h",
        ],
    );
    let graphics_probe_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_graphics_probe.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_registration_phase.cc",
            "probes/runtime_registration_phase.h",
            "compat/darwin_surface_bridge.h",
            "compat/darwin_framework_natives.h",
            "compat/darwin_hwui_gpu_mode.h",
        ],
    );
    let graphics_phase_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_graphics_phase.cc",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_frame_probe.h",
        ],
    );
    let graphics_input_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_graphics_input.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_probe_internal.h",
            "probes/runtime_process_state.h",
        ],
    );
    let graphics_state_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_graphics_state.cc",
            "probes/runtime_graphics_state.h",
            "compat/darwin_surface_bridge.h",
        ],
    );
    let graphics_session_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_graphics_session.cc",
            "probes/runtime_graphics_session.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_state.h",
            "probes/runtime_process_state.h",
            "include/darwin_art/darwin_art.h",
        ],
    );
    let jni_acceptance_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_jni_acceptance_probe.cc",
            "probes/runtime_jni_acceptance_probe.h",
            "probes/runtime_abi_probe.h",
            "probes/runtime_jni_scope.h",
        ],
    );
    let app_bootstrap_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_app_bootstrap.cc",
            "probes/runtime_app_bootstrap.h",
            "probes/runtime_process_state.h",
        ],
    );
    let app_presentation_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_app_presentation.cc",
            "probes/runtime_app_presentation.h",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_state.h",
        ],
    );
    let app_resources_inputs = probe_inputs(
        root,
        &[
            "probes/runtime_app_resources.cc",
            "probes/runtime_app_resources.h",
        ],
    );
    // Ninja normally invalidates by mtime.  These stable content stamps make
    // the probe boundary robust when a checkout/materializer preserves source
    // mtimes (and keep each phase's content identity independent of the broad
    // runtime graph digest).  The audit path regenerates the graph before
    // asking Ninja for its warm/no-op result.
    let filesystem_probe_stamp = probe_content_stamp(
        root,
        "filesystem",
        &[
            "probes/runtime_filesystem_probe.cc",
            "probes/runtime_filesystem_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-ioctl-facade/include/darwin_art_bionic_ioctl.h",
        ],
    )?;
    let network_probe_stamp = probe_content_stamp(
        root,
        "network",
        &[
            "probes/runtime_network_probe.cc",
            "probes/runtime_network_probe.h",
        ],
    )?;
    let hwui_probe_stamp = probe_content_stamp(
        root,
        "hwui",
        &[
            "probes/runtime_hwui_probe.cc",
            "probes/runtime_hwui_probe.h",
            "tools/bionic-fs-facade/include/darwin_art_bionic_fs.h",
            "tools/bionic-socket-broker-adapter/include/darwin_art_bionic_socket_broker.h",
        ],
    )?;
    let graphics_probe_stamp = probe_content_stamp(
        root,
        "graphics",
        &[
            "probes/runtime_graphics_probe.cc",
            "probes/runtime_graphics_probe.h",
            "compat/darwin_surface_bridge.h",
            "compat/darwin_framework_natives.h",
            "compat/darwin_hwui_gpu_mode.h",
        ],
    )?;
    let graphics_phase_stamp = probe_content_stamp(
        root,
        "graphics-phase",
        &[
            "probes/runtime_graphics_phase.cc",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_frame_probe.h",
        ],
    )?;
    let graphics_input_stamp = probe_content_stamp(
        root,
        "graphics-input",
        &[
            "probes/runtime_graphics_input.cc",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_probe_internal.h",
            "probes/runtime_process_state.h",
        ],
    )?;
    let graphics_state_stamp = probe_content_stamp(
        root,
        "graphics-state",
        &[
            "probes/runtime_graphics_state.cc",
            "probes/runtime_graphics_state.h",
            "compat/darwin_surface_bridge.h",
        ],
    )?;
    let graphics_session_stamp = probe_content_stamp(
        root,
        "graphics-session",
        &[
            "probes/runtime_graphics_session.cc",
            "probes/runtime_graphics_session.h",
            "probes/runtime_graphics_probe.h",
            "probes/runtime_graphics_state.h",
        ],
    )?;
    let jni_acceptance_stamp = probe_content_stamp(
        root,
        "jni-acceptance",
        &[
            "probes/runtime_jni_acceptance_probe.cc",
            "probes/runtime_jni_acceptance_probe.h",
            "probes/runtime_abi_probe.h",
            "probes/runtime_jni_scope.h",
        ],
    )?;
    let app_bootstrap_stamp = probe_content_stamp(
        root,
        "app-bootstrap",
        &[
            "probes/runtime_app_bootstrap.cc",
            "probes/runtime_app_bootstrap.h",
            "probes/runtime_process_state.h",
        ],
    )?;
    let app_presentation_stamp = probe_content_stamp(
        root,
        "app-presentation",
        &[
            "probes/runtime_app_presentation.cc",
            "probes/runtime_app_presentation.h",
            "probes/runtime_graphics_phase.h",
            "probes/runtime_graphics_state.h",
        ],
    )?;
    let app_resources_stamp = probe_content_stamp(
        root,
        "app-resources",
        &[
            "probes/runtime_app_resources.cc",
            "probes/runtime_app_resources.h",
        ],
    )?;
    // The graphics link command compiles the process entry TU as part of its
    // phase orchestration. Keep that source's content identity as an explicit
    // Ninja prerequisite so a checkout that preserves mtimes cannot reuse a
    // dylib linked against an older entry point.
    let runtime_entry_stamp = probe_content_stamp(
        root,
        "runtime-entry",
        &[
            "probes/runtime_entry_probe.cc",
            "probes/runtime_registration_phase.cc",
            "probes/runtime_registration_phase.h",
            "probes/runtime_process_state.h",
            "probes/runtime_process_options.h",
            "probes/runtime_shutdown_probe.h",
            "probes/runtime_graphics_session.h",
            "probes/runtime_app_presentation.h",
        ],
    )?;
    Ok(ProbeGraphInputs {
        filesystem_probe_inputs,
        network_probe_inputs,
        hwui_probe_inputs,
        graphics_probe_inputs,
        graphics_phase_inputs,
        graphics_input_inputs,
        graphics_state_inputs,
        graphics_session_inputs,
        jni_acceptance_inputs,
        app_bootstrap_inputs,
        app_resources_inputs,
        app_presentation_inputs,
        filesystem_probe_stamp,
        network_probe_stamp,
        hwui_probe_stamp,
        graphics_probe_stamp,
        graphics_phase_stamp,
        graphics_input_stamp,
        graphics_state_stamp,
        graphics_session_stamp,
        jni_acceptance_stamp,
        app_bootstrap_stamp,
        app_resources_stamp,
        app_presentation_stamp,
        runtime_entry_stamp,
    })
}
