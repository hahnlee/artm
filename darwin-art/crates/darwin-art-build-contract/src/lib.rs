//! Small, dependency-free contracts shared by native build orchestration.
//!
//! This crate deliberately contains no filesystem or process code.  It is the
//! stable Rust source of truth for identities that must agree between the
//! canonical ART builder and the incremental Ninja graph generator.

/// Bump when the common runtime/adapters include or command contract changes.
/// A mismatch disables cache promotion until the canonical builder repopulates
/// `_build/runtime-common`.
pub const RUNTIME_CACHE_IDENTITY: &str =
    "darwin-art-runtime-core-cache-v2-common-includes-fmt-adapters";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimeFlavor {
    Headless,
    Graphics,
}

impl RuntimeFlavor {
    pub const fn real_graphics(self) -> bool {
        matches!(self, Self::Graphics)
    }

    pub const fn output_dir(self) -> &'static str {
        match self {
            Self::Headless => "runtime-bootstrap",
            Self::Graphics => "runtime-graphics-bootstrap",
        }
    }

    pub const fn archive_name(self) -> &'static str {
        match self {
            Self::Headless => "libart-runtime-bootstrap-darwin.a",
            Self::Graphics => "libart-runtime-graphics-bootstrap-darwin.a",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flavor_contracts_are_distinct_and_stable() {
        assert!(!RuntimeFlavor::Headless.real_graphics());
        assert!(RuntimeFlavor::Graphics.real_graphics());
        assert_ne!(
            RuntimeFlavor::Headless.output_dir(),
            RuntimeFlavor::Graphics.output_dir()
        );
        assert_ne!(
            RuntimeFlavor::Headless.archive_name(),
            RuntimeFlavor::Graphics.archive_name()
        );
    }
}
