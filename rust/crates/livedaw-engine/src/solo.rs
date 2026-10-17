//! Console-level solo and mute (SETUP > AUDIO, manual pp. 45-46):
//!
//! * SOLO MODE. LIVE: each soloed strip goes to the monitor path its SOLO BUS
//!   chooses (`StripParams::monitor_bus`: A = SPEAKERS, B = PHONES, A+B), until
//!   a path is picked on the MONITORS screen while soloing. STUDIO: solos go
//!   to the solo bus selected on the MONITORS screen (`solo_bus`). SIP (solo in
//!   place): as STUDIO, and every input and aux channel that is neither soloed
//!   nor SOLO SAFE is muted, so the buses, mains and matrices hear the solo.
//! * MUTEGROUP/SIP OVERRIDE: with the preference on, MUTE on a strip muted by
//!   a mute group, a DCA mute or SIP unmutes it, until that mute's cause
//!   changes (a group engaged or released, SIP on or off).
//! * Talk tags: TALK SOLO strips solo and TALK ON strips unmute while their
//!   talk key is on (see `talk.rs`).
//!
//! Before the channels run, the audio thread computes every strip's effective
//! solo and the console's mute decision for the block ([`console_pass`]); the
//! strip's fader stage applies the decision over its own mute (own mute, mute
//! groups, DCA mutes) through [`StripConsole::apply_mute`].

use crate::atomic::AtomicF32;
use crate::params::EngineControls;
use crate::strip::Strip;
use crate::talk;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
use std::sync::Arc;

/// SOLO MODE (`EngineControls::solo_mode`).
pub const SOLO_MODES: [&str; 3] = ["LIVE", "STUDIO", "SIP"];

pub mod solo_mode {
    pub const LIVE: i32 = 0;
    pub const STUDIO: i32 = 1;
    pub const SIP: i32 = 2;
}

/// The monitor paths a monitor bus choice (`MONITOR_BUSES`: A, B, A+B) feeds,
/// as a mask: bit 0 = path A (SPEAKERS), bit 1 = path B (PHONES).
pub fn path_mask(choice: i32) -> u32 {
    match choice {
        0 => 1,
        1 => 2,
        _ => 3,
    }
}

/// A strip's MUTE as the surface shows it (`StripConsole::mute_state`, the
/// WING's `$mute`): GROUP (muted by a mute group, a DCA or SIP) flashes.
pub mod mute_state {
    pub const OFF: i32 = 0;
    /// Its own MUTE.
    pub const ON: i32 = 1;
    /// Muted by a mute group, a DCA mute or SIP.
    pub const GROUP: i32 = 2;
    /// Would be muted by a group, DCA or SIP, but MUTEGROUP/SIP OVERRIDE unmuted it.
    pub const OVERRIDDEN: i32 = 3;
}

/// The console's decision over a strip's own mute for a block.
mod force {
    pub const NONE: i32 = 0;
    pub const UNMUTE: i32 = 1;
    pub const MUTE: i32 = 2;
}

/// Cause bits: mute groups 0-7, DCAs 8-23, SIP 24.
const CAUSE_SIP: u32 = 1 << 24;

/// Console-level state kept on every strip.
#[derive(Debug, Default)]
pub struct StripConsole {
    /// HOME > TAGS talk tags (`talk::talk_tag` bits).
    pub talk_tags: AtomicU32,
    /// MUTEGROUP/SIP OVERRIDE: an explicit unmute over a group, DCA or SIP mute.
    pub mute_override: AtomicBool,
    /// [`mute_state`] of the last block.
    mute_state: AtomicI32,
    /// Effective solo of the last block (own, a DCA's or TALK SOLO).
    soloed: AtomicBool,
    /// [`force`] decision for this block (audio thread).
    force: AtomicI32,
    /// The group / DCA / SIP mute cause of the last block (audio thread).
    cause: AtomicU32,
    /// The monitor stage's solo ramp to path B (path A uses the strip's own).
    pub(crate) solo_ramp_b: AtomicF32,
}

impl StripConsole {
    pub fn mute_state(&self) -> i32 {
        self.mute_state.load(Relaxed)
    }

    /// Soloed in the last block (own solo, a soloed DCA or TALK SOLO).
    pub fn is_soloed(&self) -> bool {
        self.soloed.load(Relaxed)
    }

    /// The fader stage: the console's decision over the strip's own mute
    /// (`muted`: own mute, mute groups, DCA mutes).
    #[inline]
    pub(crate) fn apply_mute(&self, muted: bool) -> bool {
        match self.force.load(Relaxed) {
            force::UNMUTE => false,
            force::MUTE => true,
            _ => muted,
        }
    }
}

/// The console-wide solo state of a block.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct SoloBlock {
    pub any_solo: bool,
}

