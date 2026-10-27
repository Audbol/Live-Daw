//! The modulation devices' live readouts as their panels read them through
//! the slot (`FxRack::readout`, laid out by `fx::readout`): LFO positions,
//! envelope and needle.

mod common;
use common::{set_channels, unity_console};
use livedaw_dsp::wing::fx::readout::{ENVELOPE, LFO_L, LFO_R, NEEDLE};
use livedaw_dsp::wing::{model_index, FX_MODELS};
use livedaw_engine::Engine;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 256;
const SR: f32 = 48000.0;

fn engine() -> Arc<Engine> {
    let engine = Arc::new(Engine::new(0));
    unity_console(&engine);
    unsafe { engine.prepare(SR as f64, BLOCK, &[0], &[0, 1]) };
    set_channels(&engine, 1);
    engine
}

/// Runs `blocks` blocks of `signal` (a function of the sample index) into input 1.
fn run(engine: &Engine, from: usize, blocks: usize, signal: impl Fn(usize) -> f32) {
    let mut input = vec![0.0f32; BLOCK];
    let mut outs = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
    for b in 0..blocks {
        for (i, x) in input.iter_mut().enumerate() {
            *x = signal((from + b) * BLOCK + i);
        }
        let ins = [input.as_ptr()];
        let o: Vec<*mut f32> = outs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { engine.process(&ins, &o, BLOCK) };
    }
}

fn sine(amp: f32) -> impl Fn(usize) -> f32 {
    move |i| amp * (std::f32::consts::TAU * 440.0 * i as f32 / SR).sin()
}

/// Channel 1 through FX slot 1 running `token`.
fn through(engine: &Engine, token: &str) {
    assert!(engine.set_fx_model(0, model_index(FX_MODELS, token).unwrap(), true));
    let ch = engine.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);
}

#[test]
fn the_device_reports_its_lfo_envelope_and_needle() {
    let engine = engine();
    through(&engine, "PANNER");
    let m = model_index(FX_MODELS, "PANNER").unwrap();
    let p = &engine.controls.fx[0];
    let set = |id: &str, v: f32| p.values[FX_MODELS[m].param_index(id).unwrap()].store(v);
    set("spd", 2.0);
    set("phase", 180.0);
    set("depth", 100.0);
    let rack = engine.fx(0).unwrap();
    // The LFO turns at SPEED: 2 Hz is 1/24 of a cycle per 256-sample block... measured over half a second.
    run(&engine, 0, 2, sine(0.5));
    let start = rack.readout(LFO_L);
    let blocks = (0.5 * SR) as usize / BLOCK;
    let mut needle = (f32::MAX, f32::MIN);
    let mut turned = 0.0;
    let mut last = start;
    for b in 0..blocks {
        run(&engine, 2 + b, 1, sine(0.5));
        let ph = rack.readout(LFO_L);
        turned += (ph - last).rem_euclid(1.0);
        last = ph;
        let n = rack.readout(NEEDLE);
        needle = (needle.0.min(n), needle.1.max(n));
    }
    let secs = (blocks * BLOCK) as f32 / SR;
    assert!((turned / secs - 2.0).abs() < 0.05, "LFO at {} Hz", turned / secs);
    // PHASE 180: the right LFO half a cycle on; the needle swings L to R.
    let off = (rack.readout(LFO_R) - rack.readout(LFO_L)).rem_euclid(1.0);
    assert!((off - 0.5).abs() < 0.01, "right LFO {off} cycles on");
    assert!(needle.0 < -0.8 && needle.1 > 0.8, "needle {needle:?}");
    // The envelope follows the input.
    assert!(rack.readout(ENVELOPE) > 0.3);
    run(&engine, 200, 200, |_| 0.0);
    assert!(rack.readout(ENVELOPE) < 0.01);
    // A device without these reads 0; switching device clears them.
    assert!(engine.set_fx_model(0, model_index(FX_MODELS, "HALL").unwrap(), true));
    assert_eq!(engine.fx(0).unwrap().readout(LFO_L), 0.0);
}
