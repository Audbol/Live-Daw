//! Monitor path processing (DIRECT IN, EQ, PAN / WIDTH, INVERT, DELAY,
//! LIMITER on PHONES and SPEAKERS), SOURCE SOLO with the solo modes, and
//! source tags (mute groups).

use livedaw_engine::monitor::{delay_ms, direct_index, MonitorDspParams};
use livedaw_engine::params::StripKind;
use livedaw_engine::solo::{mute_state, solo_mode};
use livedaw_engine::sources::source_mode;
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 64;
const SR: f64 = 48000.0;
/// Hardware outputs of the paths: SPEAKERS 2/3, PHONES 4/5.
const SPK: usize = 2;
const PH: usize = 4;

struct Rig {
    engine: Arc<Engine>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    /// Per input: a sine (frequency, amplitude) or DC (frequency 0).
    signals: Vec<(f32, f32)>,
    t: u64,
}

impl Rig {
    fn new(num_ins: usize) -> Self {
        let engine = Arc::new(Engine::new(0));
        let in_map: Vec<i32> = (0..num_ins as i32).collect();
        let out_map: Vec<i32> = (0..6).collect();
        unsafe { engine.prepare(SR, BLOCK, &in_map, &out_map) };
        let c = &engine.controls;
        c.monitor_output_left.store(SPK as i32, Relaxed);
        c.monitor_output_right.store(SPK as i32 + 1, Relaxed);
        c.phones.output_left.store(PH as i32, Relaxed);
        c.phones.output_right.store(PH as i32 + 1, Relaxed);
        c.output_protection.store(false, Relaxed);
        Self {
            engine,
            inputs: vec![vec![0.0; BLOCK]; num_ins],
            outputs: vec![vec![0.0; BLOCK]; 6],
            signals: vec![(0.0, 0.0); num_ins],
            t: 0,
        }
    }

    fn dc(&mut self, ch: usize, v: f32) {
        self.signals[ch] = (0.0, v);
    }

    fn sine(&mut self, ch: usize, freq: f32, amp: f32) {
        self.signals[ch] = (freq, amp);
    }

    /// Runs `blocks` blocks; returns output `out` of every block.
    fn run(&mut self, blocks: usize, out: usize) -> Vec<f32> {
        let mut captured = Vec::with_capacity(blocks * BLOCK);
        for _ in 0..blocks {
            for (buf, &(f, a)) in self.inputs.iter_mut().zip(&self.signals) {
                for (i, x) in buf.iter_mut().enumerate() {
                    let t = (self.t + i as u64) as f64 / SR;
                    *x = if f == 0.0 { a } else { a * (2.0 * std::f64::consts::PI * f as f64 * t).sin() as f32 };
                }
            }
            self.t += BLOCK as u64;
            let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
            let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
            unsafe { self.engine.process(&ins, &outs, BLOCK) };
            captured.extend_from_slice(&self.outputs[out]);
        }
        captured
    }

    fn last(&self, out: usize) -> f32 {
        *self.outputs[out].last().unwrap()
    }

    fn ch(&self, i: usize) -> Arc<Strip> {
        self.engine.channel_at(i).unwrap()
    }

    fn strip(&self, kind: StripKind, i: usize) -> Arc<Strip> {
        self.engine.strip_at(kind, i).unwrap()
    }

    fn speakers(&self) -> &MonitorDspParams {
        &self.engine.controls.monitor.paths[0]
    }

    fn phones(&self) -> &MonitorDspParams {
        &self.engine.controls.monitor.paths[1]
    }
}

fn close(a: f32, b: f32, tol: f32) -> bool {
    (a - b).abs() <= tol
}

fn rms(x: &[f32]) -> f32 {
    (x.iter().map(|v| v * v).sum::<f32>() / x.len().max(1) as f32).sqrt()
}

fn db(x: f32) -> f32 {
    20.0 * x.max(1e-9).log10()
}

/// The largest step between neighbouring samples (a click detector).
fn max_step(x: &[f32]) -> f32 {
    x.windows(2).map(|w| (w[1] - w[0]).abs()).fold(0.0, f32::max)
}

/// Channel 1 (hard left) on Main 1 at `main`, channel 2 off the mains.
fn two_channels(rig: &mut Rig, main: f32, other: f32) {
    rig.engine.set_num_channels(2);
    for i in 0..2 {
        rig.ch(i).params.pan.store(-100.0);
    }
    rig.ch(1).params.mains[0].on.store(false, Relaxed);
    rig.dc(0, main);
    rig.dc(1, other);
}

