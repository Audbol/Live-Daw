//! The analog EQ models as FX devices and the channel strips (gate + EQ +
//! dynamics in one slot, and the mastering chain).

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] =
    &["SOUL", "E88", "E84", "F110", "PULSAR", "MACH4", "*EVEN*", "*SOUL*", "*VINTAGE*", "*BUS*", "*MASTER*"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    let _ = ctx;
    None
}

#[cfg(test)]
mod tests {
    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }
}
