//! Concrete owner-thread runtime session.
//!
//! This is the single Rust value that joins lifecycle state with the native
//! resources it governs.  Keeping the slots here prevents a caller from
//! accidentally dropping a provider or surface before the lifecycle has
//! recorded its reverse-order lease release.

use crate::owners::RuntimeOwners;
use crate::{RuntimeError, RuntimeLifecycle, RuntimePhase, Subsystem, SubsystemLease};

/// Owns one runtime's lifecycle and its concrete native resources.
pub struct RuntimeSession<E, P, S, G = ()> {
    lifecycle: RuntimeLifecycle,
    owners: RuntimeOwners<E, P, S, G>,
}

impl<E, P, S, G> RuntimeSession<E, P, S, G> {
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

    /// Returns the attached engine without exposing the owner slots.
    pub fn engine(&self) -> Option<&E> {
        self.owners.engine()
    }

    /// Returns the attached provider without exposing the owner slots.
    pub fn provider(&self) -> Option<&P> {
        self.owners.provider()
    }

    /// Returns the attached surface without exposing the owner slots.
    pub fn surface(&self) -> Option<&S> {
        self.owners.surface()
    }

    /// Returns the attached graphics session without exposing the owner slots.
    pub fn graphics(&self) -> Option<&G> {
        self.owners.graphics()
    }

    pub fn graphics_for_shutdown_mut(&mut self) -> Result<Option<&mut G>, RuntimeError> {
        self.release_boundary(Subsystem::Graphics)?;
        Ok(self.owners.graphics_mut())
    }

    pub fn release_engine(&mut self) -> Result<Option<E>, RuntimeError> {
        self.release_boundary(Subsystem::Engine)?;
        Ok(self.owners.take_engine())
    }

    pub fn release_provider(&mut self) -> Result<Option<P>, RuntimeError> {
        self.release_boundary(Subsystem::ElfNamespace)?;
        Ok(self.owners.take_provider())
    }

    pub fn release_surface(&mut self) -> Result<Option<S>, RuntimeError> {
        self.release_boundary(Subsystem::Surface)?;
        Ok(self.owners.take_surface())
    }

    pub fn release_graphics(&mut self) -> Result<Option<G>, RuntimeError> {
        self.release_boundary(Subsystem::Graphics)?;
        Ok(self.owners.take_graphics())
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

    pub fn attach_graphics(&mut self, graphics: G) -> Result<(), G> {
        self.owners.attach_graphics(graphics)
    }

    pub fn is_empty(&self) -> bool {
        self.owners.is_empty()
    }

    fn release_boundary(&self, subsystem: Subsystem) -> Result<(), RuntimeError> {
        self.lifecycle.assert_owner()?;
        if self.phase() != RuntimePhase::ShuttingDown {
            return Err(RuntimeError::InvalidTransition {
                from: self.phase(),
                to: RuntimePhase::ShuttingDown,
            });
        }
        if self.lifecycle.subsystem_active(subsystem) {
            return Err(RuntimeError::InvalidShutdownOrder {
                expected: subsystem,
                requested: subsystem,
            });
        }
        Ok(())
    }
}

impl<E, P, S, G> Default for RuntimeSession<E, P, S, G> {
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
        let mut session = RuntimeSession::<u8, u16, u32, u64>::new();
        session.start().unwrap();
        session.attach_engine(7).unwrap();
        session.attach_provider(8).unwrap();
        session.attach_surface(9).unwrap();
        session.attach_graphics(10).unwrap();
        let engine = session.install_subsystem(Subsystem::Engine).unwrap();
        let provider = session.install_subsystem(Subsystem::ElfNamespace).unwrap();
        let surface = session.install_subsystem(Subsystem::Surface).unwrap();
        let graphics = session.install_subsystem(Subsystem::Graphics).unwrap();
        session.mark_running().unwrap();
        session.begin_shutdown().unwrap();
        session.uninstall_subsystem(graphics).unwrap();
        assert_eq!(session.release_graphics().unwrap(), Some(10));
        session.uninstall_subsystem(surface).unwrap();
        assert_eq!(session.release_surface().unwrap(), Some(9));
        session.uninstall_subsystem(provider).unwrap();
        assert_eq!(session.release_provider().unwrap(), Some(8));
        session.uninstall_subsystem(engine).unwrap();
        assert_eq!(session.release_engine().unwrap(), Some(7));
        session.finish_shutdown().unwrap();
        assert_eq!(session.phase(), RuntimePhase::Stopped);
        assert!(session.is_empty());
    }

    #[test]
    fn resource_release_requires_shutdown_and_inactive_lease() {
        let mut session = RuntimeSession::<u8, u16, u32, u64>::new();
        session.start().unwrap();
        session.attach_engine(7).unwrap();
        let engine = session.install_subsystem(Subsystem::Engine).unwrap();
        assert!(session.release_engine().is_err());

        session.mark_running().unwrap();
        session.begin_shutdown().unwrap();
        assert!(session.release_engine().is_err());
        session.uninstall_subsystem(engine).unwrap();
        assert_eq!(session.release_engine().unwrap(), Some(7));
        session.finish_shutdown().unwrap();
    }
}
