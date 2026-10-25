//! Talkback (WING manual chapter 7, `/cfg/talk`): the talkback channel (CH40,
//! AUX8 or any input / aux channel, SETUP > TALKBACK) is sent to the buses,
//! matrices and mains each talk path (TALK A, TALK B) selects while its key is
//! on, at the TALK LEVEL or (USE SEND LEVELS, `indiv`) at the talkback
//! channel's own send levels. While a path talks, the buses it talks to are
//! dimmed by its BUS DIM and the selected monitoring path by its MON DIM.
//! Talk tags on strips (HOME > TAGS) unmute (TALK ON) or solo (TALK SOLO) them
//! while talkback is on.
//!
//! The talk signal is the talkback channel's PFL copy (processed, before its
//! fader and mute), so the channel can stay muted in the mix. It is added to a
//! destination's sum before the destination's own processing, like a send.
//! The keys' AUTO / PUSH / LATCH behaviour belongs to the application, which
//! sets `TalkPathParams::on`.

use crate::atomic::AtomicF32;
use crate::params::{StripKind, NUM_MAINS, NUM_MATRICES, WING_BUSES};
use crate::strip::Strip;
use livedaw_dsp::util::db_to_gain;
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};

/// SWITCH MODE of a talk key: AUTO (held = push, a short press latches),
/// PUSH (talk while held), LATCH (press on, press off).
pub const TALK_MODES: [&str; 3] = ["AUTO", "PUSH", "LATCH"];

pub mod talk_mode {
    pub const AUTO: i32 = 0;
    pub const PUSH: i32 = 1;
    pub const LATCH: i32 = 2;
}

/// The talk tags of HOME > TAGS, as bits of `StripConsole::talk_tags`. Tags
/// without A or B react to both talk keys.
pub const TALK_TAGS: [&str; 6] = ["TALK SOLO", "TALK ON", "TALK A SOLO", "TALK A ON", "TALK B SOLO", "TALK B ON"];

pub mod talk_tag {
    pub const SOLO: u32 = 1 << 0;
    pub const ON: u32 = 1 << 1;
    pub const A_SOLO: u32 = 1 << 2;
    pub const A_ON: u32 = 1 << 3;
    pub const B_SOLO: u32 = 1 << 4;
    pub const B_ON: u32 = 1 << 5;
    pub const ALL: u32 = 0x3f;
}

/// Default AUTO DIM amounts (dB): none, as a WING after INITIALIZE (`busdim`,
/// `mondim` 0).
pub const DEFAULT_TALK_DIM_DB: f32 = 0.0;

/// `assign_kind` without a talkback channel.
pub const TALK_OFF: i32 = -1;

/// Strips tagged to solo while `active` talks (bit 0 = A, bit 1 = B).
pub fn tag_solos(tags: u32, active: u32) -> bool {
    tag_reacts(tags, active, talk_tag::SOLO, talk_tag::A_SOLO, talk_tag::B_SOLO)
}

/// Strips tagged to unmute while `active` talks.
pub fn tag_unmutes(tags: u32, active: u32) -> bool {
    tag_reacts(tags, active, talk_tag::ON, talk_tag::A_ON, talk_tag::B_ON)
}

fn tag_reacts(tags: u32, active: u32, both: u32, a: u32, b: u32) -> bool {
    active != 0 && (tags & both != 0 || (active & 1 != 0 && tags & a != 0) || (active & 2 != 0 && tags & b != 0))
}

/// One talk path (TALK A or TALK B).
#[repr(C)]
#[derive(Debug)]
pub struct TalkPathParams {
    /// The talk key is on (set by the application from the key and SWITCH MODE).
    pub on: AtomicBool,
    /// USE SEND LEVELS (INDIVIDUAL TB SEND LEVELS): each destination gets the
    /// talkback channel's send level to it instead of the TALK LEVEL.
    pub individual: AtomicBool,
    /// Index into [`TALK_MODES`].
    pub mode: AtomicI32,
    /// AUTO DIM: attenuation (dB, >= 0) of the selected monitoring path (MON
    /// DIM) and of the buses talked to (BUS DIM) while talking.
    pub mon_dim_db: AtomicF32,
    pub bus_dim_db: AtomicF32,
    /// TALK TO BUS (bit n = bus n+1, 1-16), MATRIX (1-8), MAIN (1-4).
    pub buses: AtomicU32,
    pub matrices: AtomicU32,
    pub mains: AtomicU32,
}

impl Default for TalkPathParams {
    fn default() -> Self {
        Self {
            on: AtomicBool::new(false),
            individual: AtomicBool::new(false),
            mode: AtomicI32::new(talk_mode::AUTO),
            mon_dim_db: AtomicF32::new(DEFAULT_TALK_DIM_DB),
            bus_dim_db: AtomicF32::new(DEFAULT_TALK_DIM_DB),
            buses: AtomicU32::new(0),
            matrices: AtomicU32::new(0),
            mains: AtomicU32::new(0),
        }
    }
}

