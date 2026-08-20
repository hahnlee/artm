#![forbid(unsafe_code)]

//! Rust-owned runtime lifecycle state.
//!
//! The native engine remains behind `darwin-art-engine-sys`; this crate owns
//! the ordering and rollback state so C++ callbacks cannot invent a second,
//! conflicting lifecycle machine.

use std::{marker::PhantomData, rc::Rc, thread::ThreadId};

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
}

impl RuntimeError {
    pub const fn status(self) -> StatusCode {
        match self {
            Self::WrongOwnerThread => StatusCode::InvalidState,
            Self::InvalidTransition { .. } => StatusCode::InvalidState,
            Self::AlreadyFailed => StatusCode::Internal,
            Self::EngineFailure { .. } => StatusCode::Internal,
        }
    }
}

/// One-shot owner-thread runtime state. `Rc` makes accidental Send/Sync
/// implementation impossible; ART/HWUI JNI objects remain thread-affine.
pub struct RuntimeSession {
    owner: ThreadId,
    phase: RuntimePhase,
    failure: Option<RuntimeError>,
    _owner_thread: PhantomData<Rc<()>>,
}

impl RuntimeSession {
    pub fn new() -> Self {
        Self {
            owner: std::thread::current().id(),
            phase: RuntimePhase::New,
            failure: None,
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
        self.phase = to;
        Ok(())
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
}
