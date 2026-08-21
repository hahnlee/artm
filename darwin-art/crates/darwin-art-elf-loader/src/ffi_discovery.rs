//! Trusted directory and metadata discovery for the ELF C ABI.
//!
//! This phase owns byte-component validation and bounded sibling-graph reads;
//! handle loading and exported C entrypoints remain in `ffi.rs`.

use super::*;

fn cstring_from_dynamic(bytes: Vec<u8>, what: &'static str) -> Result<CString, FfiFailure> {
    CString::new(bytes).map_err(|_| FfiFailure::Format(format!("{what} contains embedded NUL")))
}

pub(super) fn inspection_from_bytes(bytes: &[u8]) -> Result<DarwinArtElfInspection, FfiFailure> {
    let metadata = inspect_elf_metadata(bytes).map_err(FfiFailure::Load)?;
    let soname = metadata
        .soname
        .map(|name| cstring_from_dynamic(name, "DT_SONAME"))
        .transpose()?;
    let needed = metadata
        .needed_libraries
        .into_iter()
        .map(|name| cstring_from_dynamic(name, "DT_NEEDED"))
        .collect::<Result<Vec<_>, _>>()?;
    Ok(DarwinArtElfInspection { soname, needed })
}

pub(super) fn validate_discovery_component(bytes: &[u8], what: &str) -> Result<(), FfiFailure> {
    let invalid = bytes.is_empty()
        || bytes.len() > MAX_DISCOVERY_COMPONENT_SIZE
        || bytes.contains(&0)
        || bytes.contains(&b'/')
        || bytes == b"."
        || bytes == b"..";
    if invalid {
        return Err(FfiFailure::InvalidOwned(format!(
            "{what} must be one nonempty byte component without NUL, slash, dot, or dot-dot and at most {MAX_DISCOVERY_COMPONENT_SIZE} bytes"
        )));
    }
    Ok(())
}

fn read_discovery_file(
    broker: &ReadOnlyBroker,
    component: &[u8],
    root_is_elf: Option<&mut bool>,
) -> Result<Vec<u8>, FfiFailure> {
    validate_discovery_component(component, "ELF graph filename")?;
    let opened = broker.open(component).map_err(|error| {
        FfiFailure::Io(format!("secure ELF graph component open failed: {error}"))
    })?;
    if !opened.metadata().is_file() {
        return Err(FfiFailure::InvalidOwned(
            "ELF graph component is not a regular file".to_owned(),
        ));
    }
    let declared = usize::try_from(opened.metadata().len()).map_err(|_| {
        FfiFailure::Bounds("ELF graph component size does not fit usize".to_owned())
    })?;
    let mut file = opened.into_file();
    let mut bytes = Vec::with_capacity(4);
    while bytes.len() < 4 {
        let mut prefix = [0_u8; 4];
        let read = file
            .read(&mut prefix[..4 - bytes.len()])
            .map_err(|error| FfiFailure::Io(format!("secure ELF graph read failed: {error}")))?;
        if read == 0 {
            break;
        }
        bytes.extend_from_slice(&prefix[..read]);
    }
    if let Some(root_is_elf) = root_is_elf {
        *root_is_elf = bytes.as_slice() == b"\x7fELF";
    }
    if declared == 0 || declared > MAX_DISCOVERY_FILE_SIZE {
        return Err(FfiFailure::Bounds(format!(
            "ELF graph component is outside the 1..={MAX_DISCOVERY_FILE_SIZE} byte file cap"
        )));
    }
    bytes.reserve_exact(declared.saturating_sub(bytes.len()));
    let mut file = file.take((MAX_DISCOVERY_FILE_SIZE + 1 - bytes.len()) as u64);
    file.read_to_end(&mut bytes)
        .map_err(|error| FfiFailure::Io(format!("secure ELF graph read failed: {error}")))?;
    if bytes.len() != declared {
        return Err(FfiFailure::Io(
            "ELF graph component changed size while its authorized descriptor was read".to_owned(),
        ));
    }
    Ok(bytes)
}

