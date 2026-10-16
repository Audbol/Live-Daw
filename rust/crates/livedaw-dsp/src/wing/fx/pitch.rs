//! Pitch and sub: stereo and dual pitch, double vocal, pitch fix, sub octaver,
//! sub monster and psycho bass.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["PITCH", "D-PITCH", "DOUBLE", "PCORR", "SUB", "SUB-M", "P-BASS"];

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
