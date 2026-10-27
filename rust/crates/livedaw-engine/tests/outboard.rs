//! The EXT device's outboard insert (WING manual 11.11 p. 164, Fig. 169):
//! the slot's input leaves through its FX SEND outputs and comes back from
//! the RETURN inputs, its latency compensated like a plugin's.

mod common;
use common::{set_channels, unity_console};
use livedaw_engine::fx::ext_return_input;
use livedaw_engine::params::{ext, StripKind, FX_EXT};
use livedaw_engine::{Engine, MAX_PDC};
use std::sync::atomic::Ordering::Relaxed;

const BLOCK: usize = 64;
/// Hardware inputs: 0 the microphone, 1-2 the outboard gear's return.
const MIC: i32 = 0;
const RETURN: i32 = 1;
/// Hardware outputs: 0-1 Main 1, 2-3 the FX SEND to the outboard gear.
const SEND_L: i32 = 2;
const SEND_R: i32 = 3;

/// The engine with an "outboard device": what leaves on outputs 2-3 comes
/// back on inputs 1-2 `extra` samples after the next block starts (the
/// interface's latency), through `gear` (a gain).
struct Rig {
    engine: Engine,
    extra: usize,
    gear: f32,
}

impl Rig {
    fn new(channels: usize, extra: usize) -> Self {
        let engine = Engine::new(0);
        unity_console(&engine);
        unsafe { engine.prepare(48000.0, BLOCK, &[0, 1, 2], &[0, 1, 2, 3]) };
        engine.set_io_latency(extra as u32);
        engine.controls.output_protection.store(false, Relaxed);
        set_channels(&engine, channels);
        for s in engine.strips(StripKind::Aux) {
            s.params.input_left.store(-1, Relaxed);
            s.params.input_right.store(-1, Relaxed);
        }
        for i in 0..channels {
            let p = &engine.channel_at(i).unwrap().params;
            p.input_left.store(MIC, Relaxed);
            p.input_right.store(MIC, Relaxed);
        }
        Self { engine, extra, gear: 1.0 }
    }

    /// FX 1 as an outboard insert on channel 1's PRE insert: RETURN A 2 (port
    /// 1) in `mode`, FX SEND on outputs 3-4.
    fn outboard(&self, mode: i32) {
        let e = &self.engine;
        assert!(e.set_fx_model(0, FX_EXT, true));
        let p = &e.controls.fx[0];
        p.values[ext::SOURCE].store(ext::SOURCE_OUTBOARD);
        p.values[ext::GROUP].store(3.0); // A
        p.values[ext::INPUT].store((RETURN + 1) as f32); // A 2
        p.values[ext::MODE].store(mode as f32);
        p.send_left.store(SEND_L, Relaxed);
        p.send_right.store(SEND_R, Relaxed);
        let ch = e.channel_at(0).unwrap();
        ch.params.pre_insert.on.store(true, Relaxed);
        ch.params.pre_insert.fx.store(0, Relaxed);
    }

    /// Runs `blocks` blocks with a unit impulse on the microphone at sample 0
    /// of the first (after settling on silence) and returns Main 1 left and
    /// the FX SEND left.
    fn impulse(&self, blocks: usize) -> (Vec<f32>, Vec<f32>) {
        self.engine.collect(); // the compensation plan
        let mut mic = vec![0.0f32; BLOCK];
        // The return path: the interface's extra latency, then the next block.
        let mut wire = [vec![0.0f32; BLOCK + self.extra], vec![0.0f32; BLOCK + self.extra]];
        let mut out = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK], vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
        let mut run = |mic: &[f32], out: &mut [Vec<f32>; 4]| {
            let ret: [Vec<f32>; 2] = std::array::from_fn(|c| wire[c][..BLOCK].to_vec());
            let outs: Vec<*mut f32> = out.iter_mut().map(|o| o.as_mut_ptr()).collect();
            unsafe { self.engine.process(&[mic.as_ptr(), ret[0].as_ptr(), ret[1].as_ptr()], &outs, BLOCK) };
            for c in 0..2 {
                wire[c].drain(..BLOCK);
                wire[c].extend(out[2 + c].iter().map(|x| x * self.gear));
            }
        };
        for _ in 0..(2 * MAX_PDC / BLOCK + 4) {
            run(&mic, &mut out);
        }
        let (mut main, mut send) = (Vec::new(), Vec::new());
        for b in 0..blocks {
            mic[0] = if b == 0 { 1.0 } else { 0.0 };
            run(&mic, &mut out);
            main.extend_from_slice(&out[0]);
            send.extend_from_slice(&out[2]);
        }
        (main, send)
    }
}

fn peaks(signal: &[f32]) -> Vec<(usize, f32)> {
    signal.iter().enumerate().filter(|(_, x)| x.abs() > 1e-4).map(|(i, x)| (i, *x)).collect()
}

fn close(a: f32, b: f32) -> bool {
    (a - b).abs() < 1e-4
}