/// Talkback (`EngineControls::talk`).
#[repr(C)]
#[derive(Debug)]
pub struct TalkParams {
    /// The talkback channel: `TALK_OFF`, or a strip kind (channel or aux) and
    /// its 0-based number as shown (CH40 = (0, 39), AUX8 = (1, 7)).
    pub assign_kind: AtomicI32,
    pub assign_index: AtomicI32,
    /// TALK LEVEL in dB.
    pub level_db: AtomicF32,
    pub paths: [TalkPathParams; 2],
}

impl Default for TalkParams {
    fn default() -> Self {
        Self {
            assign_kind: AtomicI32::new(TALK_OFF),
            assign_index: AtomicI32::new(0),
            level_db: AtomicF32::new(0.0),
            paths: Default::default(),
        }
    }
}

impl TalkParams {
    pub fn reset(&self) {
        let d = TalkParams::default();
        self.assign_kind.store(d.assign_kind.load(Relaxed), Relaxed);
        self.assign_index.store(d.assign_index.load(Relaxed), Relaxed);
        self.level_db.store(d.level_db.load());
        for (p, d) in self.paths.iter().zip(&d.paths) {
            p.on.store(false, Relaxed);
            p.individual.store(d.individual.load(Relaxed), Relaxed);
            p.mode.store(d.mode.load(Relaxed), Relaxed);
            p.mon_dim_db.store(d.mon_dim_db.load());
            p.bus_dim_db.store(d.bus_dim_db.load());
            p.buses.store(0, Relaxed);
            p.matrices.store(0, Relaxed);
            p.mains.store(0, Relaxed);
        }
    }

    /// The talkback channel as (kind, 0-based number), or None when OFF.
    pub fn assignment(&self) -> Option<(StripKind, usize)> {
        let kind = StripKind::from_i32(self.assign_kind.load(Relaxed)).filter(|k| k.is_input())?;
        Some((kind, usize::try_from(self.assign_index.load(Relaxed)).ok()?))
    }

    /// Talk paths whose key is on (bit 0 = A, bit 1 = B), whether or not a
    /// talkback channel is assigned.
    pub fn keys_on(&self) -> u32 {
        self.paths.iter().enumerate().filter(|(_, p)| p.on.load(Relaxed)).fold(0, |m, (i, _)| m | 1 << i)
    }
}

/// A snapshot of one talk path for a block.
#[derive(Clone, Copy, Default)]
struct PathSnap {
    individual: bool,
    buses: u32,
    matrices: u32,
    mains: u32,
    bus_dim: f32,
    mon_dim: f32,
}

/// Talkback for one block (built on the audio thread before the channels run).
pub(crate) struct TalkBlock<'a> {
    /// The talkback channel, if assigned and present.
    strip: Option<&'a Strip>,
    /// Paths talking (bit 0 = A, bit 1 = B): key on and a channel assigned.
    pub active: u32,
    level: f32,
    paths: [PathSnap; 2],
}

/// Talk gains for a destination: the talk signal's gain and the dim of the
/// destination's own sum.
#[derive(Clone, Copy, PartialEq, Debug)]
pub(crate) struct TalkGains {
    pub talk: f32,
    pub dim: f32,
}

impl TalkGains {
    pub const NONE: TalkGains = TalkGains { talk: 0.0, dim: 1.0 };
}

impl<'a> TalkBlock<'a> {
    /// `find` looks a strip up by kind and 0-based number.
    pub fn new(params: &TalkParams, find: impl Fn(StripKind, usize) -> Option<&'a Strip>) -> Self {
        let strip = params.assignment().and_then(|(kind, i)| find(kind, i));
        let active = if strip.is_some() { params.keys_on() } else { 0 };
        let snap = |p: &TalkPathParams| PathSnap {
            individual: p.individual.load(Relaxed),
            buses: p.buses.load(Relaxed),
            matrices: p.matrices.load(Relaxed),
            mains: p.mains.load(Relaxed),
            bus_dim: db_to_gain(-p.bus_dim_db.load().max(0.0)),
            mon_dim: db_to_gain(-p.mon_dim_db.load().max(0.0)),
        };
        Self { strip, active, level: db_to_gain(params.level_db.load()), paths: [snap(&params.paths[0]), snap(&params.paths[1])] }
    }

