#![forbid(unsafe_op_in_unsafe_fn)]

use std::collections::BTreeMap;
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock, RwLock};

const ANDROID_EIO: i32 = 5;
const ANDROID_ENOMEM: i32 = 12;
const ANDROID_EACCES: i32 = 13;
const ANDROID_EINVAL: i32 = 22;
const ANDROID_EOVERFLOW: i32 = 75;
const ANDROID_EOPNOTSUPP: i32 = 95;

const ANDROID_PROT_READ: i32 = 0x1;
const ANDROID_PROT_WRITE: i32 = 0x2;
const ANDROID_PROT_EXEC: i32 = 0x4;
const ANDROID_PROT_MASK: i32 = 0x7;
const ANDROID_MAP_PRIVATE: i32 = 0x02;
const ANDROID_MAP_ANONYMOUS: i32 = 0x20;

const MAX_MAPPINGS: usize = 1024;
const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;

unsafe extern "C" {
    fn darwin_art_host_vm_page_size() -> usize;
    fn darwin_art_host_vm_map(length: usize, protection: c_int, error: *mut c_int) -> *mut c_void;
    fn darwin_art_host_vm_unmap(address: *mut c_void, length: usize, error: *mut c_int) -> c_int;
    fn darwin_art_host_vm_protect(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        error: *mut c_int,
    ) -> c_int;
    fn darwin_art_host_vm_advise(
        address: *mut c_void,
        length: usize,
        advice: c_int,
        error: *mut c_int,
    ) -> c_int;
    fn darwin_art_host_vm_invalidate_icache(address: *mut c_void, length: usize);
    fn darwin_art_bionic_errno_set_from_darwin(error: c_int) -> c_int;
    fn darwin_art_bionic_errno_store(error: i32);
}

#[derive(Clone, Copy)]
struct Mapping {
    requested_length: usize,
    mapped_length: usize,
    protection: i32,
}

pub struct Provider {
    page_size: usize,
    mappings: Mutex<BTreeMap<usize, Mapping>>,
    capability_failure: AtomicBool,
}

static ACTIVE: OnceLock<RwLock<Option<Arc<Provider>>>> = OnceLock::new();

fn active_slot() -> &'static RwLock<Option<Arc<Provider>>> {
    ACTIVE.get_or_init(|| RwLock::new(None))
}

impl Provider {
    pub fn new() -> Result<Self, &'static str> {
        // SAFETY: the host helper has no pointer preconditions.
        let page_size = unsafe { darwin_art_host_vm_page_size() };
        if page_size == 0 || !page_size.is_power_of_two() {
            return Err("invalid Darwin page size");
        }
        Ok(Self {
            page_size,
            mappings: Mutex::new(BTreeMap::new()),
            capability_failure: AtomicBool::new(false),
        })
    }

    pub fn activate(self: &Arc<Self>) -> Result<Activation, &'static str> {
        let mut active = active_slot()
            .write()
            .map_err(|_| "VM activation lock poisoned")?;
        if active.is_some() {
            return Err("VM provider already active");
        }
        *active = Some(Arc::clone(self));
        Ok(Activation)
    }

    pub fn page_size(&self) -> usize {
        self.page_size
    }

    pub fn mapping_count(&self) -> usize {
        self.mappings.lock().map(|m| m.len()).unwrap_or(usize::MAX)
    }

    pub fn protection(&self, address: usize) -> Option<i32> {
        self.mappings
            .lock()
            .ok()?
            .get(&address)
            .map(|mapping| mapping.protection)
    }

    pub fn capability_failed(&self) -> bool {
        self.capability_failure.load(Ordering::Acquire)
    }
}

impl Drop for Provider {
    fn drop(&mut self) {
        let mappings = match self.mappings.get_mut() {
            Ok(value) => value,
            Err(poisoned) => poisoned.into_inner(),
        };
        for (address, mapping) in std::mem::take(mappings) {
            let mut host_error = 0;
            // SAFETY: every entry is a live complete host mapping owned by this provider.
            unsafe {
                darwin_art_host_vm_unmap(
                    address as *mut c_void,
                    mapping.mapped_length,
                    &mut host_error,
                )
            };
        }
    }
}

pub struct Activation;

impl Drop for Activation {
    fn drop(&mut self) {
        if let Ok(mut active) = active_slot().write() {
            active.take();
        }
    }
}

fn set_errno(error: i32) {
    // SAFETY: the errno provider accepts every 32-bit Android errno value.
    unsafe { darwin_art_bionic_errno_store(error) }
}