pub(super) fn discover_sibling_graph(
    directory_fd: i32,
    root_component: &[u8],
    providers: HashSet<Vec<u8>>,
    root_is_elf: &mut bool,
) -> Result<DarwinArtElfDiscoveredGraph, FfiFailure> {
    validate_discovery_component(root_component, "root ELF filename")?;
    if directory_fd < 0 {
        return Err(FfiFailure::Invalid("library directory fd is negative"));
    }
    // SAFETY: dup creates an independently owned descriptor or returns -1.
    let duplicated = unsafe { dup(directory_fd) };
    if duplicated < 0 {
        return Err(FfiFailure::Io(
            "could not duplicate trusted library directory fd".to_owned(),
        ));
    }
    // SAFETY: duplicated is a fresh descriptor now uniquely owned by File.
    let directory = unsafe { File::from_raw_fd(duplicated) };
    let broker = ReadOnlyBroker::from_directory(directory)
        .map_err(|error| FfiFailure::Io(format!("invalid trusted library directory: {error}")))?;

    let mut queue = VecDeque::from([(root_component.to_vec(), None::<Vec<u8>>)]);
    let mut queued = HashSet::from([root_component.to_vec()]);
    let mut discovered_sonames = HashSet::<Vec<u8>>::new();
    let mut names = Vec::<CString>::new();
    let mut graph_bytes = Vec::<Vec<u8>>::new();
    let mut total_size = 0_usize;
    let mut root_soname = None::<CString>;

    let mut first_component = true;
    while let Some((component, expected_soname)) = queue.pop_front() {
        if let Some(expected) = expected_soname.as_ref()
            && discovered_sonames.contains(expected)
        {
            continue;
        }
        if graph_bytes.len() >= MAX_DISCOVERY_FILES {
            return Err(FfiFailure::Bounds(format!(
                "ELF sibling graph exceeds the {MAX_DISCOVERY_FILES}-file cap"
            )));
        }
        let bytes = read_discovery_file(
            &broker,
            &component,
            first_component.then_some(&mut *root_is_elf),
        )?;
        if first_component {
            first_component = false;
            if !*root_is_elf {
                return Err(FfiFailure::Format("invalid ELF: bad magic".to_owned()));
            }
        }
        total_size = total_size
            .checked_add(bytes.len())
            .ok_or_else(|| FfiFailure::Bounds("ELF graph total size overflow".to_owned()))?;
        if total_size > MAX_DISCOVERY_TOTAL_SIZE {
            return Err(FfiFailure::Bounds(format!(
                "ELF sibling graph exceeds the {MAX_DISCOVERY_TOTAL_SIZE}-byte total cap"
            )));
        }
        let metadata = inspect_elf_metadata(&bytes).map_err(FfiFailure::Load)?;
        let embedded = metadata
            .soname
            .ok_or_else(|| FfiFailure::Format("ELF graph member lacks DT_SONAME".to_owned()))?;
        validate_discovery_component(&embedded, "embedded DT_SONAME")?;
        if providers.contains(&embedded) {
            return Err(FfiFailure::Format(
                "real ELF graph member collides with a builtin provider SONAME".to_owned(),
            ));
        }
        if let Some(expected) = expected_soname.as_ref()
            && embedded != *expected
        {
            return Err(FfiFailure::Format(format!(
                "dependency embedded DT_SONAME does not exactly match requested sibling {}",
                String::from_utf8_lossy(expected)
            )));
        }
        if !discovered_sonames.insert(embedded.clone()) {
            return Err(FfiFailure::Format(
                "two graph paths produced the same embedded DT_SONAME".to_owned(),
            ));
        }
        std::str::from_utf8(&embedded).map_err(|_| {
            FfiFailure::Format(
                "embedded DT_SONAME is not UTF-8; the closed graph namespace cannot key it"
                    .to_owned(),
            )
        })?;
        let name = cstring_from_dynamic(embedded.clone(), "DT_SONAME")?;
        if root_soname.is_none() {
            root_soname = Some(name.clone());
        }
        names.push(name);
        graph_bytes.push(bytes);

        for needed in metadata.needed_libraries {
            validate_discovery_component(&needed, "DT_NEEDED dependency filename")?;
            if providers.contains(&needed) || discovered_sonames.contains(&needed) {
                continue;
            }
            std::str::from_utf8(&needed).map_err(|_| {
                FfiFailure::Format(
                    "DT_NEEDED dependency filename is not UTF-8; the closed graph namespace cannot key it"
                        .to_owned(),
                )
            })?;
            if queued.insert(needed.clone()) {
                queue.push_back((needed.clone(), Some(needed)));
            }
        }
    }

    let mut sources = Vec::with_capacity(names.len());
    for (name, bytes) in names.iter().zip(&graph_bytes) {
        sources.push(DarwinArtElfGraphSource {
            soname: name.as_ptr(),
            bytes: bytes.as_ptr(),
            length: bytes.len(),
        });
    }
    Ok(DarwinArtElfDiscoveredGraph {
        root_soname: root_soname.expect("nonempty discovery has one root"),
        _names: names,
        _bytes: graph_bytes,
        sources,
    })
}
