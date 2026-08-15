#![forbid(unsafe_op_in_unsafe_fn)]

use std::collections::{HashMap, HashSet};
use std::ffi::{c_int, c_void};
use std::sync::{Arc, Mutex, OnceLock, RwLock};

pub type Destructor = unsafe extern "C" fn(*mut c_void);
pub type UnregisterAtforkHook = unsafe extern "C" fn(*mut c_void);
pub type StdioCleanupHook = unsafe extern "C" fn();

#[derive(Clone, Copy, Default)]
pub struct Hooks {
    pub unregister_atfork: Option<UnregisterAtforkHook>,
    pub stdio_cleanup: Option<StdioCleanupHook>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Registration {
    pub function: usize,
    pub argument: usize,
    pub dso: usize,
}

#[derive(Clone, Copy)]
struct Entry {
    function: Destructor,
    argument: usize,
    dso: usize,
}

#[derive(Default)]
struct State {
    entries: Vec<Option<Entry>>,
    published: HashSet<usize>,
    active_callbacks: HashMap<usize, usize>,
    finalize_depth: u32,
}

pub struct Lifecycle {
    state: Mutex<State>,
    hooks: Hooks,
}

static ACTIVE: OnceLock<RwLock<Option<Arc<Lifecycle>>>> = OnceLock::new();

fn active_slot() -> &'static RwLock<Option<Arc<Lifecycle>>> {
    ACTIVE.get_or_init(|| RwLock::new(None))
}

impl Lifecycle {
    pub fn new(hooks: Hooks) -> Self {
        Self {
            state: Mutex::new(State::default()),
            hooks,
        }
    }

    pub fn activate(self: &Arc<Self>) -> Result<Activation, &'static str> {
        let mut active = active_slot()
            .write()
            .map_err(|_| "activation lock poisoned")?;
        if active.is_some() {
            return Err("another DSO lifecycle provider is active");
        }
        *active = Some(Arc::clone(self));
        Ok(Activation {
            lifecycle: Arc::clone(self),
            active: true,
        })
    }

    pub fn publish(&self, dso: usize) -> Result<(), &'static str> {
        if dso == 0 {
            return Err("null is reserved for process-global registrations");
        }
        let mut state = self.state.lock().map_err(|_| "state lock poisoned")?;
        if state.published.contains(&dso) {
            return Err("DSO handle is already published");
        }
        state
            .published
            .try_reserve(1)
            .map_err(|_| "could not reserve published DSO state")?;
        state.published.insert(dso);
        Ok(())
    }

    pub fn try_unpublish(&self, dso: usize) -> Result<(), &'static str> {
        let mut state = self.state.lock().map_err(|_| "state lock poisoned")?;
        if !state.published.contains(&dso) {
            return Err("DSO handle is not published");
        }
        if state.entries.iter().flatten().any(|entry| entry.dso == dso)
            || state.active_callbacks.get(&dso).copied().unwrap_or(0) != 0
        {
            return Err("DSO still has registered or running destructors");
        }
        state.published.remove(&dso);
        state.active_callbacks.remove(&dso);
        Ok(())
    }

    pub fn registrations(&self) -> Result<Vec<Registration>, &'static str> {
        let state = self.state.lock().map_err(|_| "state lock poisoned")?;
        Ok(state
            .entries
            .iter()
            .flatten()
            .map(|entry| Registration {
                function: entry.function as usize,
                argument: entry.argument,
                dso: entry.dso,
            })
            .collect())
    }

    pub fn registration_count(&self, dso: usize) -> Result<usize, &'static str> {
        let state = self.state.lock().map_err(|_| "state lock poisoned")?;
        Ok(state
            .entries
            .iter()
            .flatten()
            .filter(|entry| dso == 0 || entry.dso == dso)
            .count())
    }

    pub fn active_callback_count(&self, dso: usize) -> Result<usize, &'static str> {
        let state = self.state.lock().map_err(|_| "state lock poisoned")?;
        if dso == 0 {
            return Ok(state.active_callbacks.values().sum());
        }
        Ok(state.active_callbacks.get(&dso).copied().unwrap_or(0))
    }

    fn register(
        &self,
        function: Option<Destructor>,
        argument: *mut c_void,
        dso: *mut c_void,
    ) -> c_int {
        let Some(function) = function else {
            return -1;
        };
        let dso = dso as usize;
        let Ok(mut state) = self.state.lock() else {
            return -1;
        };
        if dso != 0 && !state.published.contains(&dso) {
            return -1;
        }
        if state.entries.try_reserve(1).is_err() {
            return -1;
        }
        state.entries.push(Some(Entry {
            function,
            argument: argument as usize,
            dso,
        }));
        0
    }

    fn finalize(&self, dso: *mut c_void) {
        let dso = dso as usize;
        let Ok(mut state) = self.state.lock() else {
            return;
        };
        state.finalize_depth = state.finalize_depth.saturating_add(1);

        loop {
            let index = state
                .entries
                .iter()
                .rposition(|slot| slot.is_some_and(|entry| dso == 0 || entry.dso == dso));
            let Some(index) = index else {
                state.finalize_depth = state.finalize_depth.saturating_sub(1);
                if state.finalize_depth == 0 && dso != 0 {
                    state.entries.retain(Option::is_some);
                }
                break;
            };
            let entry = state.entries[index]
                .take()
                .expect("selected lifecycle entry must be present");
            *state.active_callbacks.entry(entry.dso).or_insert(0) += 1;
            drop(state);

            // SAFETY: the loader published the DSO while its executable mapping was alive.
            // The one-pointer callback uses x0 in both Android and Darwin arm64 PCS.
            unsafe { (entry.function)(entry.argument as *mut c_void) };

            let Ok(locked) = self.state.lock() else {
                return;
            };
            state = locked;
            if let Some(active) = state.active_callbacks.get_mut(&entry.dso) {
                *active -= 1;
                if *active == 0 {
                    state.active_callbacks.remove(&entry.dso);
                }
            }
        }

        drop(state);

        if dso == 0 {
            if let Some(cleanup) = self.hooks.stdio_cleanup {
                // SAFETY: the hook has fixed no-argument PCS and remains live with the provider.
                unsafe { cleanup() };
            }
        } else if let Some(unregister) = self.hooks.unregister_atfork {
            // SAFETY: the hook has fixed one-pointer PCS and remains live with the provider.
            unsafe { unregister(dso as *mut c_void) };
        }
    }
}

pub struct Activation {
    lifecycle: Arc<Lifecycle>,
    active: bool,
}

impl Drop for Activation {
    fn drop(&mut self) {
        if !self.active {
            return;
        }
        if let Ok(mut active) = active_slot().write()
            && active
                .as_ref()
                .is_some_and(|current| Arc::ptr_eq(current, &self.lifecycle))
        {
            active.take();
        }
        self.active = false;
    }
}

fn active_lifecycle() -> Option<Arc<Lifecycle>> {
    active_slot().read().ok()?.clone()
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_dso_cxa_atexit_core(
    function: Option<Destructor>,
    argument: *mut c_void,
    dso: *mut c_void,
) -> c_int {
    active_lifecycle().map_or(-1, |lifecycle| lifecycle.register(function, argument, dso))
}

#[unsafe(no_mangle)]
pub extern "C" fn darwin_art_bionic_dso_cxa_finalize_core(dso: *mut c_void) {
    if let Some(lifecycle) = active_lifecycle() {
        lifecycle.finalize(dso);
    }
}
