//! RTA, LR vector scope, LISTEN (band / key / xover solo) and TAP WIDTH /
//! TAP EQ through the whole engine.

use livedaw_dsp::wing::{model_index, EQ_MODELS_CH};
mod common;
use common::{set_channels, unity_console};
use livedaw_engine::analyser::{listen_mode, rta_band_hz, rta_view, RTA_BANDS};
use livedaw_engine::params::StripKind;
use livedaw_engine::sends::send_mode;
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

const BLOCK: usize = 256;
const SR: f32 = 48000.0;

/// Drives an engine with per-input signal generators.
struct Rig {
    engine: Arc<Engine>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    generators: Vec<Box<dyn FnMut(usize) -> f32>>,
    pos: usize,
}

impl Rig {
    fn new(num_ins: usize, num_outs: usize) -> Self {
        let engine = Arc::new(Engine::new(0));
        unity_console(&engine);
        let in_map: Vec<i32> = (0..num_ins as i32).collect();
        let out_map: Vec<i32> = (0..num_outs as i32).collect();
        unsafe { engine.prepare(SR as f64, BLOCK, &in_map, &out_map) };
        // Signals above full scale are fine here (trims, shelves).
        engine.controls.output_protection.store(false, Relaxed);
        Self {
            engine,
            inputs: vec![vec![0.0; BLOCK]; num_ins],
            outputs: vec![vec![0.0; BLOCK]; num_outs],
            generators: (0..num_ins).map(|_| Box::new(|_| 0.0f32) as Box<dyn FnMut(usize) -> f32>).collect(),
            pos: 0,
        }
    }

    fn sine(&mut self, input: usize, freq: f32, amp: f32) {
        self.generators[input] = Box::new(move |i| amp * (2.0 * std::f32::consts::PI * freq * i as f32 / SR).sin());
    }

    fn constant(&mut self, input: usize, v: f32) {
        self.generators[input] = Box::new(move |_| v);
    }

    /// Runs `blocks` blocks; returns every sample of output `out` they produced.
    fn run(&mut self, blocks: usize, out: usize) -> Vec<f32> {
        let mut captured = Vec::with_capacity(blocks * BLOCK);
        for _ in 0..blocks {
            for (buf, g) in self.inputs.iter_mut().zip(&mut self.generators) {
                for (i, x) in buf.iter_mut().enumerate() {
                    *x = g(self.pos + i);
                }
            }
            self.pos += BLOCK;
            let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
            let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
            unsafe { self.engine.process(&ins, &outs, BLOCK) };
            captured.extend_from_slice(&self.outputs[out]);
        }
        captured
    }

    fn ch(&self, i: usize) -> Arc<Strip> {
        self.engine.channel_at(i).unwrap()
    }

    /// Patches the monitor to outputs 2/3.
    fn monitor_on_2_3(&self) {
        self.engine.controls.monitor_output_left.store(2, Relaxed);
        self.engine.controls.monitor_output_right.store(3, Relaxed);
    }

    fn listen(&self, s: &Strip, mode: i32, band: i32) {
        let c = self.engine.analysis().controls();
        c.listen_kind.store(s.kind() as i32, Relaxed);
        c.listen_id.store(s.id(), Relaxed);
        c.listen_band.store(band, Relaxed);
        c.listen_mode.store(mode, Relaxed);
    }
}

fn rms(x: &[f32]) -> f32 {
    (x.iter().map(|v| v * v).sum::<f32>() / x.len().max(1) as f32).sqrt()
}

fn db(x: f32) -> f32 {
    20.0 * x.max(1e-9).log10()
}

fn largest_step(x: &[f32]) -> f32 {
    x.windows(2).map(|w| (w[1] - w[0]).abs()).fold(0.0, f32::max)
}