    /// The talkback channel while something talks.
    pub fn source(&self) -> Option<&'a Strip> {
        self.strip.filter(|_| self.active != 0)
    }

    /// The talkback channel, talking or not (talk ramps out on it).
    pub fn strip(&self) -> Option<&'a Strip> {
        self.strip
    }

    fn talking(&self) -> impl Iterator<Item = &PathSnap> {
        self.paths.iter().enumerate().filter(|(i, _)| self.active & (1 << i) != 0).map(|(_, p)| p)
    }

    /// Gains for a destination selected by `pick`; `send_db` is the talkback
    /// channel's send level to it (USE SEND LEVELS). Two paths talking to the
    /// same destination don't add up: the louder one wins, the deeper dim wins.
    fn gains(&self, pick: impl Fn(&PathSnap) -> bool, send_db: impl Fn(&Strip) -> f32, dims: bool) -> TalkGains {
        let Some(strip) = self.source() else { return TalkGains::NONE };
        let mut g = TalkGains::NONE;
        for p in self.talking().filter(|p| pick(p)) {
            let level = if p.individual { db_to_gain(send_db(strip)) } else { self.level };
            g.talk = g.talk.max(level);
            if dims {
                g.dim = g.dim.min(p.bus_dim);
            }
        }
        g
    }

    /// Bus slot `slot` (talk reaches buses 1-16 only; BUS DIM applies).
    pub fn bus(&self, slot: usize) -> TalkGains {
        if slot >= WING_BUSES {
            return TalkGains::NONE;
        }
        self.gains(
            |p| p.buses & (1 << slot) != 0,
            |s| s.sends.find(slot).map_or(crate::sends::DEFAULT_SEND_LEVEL_DB, |send| send.level_db.load()),
            true,
        )
    }

    pub fn main(&self, i: usize) -> TalkGains {
        if i >= NUM_MAINS {
            return TalkGains::NONE;
        }
        self.gains(|p| p.mains & (1 << i) != 0, |s| s.params.mains[i].level_db.load(), false)
    }

    pub fn matrix(&self, i: usize) -> TalkGains {
        if i >= NUM_MATRICES {
            return TalkGains::NONE;
        }
        self.gains(|p| p.matrices & (1 << i) != 0, |s| s.params.matrix[i].level_db.load(), false)
    }

    /// MON DIM of the selected monitoring path(s) while talking.
    pub fn mon_dim(&self) -> f32 {
        if self.strip.is_none() {
            return 1.0;
        }
        self.talking().fold(1.0, |d, p| d.min(p.mon_dim))
    }
}

/// The ramps of the talk signal and the talk dim per destination (buses
/// 1-16, mains, matrices). Each destination's task owns its entry, like the
/// send ramps it sums.
pub(crate) struct TalkRamps {
    buses: [UnsafeCell<TalkGains>; WING_BUSES],
    mains: [UnsafeCell<TalkGains>; NUM_MAINS],
    matrices: [UnsafeCell<TalkGains>; NUM_MATRICES],
}

// SAFETY: each entry is only touched by the task of its destination.
unsafe impl Sync for TalkRamps {}

impl Default for TalkRamps {
    fn default() -> Self {
        let none = || UnsafeCell::new(TalkGains::NONE);
        Self {
            buses: std::array::from_fn(|_| none()),
            mains: std::array::from_fn(|_| none()),
            matrices: std::array::from_fn(|_| none()),
        }
    }
}

impl TalkRamps {
    /// # Safety
    /// The task of `kind` destination `i` only.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get(&self, kind: StripKind, i: usize) -> Option<&mut TalkGains> {
        let cell = match kind {
            StripKind::Bus => self.buses.get(i),
            StripKind::Main => self.mains.get(i),
            StripKind::Matrix => self.matrices.get(i),
            _ => None,
        }?;
        Some(&mut *cell.get())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tags_react_to_their_paths() {
        use talk_tag::*;
        assert!(!tag_solos(SOLO | ON, 0), "nothing talks");
        assert!(tag_solos(SOLO, 1) && tag_solos(SOLO, 2));
        assert!(tag_solos(A_SOLO, 1) && !tag_solos(A_SOLO, 2));
        assert!(tag_unmutes(B_ON, 2) && !tag_unmutes(B_ON, 1) && tag_unmutes(B_ON, 3));
        assert!(!tag_unmutes(A_SOLO | B_SOLO | SOLO, 3));
    }

    #[test]
    fn assignment_and_keys() {
        let t = TalkParams::default();
        assert_eq!(t.assignment(), None);
        t.assign_kind.store(StripKind::Aux as i32, Relaxed);
        t.assign_index.store(7, Relaxed);
        assert_eq!(t.assignment(), Some((StripKind::Aux, 7)));
        t.assign_kind.store(StripKind::Bus as i32, Relaxed);
        assert_eq!(t.assignment(), None, "only input and aux channels host talkback");
        t.paths[1].on.store(true, Relaxed);
        assert_eq!(t.keys_on(), 2);
        t.reset();
        assert_eq!(t.keys_on(), 0);
    }
}
