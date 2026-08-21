//! Rust-owned provider lease accounting.
//!
//! The host crate supplies the tiny unsafe FFI adapters.  This module owns
//! the state machine around them, so cross-thread ART callbacks cannot invent
//! a second count or clear hooks while a provider lease is still active.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Condvar, Mutex};

pub struct ProviderLeaseTable {
    callbacks: ProviderCallbacks,
    state: Mutex<LeaseState>,
    quiescent: Condvar,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProviderLeaseError {
    ActiveLeases,
    Poisoned,
    CallbackPanicked,
}

struct ProviderCallbacks {
    acquire: Box<dyn Fn(u32, i32) -> i32 + Send + Sync>,
    release: Box<dyn Fn(u32) -> i32 + Send + Sync>,
    clear: Box<dyn Fn() + Send + Sync>,
}

struct LeaseState {
    counts: [u32; 7],
    in_flight: usize,
    clearing: bool,
}

impl ProviderLeaseTable {
    pub fn new<A, R, C>(acquire: A, release: R, clear: C) -> Self
    where
        A: Fn(u32, i32) -> i32 + Send + Sync + 'static,
        R: Fn(u32) -> i32 + Send + Sync + 'static,
        C: Fn() + Send + Sync + 'static,
    {
        Self {
            callbacks: ProviderCallbacks {
                acquire: Box::new(acquire),
                release: Box::new(release),
                clear: Box::new(clear),
            },
            state: Mutex::new(LeaseState {
                counts: [0; 7],
                in_flight: 0,
                clearing: false,
            }),
            quiescent: Condvar::new(),
        }
    }

    pub fn acquire(&self, kind: u32, authority_fd: i32) -> i32 {
        let Some(index) = provider_index(kind) else {
            return -1;
        };
        let Ok(mut state) = self.state.lock() else {
            return -1;
        };
        if state.clearing {
            return -1;
        }
        state.in_flight += 1;
        drop(state);

        // Never invoke foreign code while holding the lease mutex. A provider
        // callback may re-enter another provider route on the same ART thread.
        let status = catch_unwind(AssertUnwindSafe(|| {
            (self.callbacks.acquire)(kind, authority_fd)
        }))
        .unwrap_or(-1);
        let Ok(mut state) = self.state.lock() else {
            return -1;
        };
        state.in_flight = state.in_flight.saturating_sub(1);
        if status == 0 {
            state.counts[index] = state.counts[index].saturating_add(1);
        }
        self.quiescent.notify_all();
        status
    }

    pub fn release(&self, kind: u32) -> i32 {
        let Some(index) = provider_index(kind) else {
            return -1;
        };
        let Ok(mut state) = self.state.lock() else {
            return -1;
        };
        if state.clearing || state.counts[index] == 0 {
            return -1;
        }
        state.counts[index] -= 1;
        state.in_flight += 1;
        drop(state);

        // Reserve the lease before dropping the lock; this prevents clear()
        // from observing a false quiescent state while the callback runs.
        let status =
            catch_unwind(AssertUnwindSafe(|| (self.callbacks.release)(kind))).unwrap_or(-1);
        let Ok(mut state) = self.state.lock() else {
            return -1;
        };
        state.in_flight = state.in_flight.saturating_sub(1);
        if status != 0 {
            state.counts[index] = state.counts[index].saturating_add(1);
        }
        self.quiescent.notify_all();
        status
    }

    pub fn clear(&self) -> Result<(), ProviderLeaseError> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| ProviderLeaseError::Poisoned)?;
        if state.clearing {
            return Err(ProviderLeaseError::ActiveLeases);
        }
        state.clearing = true;
        while state.in_flight != 0 {
            state = self
                .quiescent
                .wait(state)
                .map_err(|_| ProviderLeaseError::Poisoned)?;
        }
        if state.counts.iter().any(|count| *count != 0) {
            state.clearing = false;
            self.quiescent.notify_all();
            return Err(ProviderLeaseError::ActiveLeases);
        }
        drop(state);

        // The owner callback is deliberately outside the mutex. Reentrant
        // clear/acquire calls fail closed through `clearing` instead of
        // deadlocking the owner thread.
        let callback_result = catch_unwind(AssertUnwindSafe(|| (self.callbacks.clear)()));
        let mut state = self
            .state
            .lock()
            .map_err(|_| ProviderLeaseError::Poisoned)?;
        state.clearing = false;
        self.quiescent.notify_all();
        callback_result
            .map(|_| ())
            .map_err(|_| ProviderLeaseError::CallbackPanicked)
    }
}

fn provider_index(kind: u32) -> Option<usize> {
    (1..=6).contains(&kind).then_some(kind as usize)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Arc,
        atomic::{AtomicU32, Ordering},
    };

    #[test]
    fn rejects_invalid_and_duplicate_release() {
        let acquired = Arc::new(AtomicU32::new(0));
        let released = Arc::new(AtomicU32::new(0));
        let acquire_count = Arc::clone(&acquired);
        let release_count = Arc::clone(&released);
        let table = ProviderLeaseTable::new(
            move |_, _| {
                acquire_count.fetch_add(1, Ordering::Relaxed);
                0
            },
            move |_| {
                release_count.fetch_add(1, Ordering::Relaxed);
                0
            },
            || {},
        );
        assert_eq!(table.acquire(0, -1), -1);
        assert_eq!(table.acquire(1, -1), 0);
        assert_eq!(table.release(1), 0);
        assert_eq!(table.release(1), -1);
        assert_eq!(acquired.load(Ordering::Relaxed), 1);
        assert_eq!(released.load(Ordering::Relaxed), 1);
        assert!(table.clear().is_ok());
    }

    #[test]
    fn clear_waits_for_all_leases() {
        let table = ProviderLeaseTable::new(|_, _| 0, |_| 0, || {});
        assert_eq!(table.acquire(2, 7), 0);
        assert_eq!(table.clear(), Err(ProviderLeaseError::ActiveLeases));
        assert_eq!(table.release(2), 0);
        assert_eq!(table.clear(), Ok(()));
    }

    #[test]
    fn foreign_acquire_callback_can_reenter_without_deadlock() {
        use std::sync::{Arc, Weak, atomic::AtomicBool};

        let entered = Arc::new(AtomicBool::new(false));
        let entered_in_callback = Arc::clone(&entered);
        let table: Arc<ProviderLeaseTable> = Arc::new_cyclic(|weak| {
            let weak: Weak<ProviderLeaseTable> = weak.clone();
            ProviderLeaseTable::new(
                move |kind, _| {
                    if kind == 2 && !entered_in_callback.swap(true, Ordering::SeqCst) {
                        weak.upgrade().expect("table alive").acquire(3, -1)
                    } else {
                        0
                    }
                },
                |_| 0,
                || {},
            )
        });
        assert_eq!(table.acquire(2, -1), 0);
        assert!(entered.load(Ordering::SeqCst));
        assert_eq!(table.release(3), 0);
        assert_eq!(table.release(2), 0);
        assert_eq!(table.clear(), Ok(()));
    }

    #[test]
    fn callback_panics_fail_closed_without_poisoning_lease_state() {
        let table = ProviderLeaseTable::new(
            |_, _| panic!("foreign acquire panic"),
            |_| panic!("foreign release panic"),
            || panic!("foreign clear panic"),
        );
        assert_eq!(table.acquire(1, -1), -1);
        assert_eq!(table.release(1), -1);
        assert_eq!(table.clear(), Err(ProviderLeaseError::CallbackPanicked));

        // The table remains usable after the callback boundary recovers.
        assert_eq!(table.acquire(1, -1), -1);
    }
}
