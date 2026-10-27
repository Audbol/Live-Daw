//! Helpers shared by the engine tests.

use livedaw_engine::params::{StripKind, WING_BUSES};
use livedaw_engine::sends::send_mode;
use livedaw_engine::solo::solo_mode;
use livedaw_engine::strip::Strip;
use livedaw_engine::Engine;
use std::sync::atomic::Ordering::Relaxed;

/// The sends of Sidecar before it took the WING's factory sends: bus sends
/// (to the console's 16 buses) and matrix sends at 0 dB with PAN LINK on,
/// bus sends PRE (TAP), matrix sends post-fader; a matrix's direct input at
/// 0 dB; an input channel's TAP point at PFL.
pub fn legacy_sends(s: &Strip) {
    if s.kind() != StripKind::Matrix && s.kind() != StripKind::Main {
        for slot in 0..WING_BUSES {
            if let Some(send) = s.sends.get_or_create(slot) {
                send.level_db.store(0.0);
                send.pan_link.store(true, Relaxed);
                send.mode.store(send_mode::PRE, Relaxed);
            }
        }
    }
    for m in &s.params.matrix {
        m.level_db.store(0.0);
        m.pan_link.store(true, Relaxed);
        m.pre.store(false, Relaxed);
    }
    s.params.direct_level_db.store(0.0);
    s.params.tap.store(5, Relaxed); // PFL
}

/// Sets the number of channels; new channels get their fader at 0 dB (a new
/// channel's fader is down, as on the WING), so a patched input reaches the
/// mains, and the sends of [`legacy_sends`].
pub fn set_channels(e: &Engine, count: usize) {
    let before = e.num_channels();
    e.set_num_channels(count);
    for ch in e.strips(StripKind::Channel).iter().skip(before) {
        ch.params.fader_db.store(0.0);
        legacy_sends(ch);
    }
}

/// The fixed strips (aux, buses, mains, matrices) and the DCAs at 0 dB with
/// their EQ on, buses not feeding the mains, and the monitoring of Sidecar
/// before it took the WING's factory monitor settings (SOLO MODE STUDIO to
/// PH+SPK, mains and matrices AFL, no PFL DIM, BAND SOLO +12 dB, talkback
/// AUTO DIM 9 dB), the sends of [`legacy_sends`], AUX 2 not patched,
/// INPUT SELECT per strip and MUTEGROUP/SIP OVERRIDE off: the console these
/// tests were written for (a factory WING has the faders down and the EQs
/// off, solos LIVE to the phones).
pub fn unity_console(e: &Engine) {
    for kind in [StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix] {
        for s in e.strips(kind) {
            s.params.fader_db.store(0.0);
            s.params.eq.on.store(true, Relaxed);
            if kind == StripKind::Bus {
                s.params.mains[0].on.store(false, Relaxed);
            }
            if kind == StripKind::Aux {
                s.params.stereo.store(false, Relaxed);
                s.params.input_left.store(-1, Relaxed);
                s.params.input_right.store(-1, Relaxed);
            }
            legacy_sends(&s);
        }
    }
    for ch in e.strips(StripKind::Channel) {
        legacy_sends(&ch);
    }
    e.controls.mute_override.store(false, Relaxed);
    for d in &e.controls.dcas {
        d.fader_db.store(0.0);
    }
    let c = &e.controls;
    c.solo_mode.store(solo_mode::STUDIO, Relaxed);
    c.solo_bus.store(2, Relaxed);
    c.main_solo_afl.store(true, Relaxed);
    c.matrix_solo_afl.store(true, Relaxed);
    for src in [&c.speakers, &c.phones.source] {
        src.pfl_dim_db.store(0.0);
    }
    for path in &c.monitor.paths {
        path.band_solo_db.store(12.0);
    }
    for talk in &c.talk.paths {
        talk.mon_dim_db.store(9.0);
        talk.bus_dim_db.store(9.0);
    }
}
