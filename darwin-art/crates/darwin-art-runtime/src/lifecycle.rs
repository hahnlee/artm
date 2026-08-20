//! Production lifecycle state without type-erased resource storage.
//!
//! `RuntimeLifecycle` owns only phase/lease/thread-affinity state. Concrete
//! engine, provider, and surface values live in `RuntimeOwners`, so the
//! production host never hides native resources behind `Any` or cleanup
//! closures.

use std::{
    collections::BTreeMap,
    marker::PhantomData,
    rc::Rc,
    thread::{self, ThreadId},
};

use super::{RuntimeError, RuntimePhase, Subsystem, SubsystemLease};

/// Owner-thread lifecycle coordinator for production runtime resources.
pub struct RuntimeLifecycle {
    owner: ThreadId,
    phase: RuntimePhase,
    failure: Option<RuntimeError>,
    subsystems: BTreeMap<Subsystem, u64>,
    install_order: Vec<Subsystem>,
    next_generation: u64,
    _owner_thread: PhantomData<Rc<()>>,
}

impl RuntimeLifecycle {
    pub fn new() -> Self {
        Self {
            owner: thread::current().id(),
            phase: RuntimePhase::New,
            failure: None,
            subsystems: BTreeMap::new(),
            install_order: Vec::new(),
            next_generation: 1,
            _owner_thread: PhantomData,
        }
    }

    pub const fn phase(&self) -> RuntimePhase {
        self.phase
    }

    pub const fn failure(&self) -> Option<RuntimeError> {
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
        Ok(())
    }

    pub fn assert_owner(&self) -> Result<(), RuntimeError> {
        if thread::current().id() == self.owner {
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

impl Default for RuntimeLifecycle {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn concrete_lifecycle_has_no_resource_cleanup_side_channel() {
        let mut lifecycle = RuntimeLifecycle::new();
        lifecycle.start().unwrap();
        let engine = lifecycle.install_subsystem(Subsystem::Engine).unwrap();
        let surface = lifecycle.install_subsystem(Subsystem::Surface).unwrap();
        assert_eq!(
            lifecycle.uninstall_subsystem(engine),
            Err(RuntimeError::InvalidShutdownOrder {
                expected: Subsystem::Surface,
                requested: Subsystem::Engine,
            })
        );
        lifecycle.uninstall_subsystem(surface).unwrap();
        lifecycle.uninstall_subsystem(engine).unwrap();
        lifecycle.mark_running().unwrap();
        lifecycle.begin_shutdown().unwrap();
        lifecycle.finish_shutdown().unwrap();
    }
}
