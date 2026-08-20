//! Rust-owned provider lease accounting.
//!
//! The host crate supplies the tiny unsafe FFI adapters.  This module owns
//! the state machine around them, so cross-thread ART callbacks cannot invent
//! a second count or clear hooks while a provider lease is still active.

use std::sync::Mutex;

pub struct ProviderLeaseTable {
    callbacks: ProviderCallbacks,
    counts: Mutex<[u32; 7]>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProviderLeaseError {
    ActiveLeases,
    Poisoned,
}

struct ProviderCallbacks {
    acquire: Box<dyn Fn(u32, i32) -> i32 + Send + Sync>,
    release: Box<dyn Fn(u32) -> i32 + Send + Sync>,
    clear: Box<dyn Fn()>,
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
            counts: Mutex::new([0; 7]),
        }
    }

    pub fn acquire(&self, kind: u32, authority_fd: i32) -> i32 {
        let Some(index) = provider_index(kind) else {
            return -1;
        };
        let status = (self.callbacks.acquire)(kind, authority_fd);
        if status != 0 {
            return status;
        }
        let Ok(mut counts) = self.counts.lock() else {
            // The foreign owner has acquired the provider already. Do not
            // claim a Rust lease that cannot be released safely.
            return -1;
        };
        counts[index] = counts[index].saturating_add(1);
        0
    }

    pub fn release(&self, kind: u32) -> i32 {
        let Some(index) = provider_index(kind) else {
            return -1;
        };
        let Ok(mut counts) = self.counts.lock() else {
            return -1;
        };
        if counts[index] == 0 {
            return -1;
        }
        let status = (self.callbacks.release)(kind);
        if status == 0 {
            counts[index] -= 1;
        }
        status
    }

    pub fn clear(&self) -> Result<(), ProviderLeaseError> {
        let counts = self
            .counts
            .lock()
            .map_err(|_| ProviderLeaseError::Poisoned)?;
        if counts.iter().any(|count| *count != 0) {
            return Err(ProviderLeaseError::ActiveLeases);
        }
        (self.callbacks.clear)();
        Ok(())
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
}