#[test]
fn direct_in_mixes_any_strip_into_one_path() {
    let mut rig = Rig::new(2);
    two_channels(&mut rig, 0.25, 0.5);
    rig.run(4, PH);
    assert!(close(rig.last(SPK), 0.25, 1e-5) && close(rig.last(PH), 0.25, 1e-5));

    // PHONES: DIRECT IN CH.2 at -6 dB; the SPEAKERS don't get it.
    rig.phones().direct_source.store(direct_index(StripKind::Channel, 1), Relaxed);
    rig.phones().direct_level_db.store(-6.0206);
    let ramp = rig.run(4, PH);
    assert!(close(rig.last(PH), 0.5, 1e-4), "{}", rig.last(PH));
    assert!(close(rig.last(SPK), 0.25, 1e-5));
    assert!(max_step(&ramp) < 0.01, "faded in");

    // A bus as the SPEAKERS' direct input: channel 2 feeds bus 3 only.
    let ch2 = rig.ch(1);
    let send = ch2.sends.get_or_create(2).unwrap();
    send.enabled.store(true, Relaxed);
    rig.speakers().direct_source.store(direct_index(StripKind::Bus, 2), Relaxed);
    rig.run(4, SPK);
    assert!(close(rig.last(SPK), 0.75, 1e-4), "{}", rig.last(SPK));
    // Changing the source crossfades; OFF removes it.
    rig.speakers().direct_source.store(-1, Relaxed);
    let fade = rig.run(4, SPK);
    assert!(max_step(&fade) < 0.02);
    assert!(close(rig.last(SPK), 0.25, 1e-5));

    // DIRECT IN plays while a solo replaces the source.
    rig.engine.controls.solo_bus.store(0, Relaxed); // PHONES
    rig.ch(0).params.solo.store(true, Relaxed);
    rig.run(4, PH);
    assert!(close(rig.last(PH), 0.25 + 0.25, 1e-4), "PFL of channel 1 + direct in: {}", rig.last(PH));
}

#[test]
fn eq_shapes_only_its_path() {
    let mut rig = Rig::new(1);
    rig.engine.set_num_channels(1);
    rig.sine(0, 1000.0, 0.25);
    let flat = rms(&rig.run(40, PH)[BLOCK * 20..]);

    let engine = rig.engine.clone();
    let p = &engine.controls.monitor.paths[1];
    p.eq[3].freq.store(1000.0);
    p.eq[3].gain_db.store(6.0);
    rig.run(4, PH);
    assert!(close(rms(&rig.run(20, PH)[BLOCK * 10..]), flat, 1e-3), "EQ off: flat");
    p.eq_on.store(true, Relaxed);
    let on = rig.run(40, PH);
    assert!((db(rms(&on[BLOCK * 20..])) - db(flat) - 6.0).abs() < 0.3, "{}", db(rms(&on[BLOCK * 20..])) - db(flat));
    let spk = rms(&rig.run(20, SPK)[BLOCK * 10..]);
    assert!(close(spk, flat, 1e-3), "the SPEAKERS stay flat");
    // Far from the band nothing changes; the high shelf lifts the top.
    rig.sine(0, 40.0, 0.25);
    rig.run(20, PH);
    let low = rms(&rig.run(20, PH)[BLOCK * 10..]);
    assert!((db(low) - db(flat)).abs() < 0.3, "{}", db(low) - db(flat));
    p.eq[7].gain_db.store(-12.0);
    rig.sine(0, 16000.0, 0.25);
    rig.run(40, PH);
    let high = rms(&rig.run(20, PH)[BLOCK * 10..]);
    assert!(db(high) - db(flat) < -9.0, "{}", db(high) - db(flat));
    // Off again glides back to flat without clicks.
    p.eq_on.store(false, Relaxed);
    let back = rig.run(40, PH);
    assert!(max_step(&back) < 0.5);
    assert!(close(rms(&back[BLOCK * 30..]), flat, 2e-3));
}

