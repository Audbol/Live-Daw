//! Stable references to strips that survive save/load ("ch:3", "bus:0", "main:0").
//!
//! Channels are referenced by their id (1-based, kept when channels are added
//! or removed); aux channels, buses, mains and matrices by their 0-based slot.

use crate::xml::parse_int_prefix;
use livedaw_engine::{Strip, StripKind};
use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct StripRef {
    pub kind: StripKind,
    pub id: i32,
}

impl StripRef {
    pub fn new(kind: StripKind, id: i32) -> Self {
        Self { kind, id }
    }

    pub fn of(s: &Strip) -> Self {
        Self { kind: s.kind(), id: s.id() }
    }

    /// Lenient: unknown kinds are channels, a bad id is 0.
    pub fn parse(s: &str) -> Self {
        let (kind, id) = s.split_once(':').unwrap_or((s, ""));
        Self { kind: StripKind::parse(kind), id: parse_int_prefix(id) }
    }
}

impl fmt::Display for StripRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}", self.kind.as_str(), self.id)
    }
}

/// "ch:1,bus:0" (empty parts skipped).
pub fn refs_to_string(refs: &[StripRef]) -> String {
    refs.iter().map(ToString::to_string).collect::<Vec<_>>().join(",")
}

pub fn refs_from_string(s: &str) -> Vec<StripRef> {
    s.split(',').map(str::trim).filter(|p| !p.is_empty()).map(StripRef::parse).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let refs = vec![
            StripRef::new(StripKind::Channel, 12),
            StripRef::new(StripKind::Aux, 0),
            StripRef::new(StripKind::Bus, 3),
            StripRef::new(StripKind::Main, 0),
            StripRef::new(StripKind::Matrix, 7),
        ];
        let s = refs_to_string(&refs);
        assert_eq!(s, "ch:12,aux:0,bus:3,main:0,mtx:7");
        assert_eq!(refs_from_string(&s), refs);
        assert_eq!(refs_from_string(""), vec![]);
        assert_eq!(StripRef::parse("bogus"), StripRef::new(StripKind::Channel, 0));
    }
}
