//! Concrete owner-thread runtime session.
//!
//! This is the single Rust value that joins lifecycle state with the native
//! resources it governs.  Keeping the slots here prevents a caller from
//! accidentally dropping a provider or surface before the lifecycle has
//! recorded its reverse-order lease release.

use crate::{
    RuntimeError, RuntimeLifecycle, RuntimeOwners, RuntimePhase, Subsystem, SubsystemLease,
};

/// Owns one runtime's lifecycle and its concrete native resources.
pub struct RuntimeSession<E, P, S> {
    lifecycle: RuntimeLifecycle,
    owners: RuntimeOwners<E, P, S>,
}

impl<E, P, S> RuntimeSession<E, P, S> {
    pub fn new() -> Self {
        Self {
            lifecycle: RuntimeLifecycle::new(),
            owners: RuntimeOwners::new(),
        }
    }

    pub const fn phase(&self) -> RuntimePhase {
        self.lifecycle.phase()
    }

    pub const fn failure(&self) -> Option<RuntimeError> {
        self.lifecycle.failure()
    }

    pub fn start(&mut self) -> Result<(), RuntimeError> {
        self.lifecycle.start()
    }

    pub fn mark_running(&mut self) -> Result<(), RuntimeError> {
        self.lifecycle.mark_running()
    }

    pub fn begin_shutdown(&mut self) -> Result<(), RuntimeError> {
        self.lifecycle.begin_shutdown()
    }

    pub fn finish_shutdown(&mut self) -> Result<(), RuntimeError> {
        self.lifecycle.finish_shutdown()
    }

    pub fn fail(&mut self, error: RuntimeError) {
        self.lifecycle.fail(error);
    }

    pub fn install_subsystem(
        &mut self,
        subsystem: Subsystem,
    ) -> Result<SubsystemLease, RuntimeError> {
        self.lifecycle.install_subsystem(subsystem)
    }

    pub fn uninstall_subsystem(&mut self, lease: SubsystemLease) -> Result<(), RuntimeError> {
        self.lifecycle.uninstall_subsystem(lease)
    }

    pub fn assert_owner(&self) -> Result<(), RuntimeError> {
        self.lifecycle.assert_owner()
    }

    pub fn owners(&self) -> &RuntimeOwners<E, P, S> {
        &self.owners
    }

    pub fn owners_mut(&mut self) -> &mut RuntimeOwners<E, P, S> {
        &mut self.owners
    }

    pub fn attach_engine(&mut self, engine: E) -> Result<(), E> {
        self.owners.attach_engine(engine)
    }

    pub fn attach_provider(&mut self, provider: P) -> Result<(), P> {
        self.owners.attach_provider(provider)
    }

    pub fn attach_surface(&mut self, surface: S) -> Result<(), S> {
        self.owners.attach_surface(surface)
    }

    pub fn take_engine(&mut self) -> Option<E> {
        self.owners.take_engine()
    }

    pub fn take_provider(&mut self) -> Option<P> {
        self.owners.take_provider()
    }

    pub fn take_surface(&mut self) -> Option<S> {
        self.owners.take_surface()
    }

    pub fn is_empty(&self) -> bool {
        self.owners.is_empty()
    }
}

impl<E, P, S> Default for RuntimeSession<E, P, S> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::RuntimeSession;
    use crate::{RuntimePhase, Subsystem};

    #[test]
    fn session_keeps_resources_and_lifecycle_in_one_owner_value() {
        let mut session = RuntimeSession::<u8, u16, u32>::new();
        session.start().unwrap();
        session.attach_engine(7).unwrap();
        session.attach_provider(8).unwrap();
        session.attach_surface(9).unwrap();
        let engine = session.install_subsystem(Subsystem::Engine).unwrap();
        let provider = session.install_subsystem(Subsystem::ElfNamespace).unwrap();
        let surface = session.install_subsystem(Subsystem::Surface).unwrap();
        session.mark_running().unwrap();
        session.begin_shutdown().unwrap();
        session.uninstall_subsystem(surface).unwrap();
        assert_eq!(session.take_surface(), Some(9));
        session.uninstall_subsystem(provider).unwrap();
        assert_eq!(session.take_provider(), Some(8));
        session.uninstall_subsystem(engine).unwrap();
        assert_eq!(session.take_engine(), Some(7));
        session.finish_shutdown().unwrap();
        assert_eq!(session.phase(), RuntimePhase::Stopped);
        assert!(session.is_empty());
    }
}
