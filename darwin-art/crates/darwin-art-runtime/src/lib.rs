#![forbid(unsafe_code)]

//! Rust-owned runtime lifecycle state.
//!
//! The native engine remains behind `darwin-art-engine-sys`; this crate owns
//! the ordering and rollback state so C++ callbacks cannot invent a second,
//! conflicting lifecycle machine.

use std::{any::Any, collections::BTreeMap, marker::PhantomData, rc::Rc, thread::ThreadId};

use darwin_art_abi::StatusCode;

mod owners;
mod provider;

pub use owners::RuntimeOwners;
pub use provider::{ProviderLeaseError, ProviderLeaseTable};

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

trait OwnedSubsystem {
    fn resource_any(&self) -> &dyn Any;
    fn cleanup(&mut self) -> Result<(), RuntimeError>;
}

struct OwnedSubsystemResource<T, F> {
    resource: Option<T>,
    cleanup: Option<F>,
}

struct OwnedSubsystemResourceWithCleanup<T, F> {
    resource: Option<T>,
    cleanup: Option<F>,
}

impl<T, F> OwnedSubsystem for OwnedSubsystemResource<T, F>
where
    T: Any,
    F: FnOnce() -> Result<(), RuntimeError> + 'static,
{
    fn resource_any(&self) -> &dyn Any {
        self.resource
            .as_ref()
            .expect("owned subsystem resource must remain live")
    }

    fn cleanup(&mut self) -> Result<(), RuntimeError> {
        self.cleanup
            .take()
            .expect("owned subsystem cleanup must run once")()
    }
}

impl<T, F> OwnedSubsystem for OwnedSubsystemResourceWithCleanup<T, F>
where
    T: Any,
    F: FnOnce(&mut T) -> Result<(), RuntimeError> + 'static,
{
    fn resource_any(&self) -> &dyn Any {
        self.resource
            .as_ref()
            .expect("owned subsystem resource must remain live")
    }

    fn cleanup(&mut self) -> Result<(), RuntimeError> {
        let cleanup = self
            .cleanup
            .take()
            .expect("owned subsystem cleanup must run once");
        cleanup(
            self.resource
                .as_mut()
                .expect("owned subsystem resource must remain live"),
        )
    }
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
    owned_resources: BTreeMap<Subsystem, Box<dyn OwnedSubsystem>>,
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
            owned_resources: BTreeMap::new(),
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
        self.install_owned_subsystem(subsystem, (), cleanup)
    }

    /// Installs a subsystem and transfers a concrete Rust resource into the
    /// session. The resource is removed only after its native cleanup has
    /// returned, so foreign teardown always runs while the owning image/guard
    /// is still alive.
    pub fn install_owned_subsystem<T, F>(
        &mut self,
        subsystem: Subsystem,
        resource: T,
        cleanup: F,
    ) -> Result<SubsystemLease, RuntimeError>
    where
        T: 'static,
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
        self.owned_resources.insert(
            subsystem,
            Box::new(OwnedSubsystemResource {
                resource: Some(resource),
                cleanup: Some(cleanup),
            }),
        );
        self.install_order.push(subsystem);
        Ok(SubsystemLease {
            subsystem,
            generation,
        })
    }

    /// Installs a concrete resource whose cleanup receives the same owned
    /// value. This is the preferred form for native handles: the shutdown
    /// callback cannot accidentally capture a second `Rc<RefCell<T>>` alias,
    /// and the resource remains alive until cleanup returns.
    pub fn install_owned_subsystem_with_resource_cleanup<T, F>(
        &mut self,
        subsystem: Subsystem,
        resource: T,
        cleanup: F,
    ) -> Result<SubsystemLease, RuntimeError>
    where
        T: 'static,
        F: FnOnce(&mut T) -> Result<(), RuntimeError> + 'static,
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
        self.owned_resources.insert(
            subsystem,
            Box::new(OwnedSubsystemResourceWithCleanup {
                resource: Some(resource),
                cleanup: Some(cleanup),
            }),
        );
        self.install_order.push(subsystem);
        Ok(SubsystemLease {
            subsystem,
            generation,
        })
    }

    /// Borrows a session-owned resource for owner-thread orchestration. The
    /// caller must not retain the reference across a subsystem uninstall.
    pub fn owned_resource<T: 'static>(&self, subsystem: Subsystem) -> Option<&T> {
        self.owned_resources
            .get(&subsystem)
            .and_then(|resource| resource.resource_any().downcast_ref::<T>())
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
        let mut resource = self
            .owned_resources
            .remove(&lease.subsystem)
            .expect("subsystem resource must accompany its lease");
        resource.cleanup()
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
            if let Some(mut resource) = self.owned_resources.remove(&subsystem) {
                let _ = resource.cleanup();
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

    #[test]
    fn owned_cleanup_is_consumed_by_normal_uninstall() {
        use std::cell::Cell;
        use std::rc::Rc;

        let calls = Rc::new(Cell::new(0));
        let mut runtime = RuntimeSession::new();
        runtime.start().unwrap();
        let cleanup_calls = Rc::clone(&calls);
        let lease = runtime
            .install_subsystem_with_cleanup(Subsystem::Engine, move || {
                cleanup_calls.set(cleanup_calls.get() + 1);
                Ok(())
            })
            .unwrap();
        runtime.begin_shutdown().unwrap();
        runtime.uninstall_subsystem(lease).unwrap();
        runtime.finish_shutdown().unwrap();
        drop(runtime);
        assert_eq!(calls.get(), 1);
    }

    #[test]
    fn owned_resource_is_visible_only_while_lease_is_active() {
        let mut runtime = RuntimeSession::new();
        runtime.start().unwrap();
        let lease = runtime
            .install_owned_subsystem(Subsystem::Engine, String::from("engine"), || Ok(()))
            .unwrap();
        assert_eq!(
            runtime
                .owned_resource::<String>(Subsystem::Engine)
                .map(String::as_str),
            Some("engine")
        );
        runtime.begin_shutdown().unwrap();
        runtime.uninstall_subsystem(lease).unwrap();
        assert!(
            runtime
                .owned_resource::<String>(Subsystem::Engine)
                .is_none()
        );
        runtime.finish_shutdown().unwrap();
    }
}
