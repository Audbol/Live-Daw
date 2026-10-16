//! Guitar and bass amps: rack amp, UK rock, angel, jazz clean and deluxe.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["RACKAMP", "UKROCK", "ANGEL", "JAZZC", "DELUXE"];

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