#[test]
fn rta_reads_the_selected_strip_at_its_tap() {
    let mut rig = Rig::new(2, 2);
    set_channels(&rig.engine, 2);
    let f = rta_band_hz(57); // a band centre near 1 kHz
    rig.sine(0, f, 0.5);
    rig.sine(1, rta_band_hz(30), 0.5);
    let ch = rig.ch(0);
    let engine = rig.engine.clone();
    let a = engine.analysis();
    let c = a.controls();
    c.strip_kind.store(StripKind::Channel as i32, Relaxed);
    c.strip_id.store(ch.id(), Relaxed);
    c.view.store(rta_view::HOME, Relaxed);
    c.home.tap.store(0, Relaxed); // IN: before the trim
    c.home.detector.store(0, Relaxed);
    c.rta_on.store(true, Relaxed);
    ch.params.input.trim_db.store(-12.0);
    a.ensure_thread();
    std::thread::sleep(Duration::from_millis(50));

    let mut levels = [0.0; RTA_BANDS];
    let mut peaks = [0.0; RTA_BANDS];
    let read = |rig: &mut Rig, levels: &mut [f32; RTA_BANDS], peaks: &mut [f32; RTA_BANDS], want: f32| {
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            rig.run(40, 0);
            std::thread::sleep(Duration::from_millis(20));
            rig.engine.analysis().read_rta(levels, peaks);
            if (levels[57] - want).abs() < 1.5 || Instant::now() > deadline {
                return;
            }
        }
    };
    read(&mut rig, &mut levels, &mut peaks, -6.0);
    assert!((levels[57] + 6.0).abs() < 1.5, "IN: {}", levels[57]);
    // Channel 2's tone would read about -6 dB here; what remains is the
    // window leakage of channel 1's own tone (about -58 dB on some platforms).
    assert!(levels[30] < -45.0, "channel 2's tone isn't on channel 1: {}", levels[30]);
    assert!(peaks[57] >= levels[57] - 0.01);

    // POST EQ (after the trim): 12 dB down.
    c.home.tap.store(1, Relaxed);
    read(&mut rig, &mut levels, &mut peaks, -18.0);
    assert!((levels[57] + 18.0).abs() < 1.5, "EQ: {}", levels[57]);

    // The EQ view, PRE the EQ, with a -9 dB WING EQ band on the tone: PRE sees
    // the tone at -18 dB, POST at -27 dB.
    let spec = &EQ_MODELS_CH[model_index(EQ_MODELS_CH, "STD").unwrap()];
    let eq = &ch.params.eq;
    eq.on.store(true, Relaxed); // a new channel's EQ is off, as on the WING
    eq.set_model(model_index(EQ_MODELS_CH, "STD").unwrap(), false);
    eq.values[spec.param_index("2f").unwrap()].store(f);
    eq.values[spec.param_index("2g").unwrap()].store(-9.0);
    c.view.store(rta_view::EQ, Relaxed);
    c.eq.detector.store(0, Relaxed);
    c.eq.tap.store(0, Relaxed);
    read(&mut rig, &mut levels, &mut peaks, -18.0);
    assert!((levels[57] + 18.0).abs() < 1.5, "PRE EQ: {}", levels[57]);
    c.eq.tap.store(1, Relaxed);
    read(&mut rig, &mut levels, &mut peaks, -27.0);
    assert!((levels[57] + 27.0).abs() < 1.5, "POST EQ: {}", levels[57]);

    // Another strip: channel 2's tone.
    c.strip_id.store(rig.ch(1).id(), Relaxed);
    c.view.store(rta_view::HOME, Relaxed);
    c.home.tap.store(0, Relaxed);
    let deadline = Instant::now() + Duration::from_secs(10);
    while levels[30] < -7.5 && Instant::now() < deadline {
        rig.run(40, 0);
        std::thread::sleep(Duration::from_millis(20));
        a.read_rta(&mut levels, &mut peaks);
    }
    assert!((levels[30] + 6.0).abs() < 1.5, "channel 2: {}", levels[30]);
    a.shutdown();
}