#[test]
fn invert_pan_and_width_on_each_path() {
    let mut rig = Rig::new(2);
    rig.engine.set_num_channels(2);
    // Channel 1 hard left, channel 2 hard right.
    rig.ch(0).params.pan.store(-100.0);
    rig.ch(1).params.pan.store(100.0);
    rig.dc(0, 0.4);
    rig.dc(1, 0.2);
    rig.run(4, PH);
    assert!(close(rig.last(SPK), 0.4, 1e-5) && close(rig.last(SPK + 1), 0.2, 1e-5));

    rig.speakers().invert.store(true, Relaxed);
    let flip = rig.run(4, SPK);
    assert!(close(rig.last(SPK), -0.4, 1e-5) && close(rig.last(SPK + 1), -0.2, 1e-5));
    assert!(close(rig.last(PH), 0.4, 1e-5), "PHONES unchanged");
    assert!(max_step(&flip) < 0.05, "ramped through zero");

    // WIDTH 0: mono on the PHONES; -100: the sides swapped.
    rig.phones().width.store(0.0);
    rig.run(4, PH);
    assert!(close(rig.last(PH), 0.3, 1e-5) && close(rig.last(PH + 1), 0.3, 1e-5));
    rig.phones().width.store(-100.0);
    rig.run(4, PH);
    assert!(close(rig.last(PH), 0.2, 1e-5) && close(rig.last(PH + 1), 0.4, 1e-5));
    // PAN hard left silences the right side.
    rig.phones().width.store(100.0);
    rig.phones().pan.store(-100.0);
    rig.run(4, PH);
    assert!(close(rig.last(PH), 0.4, 1e-5) && close(rig.last(PH + 1), 0.0, 1e-6));
}

#[test]
fn delay_by_distance() {
    let mut rig = Rig::new(1);
    rig.engine.set_num_channels(1);
    rig.run(2, SPK);
    // 3.43 m = 10 ms = 480 samples at 48 kHz.
    assert!((delay_ms(3.43) - 10.0).abs() < 1e-3);
    rig.speakers().delay_m.store(3.43);
    rig.speakers().delay_on.store(true, Relaxed);
    rig.run(20, SPK);
    // A step on the input: the SPEAKERS follow 480 samples later, the PHONES at once.
    rig.dc(0, 0.5);
    let mut spk = Vec::new();
    let mut ph = Vec::new();
    for _ in 0..12 {
        rig.run(1, SPK);
        spk.extend_from_slice(&rig.outputs[SPK]);
        ph.extend_from_slice(&rig.outputs[PH]);
    }
    let first = |x: &[f32]| x.iter().position(|v| v.abs() > 0.1).unwrap();
    assert_eq!(first(&spk) - first(&ph), 480);
    // DELAY off: back to no delay, crossfaded.
    rig.speakers().delay_on.store(false, Relaxed);
    rig.run(2, SPK);
    assert!(close(rig.last(SPK), rig.last(PH), 1e-6));
}

#[test]
fn limiter_holds_its_threshold_and_meters_gain_reduction() {
    let mut rig = Rig::new(1);
    rig.engine.set_num_channels(1);
    rig.ch(0).params.pan.store(-100.0);
    rig.sine(0, 200.0, 0.9);
    rig.run(10, SPK);
    let engine = rig.engine.clone();
    let gr = &engine.controls.monitor.paths[0].limiter_gr_db;
    gr.exchange(0.0);
    rig.speakers().limiter_db.store(-12.0);
    let out = rig.run(40, SPK);
    let ceiling = 10f32.powf(-12.0 / 20.0);
    assert!(out.iter().all(|x| x.abs() <= ceiling + 1e-5));
    assert!(gr.exchange(0.0) > 6.0);
    // The PHONES aren't limited.
    let ph = rig.run(10, PH);
    assert!(ph.iter().any(|x| x.abs() > 0.8));
    // Below the threshold: unity, no reduction metered.
    rig.sine(0, 200.0, 0.1);
    rig.run(600, SPK);
    gr.exchange(0.0);
    let quiet = rig.run(10, SPK);
    assert!(quiet.iter().any(|x| x.abs() > 0.099));
    assert_eq!(gr.load(), 0.0);
}

