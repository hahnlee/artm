use crate::{
    ClosedElfNamespace, LoadError, LoadedElf, LoadedElfGraph, NamespaceError, ResolveError,
    ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::ffi::{CStr, CString, c_char, c_void};
use std::num::NonZeroUsize;
use std::os::unix::ffi::OsStrExt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::ptr;
use std::ptr::NonNull;
use std::sync::{Mutex, MutexGuard};
use std::{fs::File, io::Read};

const ABI_VERSION: u32 = 1;
const MAX_INPUT_SIZE: usize = 1024 * 1024 * 1024;

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DarwinArtElfStatus {
    Ok = 0,
    InvalidArgument = 1,
    Io = 2,
    Format = 3,
    Bounds = 4,
    Capability = 5,
    Protection = 6,
    Resolver = 7,
    UnresolvedSymbol = 8,
    SymbolNotFound = 9,
    InvalidSymbol = 10,
    Lifecycle = 11,
    System = 12,
    Poisoned = 13,
    Panic = 14,
}

#[repr(C)]
pub struct DarwinArtElfErrorBuffer {
    pub data: *mut c_char,
    pub capacity: usize,
    pub required: usize,
}

#[repr(C)]
pub struct DarwinArtElfSymbolRequest {
    pub abi_version: u32,
    pub symbol: *const c_char,
    pub version_soname: *const c_char,
    pub version_name: *const c_char,
    pub version_flags: u16,
    pub version_hidden: u8,
    pub reserved: u8,
    pub needed_libraries: *const *const c_char,
    pub needed_library_count: usize,
}

pub type DarwinArtElfResolverCallback = unsafe extern "C" fn(
    context: *mut c_void,
    request: *const DarwinArtElfSymbolRequest,
    out_address: *mut usize,
    error: *mut DarwinArtElfErrorBuffer,
) -> i32;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DarwinArtElfLoadOptions {
    pub abi_version: u32,
    pub resolver: Option<DarwinArtElfResolverCallback>,
    pub resolver_context: *mut c_void,
}

pub struct DarwinArtElfHandle {
    image: Mutex<LoadedElf>,
}

pub struct DarwinArtElfGraphHandle {
    graph: Mutex<LoadedElfGraph>,
}

#[repr(C)]
pub struct DarwinArtElfGraphSource {
    pub soname: *const c_char,
    pub bytes: *const u8,
    pub length: usize,
}

enum FfiFailure {
    Invalid(&'static str),
    Io(String),
    Load(LoadError),
    Namespace(NamespaceError),
    Poisoned,
}

impl FfiFailure {
    fn status(&self) -> DarwinArtElfStatus {
        match self {
            Self::Invalid(_) => DarwinArtElfStatus::InvalidArgument,
            Self::Io(_) => DarwinArtElfStatus::Io,
            Self::Poisoned => DarwinArtElfStatus::Poisoned,
            Self::Load(error) => load_error_status(error),
            Self::Namespace(error) => match error {
                NamespaceError::DuplicateSoname(_) => DarwinArtElfStatus::InvalidArgument,
                NamespaceError::UnknownDependency { .. } => DarwinArtElfStatus::UnresolvedSymbol,
                NamespaceError::SonameMismatch { .. } => DarwinArtElfStatus::Format,
                NamespaceError::Load { source, .. } => load_error_status(source),
            },
        }
    }

    fn message(&self) -> String {
        match self {
            Self::Invalid(message) => (*message).to_owned(),
            Self::Io(message) => message.clone(),
            Self::Load(error) => error.to_string(),
            Self::Namespace(error) => error.to_string(),
            Self::Poisoned => "ELF handle state was poisoned by a contained panic".to_owned(),
        }
    }
}

fn load_error_status(error: &LoadError) -> DarwinArtElfStatus {
    match error {
        LoadError::Format(_) => DarwinArtElfStatus::Format,
        LoadError::Bounds(_) => DarwinArtElfStatus::Bounds,
        LoadError::Capability(_) => DarwinArtElfStatus::Capability,
        LoadError::Protection(_) => DarwinArtElfStatus::Protection,
        LoadError::Resolver { .. } => DarwinArtElfStatus::Resolver,
        LoadError::UnresolvedSymbol { .. } => DarwinArtElfStatus::UnresolvedSymbol,
        LoadError::SymbolNotFound(_) => DarwinArtElfStatus::SymbolNotFound,
        LoadError::InvalidSymbol(_) => DarwinArtElfStatus::InvalidSymbol,
        LoadError::InitializersAlreadyRun | LoadError::InitializersNotRun => {
            DarwinArtElfStatus::Lifecycle
        }
        LoadError::System { .. } => DarwinArtElfStatus::System,
    }
}

struct ErrorSink {
    buffer: *mut DarwinArtElfErrorBuffer,
}

impl ErrorSink {
    unsafe fn new(buffer: *mut DarwinArtElfErrorBuffer) -> Result<Self, DarwinArtElfStatus> {
        if buffer.is_null() {
            return Ok(Self { buffer });
        }
        // SAFETY: the C ABI requires a live writable error-buffer structure for this call.
        let buffer_ref = unsafe { &mut *buffer };
        buffer_ref.required = 0;
        if buffer_ref.capacity != 0 {
            if buffer_ref.data.is_null() {
                return Err(DarwinArtElfStatus::InvalidArgument);
            }
            // SAFETY: the caller promises capacity writable bytes at data.
            unsafe { *buffer_ref.data = 0 };
        }
        Ok(Self { buffer })
    }

    fn write(&mut self, message: &str) {
        if self.buffer.is_null() {
            return;
        }
        // SAFETY: validated by ErrorSink::new and alive for the duration of the FFI call.
        let buffer = unsafe { &mut *self.buffer };
        buffer.required = message.len().saturating_add(1);
        if buffer.capacity == 0 {
            return;
        }
        let copied = message.len().min(buffer.capacity - 1);
        // SAFETY: ErrorSink::new validated data and the copy is bounded by capacity.
        unsafe {
            ptr::copy_nonoverlapping(message.as_ptr(), buffer.data.cast::<u8>(), copied);
            *buffer.data.add(copied) = 0;
        }
    }
}

fn ffi_call(
    error: *mut DarwinArtElfErrorBuffer,
    operation: impl FnOnce() -> Result<(), FfiFailure>,
) -> DarwinArtElfStatus {
    // SAFETY: pointer validity is part of the C ABI contract; shape is checked here.
    let mut sink = match unsafe { ErrorSink::new(error) } {
        Ok(sink) => sink,
        Err(status) => return status,
    };
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(())) => DarwinArtElfStatus::Ok,
        Ok(Err(failure)) => {
            let status = failure.status();
            sink.write(&failure.message());
            status
        }
        Err(payload) => {
            let message = payload
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
                .unwrap_or("unknown Rust panic");
            sink.write(&format!("Rust panic contained at ELF C ABI: {message}"));
            DarwinArtElfStatus::Panic
        }
    }
}