#[test]
fn scope_takes_left_and_right_after_the_input_block() {
    let mut rig = Rig::new(2, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.stereo.store(true, Relaxed);
    ch.params.input_right.store(1, Relaxed);
    ch.params.input.trim_db.store(-6.0206);
    ch.params.pan.store(-100.0); // pan doesn't matter: it is after the scope
    rig.constant(0, 0.8);
    rig.constant(1, -0.4);
    let engine = rig.engine.clone();
    let c = engine.analysis().controls();
    c.strip_kind.store(StripKind::Channel as i32, Relaxed);
    c.strip_id.store(ch.id(), Relaxed);
    c.scope_on.store(true, Relaxed);
    rig.run(8, 0);

    let a = engine.analysis();
    assert!((a.scope_rate() - 24000.0).abs() < 1.0);
    let mut pairs = vec![0.0f32; 2 * 1024];
    let n = a.read_scope(&mut pairs);
    assert_eq!(n, 1024, "8 blocks of 256 at half rate");
    for p in pairs.chunks(2).skip(200) {
        assert!((p[0] - 0.4).abs() < 1e-4 && (p[1] + 0.2).abs() < 1e-4, "{p:?}");
    }
    // Off: nothing new arrives.
    c.scope_on.store(false, Relaxed);
    let before = a.read_scope(&mut pairs);
    rig.run(4, 0);
    assert_eq!(a.read_scope(&mut pairs), before);
}

#[test]
fn band_solo_plays_the_selected_band_on_the_monitor() {
    let mut rig = Rig::new(1, 4);
    set_channels(&rig.engine, 1);
    rig.monitor_on_2_3();
    let ch = rig.ch(0);
    let table = &EQ_MODELS_CH;
    let std = model_index(table, "STD").unwrap();
    ch.params.eq.set_model(std, false);
    let spec = &table[std];
    // Band "2" (index 2: l, 1, 2, ...) at 2 kHz, Q 4.
    ch.params.eq.values[spec.param_index("2f").unwrap()].store(2000.0);
    ch.params.eq.values[spec.param_index("2q").unwrap()].store(4.0);
    rig.engine.controls.monitor.paths[0].band_solo_db.store(0.0);

    // Without LISTEN the monitor follows Main 1.
    rig.sine(0, 2000.0, 0.5);
    let main = rms(&rig.run(20, 2)[BLOCK * 10..]);
    // The sine's RMS; the centred channel is at unity on each side.
    assert!((main - 0.5 * std::f32::consts::FRAC_1_SQRT_2).abs() < 0.01, "{main}");

    rig.listen(&ch, listen_mode::BAND, 2);
    let at_band = rms(&rig.run(40, 2)[BLOCK * 20..]);
    assert!((db(at_band) - db(0.5 * std::f32::consts::FRAC_1_SQRT_2)).abs() < 0.5, "{}", db(at_band));
    assert!(rig.engine.controls.any_solo_active.load(Relaxed));

    // Far from the band: well down.
    rig.sine(0, 200.0, 0.5);
    let off_band = rms(&rig.run(40, 2)[BLOCK * 20..]);
    assert!(db(off_band) < db(at_band) - 20.0, "{} vs {}", db(off_band), db(at_band));

    // The listening path's BAND SOLO lifts it (default 12 dB; from a quieter
    // tone, so the path's LIMITER at 0 dB stays out of the way).
    rig.sine(0, 2000.0, 0.125);
    rig.engine.controls.monitor.paths[0].band_solo_db.store(12.0);
    let trimmed = rms(&rig.run(40, 2)[BLOCK * 20..]);
    assert!((db(trimmed) - (db(at_band) - 12.04) - 12.0).abs() < 0.5, "{} vs {}", db(trimmed), db(at_band));
    // The PHONES' BAND SOLO doesn't apply: the band plays on the SPEAKERS.
    rig.engine.controls.monitor.paths[1].band_solo_db.store(0.0);
    let same = rms(&rig.run(20, 2)[BLOCK * 10..]);
    assert!((db(same) - db(trimmed)).abs() < 0.1);

    // Off again: back to Main 1, without a click.
    rig.engine.analysis().controls().listen_mode.store(listen_mode::OFF, Relaxed);
    rig.run(4, 2);
    rig.sine(0, 2000.0, 0.5);
    let back = rig.run(20, 2);
    assert!(!rig.engine.controls.any_solo_active.load(Relaxed));
    assert!((rms(&back[BLOCK * 10..]) - main).abs() < 0.01);
}

#[test]
fn listen_replaces_the_solo_bus_and_switches_without_clicks() {
    let mut rig = Rig::new(2, 4);
    set_channels(&rig.engine, 2);
    rig.monitor_on_2_3();
    let (a, b) = (rig.ch(0), rig.ch(1));
    rig.sine(0, 1000.0, 0.5);
    rig.sine(1, 300.0, 0.5);
    let engine = rig.engine.clone();
    let ctl = engine.analysis().controls();
    engine.controls.monitor.paths[0].band_solo_db.store(0.0); // BAND SOLO of the SPEAKERS (the listening path)
                                                              // Channel 2 soloed (PFL): the monitor plays it.
    b.params.solo.store(true, Relaxed);
    rig.run(10, 2);
    // XOVER SOLO on channel 1 with a low pass far below its tone: silence,
    // not channel 2.
    a.params.dynamics.xo_kind.store(1, Relaxed); // LO
    a.params.dynamics.xo_freq.store(100.0);
    rig.listen(&a, listen_mode::XOVER, 0);
    let mut out = rig.run(30, 2);
    assert!(rms(&out[BLOCK * 10..]) < 0.01, "{}", rms(&out[BLOCK * 10..]));
    // A high pass instead: channel 1's tone.
    a.params.dynamics.xo_kind.store(2, Relaxed); // HI
    out.extend(rig.run(30, 2));
    let tail = &out[out.len() - BLOCK * 10..];
    assert!((rms(tail) - 0.5 * std::f32::consts::FRAC_1_SQRT_2).abs() < 0.02, "{}", rms(tail));
    // Switch to KEY SOLO of channel 2's gate (self-keyed, no key filter):
    // channel 2's input, faded across.
    rig.listen(&b, listen_mode::GATE_KEY, 0);
    out.extend(rig.run(30, 2));
    let tail = &out[out.len() - BLOCK * 10..];
    assert!((rms(tail) - 0.5 * std::f32::consts::FRAC_1_SQRT_2).abs() < 0.02);
    // Off: the soloed channel 2 again.
    ctl.listen_mode.store(listen_mode::OFF, Relaxed);
    out.extend(rig.run(30, 2));
    // Sines at 1 kHz / 300 Hz step at most 2 pi f A / sr per sample; a click would be far more.
    let max_step = 2.0 * std::f32::consts::PI * 1000.0 * 0.5 / SR * 1.5;
    let worst = out.windows(2).enumerate().max_by(|a, b| (a.1[1] - a.1[0]).abs().total_cmp(&(b.1[1] - b.1[0]).abs())).unwrap().0;
    assert!(largest_step(&out) < max_step, "step {} > {max_step} at {} (block {})", largest_step(&out), worst, worst / BLOCK);
}

#[test]
fn key_solo_plays_the_filtered_key_even_from_another_channel() {
    let mut rig = Rig::new(2, 4);
    set_channels(&rig.engine, 2);
    rig.monitor_on_2_3();
    let ch = rig.ch(0);
    rig.sine(0, 100.0, 0.5);
    rig.sine(1, 5000.0, 0.5);
    let gate = &ch.params.gate;
    gate.sc_filter.store(2, Relaxed); // HP12
    gate.sc_freq.store(1000.0);
    rig.listen(&ch, listen_mode::GATE_KEY, 0);
    // Self-keyed: the 100 Hz tone through a 1 kHz high pass.
    let own = rms(&rig.run(40, 2)[BLOCK * 20..]);
    assert!(own < 0.01, "{own}");
    // Keyed from channel 2 (5 kHz): it passes (mono, on both sides).
    gate.sc_source.store(1, Relaxed);
    let keyed = rig.run(40, 2);
    let right = rig.run(1, 3);
    assert!((rms(&keyed[BLOCK * 20..]) - 0.5 * std::f32::consts::FRAC_1_SQRT_2).abs() < 0.02);
    assert!(rms(&right) > 0.3);
    // The DYNAMICS slot's key, with its own (empty) filter: channel 1 itself.
    rig.listen(&ch, listen_mode::DYN_KEY, 0);
    let dyn_key = rms(&rig.run(40, 2)[BLOCK * 20..]);
    assert!((dyn_key - 0.5 * std::f32::consts::FRAC_1_SQRT_2).abs() < 0.02, "{dyn_key}");
    // A strip without a gate slot (a bus) has no gate key: silence.
    let bus = rig.engine.strip_at(StripKind::Bus, 0).unwrap();
    rig.listen(&bus, listen_mode::GATE_KEY, 0);
    assert!(rms(&rig.run(20, 2)[BLOCK * 10..]) < 1e-6);
}

#[test]
fn listening_to_a_removed_strip_stops_safely() {
    let mut rig = Rig::new(2, 4);
    set_channels(&rig.engine, 2);
    rig.monitor_on_2_3();
    let ch = rig.ch(1);
    rig.sine(1, 1000.0, 0.5);
    rig.listen(&ch, listen_mode::DYN_KEY, 0);
    assert!(rms(&rig.run(10, 2)[BLOCK * 5..]) > 0.1);
    assert!(rig.engine.remove_strip(Arc::as_ptr(&ch)));
    drop(ch);
    rig.run(10, 2);
    rig.engine.collect();
    rig.engine.garbage().collect_all_now();
    assert!(rms(&rig.run(10, 2)) < 1e-6);
}

#[test]
fn tap_width_and_tap_eq_shape_tap_sends_only() {
    let mut rig = Rig::new(2, 8);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    let p = &ch.params;
    p.stereo.store(true, Relaxed);
    p.input_right.store(1, Relaxed);
    p.mains[0].on.store(false, Relaxed);
    rig.constant(0, 0.6);
    rig.constant(1, 0.0);
    // Bus 1 (TAP send) on outputs 4/5, bus 2 (POST send) on 6/7.
    for (slot, mode, out) in [(0, send_mode::PRE, 4), (1, send_mode::POST, 6)] {
        let bus = rig.engine.bus_in_slot(slot).unwrap();
        bus.params.output_left.store(out, Relaxed);
        bus.params.output_right.store(out + 1, Relaxed);
        let send = ch.sends.get_or_create(slot as usize).unwrap();
        send.enabled.store(true, Relaxed);
        send.mode.store(mode, Relaxed);
    }
    rig.run(10, 4);
    let last = |rig: &mut Rig, out: usize| *rig.run(10, out).last().unwrap();
    assert!((last(&mut rig, 4) - 0.6).abs() < 1e-4 && last(&mut rig, 5).abs() < 1e-4);

    // TAP WIDTH 0 %: the TAP send is mono; the POST send keeps its width.
    p.tap_width.store(0.0);
    assert!((last(&mut rig, 4) - 0.3).abs() < 1e-4 && (last(&mut rig, 5) - 0.3).abs() < 1e-4);
    assert!((last(&mut rig, 6) - 0.6).abs() < 1e-4 && last(&mut rig, 7).abs() < 1e-4);
    // -100 %: swapped.
    p.tap_width.store(-100.0);
    assert!(last(&mut rig, 4).abs() < 1e-4 && (last(&mut rig, 5) - 0.6).abs() < 1e-4);
    p.tap_width.store(100.0);

    // TAP EQ: a +6 dB low shelf lifts DC on the TAP send only.
    p.tap_eq.on.store(true, Relaxed);
    p.tap_eq.bands[0].gain_db.store(6.0);
    rig.run(40, 4);
    assert!((last(&mut rig, 4) - 0.6 * 1.9953).abs() < 0.01, "{}", last(&mut rig, 4));
    assert!((last(&mut rig, 6) - 0.6).abs() < 1e-4);
    // Off glides back.
    p.tap_eq.on.store(false, Relaxed);
    rig.run(40, 4);
    assert!((last(&mut rig, 4) - 0.6).abs() < 1e-3);
}

#[test]
fn reset_restores_the_analysis_defaults() {
    let e = Engine::new(0);
    let c = e.analysis().controls();
    c.home.range.store(1, Relaxed);
    c.listen_mode.store(listen_mode::BAND, Relaxed);
    c.eq.gain_db.store(20.0);
    e.reset();
    assert_eq!(c.home.range.load(Relaxed), 0, "30 dB, as a WING after INITIALIZE");
    assert_eq!(c.listen_mode.load(Relaxed), listen_mode::OFF);
    assert_eq!(c.eq.gain_db.load(), 0.0);
}

#[test]
fn rta_reads_a_monitor_path_either_side_of_its_eq() {
    use livedaw_engine::analyser::ANALYSE_MONITOR;
    let mut rig = Rig::new(1, 2);
    set_channels(&rig.engine, 1);
    let f = rta_band_hz(57);
    rig.sine(0, f, 0.5); // on Main 1, the source of both monitor paths
    let engine = rig.engine.clone();
    let a = engine.analysis();
    let c = a.controls();
    // PHONES (path 1), with a -9 dB band on the tone.
    let phones = &engine.controls.monitor.paths[1];
    phones.eq[3].freq.store(f);
    phones.eq[3].gain_db.store(-9.0);
    phones.eq_on.store(true, Relaxed);
    c.strip_kind.store(ANALYSE_MONITOR, Relaxed);
    c.strip_id.store(1, Relaxed);
    c.view.store(rta_view::MONITOR, Relaxed);
    c.eq.detector.store(0, Relaxed);
    c.eq.tap.store(0, Relaxed); // PRE
    c.rta_on.store(true, Relaxed);
    a.ensure_thread();
    std::thread::sleep(Duration::from_millis(50));

    let mut levels = [0.0; RTA_BANDS];
    let mut peaks = [0.0; RTA_BANDS];
    // Waits until the band holds still (and above the floor) near `expect`:
    // right after a switch the band can still hold the previous reading
    // steady, which isn't the new one yet.
    let settle = |rig: &mut Rig, levels: &mut [f32; RTA_BANDS], peaks: &mut [f32; RTA_BANDS], expect: Option<f32>| {
        let deadline = Instant::now() + Duration::from_secs(10);
        let mut last = f32::MIN;
        loop {
            rig.run(40, 0);
            std::thread::sleep(Duration::from_millis(20));
            rig.engine.analysis().read_rta(levels, peaks);
            let level = levels[57];
            let near = expect.is_none_or(|e| (level - e).abs() < 1.5);
            if (level > -60.0 && near && (level - last).abs() < 0.05) || Instant::now() > deadline {
                return level;
            }
            last = level;
        }
    };
    let pre = settle(&mut rig, &mut levels, &mut peaks, None);
    assert!(pre > -20.0, "the main's tone reaches the path: {pre}");
    c.eq.tap.store(1, Relaxed); // POST
    let post = settle(&mut rig, &mut levels, &mut peaks, Some(pre - 9.0));
    assert!((pre - post - 9.0).abs() < 1.5, "PRE {pre} dB, POST {post} dB");
    // SPEAKERS (path 0) have no EQ: POST reads the PRE level.
    c.strip_id.store(0, Relaxed);
    let speakers = settle(&mut rig, &mut levels, &mut peaks, Some(pre));
    assert!((speakers - pre).abs() < 1.5, "SPEAKERS {speakers} dB, PHONES PRE {pre} dB");
    a.shutdown();
}