fn provider() -> Option<Arc<Provider>> {
    match active_slot().read() {
        Ok(active) => match active.clone() {
            Some(provider) => Some(provider),
            None => {
                set_errno(ANDROID_EIO);
                None
            }
        },
        Err(_) => {
            set_errno(ANDROID_EIO);
            None
        }
    }
}

fn fail_host(provider: &Provider, host_error: i32) {
    // SAFETY: this does not expose host errno storage and preserves host errno.
    if host_error == 0 || unsafe { darwin_art_bionic_errno_set_from_darwin(host_error) } == 0 {
        provider.capability_failure.store(true, Ordering::Release);
        set_errno(ANDROID_EIO);
    }
}

fn host_protection(android: i32) -> Option<i32> {
    if android & !ANDROID_PROT_MASK != 0 {
        return None;
    }
    let mut host = 0;
    if android & ANDROID_PROT_READ != 0 {
        host |= 1;
    }
    if android & ANDROID_PROT_WRITE != 0 {
        host |= 2;
    }
    if android & ANDROID_PROT_EXEC != 0 {
        host |= 4;
    }
    Some(host)
}

fn whole_mapping(mapping: Mapping, length: usize) -> bool {
    length == mapping.requested_length || length == mapping.mapped_length
}

fn round_length(length: usize, page_size: usize) -> Option<usize> {
    length
        .checked_add(page_size - 1)
        .map(|value| value & !(page_size - 1))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_vm_mmap_core(
    address: *mut c_void,
    length: usize,
    protection: c_int,
    flags: c_int,
    fd: c_int,
    offset: i64,
) -> *mut c_void {
    let Some(provider) = provider() else {
        return MAP_FAILED;
    };
    if length == 0 {
        set_errno(ANDROID_EINVAL);
        return MAP_FAILED;
    }
    if length > isize::MAX as usize {
        set_errno(ANDROID_EOVERFLOW);
        return MAP_FAILED;
    }
    if offset < 0 || offset as usize % provider.page_size != 0 {
        set_errno(ANDROID_EINVAL);
        return MAP_FAILED;
    }
    if !address.is_null() || flags != ANDROID_MAP_PRIVATE | ANDROID_MAP_ANONYMOUS || offset != 0 {
        set_errno(ANDROID_EOPNOTSUPP);
        return MAP_FAILED;
    }
    let _ignored_anonymous_fd = fd;
    let Some(host_protection) = host_protection(protection) else {
        set_errno(ANDROID_EINVAL);
        return MAP_FAILED;
    };
    if protection & ANDROID_PROT_WRITE != 0 && protection & ANDROID_PROT_EXEC != 0 {
        set_errno(ANDROID_EACCES);
        return MAP_FAILED;
    }
    let Some(mapped_length) = round_length(length, provider.page_size) else {
        set_errno(ANDROID_EOVERFLOW);
        return MAP_FAILED;
    };
    let mut mappings = match provider.mappings.lock() {
        Ok(value) => value,
        Err(_) => {
            provider.capability_failure.store(true, Ordering::Release);
            set_errno(ANDROID_EIO);
            return MAP_FAILED;
        }
    };
    if mappings.len() >= MAX_MAPPINGS {
        set_errno(ANDROID_ENOMEM);
        return MAP_FAILED;
    }
    let mut host_error = 0;
    // SAFETY: mapped_length is nonzero and rounded; the host helper fixes anonymous/private flags.
    let result = unsafe { darwin_art_host_vm_map(mapped_length, host_protection, &mut host_error) };
    if result == MAP_FAILED {
        fail_host(&provider, host_error);
        return MAP_FAILED;
    }
    let old = mappings.insert(
        result as usize,
        Mapping {
            requested_length: length,
            mapped_length,
            protection,
        },
    );
    if old.is_some() {
        let mut ignored = 0;
        // SAFETY: result is the just-created complete mapping.
        unsafe { darwin_art_host_vm_unmap(result, mapped_length, &mut ignored) };
        provider.capability_failure.store(true, Ordering::Release);
        set_errno(ANDROID_EIO);
        return MAP_FAILED;
    }
    result
}

#[unsafe(no_mangle)]
/// # Safety
/// `address` is an opaque guest value and is passed to Darwin only after the
/// side table proves ownership of the complete mapping.
pub unsafe extern "C" fn darwin_art_bionic_vm_munmap_core(
    address: *mut c_void,
    length: usize,
) -> c_int {
    let Some(provider) = provider() else {
        return -1;
    };
    if address.is_null() || length == 0 || address as usize % provider.page_size != 0 {
        set_errno(ANDROID_EINVAL);
        return -1;
    }
    let mut mappings = match provider.mappings.lock() {
        Ok(value) => value,
        Err(_) => {
            provider.capability_failure.store(true, Ordering::Release);
            set_errno(ANDROID_EIO);
            return -1;
        }
    };
    let Some(mapping) = mappings.get(&(address as usize)).copied() else {
        set_errno(ANDROID_EINVAL);
        return -1;
    };
    if !whole_mapping(mapping, length) {
        set_errno(ANDROID_EOPNOTSUPP);
        return -1;
    }
    let mut host_error = 0;
    // SAFETY: the side table proves this is the complete live owned mapping.
    if unsafe { darwin_art_host_vm_unmap(address, mapping.mapped_length, &mut host_error) } != 0 {
        fail_host(&provider, host_error);
        return -1;
    }
    mappings.remove(&(address as usize));
    0
}

#[unsafe(no_mangle)]
/// # Safety
/// `address` is an opaque guest value and is passed to Darwin only after the
/// side table proves ownership of the complete mapping.
pub unsafe extern "C" fn darwin_art_bionic_vm_mprotect_core(
    address: *mut c_void,
    length: usize,
    protection: c_int,
) -> c_int {
    let Some(provider) = provider() else {
        return -1;
    };
    if address.is_null() || length == 0 || address as usize % provider.page_size != 0 {
        set_errno(ANDROID_EINVAL);
        return -1;
    }
    let Some(host_protection) = host_protection(protection) else {
        set_errno(ANDROID_EINVAL);
        return -1;
    };
    if protection & ANDROID_PROT_WRITE != 0 && protection & ANDROID_PROT_EXEC != 0 {
        set_errno(ANDROID_EACCES);
        return -1;
    }
    let mut mappings = match provider.mappings.lock() {
        Ok(value) => value,
        Err(_) => {
            provider.capability_failure.store(true, Ordering::Release);
            set_errno(ANDROID_EIO);
            return -1;
        }
    };
    let Some(mapping) = mappings.get_mut(&(address as usize)) else {
        set_errno(ANDROID_ENOMEM);
        return -1;
    };
    if !whole_mapping(*mapping, length) {
        set_errno(ANDROID_EOPNOTSUPP);
        return -1;
    }
    let mut host_error = 0;
    // SAFETY: the side table proves this is the complete live owned mapping.
    if unsafe {
        darwin_art_host_vm_protect(
            address,
            mapping.mapped_length,
            host_protection,
            &mut host_error,
        )
    } != 0
    {
        fail_host(&provider, host_error);
        return -1;
    }
    mapping.protection = protection;
    if protection & ANDROID_PROT_EXEC != 0 {
        // SAFETY: the mapping remains live under the table lock for this complete range.
        unsafe { darwin_art_host_vm_invalidate_icache(address, mapping.mapped_length) };
    }
    0
}

#[unsafe(no_mangle)]
/// # Safety
/// `address` is an opaque guest value and is passed to Darwin only after the
/// side table proves ownership of the complete mapping.
pub unsafe extern "C" fn darwin_art_bionic_vm_madvise_core(
    address: *mut c_void,
    length: usize,
    advice: c_int,
) -> c_int {
    let Some(provider) = provider() else {
        return -1;
    };
    if address.is_null() || length == 0 || address as usize % provider.page_size != 0 {
        set_errno(ANDROID_EINVAL);
        return -1;
    }
    let host_advice = match advice {
        0..=3 => advice,
        4 => 11,
        8 => 5,
        9..=25 | 100 | 101 => {
            set_errno(ANDROID_EOPNOTSUPP);
            return -1;
        }
        _ => {
            set_errno(ANDROID_EINVAL);
            return -1;
        }
    };
    let mappings = match provider.mappings.lock() {
        Ok(value) => value,
        Err(_) => {
            provider.capability_failure.store(true, Ordering::Release);
            set_errno(ANDROID_EIO);
            return -1;
        }
    };
    let Some(mapping) = mappings.get(&(address as usize)).copied() else {
        set_errno(ANDROID_ENOMEM);
        return -1;
    };
    if !whole_mapping(mapping, length) {
        set_errno(ANDROID_EOPNOTSUPP);
        return -1;
    }
    let mut host_error = 0;
    // SAFETY: the side table proves this is the complete live owned mapping.
    if unsafe {
        darwin_art_host_vm_advise(address, mapping.mapped_length, host_advice, &mut host_error)
    } != 0
    {
        fail_host(&provider, host_error);
        return -1;
    }
    0
}
