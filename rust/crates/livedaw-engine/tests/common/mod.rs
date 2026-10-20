//! Helpers shared by the engine tests.

use livedaw_engine::params::StripKind;
use livedaw_engine::Engine;
use std::sync::atomic::Ordering::Relaxed;

/// Sets the number of channels; new channels get their fader at 0 dB (a new
/// channel's fader is down, as on the WING), so a patched input reaches the mains.
pub fn set_channels(e: &Engine, count: usize) {
    let before = e.num_channels();
    e.set_num_channels(count);
    for ch in e.strips(StripKind::Channel).iter().skip(before) {
        ch.params.fader_db.store(0.0);
    }
}

/// The fixed strips (aux, buses, mains, matrices) and the DCAs at 0 dB with
/// their EQ on, buses not feeding the mains: the console these tests were
/// written for (a factory WING has the faders down and the EQs off).
pub fn unity_console(e: &Engine) {
    for kind in [StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix] {
        for s in e.strips(kind) {
            s.params.fader_db.store(0.0);
            s.params.eq.on.store(true, Relaxed);
            if kind == StripKind::Bus {
                s.params.mains[0].on.store(false, Relaxed);
            }
        }
    }
    for d in &e.controls.dcas {
        d.fader_db.store(0.0);
    }
}