#[test]
fn source_solo_patches_and_solos_its_host() {
    let mut rig = Rig::new(4);
    rig.engine.set_num_channels(40);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    let m = &c.monitor;
    for i in 0..40 {
        rig.ch(i).params.pan.store(-100.0);
        rig.ch(i).params.input_left.store(-1, Relaxed);
    }
    // Channel 1 on input 1 in the mix; CH39 patched to input 4 (silent), off the mains.
    rig.ch(0).params.input_left.store(0, Relaxed);
    rig.ch(38).params.input_left.store(3, Relaxed);
    rig.ch(38).params.mains[0].on.store(false, Relaxed);
    rig.dc(0, 0.1);
    rig.dc(2, 0.3);
    c.solo_bus.store(1, Relaxed); // SPEAKERS
    rig.run(4, SPK);
    assert!(close(rig.last(SPK), 0.1, 1e-5));

    // SOURCE SOLO OFF: soloing a source does nothing.
    m.source_solo_port.store(2, Relaxed);
    m.source_solo_on.store(true, Relaxed);
    rig.run(4, SPK);
    assert!(close(rig.last(SPK), 0.1, 1e-5));
    assert!(!c.any_solo_active.load(Relaxed));

    // CH39 hosts it: STUDIO plays input 3 on the selected path.
    m.source_solo.store(1, Relaxed);
    rig.run(4, SPK);
    assert!(c.any_solo_active.load(Relaxed));
    assert!(rig.ch(38).console.is_soloed());
    assert!(close(rig.last(SPK), 0.3, 1e-5), "{}", rig.last(SPK));
    assert!(close(rig.last(PH), 0.1, 1e-5), "the PHONES keep the source");
    assert_eq!(rig.ch(38).params.input_left.load(Relaxed), 3, "its own patch stays");

    // LIVE: the host's SOLO BUS decides (PHONES).
    c.solo_mode.store(solo_mode::LIVE, Relaxed);
    rig.ch(38).params.monitor_bus.store(0, Relaxed);
    rig.run(4, PH);
    assert!(close(rig.last(PH), 0.3, 1e-5) && close(rig.last(SPK), 0.1, 1e-5));

    // SIP: the other inputs are muted while the source plays in place.
    c.solo_mode.store(solo_mode::SIP, Relaxed);
    rig.ch(38).params.mains[0].on.store(true, Relaxed);
    rig.run(4, 0);
    assert!(c.sip_active.load(Relaxed));
    assert_eq!(rig.ch(0).console.mute_state(), mute_state::GROUP);
    assert!(close(rig.last(0), 0.3, 1e-5), "main 1 hears the source: {}", rig.last(0));

    // A stereo pair plays both ports; AUX7 can host instead.
    c.solo_mode.store(solo_mode::STUDIO, Relaxed);
    rig.ch(38).params.mains[0].on.store(false, Relaxed);
    engine.sources().set_mode(2, source_mode::STEREO);
    rig.dc(3, 0.2);
    m.source_solo.store(2, Relaxed);
    let aux7 = rig.strip(StripKind::Aux, 6);
    aux7.params.pan.store(0.0);
    rig.run(4, SPK);
    assert!(aux7.console.is_soloed() && !rig.ch(38).console.is_soloed());
    assert!(close(rig.last(SPK), 0.3, 1e-5) && close(rig.last(SPK + 1), 0.2, 1e-5));

    // Released: the source solo ends, the mix is back.
    m.source_solo_on.store(false, Relaxed);
    rig.run(4, SPK);
    assert!(!c.any_solo_active.load(Relaxed));
    assert!(close(rig.last(SPK), 0.1, 1e-5));
}

#[test]
fn sources_follow_their_mute_groups() {
    let mut rig = Rig::new(2);
    rig.engine.set_num_channels(1);
    rig.ch(0).params.pan.store(-100.0);
    rig.dc(0, 0.5);
    let engine = rig.engine.clone();
    let src = engine.sources().get(0).unwrap();
    src.mute_groups.store(0b100, Relaxed); // MGRP 3
    rig.run(4, 0);
    assert!(close(rig.last(0), 0.5, 1e-5));
    assert!(!src.is_muted(0) && src.is_muted(0b100));
    rig.engine.controls.mute_groups.store(0b100, Relaxed);
    let fade = rig.run(4, 0);
    assert!(close(rig.last(0), 0.0, 1e-6));
    assert!(max_step(&fade) < 0.05, "ramped");
    rig.engine.controls.mute_groups.store(0b010, Relaxed);
    rig.run(4, 0);
    assert!(close(rig.last(0), 0.5, 1e-5));
    assert!(!src.is_default());
    engine.sources().initialize(0);
    assert_eq!(src.mute_groups.load(Relaxed), 0);
}

#[test]
fn reset_restores_the_monitor_defaults() {
    let rig = Rig::new(1);
    let m = &rig.engine.controls.monitor;
    m.paths[1].eq_on.store(true, Relaxed);
    m.paths[1].eq[2].gain_db.store(4.0);
    m.paths[0].limiter_db.store(-20.0);
    m.source_solo.store(1, Relaxed);
    rig.engine.reset();
    assert!(!m.paths[1].eq_on.load(Relaxed));
    assert_eq!(m.paths[1].eq[2].gain_db.load(), 0.0);
    assert_eq!(m.paths[0].limiter_db.load(), 0.0);
    assert_eq!(m.source_solo.load(Relaxed), 0);
}
