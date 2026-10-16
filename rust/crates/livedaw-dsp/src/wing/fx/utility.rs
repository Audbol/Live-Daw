//! Standard processors: graphic EQs, precision limiter, 2-band de-esser, enhancer,
//! exciter, velvet imager, speaker manager, combinator, triple DEQ, BodyRez and
//! tape machine.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] =
    &["GEQ", "PIA", "LIMITER", "DE-S2", "ENHANCE", "EXCITER", "V-IMG", "SPKMAN", "C5-CMB", "DEQ3", "BODY", "TAPE"];

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
