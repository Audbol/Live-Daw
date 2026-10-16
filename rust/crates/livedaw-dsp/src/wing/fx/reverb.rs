//! Reverbs: the console's 480L-style halls, rooms, chambers, plates, concert and
//! ambience, the vintage room / reverb / plate, gated, reverse, delay+reverb,
//! shimmer, spring, VSS3 and blue plate.

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &[
    "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED", "REVERSE", "DEL/REV",
    "SHIMMER", "SPRING", "VSS3", "BPLATE",
];

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
