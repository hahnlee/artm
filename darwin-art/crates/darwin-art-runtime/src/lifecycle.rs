//! Production lifecycle state without type-erased resource storage.
//!
//! `RuntimeLifecycle` owns only phase/lease/thread-affinity state. Concrete
//! engine, provider, and surface values live in `RuntimeOwners`, so the
//! production host never hides native resources behind `Any` or cleanup
//! closures.

use core::ffi::c_void;
use darwin_art_engine_sys::{
    LifecycleBeginFn, LifecycleFailedFn, LifecycleFinishFn, LifecycleHooks,
};
use std::{
    collections::BTreeMap,
    marker::PhantomData,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    thread::{self, ThreadId},
};

use super::{RuntimeError, RuntimePhase, Subsystem, SubsystemLease};

/// Owner-thread lifecycle coordinator for production runtime resources.
pub struct RuntimeLifecycle {
    session_id: u64,
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
        static NEXT_SESSION_ID: AtomicU64 = AtomicU64::new(1);
        Self {
            session_id: NEXT_SESSION_ID.fetch_add(1, Ordering::Relaxed),
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

    /// Build the optional native lifecycle bridge for one synchronous ART
    /// invocation. The returned table contains only an opaque pointer to this
    /// lifecycle object; it must not outlive the owning `RuntimeSession` or be
    /// called from another thread. The host keeps the table alive until the
    /// matching native shutdown has completed.
    pub fn native_hooks(&mut self) -> LifecycleHooks {
        LifecycleHooks {
            struct_size: core::mem::size_of::<LifecycleHooks>() as u32,
            abi_version: 1,
            context: (self as *mut Self).cast::<c_void>(),
            begin_run: Some(native_begin_run as LifecycleBeginFn),
            finish_run: Some(native_finish_run as LifecycleFinishFn),
            begin_shutdown: Some(native_begin_shutdown as LifecycleBeginFn),
            mark_failed: Some(native_mark_failed as LifecycleFailedFn),
        }
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
            session_id: self.session_id,
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
        if lease.session_id != self.session_id {
            return Err(RuntimeError::SubsystemNotActive {
                subsystem: lease.subsystem,
            });
        }
        self.install_order.pop();
        self.subsystems.remove(&lease.subsystem);
        Ok(())
    }

    /// Remove the newest lease owned by this lifecycle.
    ///
    /// Production teardown should not keep lease tokens in a second host-side
    /// state machine. The lifecycle already owns the generation and session
    /// identity, so it can mint the checked token internally and return only
    /// the subsystem that was removed.
    pub fn uninstall_latest_subsystem(&mut self) -> Result<Option<Subsystem>, RuntimeError> {
        self.assert_owner()?;
        let Some(&subsystem) = self.install_order.last() else {
            return Ok(None);
        };
        let generation = self
            .subsystems
            .get(&subsystem)
            .copied()
            .ok_or(RuntimeError::SubsystemNotActive { subsystem })?;
        self.uninstall_subsystem(SubsystemLease {
            subsystem,
            generation,
            session_id: self.session_id,
        })?;
        Ok(Some(subsystem))
    }

    pub fn subsystem_active(&self, subsystem: Subsystem) -> bool {
        self.subsystems.contains_key(&subsystem)
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

unsafe extern "C" fn native_begin_run(context: *mut c_void) -> i32 {
    let lifecycle = unsafe { &mut *context.cast::<RuntimeLifecycle>() };
    match lifecycle.phase {
        RuntimePhase::Bootstrapping => 0,
        _ => RuntimeError::InvalidTransition {
            from: lifecycle.phase,
            to: RuntimePhase::Bootstrapping,
        }
        .status() as i32,
    }
}

unsafe extern "C" fn native_finish_run(context: *mut c_void, runtime_created: i32) -> i32 {
    let lifecycle = unsafe { &mut *context.cast::<RuntimeLifecycle>() };
    if runtime_created == 0 {
        lifecycle.fail(RuntimeError::EngineFailure { status: 1 });
        return 1;
    }
    lifecycle
        .mark_running()
        .map(|_| 0)
        .unwrap_or_else(|error| error.status() as i32)
}

unsafe extern "C" fn native_begin_shutdown(context: *mut c_void) -> i32 {
    let lifecycle = unsafe { &mut *context.cast::<RuntimeLifecycle>() };
    if lifecycle.phase() == RuntimePhase::ShuttingDown {
        return 0;
    }
    lifecycle
        .begin_shutdown()
        .map(|_| 0)
        .unwrap_or_else(|error| error.status() as i32)
}

unsafe extern "C" fn native_mark_failed(context: *mut c_void, status: i32) {
    let lifecycle = unsafe { &mut *context.cast::<RuntimeLifecycle>() };
    lifecycle.fail(RuntimeError::EngineFailure { status });
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

    #[test]
    fn subsystem_leases_cannot_cross_runtime_sessions() {
        let mut first = RuntimeLifecycle::new();
        first.start().unwrap();
        let first_lease = first.install_subsystem(Subsystem::Engine).unwrap();

        let mut second = RuntimeLifecycle::new();
        second.start().unwrap();
        let second_lease = second.install_subsystem(Subsystem::Engine).unwrap();

        // Both sessions intentionally use generation one.  The opaque
        // session discriminator is what prevents a stale lease from tearing
        // down a different runtime's engine.
        assert_eq!(
            second.uninstall_subsystem(first_lease),
            Err(RuntimeError::SubsystemNotActive {
                subsystem: Subsystem::Engine,
            })
        );
        second.begin_shutdown().unwrap();
        second.uninstall_subsystem(second_lease).unwrap();
        second.finish_shutdown().unwrap();
        first.begin_shutdown().unwrap();
        first.uninstall_subsystem(first_lease).unwrap();
        first.finish_shutdown().unwrap();
    }

    #[test]
    fn native_hooks_drive_the_same_lifecycle_owner() {
        let mut lifecycle = RuntimeLifecycle::new();
        lifecycle.start().unwrap();
        let hooks = lifecycle.native_hooks();
        unsafe {
            assert_eq!((hooks.begin_run.unwrap())(hooks.context), 0);
            assert_eq!((hooks.finish_run.unwrap())(hooks.context, 1), 0);
            assert_eq!((hooks.begin_shutdown.unwrap())(hooks.context), 0);
            (hooks.mark_failed.unwrap())(hooks.context, 70);
        }
        assert_eq!(lifecycle.phase(), RuntimePhase::Failed);
    }

    #[test]
    fn owner_can_uninstall_latest_without_exporting_lease_tokens() {
        let mut lifecycle = RuntimeLifecycle::new();
        lifecycle.start().unwrap();
        lifecycle.install_subsystem(Subsystem::Engine).unwrap();
        lifecycle.install_subsystem(Subsystem::Surface).unwrap();
        lifecycle.begin_shutdown().unwrap();
        assert_eq!(
            lifecycle.uninstall_latest_subsystem().unwrap(),
            Some(Subsystem::Surface)
        );
        assert_eq!(
            lifecycle.uninstall_latest_subsystem().unwrap(),
            Some(Subsystem::Engine)
        );
        assert_eq!(lifecycle.uninstall_latest_subsystem().unwrap(), None);
        lifecycle.finish_shutdown().unwrap();
    }
}
