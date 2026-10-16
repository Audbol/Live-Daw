//! Delays: stereo, ultratap, tape, oilcan and BBD.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["ST-DL", "TAP-DL", "TAPE-DL", "OILCAN", "BBD-DL"];

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
