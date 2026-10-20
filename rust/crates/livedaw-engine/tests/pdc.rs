//! Plugin delay compensation: parallel paths with plugins of different
//! latencies arrive at their bus / main in time.

mod common;
use common::{set_channels, unity_console};
use livedaw_engine::params::StripKind;
use livedaw_engine::{pdc_tap, Engine, InsertCallbacks, MAX_PDC};
use std::ffi::c_void;
use std::sync::atomic::Ordering::Relaxed;

const BLOCK: usize = 64;
const CENTRE: f32 = std::f32::consts::FRAC_1_SQRT_2;

/// A plugin that delays its input by `latency` samples (as a lookahead
/// limiter or a linear-phase EQ would) and reports it.
struct DelayPlugin {
    latency: usize,
    buf: [Vec<f32>; 2],
    pos: usize,
}

unsafe extern "C" fn delay_process(ctx: *mut c_void, ch: *const *mut f32, nc: i32, n: i32) -> i32 {
    let p = &mut *(ctx as *mut DelayPlugin);
    let len = p.buf[0].len();
    for c in 0..nc as usize {
        let data = std::slice::from_raw_parts_mut(*ch.add(c), n as usize);
        let mut w = p.pos;
        for x in data.iter_mut() {
            p.buf[c][w] = *x;
            *x = p.buf[c][(w + len - p.latency) % len];
            w = (w + 1) % len;
        }
    }
    p.pos = (p.pos + n as usize) % len;
    0
}

unsafe extern "C" fn delay_release(ctx: *mut c_void) {
    drop(Box::from_raw(ctx as *mut DelayPlugin));
}

fn delay_plugin(latency: usize) -> InsertCallbacks {
    let len = latency + 1;
    let ctx = Box::into_raw(Box::new(DelayPlugin { latency, buf: [vec![0.0; len], vec![0.0; len]], pos: 0 }));
    InsertCallbacks { process: delay_process, release: Some(delay_release), ctx: ctx as *mut c_void }
}

/// Loads a delay plugin into FX slot `slot` and reports its latency.
fn load(engine: &Engine, slot: usize, latency: usize) {
    engine.set_fx_plugin(slot, Some(delay_plugin(latency)));
    engine.fx(slot).unwrap().set_latency(latency as u32);
}

/// One hardware input, two outputs (Main 1), no worker threads.
struct Rig {
    engine: Engine,
}

impl Rig {
    fn new(channels: usize) -> Self {
        let engine = Engine::new(0);
        unity_console(&engine);
        unsafe { engine.prepare(48000.0, BLOCK, &[0], &[0, 1]) };
        engine.controls.output_protection.store(false, Relaxed); // sums go above 0 dBFS
        set_channels(&engine, channels);
        for i in 0..channels {
            engine.channel_at(i).unwrap().params.input_left.store(0, Relaxed); // all on input 1
        }
        Self { engine }
    }

    /// Runs `blocks` blocks with a unit impulse at sample 0 of the first and
    /// returns Main 1 left, after settling the ramps on silence.
    fn impulse(&self, blocks: usize) -> Vec<f32> {
        self.engine.collect(); // works out the compensation plan
        let mut input = vec![0.0f32; BLOCK];
        let mut out = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
        let run = |input: &[f32], out: &mut [Vec<f32>; 2]| {
            let outs: Vec<*mut f32> = out.iter_mut().map(|o| o.as_mut_ptr()).collect();
            unsafe { self.engine.process(&[input.as_ptr()], &outs, BLOCK) };
        };
        for _ in 0..(2 * MAX_PDC / BLOCK + 4) {
            run(&input, &mut out);
        }
        let mut captured = Vec::new();
        for b in 0..blocks {
            input[0] = if b == 0 { 1.0 } else { 0.0 };
            run(&input, &mut out);
            captured.extend_from_slice(&out[0]);
        }
        captured
    }
}

/// The samples where `signal` isn't silent, with their values.
fn peaks(signal: &[f32]) -> Vec<(usize, f32)> {
    signal.iter().enumerate().filter(|(_, x)| x.abs() > 1e-4).map(|(i, x)| (i, *x)).collect()
}

fn close(a: f32, b: f32) -> bool {
    (a - b).abs() < 1e-4
}

#[test]
fn channels_summed_into_a_main_line_up() {
    let rig = Rig::new(2);
    let e = &rig.engine;
    load(e, 0, 100);
    let ch = e.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);

    // Both channels' impulses arrive together, 100 samples late.
    let out = rig.impulse(4);
    assert_eq!(peaks(&out).len(), 1, "{:?}", peaks(&out));
    let (at, v) = peaks(&out)[0];
    assert_eq!(at, 100);
    assert!(close(v, 2.0 * CENTRE), "{v}");
    let main = e.main(0).unwrap();
    assert_eq!(main.pdc.target(), 100);
    assert_eq!(e.pdc().total.load(Relaxed), 100);
    assert!(e.channel_at(1).unwrap().pdc.has_ring(pdc_tap::OUT));
    assert!(!ch.pdc.has_ring(pdc_tap::OUT)); // the late one is read as it is

    // DELAY COMPENSATION off: the plain channel arrives first.
    e.pdc().enabled.store(false, Relaxed);
    let out = rig.impulse(4);
    let p = peaks(&out);
    assert_eq!(p.iter().map(|(i, _)| *i).collect::<Vec<_>>(), [0, 100]);
    assert_eq!(e.pdc().total.load(Relaxed), 0);
    assert!(!e.channel_at(1).unwrap().pdc.has_ring(pdc_tap::OUT)); // retired
    e.pdc().enabled.store(true, Relaxed);

    // A bypassed plugin adds no latency (it isn't run), so nothing is delayed.
    e.controls.fx[0].bypass.store(true, Relaxed);
    let out = rig.impulse(4);
    assert_eq!(peaks(&out).len(), 1);
    assert_eq!(peaks(&out)[0].0, 0);
}

