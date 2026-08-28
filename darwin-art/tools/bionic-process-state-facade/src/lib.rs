#![forbid(unsafe_op_in_unsafe_fn)]

use std::collections::BTreeMap;
use std::ffi::{CStr, c_char, c_int};
use std::ptr;
use std::sync::Mutex;
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
static PROCESS_OWNER: Mutex<Option<ProcessOwner>> = Mutex::new(None);

// A process always exposes a valid, NUL-terminated environment vector, even
// before the host installs its capability-filtered Android snapshot. This is
// the same empty environment shape accepted by execve and prevents native
// code from having to special-case a null `environ` pointer.
static mut EMPTY_ENVIRONMENT: [*mut c_char; 1] = [ptr::null_mut()];

#[unsafe(no_mangle)]
pub static mut darwin_art_bionic_environ: *mut *mut c_char =
    (&raw mut EMPTY_ENVIRONMENT).cast::<*mut c_char>();

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
    _environment_strings: Vec<Box<[u8]>>,
    environment_pointers: Vec<usize>,
    properties: BTreeMap<Vec<u8>, PropertyEntry>,
    auxv: BTreeMap<u64, u64>,
    _random: Box<[u8; 16]>,
}

struct ProcessOwner {
    // Activation must be dropped before its snapshot storage.
    activation: Activation,
    _snapshot: Arc<Snapshot>,
}

struct PropertyEntry {
    name: Box<[u8]>,
    value: Box<[u8]>,
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
        let environment_strings = environment
            .iter()
            .map(|(name, value)| {
                let mut entry = Vec::with_capacity(name.len() + value.len() + 1);
                entry.extend_from_slice(name);
                entry.push(b'=');
                entry.extend_from_slice(value);
                entry.into_boxed_slice()
            })
            .collect::<Vec<_>>();
        let mut environment_pointers = environment_strings
            .iter()
            .map(|entry| entry.as_ptr() as usize)
            .collect::<Vec<_>>();
        environment_pointers.push(0);
        let properties = collect_snapshot(properties, true)?
            .into_iter()
            .map(|(name, value)| {
                let mut terminated_name = name.clone();
                terminated_name.push(0);
                (
                    name,
                    PropertyEntry {
                        name: terminated_name.into_boxed_slice(),
                        value,
                    },
                )
            })
            .collect();
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
            _environment_strings: environment_strings,
            environment_pointers,
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
        // SAFETY: activation is serialized by ACTIVE's write lock. The pointer
        // array and its strings are owned by the Arc installed below and remain
        // stable until Activation clears the exported slot before dropping it.
        unsafe {
            darwin_art_bionic_environ = self.environment_pointers.as_ptr().cast_mut().cast();
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
                    // SAFETY: teardown holds the same exclusive activation lock.
                    unsafe {
                        darwin_art_bionic_environ =
                            (&raw mut EMPTY_ENVIRONMENT).cast::<*mut c_char>()
                    };
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
    unsafe {
        ptr::copy_nonoverlapping(
            source.value.as_ptr().cast::<c_char>(),
            value,
            source.value.len(),
        )
    };
    (source.value.len() - 1) as c_int
}

#[unsafe(no_mangle)]
/// # Safety
/// `name` must point to a readable NUL-terminated property name.
pub unsafe extern "C" fn darwin_art_bionic_process_property_find_core(
    name: *const c_char,
) -> *const std::ffi::c_void {
    let Some(name) = (unsafe { c_bytes(name) }) else {
        set_errno(ANDROID_EFAULT);
        return ptr::null();
    };
    let Some(snapshot) = active_snapshot() else {
        missing_snapshot();
        return ptr::null();
    };
    snapshot
        .properties
        .get(name)
        .map_or(ptr::null(), |property| {
            (property as *const PropertyEntry).cast()
        })
}

type PropertyReadCallback = unsafe extern "C" fn(
    cookie: *mut std::ffi::c_void,
    name: *const c_char,
    value: *const c_char,
    serial: u32,
);

#[unsafe(no_mangle)]
/// # Safety
/// `property` must be a token returned by the active snapshot's find call and
/// `callback`, when present, must be callable for the duration of this call.
pub unsafe extern "C" fn darwin_art_bionic_process_property_read_callback_core(
    property: *const std::ffi::c_void,
    callback: Option<PropertyReadCallback>,
    cookie: *mut std::ffi::c_void,
) {
    if property.is_null() || callback.is_none() {
        set_errno(ANDROID_EFAULT);
        return;
    }
    let Some(snapshot) = active_snapshot() else {
        missing_snapshot();
        return;
    };
    let Some(entry) = snapshot
        .properties
        .values()
        .find(|entry| std::ptr::eq(*entry as *const PropertyEntry, property.cast()))
    else {
        set_errno(ANDROID_EFAULT);
        return;
    };
    // SAFETY: entry storage belongs to the active snapshot and the callback is
    // required by the guest ABI to consume both strings synchronously.
    unsafe {
        callback.unwrap_unchecked()(
            cookie,
            entry.name.as_ptr().cast(),
            entry.value.as_ptr().cast(),
            0,
        )
    };
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

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_process_state_process_install() -> c_int {
    let mut owner = match PROCESS_OWNER.lock() {
        Ok(owner) => owner,
        Err(_) => return -1,
    };
    if owner.is_some() {
        return -1;
    }
    let mut random = [0_u8; 16];
    unsafe extern "C" {
        fn getentropy(buffer: *mut std::ffi::c_void, length: usize) -> c_int;
    }
    // SAFETY: random is writable for exactly the requested byte count.
    if unsafe { getentropy(random.as_mut_ptr().cast(), random.len()) } != 0 {
        return -1;
    }
    let snapshot = match Snapshot::new(
        vec![
            (b"ANDROID_ROOT".to_vec(), b"/system".to_vec()),
            (b"ANDROID_DATA".to_vec(), b"/data".to_vec()),
            (b"ANDROID_STORAGE".to_vec(), b"/storage".to_vec()),
            (
                b"EXTERNAL_STORAGE".to_vec(),
                b"/storage/emulated/0".to_vec(),
            ),
            (b"LANG".to_vec(), b"en-US".to_vec()),
        ],
        vec![
            (b"ro.build.version.sdk".to_vec(), b"36".to_vec()),
            (b"ro.build.version.release".to_vec(), b"16".to_vec()),
            (b"ro.product.cpu.abi".to_vec(), b"arm64-v8a".to_vec()),
            (b"ro.product.cpu.abilist".to_vec(), b"arm64-v8a".to_vec()),
        ],
        AuxSnapshot {
            page_size: 16_384,
            hwcap: SAFE_HWCAP,
            hwcap2: 0,
            secure: false,
            random,
        },
    ) {
        Ok(snapshot) => Arc::new(snapshot),
        Err(_) => return -1,
    };
    let activation = match snapshot.activate() {
        Ok(activation) => activation,
        Err(_) => return -1,
    };
    *owner = Some(ProcessOwner {
        activation,
        _snapshot: snapshot,
    });
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_process_state_process_uninstall() -> c_int {
    let mut owner = match PROCESS_OWNER.lock() {
        Ok(owner) => owner,
        Err(_) => return -1,
    };
    let Some(process) = owner.take() else {
        return -1;
    };
    drop(process.activation);
    drop(process._snapshot);
    0
}
