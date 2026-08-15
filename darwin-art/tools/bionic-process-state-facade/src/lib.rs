#![forbid(unsafe_op_in_unsafe_fn)]

use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock, RwLock};

const PROP_VALUE_MAX: usize = 92;
const ANDROID_ENOENT: i32 = 2;
const ANDROID_EIO: i32 = 5;
const ANDROID_EFAULT: i32 = 14;
const AT_PAGESZ: u64 = 6;
const AT_HWCAP: u64 = 16;
const AT_SECURE: u64 = 23;
const AT_RANDOM: u64 = 25;
const AT_HWCAP2: u64 = 26;
const SAFE_HWCAP: u64 = 3; // AArch64 FP | ASIMD baseline only.

unsafe extern "C" {
    fn darwin_art_bionic_errno_store(android_errno: i32);
}

static ACTIVE: OnceLock<RwLock<Option<Arc<Snapshot>>>> = OnceLock::new();
static CAPABILITY_FAILURE: AtomicBool = AtomicBool::new(false);
static DROP_COUNT: AtomicUsize = AtomicUsize::new(0);

fn active_slot() -> &'static RwLock<Option<Arc<Snapshot>>> {
    ACTIVE.get_or_init(|| RwLock::new(None))
}

#[derive(Clone, Copy, Debug)]
pub struct AuxSnapshot {
    pub page_size: u64,
    pub hwcap: u64,
    pub hwcap2: u64,
    pub secure: bool,
    pub random: [u8; 16],
}

pub struct Snapshot {
    environment: BTreeMap<Vec<u8>, Box<[u8]>>,
    properties: BTreeMap<Vec<u8>, Box<[u8]>>,
    auxv: BTreeMap<u64, u64>,
    _random: Box<[u8; 16]>,
}

impl Snapshot {
    pub fn new(
        environment: Vec<(Vec<u8>, Vec<u8>)>,
        properties: Vec<(Vec<u8>, Vec<u8>)>,
        aux: AuxSnapshot,
    ) -> Result<Self, &'static str> {
        if aux.page_size < 4096 || !aux.page_size.is_power_of_two() {
            return Err("invalid Android page size");
        }
        if aux.hwcap & !SAFE_HWCAP != 0 || aux.hwcap2 != 0 {
            return Err("unsupported Android arm64 hardware capability claim");
        }
        let environment = collect_snapshot(environment, false)?;
        let properties = collect_snapshot(properties, true)?;
        let random = Box::new(aux.random);
        let random_address = (&*random as *const [u8; 16]) as usize as u64;
        let auxv = BTreeMap::from([
            (AT_PAGESZ, aux.page_size),
            (AT_HWCAP, aux.hwcap),
            (AT_SECURE, u64::from(aux.secure)),
            (AT_RANDOM, random_address),
            (AT_HWCAP2, aux.hwcap2),
        ]);
        Ok(Self {
            environment,
            properties,
            auxv,
            _random: random,
        })
    }

    pub fn activate(self: &Arc<Self>) -> Result<Activation, &'static str> {
        let mut active = active_slot()
            .write()
            .map_err(|_| "activation lock poisoned")?;
        if active.is_some() {
            return Err("another process snapshot is active");
        }
        *active = Some(Arc::clone(self));
        Ok(Activation { active: true })
    }
}

impl Drop for Snapshot {
    fn drop(&mut self) {
        DROP_COUNT.fetch_add(1, Ordering::AcqRel);
    }
}

pub struct Activation {
    active: bool,
}

impl Drop for Activation {
    fn drop(&mut self) {
        if self.active {
            match active_slot().write() {
                Ok(mut active) => {
                    active.take();
                }
                Err(_) => CAPABILITY_FAILURE.store(true, Ordering::Release),
            }
            self.active = false;
        }
    }
}

