#![forbid(unsafe_code)]

//! Rust-owned runtime lifecycle state.
//!
//! The native engine remains behind `darwin-art-engine-sys`; this crate owns
//! the ordering and rollback state so C++ callbacks cannot invent a second,
//! conflicting lifecycle machine.

use std::{collections::BTreeMap, marker::PhantomData, rc::Rc, thread::ThreadId};

use darwin_art_abi::StatusCode;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimePhase {
    New,
    Bootstrapping,
    Running,
    ShuttingDown,
    Stopped,
    Failed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimeError {
    WrongOwnerThread,
    InvalidTransition {
        from: RuntimePhase,
        to: RuntimePhase,
    },
    AlreadyFailed,
    EngineFailure {
        status: i32,
    },
    SubsystemNotActive {
        subsystem: Subsystem,
    },
    InvalidShutdownOrder {
        expected: Subsystem,
        requested: Subsystem,
    },
}

impl RuntimeError {
    pub const fn status(self) -> StatusCode {
        match self {
            Self::WrongOwnerThread => StatusCode::InvalidState,
            Self::InvalidTransition { .. } => StatusCode::InvalidState,
            Self::AlreadyFailed => StatusCode::Internal,
            Self::EngineFailure { .. } => StatusCode::Internal,
            Self::SubsystemNotActive { .. } => StatusCode::InvalidState,
            Self::InvalidShutdownOrder { .. } => StatusCode::InvalidState,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum Subsystem {
    Engine,
    ElfNamespace,
    Filesystem,
    Network,
    Surface,
    Input,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SubsystemLease {
    subsystem: Subsystem,
    generation: u64,
}

impl SubsystemLease {
    pub const fn subsystem(self) -> Subsystem {
        self.subsystem
    }
}

/// One-shot owner-thread runtime state. `Rc` makes accidental Send/Sync
/// implementation impossible; ART/HWUI JNI objects remain thread-affine.
pub struct RuntimeSession {
    owner: ThreadId,
    phase: RuntimePhase,
    failure: Option<RuntimeError>,
    subsystems: BTreeMap<Subsystem, u64>,
    cleanups: BTreeMap<Subsystem, Box<dyn FnOnce() -> Result<(), RuntimeError>>>,
    install_order: Vec<Subsystem>,
    next_generation: u64,
    _owner_thread: PhantomData<Rc<()>>,
}

impl RuntimeSession {
    pub fn new() -> Self {
        Self {
            owner: std::thread::current().id(),
            phase: RuntimePhase::New,
            failure: None,
            subsystems: BTreeMap::new(),
            cleanups: BTreeMap::new(),
            install_order: Vec::new(),
            next_generation: 1,
            _owner_thread: PhantomData,
        }
    }

    pub fn phase(&self) -> RuntimePhase {
        self.phase
    }

    pub fn failure(&self) -> Option<RuntimeError> {
        self.failure
    }

    pub fn start(&mut self) -> Result<(), RuntimeError> {
        self.transition(RuntimePhase::Bootstrapping)
    }

    pub fn mark_running(&mut self) -> Result<(), RuntimeError> {
        self.transition(RuntimePhase::Running)
    }

    pub fn begin_shutdown(&mut self) -> Result<(), RuntimeError> {
        self.transition(RuntimePhase::ShuttingDown)
    }

    pub fn finish_shutdown(&mut self) -> Result<(), RuntimeError> {
        self.transition(RuntimePhase::Stopped)
    }

    pub fn fail(&mut self, error: RuntimeError) {
        self.phase = RuntimePhase::Failed;
        self.failure = Some(error);
    }

    pub fn install_subsystem(
        &mut self,
        subsystem: Subsystem,
    ) -> Result<SubsystemLease, RuntimeError> {
        self.install_subsystem_with_cleanup(subsystem, || Ok(()))
    }

    /// Installs a subsystem together with its one-shot native cleanup.
    ///
    /// The callback is owned by the session and is invoked exactly once when
    /// the matching lease is uninstalled or when the session is dropped during
    /// rollback. This keeps native lifetime ownership in Rust without making
    /// the runtime crate depend on a particular C++ object type.
    pub fn install_subsystem_with_cleanup<F>(
        &mut self,
        subsystem: Subsystem,
        cleanup: F,
    ) -> Result<SubsystemLease, RuntimeError>
    where
        F: FnOnce() -> Result<(), RuntimeError> + 'static,
    {
        self.assert_owner()?;
        if !matches!(
            self.phase,
            RuntimePhase::Bootstrapping | RuntimePhase::Running
        ) {
            return Err(RuntimeError::InvalidTransition {
                from: self.phase,
                to: self.phase,
            });
        }
        if self.subsystems.contains_key(&subsystem) {
            return Err(RuntimeError::SubsystemNotActive { subsystem });
        }
        let generation = self.next_generation;
        self.next_generation = self.next_generation.saturating_add(1);
        self.subsystems.insert(subsystem, generation);
        self.cleanups.insert(subsystem, Box::new(cleanup));
        self.install_order.push(subsystem);
        Ok(SubsystemLease {
            subsystem,
            generation,
        })
    }

    pub fn uninstall_subsystem(&mut self, lease: SubsystemLease) -> Result<(), RuntimeError> {
        self.assert_owner()?;
        let Some(expected) = self.install_order.last().copied() else {
            return Err(RuntimeError::SubsystemNotActive {
                subsystem: lease.subsystem,
            });
        };
        if expected != lease.subsystem {
            return Err(RuntimeError::InvalidShutdownOrder {
                expected,
                requested: lease.subsystem,
            });
        }
        if self.subsystems.get(&lease.subsystem).copied() != Some(lease.generation) {
            return Err(RuntimeError::SubsystemNotActive {
                subsystem: lease.subsystem,
            });
        }
        self.install_order.pop();
        self.subsystems.remove(&lease.subsystem);
        let cleanup = self
            .cleanups
            .remove(&lease.subsystem)
            .expect("subsystem cleanup must accompany its lease");
        cleanup()
    }

    pub fn assert_owner(&self) -> Result<(), RuntimeError> {
        if std::thread::current().id() == self.owner {
            Ok(())
        } else {
            Err(RuntimeError::WrongOwnerThread)
        }
    }

    fn transition(&mut self, to: RuntimePhase) -> Result<(), RuntimeError> {
        self.assert_owner()?;
        if self.phase == RuntimePhase::Failed {
            return Err(RuntimeError::AlreadyFailed);
        }
        let valid = matches!(
            (self.phase, to),
            (RuntimePhase::New, RuntimePhase::Bootstrapping)
                | (RuntimePhase::Bootstrapping, RuntimePhase::Running)
                | (RuntimePhase::Bootstrapping, RuntimePhase::ShuttingDown)
                | (RuntimePhase::Running, RuntimePhase::ShuttingDown)
                | (RuntimePhase::ShuttingDown, RuntimePhase::Stopped)
        );
        if !valid {
            return Err(RuntimeError::InvalidTransition {
                from: self.phase,
                to,
            });
        }
        if to == RuntimePhase::Stopped && !self.install_order.is_empty() {
            return Err(RuntimeError::InvalidTransition {
                from: self.phase,
                to,
            });
        }
        self.phase = to;
        Ok(())
    }
}

impl Drop for RuntimeSession {
    fn drop(&mut self) {
        // Drop is a best-effort rollback path. Normal shutdown uses
        // uninstall_subsystem so it can surface cleanup errors; a partially
        // bootstrapped session still must release every owned callback in the
        // reverse install order without panicking across a host boundary.
        while let Some(subsystem) = self.install_order.pop() {
            self.subsystems.remove(&subsystem);
            if let Some(cleanup) = self.cleanups.remove(&subsystem) {
                let _ = cleanup();
            }
        }
    }
}

impl Default for RuntimeSession {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifecycle_is_monotonic_and_supports_bootstrap_rollback() {
        let mut runtime = RuntimeSession::new();
        assert_eq!(runtime.phase(), RuntimePhase::New);
        runtime.start().unwrap();
        runtime.begin_shutdown().unwrap();
        runtime.finish_shutdown().unwrap();
        assert_eq!(runtime.phase(), RuntimePhase::Stopped);

        let mut failed = RuntimeSession::new();
        failed.start().unwrap();
        failed.fail(RuntimeError::AlreadyFailed);
        assert_eq!(failed.phase(), RuntimePhase::Failed);
        assert_eq!(failed.failure(), Some(RuntimeError::AlreadyFailed));
        assert_eq!(failed.mark_running(), Err(RuntimeError::AlreadyFailed));
    }

    #[test]
    fn invalid_transitions_are_rejected() {
        let mut runtime = RuntimeSession::new();
        assert!(matches!(
            runtime.finish_shutdown(),
            Err(RuntimeError::InvalidTransition { .. })
        ));
        runtime.start().unwrap();
        runtime.mark_running().unwrap();
        assert!(matches!(
            runtime.start(),
            Err(RuntimeError::InvalidTransition { .. })
        ));
    }

    #[test]
    fn subsystem_leases_enforce_reverse_teardown_and_generation() {
        let mut runtime = RuntimeSession::new();
        runtime.start().unwrap();
        let engine = runtime.install_subsystem(Subsystem::Engine).unwrap();
        let fs = runtime.install_subsystem(Subsystem::Filesystem).unwrap();

        assert_eq!(
            runtime.uninstall_subsystem(engine),
            Err(RuntimeError::InvalidShutdownOrder {
                expected: Subsystem::Filesystem,
                requested: Subsystem::Engine,
            })
        );
        runtime.begin_shutdown().unwrap();
        assert!(runtime.finish_shutdown().is_err());
        runtime.uninstall_subsystem(fs).unwrap();
        runtime.uninstall_subsystem(engine).unwrap();
        runtime.finish_shutdown().unwrap();
        assert_eq!(runtime.phase(), RuntimePhase::Stopped);
    }

    #[test]
    fn owned_cleanups_run_once_in_reverse_order_on_drop() {
        use std::cell::RefCell;
        use std::rc::Rc;

        let events = Rc::new(RefCell::new(Vec::new()));
        {
            let mut runtime = RuntimeSession::new();
            runtime.start().unwrap();
            let engine_events = Rc::clone(&events);
            runtime
                .install_subsystem_with_cleanup(Subsystem::Engine, move || {
                    engine_events.borrow_mut().push(Subsystem::Engine);
                    Ok(())
                })
                .unwrap();
            let surface_events = Rc::clone(&events);
            runtime
                .install_subsystem_with_cleanup(Subsystem::Surface, move || {
                    surface_events.borrow_mut().push(Subsystem::Surface);
                    Ok(())
                })
                .unwrap();
        }
        assert_eq!(
            events.borrow().as_slice(),
            &[Subsystem::Surface, Subsystem::Engine]
        );
    }
}
