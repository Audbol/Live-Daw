//! Modulation: dimension chorus, stereo chorus and flanger, phaser, tremolo/panner,
//! rotary speaker and the mood filter.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["DIMCRS", "CHORUS", "FLANGER", "PHASER", "PANNER", "ROTARY", "MOOD"];

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