/// The mute groups and DCA mutes acting on `s` (cause bits).
fn group_cause(s: &Strip, c: &EngineControls, mute_groups: u32, dca_mute_groups: bool) -> u32 {
    let p = &s.params;
    let mut cause = p.mute_group_mask.load(Relaxed) & mute_groups & 0xff;
    let mask = p.dca_mask.load(Relaxed);
    if dca_mute_groups && mask != 0 {
        for (i, d) in c.dcas.iter().enumerate() {
            if mask & (1 << i) != 0 && d.mute.load(Relaxed) {
                cause |= 1 << (8 + i);
            }
        }
    }
    cause
}

/// Audio thread, before the channels run: every strip's effective solo, then
/// SIP and the mute decisions (talk tags, SIP, MUTEGROUP/SIP OVERRIDE).
pub(crate) fn console_pass<'a, I>(c: &EngineControls, strips: I, talk_active: u32) -> SoloBlock
where
    I: Iterator<Item = &'a Arc<Strip>> + Clone,
{
    let mut any_solo = false;
    let mut any_input_solo = false;
    for s in strips.clone() {
        let tags = s.console.talk_tags.load(Relaxed);
        let soloed = s.is_soloed(&c.dcas) || talk::tag_solos(tags, talk_active);
        s.console.soloed.store(soloed, Relaxed);
        any_solo |= soloed;
        any_input_solo |= soloed && s.kind().is_input();
    }
    let sip = c.solo_mode.load(Relaxed) == solo_mode::SIP && any_input_solo;
    c.sip_active.store(sip, Relaxed);

    let mute_groups = c.mute_groups.load(Relaxed);
    let dca_mute_groups = c.dca_mute_groups.load(Relaxed);
    let override_enabled = c.mute_override.load(Relaxed);
    for s in strips {
        let k = &s.console;
        let own = s.params.mute.load(Relaxed);
        let sip_mute = sip && s.kind().is_input() && !k.soloed.load(Relaxed) && !s.params.solo_safe.load(Relaxed);
        let cause = group_cause(s, c, mute_groups, dca_mute_groups) | if sip_mute { CAUSE_SIP } else { 0 };
        let last_cause = k.cause.swap(cause, Relaxed);

        // The override holds while the cause it overrode stays the same.
        let mut overridden = k.mute_override.load(Relaxed);
        if overridden && (!override_enabled || cause == 0 || cause != last_cause) {
            k.mute_override.store(false, Relaxed);
            overridden = false;
        }

        let (decision, state) = if talk::tag_unmutes(k.talk_tags.load(Relaxed), talk_active) {
            (force::UNMUTE, mute_state::OFF)
        } else if own {
            (force::NONE, mute_state::ON)
        } else if cause != 0 && overridden {
            (force::UNMUTE, mute_state::OVERRIDDEN)
        } else if cause & CAUSE_SIP != 0 {
            (force::MUTE, mute_state::GROUP)
        } else if cause != 0 {
            (force::NONE, mute_state::GROUP)
        } else {
            (force::NONE, mute_state::OFF)
        };
        k.force.store(decision, Relaxed);
        k.mute_state.store(state, Relaxed);
    }
    SoloBlock { any_solo }
}

/// Message thread: the MUTE key of a strip. Releases its own mute; with
/// MUTEGROUP/SIP OVERRIDE on, unmutes a strip muted by a group, DCA or SIP (or
/// takes that unmute back); otherwise mutes it.
pub fn press_mute(c: &EngineControls, s: &Strip) {
    let p = &s.params;
    let k = &s.console;
    if p.mute.load(Relaxed) {
        p.mute.store(false, Relaxed);
        return;
    }
    if c.mute_override.load(Relaxed) {
        match k.mute_state() {
            mute_state::GROUP => {
                k.mute_override.store(true, Relaxed);
                k.mute_state.store(mute_state::OVERRIDDEN, Relaxed);
                return;
            }
            mute_state::OVERRIDDEN => {
                k.mute_override.store(false, Relaxed);
                k.mute_state.store(mute_state::GROUP, Relaxed);
                return;
            }
            _ => {}
        }
    }
    p.mute.store(true, Relaxed);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn path_masks() {
        assert_eq!(path_mask(0), 1);
        assert_eq!(path_mask(1), 2);
        assert_eq!(path_mask(2), 3);
    }

    #[test]
    fn decisions_apply_over_the_strip_mute() {
        let k = StripConsole::default();
        assert!(k.apply_mute(true) && !k.apply_mute(false));
        k.force.store(force::UNMUTE, Relaxed);
        assert!(!k.apply_mute(true));
        k.force.store(force::MUTE, Relaxed);
        assert!(k.apply_mute(false));
    }
}
