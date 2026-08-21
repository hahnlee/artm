use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use super::super::ninja_path;
use super::cache::CachedNativeObject;
use super::inputs::collect_files;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum FoundationFamily {
    Hwui,
    GraphicsJni,
    Icu,
}

pub(crate) fn foundation_inputs(root: &Path) -> Vec<PathBuf> {
    let mut paths = vec![
        PathBuf::from("tools/build-android16-hwui-static-foundation.sh"),
        PathBuf::from("tools/build-android16-android-graphics-jni.sh"),
        PathBuf::from("upstream/android16-hwui-static-foundation.lock"),
        PathBuf::from("upstream/android16-android-graphics-jni.lock"),
        PathBuf::from("upstream/android16-hwui-gpu.lock"),
        PathBuf::from("tools/build-android16-icu-foundation.sh"),
        PathBuf::from("upstream/android16-icu-foundation.lock"),
        PathBuf::from("patches/frameworks-base/0001-darwin-android-critical-jni-abi.patch"),
        PathBuf::from("patches/frameworks-base/0002-darwin-lazy-native-window-jni.patch"),
        PathBuf::from("patches/frameworks-base/0003-darwin-hwui-gpu-layoutlib.patch"),
        PathBuf::from("patches/frameworks-base/0005-darwin-hwui-animation-pulse.patch"),
    ];
    for directory in [
        "_aosp/frameworks/base/libs/hwui",
        "_aosp/external/libjpeg-turbo",
        "_aosp/external/libultrahdr",
        "_aosp/frameworks/native/libs/gui",
        "_aosp/frameworks/av/media/ndk",
        "_aosp/hardware/libhardware/include_all",
        "_aosp/external/icu-graphics",
    ] {
        collect_files(&root.join(directory), root, &mut paths);
    }
    paths.sort();
    paths.dedup();
    paths.retain(|path| root.join(path).is_file());
    paths
}

pub(crate) fn foundation_input_list(
    root: &Path,
    inputs: &[PathBuf],
    family: FoundationFamily,
) -> String {
    inputs
        .iter()
        .filter(|path| is_foundation_family_input(path, family))
        .map(|path| ninja_path(&root.join(path)))
        .collect::<Vec<_>>()
        .join(" ")
}

pub(crate) fn is_foundation_family_input(path: &Path, family: FoundationFamily) -> bool {
    let path = path.to_string_lossy();
    let shared = path.starts_with("patches/frameworks-base/");
    match family {
        FoundationFamily::Hwui => {
            shared
                || matches!(
                    path.as_ref(),
                    "tools/build-android16-hwui-static-foundation.sh"
                        | "upstream/android16-hwui-static-foundation.lock"
                        | "upstream/android16-hwui-gpu.lock"
                )
                || path.starts_with("_aosp/frameworks/base/libs/hwui/")
                || path.starts_with("_aosp/hwui-static-deps/")
        }
        FoundationFamily::GraphicsJni => {
            shared
                || matches!(
                    path.as_ref(),
                    "tools/build-android16-android-graphics-jni.sh"
                        | "upstream/android16-android-graphics-jni.lock"
                        | "upstream/android16-hwui-gpu.lock"
                )
                || path.starts_with("_aosp/frameworks/base/libs/hwui/")
                || path.starts_with("_aosp/external/libjpeg-turbo/")
                || path.starts_with("_aosp/external/libultrahdr/")
                || path.starts_with("_aosp/frameworks/native/libs/gui/")
                || path.starts_with("_aosp/frameworks/av/media/ndk/")
                || path.starts_with("_aosp/hardware/libhardware/include_all/")
        }
        FoundationFamily::Icu => {
            path == "tools/build-android16-icu-foundation.sh"
                || path == "upstream/android16-icu-foundation.lock"
                || path.starts_with("_aosp/external/icu-graphics/")
        }
    }
}

pub(crate) fn cached_foundation_objects(object_dir: &Path) -> io::Result<Vec<CachedNativeObject>> {
    let mut objects = Vec::new();
    let mut pending = vec![object_dir.to_path_buf()];
    while let Some(directory) = pending.pop() {
        let Ok(entries) = fs::read_dir(directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                pending.push(path);
                continue;
            }
            if path.extension().and_then(|ext| ext.to_str()) != Some("command") {
                continue;
            }
            let object = path.with_extension("");
            if !object.is_file() {
                continue;
            }
            let command = fs::read_to_string(&path)?.trim().to_owned();
            let tokens = command.split_whitespace().collect::<Vec<_>>();
            let Some(source_index) = tokens.iter().position(|token| *token == "-c") else {
                continue;
            };
            let Some(source_token) = tokens.get(source_index + 1) else {
                continue;
            };
            let source = PathBuf::from(source_token.trim_matches('\''));
            if !source.is_file() {
                continue;
            }
            objects.push(CachedNativeObject {
                object,
                source,
                command,
                shell_quoted: true,
            });
        }
    }
    objects.sort_by(|left, right| left.object.cmp(&right.object));
    Ok(objects)
}