#[test]
fn the_send_goes_out_and_the_return_comes_back_in_time() {
    let rig = Rig::new(2, 10);
    rig.outboard(ext::MODE_STEREO);
    let e = &rig.engine;
    let fx = e.fx(0).unwrap();
    let p = &e.controls.fx[0];
    // One block plus the interface's 10 samples.
    assert_eq!(fx.latency(p), BLOCK as u32 + 10);

    // The send leaves at once; channel 1 (through the gear) and channel 2
    // (direct) arrive at Main 1 together, a round trip late.
    let (main, send) = rig.impulse(4);
    assert_eq!(peaks(&send), [(0, 1.0)]);
    let at = BLOCK + 10;
    assert_eq!(peaks(&main).len(), 1, "{:?}", peaks(&main));
    assert_eq!(peaks(&main)[0].0, at);
    assert!(close(peaks(&main)[0].1, 2.0));
    assert_eq!(e.main(0).unwrap().pdc.target(), at as u32);

    // LATENCY (samples) covers the gear's own delay; TRIM sets the send level.
    p.values[ext::LATENCY].store(48.0); // samples
    p.values[ext::TRIM].store(-6.0);
    rig.engine.set_io_latency(10);
    let (main, send) = rig.impulse(4);
    assert!(close(send[0], 0.501_187), "{}", send[0]);
    let p2 = peaks(&main);
    // The gear has no delay of its own here, so the return now arrives 48
    // samples before the compensated direct channel.
    assert_eq!(p2.iter().map(|(i, _)| *i).collect::<Vec<_>>(), [at, at + 48]);
    assert!(close(p2[0].1, 0.501_187) && close(p2[1].1, 1.0));
}

#[test]
fn fx_mix_delays_the_dry_signal_by_the_round_trip() {
    let rig = Rig::new(1, 0);
    rig.outboard(ext::MODE_STEREO);
    rig.engine.controls.fx[0].mix.store(50.0);
    let (main, _) = rig.impulse(4);
    // Half the dry signal, delayed, meets half the return.
    assert_eq!(peaks(&main), [(BLOCK, 1.0)]);
}

#[test]
fn mono_and_mid_side_modes() {
    // MID/SIDE: the send is M and S, the return decoded back to L / R.
    let rig = Rig::new(1, 0);
    rig.outboard(ext::MODE_MID_SIDE);
    let (main, send) = rig.impulse(4);
    assert_eq!(peaks(&send), [(0, 1.0)]); // a mono channel: M = x, S = 0
    assert_eq!(peaks(&main), [(BLOCK, 1.0)]);

    // MONO: one return input on both sides; the gear's gain shows.
    let mut rig = Rig::new(1, 0);
    rig.gear = 0.5;
    rig.outboard(ext::MODE_MONO);
    let (main, _) = rig.impulse(4);
    assert_eq!(peaks(&main), [(BLOCK, 0.5)]);
}

#[test]
fn without_a_return_or_bypassed_the_slot_passes_through() {
    let rig = Rig::new(1, 0);
    rig.outboard(ext::MODE_STEREO);
    let e = &rig.engine;
    let p = &e.controls.fx[0];
    p.values[ext::GROUP].store(0.0); // RETURN OFF
    assert_eq!(e.fx(0).unwrap().latency(p), 0);
    let (main, send) = rig.impulse(2);
    assert_eq!(peaks(&main), [(0, 1.0)]);
    assert_eq!(peaks(&send), [(0, 1.0)], "the send still goes out");

    p.values[ext::GROUP].store(3.0);
    p.bypass.store(true, Relaxed);
    let (main, send) = rig.impulse(2);
    assert_eq!(peaks(&main), [(0, 1.0)]);
    assert!(peaks(&send).is_empty());
    assert_eq!(e.pdc().total.load(Relaxed), 0);

    // SOURCE PLUGIN without a plugin: an empty plugin slot, as before.
    p.bypass.store(false, Relaxed);
    p.values[ext::SOURCE].store(ext::SOURCE_PLUGIN);
    let (main, send) = rig.impulse(2);
    assert_eq!(peaks(&main), [(0, 1.0)]);
    assert!(peaks(&send).is_empty());
}

#[test]
fn return_groups_map_like_the_remote_patch() {
    assert_eq!(ext_return_input("OFF", 1), None);
    assert_eq!(ext_return_input("LCL", 3), Some(2));
    assert_eq!(ext_return_input("A", 3), Some(2));
    assert_eq!(ext_return_input("B", 1), Some(48));
    assert_eq!(ext_return_input("C", 2), Some(97));
    assert_eq!(ext_return_input("CRD", 1), Some(livedaw_engine::PLAYBACK_SOURCE_BASE));
    assert_eq!(ext_return_input("PLAY", 2), Some(livedaw_engine::USB_PLAYER_SOURCE_BASE + 1));
    assert_eq!(ext_return_input("PLAY", 9), None);
}