struct CallbackResolver {
    callback: DarwinArtElfResolverCallback,
    context: *mut c_void,
}

impl SymbolResolver for CallbackResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        let symbol = make_c_string(request.symbol, "symbol")?;
        let needed = request
            .needed_libraries
            .iter()
            .map(|name| make_c_string(name, "DT_NEEDED"))
            .collect::<Result<Vec<_>, _>>()?;
        let needed_pointers = needed.iter().map(|name| name.as_ptr()).collect::<Vec<_>>();
        let version_soname = request
            .version
            .map(|version| make_c_string(version.soname, "version SONAME"))
            .transpose()?;
        let version_name = request
            .version
            .map(|version| make_c_string(version.name, "version name"))
            .transpose()?;
        let c_request = DarwinArtElfSymbolRequest {
            abi_version: ABI_VERSION,
            symbol: symbol.as_ptr(),
            version_soname: version_soname
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
            version_name: version_name
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
            version_flags: request.version.map_or(0, |version| version.flags),
            version_hidden: u8::from(request.version.is_some_and(|version| version.hidden)),
            reserved: 0,
            needed_libraries: if needed_pointers.is_empty() {
                ptr::null()
            } else {
                needed_pointers.as_ptr()
            },
            needed_library_count: needed_pointers.len(),
        };
        let mut address = 0_usize;
        let mut callback_message = [0_u8; 512];
        let mut callback_error = DarwinArtElfErrorBuffer {
            data: callback_message.as_mut_ptr().cast(),
            capacity: callback_message.len(),
            required: 0,
        };
        // SAFETY: all request pointers remain alive through this synchronous callback. The C
        // contract forbids callback exceptions/unwinds and requires valid output pointers.
        let status =
            unsafe { (self.callback)(self.context, &c_request, &mut address, &mut callback_error) };
        match status {
            0 => {
                let address = NonZeroUsize::new(address).ok_or_else(|| {
                    ResolveError::Rejected("resolver returned FOUND with a null address".to_owned())
                })?;
                // SAFETY: callback implementors own the provider ABI/lifetime guarantee. The C
                // header requires the address to remain valid until handle unload.
                Ok(Some(unsafe { ResolvedSymbol::new(address) }))
            }
            1 => Ok(None),
            2 => Err(ResolveError::Rejected(callback_error_message(
                &callback_message,
            ))),
            other => Err(ResolveError::Rejected(format!(
                "resolver returned invalid status {other}"
            ))),
        }
    }
}

