//! Recall safes of sources and outputs: the GLOBAL SAFE window's SOURCE and
//! OUTPUT groups (the WING's `/$ctl/safes/source/*`, `/$ctl/safes/output/*`).
//! A safe source keeps its settings and name, a safe output its patch, when
//! a scene is loaded (or a GO steps to one); RESTORE, PARTIAL LOAD with NO
//! GLOB and snippets ignore them, as the strips' recall safe (manual 5.6
//! p. 54-55; WING remote protocols, Global Safes). Like the strips' SAFE
//! keys they are the console's: saved with the show, cleared by a new one.

use crate::sources::NUM_SOURCES;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};

/// Hardware output ports with a safe (as many as there are hardware sources).
pub const NUM_SAFE_OUTPUTS: usize = crate::sources::NUM_HW_SOURCES;

const fn words(bits: usize) -> usize {
    bits.div_ceil(64)
}

/// A fixed set of flags, readable and writable from any thread.
pub struct FlagSet<const W: usize> {
    words: [AtomicU64; W],
    len: usize,
}

impl<const W: usize> FlagSet<W> {
    fn new(len: usize) -> Self {
        Self { words: std::array::from_fn(|_| AtomicU64::new(0)), len: len.min(W * 64) }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, i: usize) -> bool {
        i < self.len && self.words[i / 64].load(Relaxed) & (1 << (i % 64)) != 0
    }

    pub fn set(&self, i: usize, on: bool) {
        if i >= self.len {
            return;
        }
        let bit = 1 << (i % 64);
        if on {
            self.words[i / 64].fetch_or(bit, Relaxed);
        } else {
            self.words[i / 64].fetch_and(!bit, Relaxed);
        }
    }

    pub fn any(&self) -> bool {
        self.words.iter().any(|w| w.load(Relaxed) != 0)
    }

    pub fn clear(&self) {
        self.words.iter().for_each(|w| w.store(0, Relaxed));
    }

    /// The indices that are set, in order.
    pub fn indices(&self) -> Vec<usize> {
        (0..self.len).filter(|&i| self.get(i)).collect()
    }

    /// The set as numbers and ranges from 0 ("0-7,12"), for files.
    pub fn to_ranges(&self) -> String {
        let set = self.indices();
        let mut out: Vec<String> = Vec::new();
        let mut k = 0;
        while k < set.len() {
            let start = set[k];
            while k + 1 < set.len() && set[k + 1] == set[k] + 1 {
                k += 1;
            }
            out.push(if set[k] > start { format!("{start}-{}", set[k]) } else { start.to_string() });
            k += 1;
        }
        out.join(",")
    }

    /// Replaces the set with [`FlagSet::to_ranges`]' text.
    pub fn set_ranges(&self, text: &str) {
        self.clear();
        for part in text.split(',').map(str::trim).filter(|p| !p.is_empty()) {
            let (a, b) = part.split_once('-').unwrap_or((part, part));
            let (Ok(a), Ok(b)) = (a.trim().parse::<usize>(), b.trim().parse::<usize>()) else { continue };
            for i in a..=b.min(self.len.saturating_sub(1)) {
                self.set(i, true);
            }
        }
    }
}

/// The source and output safes.
pub struct IoSafes {
    /// By source table index (`sources::source_index`).
    pub sources: FlagSet<{ words(NUM_SOURCES) }>,
    /// By hardware output port.
    pub outputs: FlagSet<{ words(NUM_SAFE_OUTPUTS) }>,
}

impl Default for IoSafes {
    fn default() -> Self {
        Self { sources: FlagSet::new(NUM_SOURCES), outputs: FlagSet::new(NUM_SAFE_OUTPUTS) }
    }
}

impl IoSafes {
    pub fn any(&self) -> bool {
        self.sources.any() || self.outputs.any()
    }

    pub fn clear(&self) {
        self.sources.clear();
        self.outputs.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_and_ranges_round_trip() {
        let s = IoSafes::default();
        assert!(!s.any());
        for i in [0, 1, 2, 5, 63, 64, 645] {
            s.sources.set(i, true);
        }
        s.sources.set(NUM_SOURCES + 3, true); // out of range: ignored
        assert_eq!(s.sources.to_ranges(), "0-2,5,63-64,645");
        let t = IoSafes::default();
        t.sources.set_ranges(&s.sources.to_ranges());
        assert_eq!(t.sources.indices(), s.sources.indices());
        s.sources.set(1, false);
        assert!(!s.sources.get(1) && s.sources.get(2));
        s.outputs.set(7, true);
        assert!(s.any());
        s.clear();
        assert!(!s.any());
    }
}