fn collect_snapshot(
    entries: Vec<(Vec<u8>, Vec<u8>)>,
    property: bool,
) -> Result<BTreeMap<Vec<u8>, Box<[u8]>>, &'static str> {
    let mut result = BTreeMap::new();
    for (name, mut value) in entries {
        if name.is_empty() || name.contains(&0) || (!property && name.contains(&b'=')) {
            return Err("invalid snapshot name");
        }
        if value.contains(&0) || (property && value.len() >= PROP_VALUE_MAX) {
            return Err("invalid snapshot value");
        }
        value.push(0);
        if result.insert(name, value.into_boxed_slice()).is_some() {
            return Err("duplicate snapshot name");
        }
    }
    Ok(result)
}

fn set_errno(value: i32) {
    // SAFETY: linked Bionic errno storage is pthread-local and value-only.
    unsafe { darwin_art_bionic_errno_store(value) };
}

fn active_snapshot() -> Option<Arc<Snapshot>> {
    match active_slot().read() {
        Ok(active) => active.clone(),
        Err(_) => {
            CAPABILITY_FAILURE.store(true, Ordering::Release);
            set_errno(ANDROID_EIO);
            None
        }
    }
}

fn missing_snapshot() {
    CAPABILITY_FAILURE.store(true, Ordering::Release);
    set_errno(ANDROID_EIO);
}

unsafe fn c_bytes<'a>(value: *const c_char) -> Option<&'a [u8]> {
    if value.is_null() {
        return None;
    }
    // SAFETY: guest ABI requires a readable NUL-terminated string.
    Some(unsafe { CStr::from_ptr(value) }.to_bytes())
}

#[unsafe(no_mangle)]
/// # Safety
/// `name` must point to a readable NUL-terminated environment name.
pub unsafe extern "C" fn darwin_art_bionic_process_getenv_core(name: *const c_char) -> *mut c_char {
    let Some(name) = (unsafe { c_bytes(name) }) else {
        set_errno(ANDROID_EFAULT);
        return ptr::null_mut();
    };
    let Some(snapshot) = active_snapshot() else {
        missing_snapshot();
        return ptr::null_mut();
    };
    snapshot
        .environment
        .get(name)
        .map_or(ptr::null_mut(), |value| {
            value.as_ptr().cast::<c_char>().cast_mut()
        })
}

#[unsafe(no_mangle)]
/// # Safety
/// `name` must be readable and NUL-terminated. `value` must be writable for
/// Android `PROP_VALUE_MAX` (92) bytes.
pub unsafe extern "C" fn darwin_art_bionic_process_property_get_core(
    name: *const c_char,
    value: *mut c_char,
) -> c_int {
    if value.is_null() {
        set_errno(ANDROID_EFAULT);
        return 0;
    }
    let Some(name) = (unsafe { c_bytes(name) }) else {
        set_errno(ANDROID_EFAULT);
        // SAFETY: non-null value is writable by ABI contract.
        unsafe { value.write(0) };
        return 0;
    };
    let Some(snapshot) = active_snapshot() else {
        missing_snapshot();
        // SAFETY: non-null value is writable by ABI contract.
        unsafe { value.write(0) };
        return 0;
    };
    let Some(source) = snapshot.properties.get(name) else {
        // SAFETY: property ABI requires at least PROP_VALUE_MAX writable bytes.
        unsafe { value.write(0) };
        return 0;
    };
    // SAFETY: construction bounds source to at most PROP_VALUE_MAX bytes including NUL.
    unsafe { ptr::copy_nonoverlapping(source.as_ptr().cast::<c_char>(), value, source.len()) };
    (source.len() - 1) as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_process_getauxval_core(kind: u64) -> u64 {
    let Some(snapshot) = active_snapshot() else {
        missing_snapshot();
        return 0;
    };
    match snapshot.auxv.get(&kind) {
        Some(value) => *value,
        None => {
            set_errno(ANDROID_ENOENT);
            0
        }
    }
}

pub fn capability_failed() -> bool {
    CAPABILITY_FAILURE.load(Ordering::Acquire)
}

pub fn drop_count() -> usize {
    DROP_COUNT.load(Ordering::Acquire)
}