fn make_c_string(value: &str, field: &'static str) -> Result<CString, ResolveError> {
    CString::new(value)
        .map_err(|_| ResolveError::Rejected(format!("{field} contains an interior NUL")))
}

fn callback_error_message(buffer: &[u8]) -> String {
    let end = buffer
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(buffer.len());
    if end == 0 {
        "resolver callback reported an error".to_owned()
    } else {
        String::from_utf8_lossy(&buffer[..end]).into_owned()
    }
}

fn options_from_pointer(
    options: *const DarwinArtElfLoadOptions,
) -> Result<Option<DarwinArtElfLoadOptions>, FfiFailure> {
    if options.is_null() {
        return Ok(None);
    }
    // SAFETY: caller supplies a readable options structure for the duration of the call.
    let options = unsafe { *options };
    if options.abi_version != ABI_VERSION {
        return Err(FfiFailure::Invalid("unsupported load-options ABI version"));
    }
    if options.resolver.is_none() && !options.resolver_context.is_null() {
        return Err(FfiFailure::Invalid(
            "resolver_context is non-null without a resolver callback",
        ));
    }
    Ok(Some(options))
}

fn load_image(
    bytes: &[u8],
    options: Option<DarwinArtElfLoadOptions>,
) -> Result<LoadedElf, FfiFailure> {
    if bytes.len() > MAX_INPUT_SIZE {
        return Err(FfiFailure::Invalid("ELF input exceeds 1 GiB limit"));
    }
    match options.and_then(|options| {
        options
            .resolver
            .map(|callback| (callback, options.resolver_context))
    }) {
        Some((callback, context)) => {
            let mut resolver = CallbackResolver { callback, context };
            LoadedElf::load_with_resolver(bytes, &mut resolver).map_err(FfiFailure::Load)
        }
        None => LoadedElf::load(bytes).map_err(FfiFailure::Load),
    }
}

fn lock_image(handle: &DarwinArtElfHandle) -> Result<MutexGuard<'_, LoadedElf>, FfiFailure> {
    handle.image.lock().map_err(|_| FfiFailure::Poisoned)
}

fn lock_graph(
    handle: &DarwinArtElfGraphHandle,
) -> Result<MutexGuard<'_, LoadedElfGraph>, FfiFailure> {
    handle.graph.lock().map_err(|_| FfiFailure::Poisoned)
}