#[test]
fn a_bus_with_a_plugin_lines_up_with_the_direct_path() {
    // CH 1 -> Main 1 directly and through Bus 1, which has a 48-sample plugin.
    let rig = Rig::new(1);
    let e = &rig.engine;
    load(e, 3, 48);
    let ch = e.channel_at(0).unwrap();
    let bus = e.strip_at(StripKind::Bus, 0).unwrap();
    let send = ch.sends.get_or_create(0).unwrap();
    send.enabled.store(true, Relaxed);
    send.mode.store(1, Relaxed); // POST
    bus.params.pre_insert.on.store(true, Relaxed);
    bus.params.pre_insert.fx.store(3, Relaxed);
    bus.params.mains[0].on.store(true, Relaxed);

    let out = rig.impulse(4);
    let p = peaks(&out);
    assert_eq!(p.len(), 1, "{p:?}");
    assert_eq!(p[0].0, 48);
    // The centred mono channel, directly and through the bus (panned in, stereo out).
    assert!(close(p[0].1, 2.0 * CENTRE), "{}", p[0].1);
    assert_eq!(bus.pdc.target(), 0);
    assert_eq!(bus.pdc.latency(pdc_tap::OUT), 48);
    assert_eq!(e.main(0).unwrap().pdc.target(), 48);

    // A plugin on the channel too: the bus sum is aligned first, then the main.
    load(e, 4, 20);
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(4, Relaxed);
    let out = rig.impulse(4);
    let p = peaks(&out);
    assert_eq!(p.len(), 1, "{p:?}");
    assert_eq!(p[0].0, 68);
    assert_eq!(e.pdc().total.load(Relaxed), 68);
}

#[test]
fn a_wet_dry_mix_delays_the_dry_signal() {
    let rig = Rig::new(1);
    let e = &rig.engine;
    load(e, 0, 30);
    let ch = e.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);
    e.controls.fx[0].mix.store(50.0);
    let out = rig.impulse(2);
    let p = peaks(&out);
    assert_eq!(p.len(), 1, "{p:?}");
    assert_eq!(p[0].0, 30);
    assert!(close(p[0].1, CENTRE), "{}", p[0].1);
}

#[test]
fn compensation_is_capped_and_reported() {
    let rig = Rig::new(2);
    let e = &rig.engine;
    load(e, 0, MAX_PDC + 500);
    let ch = e.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);
    let out = rig.impulse((MAX_PDC + 600) / BLOCK + 2);
    // The plain channel is delayed by the maximum; the plugin's path stays 500 later.
    let p = peaks(&out);
    assert_eq!(p.iter().map(|(i, _)| *i).collect::<Vec<_>>(), [MAX_PDC, MAX_PDC + 500]);
    assert!(e.pdc().limited.load(Relaxed));

    load(e, 0, 64);
    rig.engine.collect();
    assert!(!e.pdc().limited.load(Relaxed));
}

#[test]
fn the_tap_point_decides_whether_ins_1_counts() {
    let rig = Rig::new(2);
    let e = &rig.engine;
    load(e, 0, 40);
    let (a, b) = (e.channel_at(0).unwrap(), e.channel_at(1).unwrap());
    a.params.pre_insert.on.store(true, Relaxed);
    a.params.pre_insert.fx.store(0, Relaxed);
    // Both feed Bus 2 in TAP mode, tapped at the INPUT: before INS 1, no latency.
    for s in [&a, &b] {
        s.params.mains[0].on.store(false, Relaxed);
        s.params.tap.store(0, Relaxed);
        s.sends.get_or_create(1).unwrap().enabled.store(true, Relaxed);
    }
    let bus = e.strip_at(StripKind::Bus, 1).unwrap();
    bus.params.mains[0].on.store(true, Relaxed);
    let out = rig.impulse(2);
    assert_eq!(peaks(&out).len(), 1);
    assert_eq!(peaks(&out)[0].0, 0);
    assert_eq!(a.pdc.latency(pdc_tap::TAP), 0);

    // Tapped pre-fader (after INS 1): 40 samples, so CH 2 waits.
    a.params.tap.store(5, Relaxed);
    let out = rig.impulse(2);
    assert_eq!(peaks(&out).len(), 1, "{:?}", peaks(&out));
    assert_eq!(peaks(&out)[0].0, 40);
}