unsafe fn required_utf8(value: *const c_char, field: &'static str) -> Result<String, FfiFailure> {
    if value.is_null() {
        return Err(FfiFailure::Invalid(field));
    }
    // SAFETY: the caller promises a live NUL-terminated string for this synchronous call.
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| FfiFailure::Invalid("ELF graph string is not UTF-8"))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_elf_abi_version() -> u32 {
    ABI_VERSION
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_load_bytes(
    bytes: *const u8,
    length: usize,
    options: *const DarwinArtElfLoadOptions,
    out_handle: *mut *mut DarwinArtElfHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if out_handle.is_null() {
            return Err(FfiFailure::Invalid("out_handle is null"));
        }
        // SAFETY: validated non-null writable out parameter.
        unsafe { *out_handle = ptr::null_mut() };
        if length != 0 && bytes.is_null() {
            return Err(FfiFailure::Invalid("bytes is null for nonzero length"));
        }
        let options = options_from_pointer(options)?;
        // SAFETY: C contract supplies length readable bytes; a null pointer is accepted only for
        // the zero-length case and replaced with a non-null dangling pointer.
        let bytes = unsafe {
            std::slice::from_raw_parts(
                if bytes.is_null() {
                    NonZeroUsize::MIN.get() as *const u8
                } else {
                    bytes
                },
                length,
            )
        };
        let image = load_image(bytes, options)?;
        let handle = Box::new(DarwinArtElfHandle {
            image: Mutex::new(image),
        });
        // SAFETY: out_handle is writable and now owns the Box until unload.
        unsafe { *out_handle = Box::into_raw(handle) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_load_path(
    path: *const c_char,
    options: *const DarwinArtElfLoadOptions,
    out_handle: *mut *mut DarwinArtElfHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if out_handle.is_null() {
            return Err(FfiFailure::Invalid("out_handle is null"));
        }
        // SAFETY: validated non-null writable out parameter.
        unsafe { *out_handle = ptr::null_mut() };
        if path.is_null() {
            return Err(FfiFailure::Invalid("path is null"));
        }
        // SAFETY: C contract supplies a NUL-terminated path string.
        let path = unsafe { CStr::from_ptr(path) };
        let path = Path::new(std::ffi::OsStr::from_bytes(path.to_bytes()));
        let file = File::open(path)
            .map_err(|error| FfiFailure::Io(format!("open {}: {error}", path.display())))?;
        let metadata = file
            .metadata()
            .map_err(|error| FfiFailure::Io(format!("metadata {}: {error}", path.display())))?;
        if metadata.len() > MAX_INPUT_SIZE as u64 {
            return Err(FfiFailure::Invalid("ELF file exceeds 1 GiB limit"));
        }
        // Read from the already-open descriptor so a path rename cannot switch the file between
        // metadata and content. The limiter also closes the grow-after-stat allocation race.
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        file.take(MAX_INPUT_SIZE as u64 + 1)
            .read_to_end(&mut bytes)
            .map_err(|error| FfiFailure::Io(format!("read {}: {error}", path.display())))?;
        if bytes.len() > MAX_INPUT_SIZE {
            return Err(FfiFailure::Invalid("ELF file exceeds 1 GiB limit"));
        }
        let image = load_image(&bytes, options_from_pointer(options)?)?;
        let handle = Box::new(DarwinArtElfHandle {
            image: Mutex::new(image),
        });
        // SAFETY: out_handle is writable and now owns the Box until unload.
        unsafe { *out_handle = Box::into_raw(handle) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_graph_load(
    root_soname: *const c_char,
    sources: *const DarwinArtElfGraphSource,
    source_count: usize,
    provider_sonames: *const *const c_char,
    provider_count: usize,
    options: *const DarwinArtElfLoadOptions,
    out_handle: *mut *mut DarwinArtElfGraphHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if out_handle.is_null() {
            return Err(FfiFailure::Invalid("out_handle is null"));
        }
        // SAFETY: validated non-null writable out parameter.
        unsafe { *out_handle = ptr::null_mut() };
        if source_count == 0 {
            return Err(FfiFailure::Invalid("ELF graph has no sources"));
        }
        if sources.is_null() {
            return Err(FfiFailure::Invalid("ELF graph sources are null"));
        }
        if source_count > MAX_INPUT_SIZE / std::mem::size_of::<DarwinArtElfGraphSource>() {
            return Err(FfiFailure::Invalid("ELF graph source count is excessive"));
        }
        if provider_count != 0 && provider_sonames.is_null() {
            return Err(FfiFailure::Invalid("ELF graph providers are null"));
        }
        if provider_count > MAX_INPUT_SIZE / std::mem::size_of::<*const c_char>() {
            return Err(FfiFailure::Invalid("ELF graph provider count is excessive"));
        }
        // SAFETY: the C contract supplies source_count readable source records.
        let sources = unsafe { std::slice::from_raw_parts(sources, source_count) };
        // SAFETY: the C contract supplies provider_count readable string pointers.
        let providers = unsafe {
            std::slice::from_raw_parts(
                if provider_sonames.is_null() {
                    NonNull::<*const c_char>::dangling().as_ptr()
                } else {
                    provider_sonames
                },
                provider_count,
            )
        };
        // SAFETY: validated by required_utf8 for this synchronous call.
        let root = unsafe { required_utf8(root_soname, "root_soname is null")? };
        let mut namespace = ClosedElfNamespace::new();
        let mut total_size = 0_usize;
        for source in sources {
            // SAFETY: source strings and byte ranges are borrowed for this synchronous call.
            let soname = unsafe { required_utf8(source.soname, "source SONAME is null")? };
            if source.length != 0 && source.bytes.is_null() {
                return Err(FfiFailure::Invalid(
                    "source bytes are null for nonzero length",
                ));
            }
            total_size = total_size
                .checked_add(source.length)
                .ok_or(FfiFailure::Invalid("ELF graph byte size overflow"))?;
            if total_size > MAX_INPUT_SIZE {
                return Err(FfiFailure::Invalid("ELF graph exceeds 1 GiB limit"));
            }
            // SAFETY: validated null/length shape and guaranteed readable by the C contract.
            let bytes = unsafe {
                std::slice::from_raw_parts(
                    if source.bytes.is_null() {
                        NonZeroUsize::MIN.get() as *const u8
                    } else {
                        source.bytes
                    },
                    source.length,
                )
            };
            namespace
                .add_elf(soname, bytes)
                .map_err(FfiFailure::Namespace)?;
        }
        for &provider in providers {
            // SAFETY: provider strings are borrowed for this synchronous call.
            let provider = unsafe { required_utf8(provider, "provider SONAME is null")? };
            namespace
                .add_provider(provider)
                .map_err(FfiFailure::Namespace)?;
        }

        let options = options_from_pointer(options)?;
        let graph = match options.and_then(|options| {
            options
                .resolver
                .map(|callback| (callback, options.resolver_context))
        }) {
            Some((callback, context)) => {
                let mut resolver = CallbackResolver { callback, context };
                namespace
                    .load_with_resolver(&root, &mut resolver)
                    .map_err(FfiFailure::Namespace)?
            }
            None => namespace.load(&root).map_err(FfiFailure::Namespace)?,
        };
        let handle = Box::new(DarwinArtElfGraphHandle {
            graph: Mutex::new(graph),
        });
        // Publish only after recursive relocation and every constructor has succeeded.
        unsafe { *out_handle = Box::into_raw(handle) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_graph_lookup_root(
    handle: *mut DarwinArtElfGraphHandle,
    name: *const c_char,
    out_address: *mut usize,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if out_address.is_null() {
            return Err(FfiFailure::Invalid("out_address is null"));
        }
        unsafe { *out_address = 0 };
        if name.is_null() {
            return Err(FfiFailure::Invalid("name is null"));
        }
        // SAFETY: the C contract requires a live handle and NUL-terminated symbol name.
        let handle = unsafe { handle.as_ref() }.ok_or(FfiFailure::Invalid("handle is null"))?;
        let name = unsafe { CStr::from_ptr(name) }
            .to_str()
            .map_err(|_| FfiFailure::Invalid("symbol name is not UTF-8"))?;
        let address = lock_graph(handle)?
            .lookup_root_exported(name)
            .map_err(FfiFailure::Load)?;
        unsafe { *out_address = address };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_graph_unload(
    handle: *mut *mut DarwinArtElfGraphHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if handle.is_null() {
            return Err(FfiFailure::Invalid("handle pointer is null"));
        }
        let value = unsafe { *handle };
        if value.is_null() {
            return Ok(());
        }
        // Null before Drop: a contained Rust panic cannot leave a dangling published handle.
        unsafe { *handle = ptr::null_mut() };
        drop(unsafe { Box::from_raw(value) });
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_run_initializers(
    handle: *mut DarwinArtElfHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        // SAFETY: the C contract requires a live handle returned by this library.
        let handle = unsafe { handle.as_mut() }.ok_or(FfiFailure::Invalid("handle is null"))?;
        lock_image(handle)?
            .run_initializers()
            .map_err(FfiFailure::Load)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_lookup(
    handle: *mut DarwinArtElfHandle,
    name: *const c_char,
    out_address: *mut usize,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if out_address.is_null() {
            return Err(FfiFailure::Invalid("out_address is null"));
        }
        // SAFETY: validated writable out parameter.
        unsafe { *out_address = 0 };
        if name.is_null() {
            return Err(FfiFailure::Invalid("name is null"));
        }
        // SAFETY: the C contract requires a live handle and NUL-terminated name.
        let handle = unsafe { handle.as_ref() }.ok_or(FfiFailure::Invalid("handle is null"))?;
        let name = unsafe { CStr::from_ptr(name) }
            .to_str()
            .map_err(|_| FfiFailure::Invalid("symbol name is not UTF-8"))?;
        let address = lock_image(handle)?
            .lookup_exported(name)
            .map_err(FfiFailure::Load)?;
        unsafe { *out_address = address };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn darwin_art_elf_unload(
    handle: *mut *mut DarwinArtElfHandle,
    error: *mut DarwinArtElfErrorBuffer,
) -> DarwinArtElfStatus {
    ffi_call(error, || {
        if handle.is_null() {
            return Err(FfiFailure::Invalid("handle pointer is null"));
        }
        // SAFETY: validated readable/writable pointer-to-handle.
        let value = unsafe { *handle };
        if value.is_null() {
            return Ok(());
        }
        // Null first so even a contained panic during Drop cannot expose a dangling handle.
        unsafe { *handle = ptr::null_mut() };
        // SAFETY: this is the unique Box pointer returned by load and has not been unloaded.
        drop(unsafe { Box::from_raw(value) });
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_elf_status_name(status: i32) -> *const c_char {
    match status {
        0 => c"ok".as_ptr(),
        1 => c"invalid_argument".as_ptr(),
        2 => c"io".as_ptr(),
        3 => c"format".as_ptr(),
        4 => c"bounds".as_ptr(),
        5 => c"capability".as_ptr(),
        6 => c"protection".as_ptr(),
        7 => c"resolver".as_ptr(),
        8 => c"unresolved_symbol".as_ptr(),
        9 => c"symbol_not_found".as_ptr(),
        10 => c"invalid_symbol".as_ptr(),
        11 => c"lifecycle".as_ptr(),
        12 => c"system".as_ptr(),
        13 => c"poisoned".as_ptr(),
        14 => c"panic".as_ptr(),
        _ => c"unknown".as_ptr(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_call_contains_panics_and_terminates_error() {
        let mut storage = [0_u8; 64];
        let mut error = DarwinArtElfErrorBuffer {
            data: storage.as_mut_ptr().cast(),
            capacity: storage.len(),
            required: 0,
        };
        let status = ffi_call(&mut error, || -> Result<(), FfiFailure> {
            panic!("ffi panic probe")
        });
        assert_eq!(status, DarwinArtElfStatus::Panic);
        assert!(error.required > 1);
        assert_eq!(storage[storage.len() - 1], 0);
        let end = storage.iter().position(|byte| *byte == 0).unwrap();
        assert!(String::from_utf8_lossy(&storage[..end]).contains("ffi panic probe"));
    }

    #[test]
    fn error_buffer_reports_required_size_when_truncated() {
        let mut storage = [0_u8; 4];
        let mut buffer = DarwinArtElfErrorBuffer {
            data: storage.as_mut_ptr().cast(),
            capacity: storage.len(),
            required: 0,
        };
        let mut sink = unsafe { ErrorSink::new(&mut buffer) }.unwrap();
        sink.write("long message");
        assert_eq!(buffer.required, "long message".len() + 1);
        assert_eq!(&storage, b"lon\0");
    }
}
