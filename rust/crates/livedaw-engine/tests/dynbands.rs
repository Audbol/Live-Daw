//! The dynamic EQs' live band gains, as the screens read them: the DYNAMICS
//! slot's DEQ2 (`Strip::band_meters`) and an FX slot's TRIPLE DEQ
//! (`FxRack::band_gain`), signed - cuts below 0, boosts above.

mod common;
use common::{set_channels, unity_console};
use livedaw_dsp::wing::{fx, model_index, ParamKind, DYN_MODELS, FX_MODELS};
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
fn run(engine: &Engine, blocks: usize, signal: impl Fn(usize) -> f32) {
    let mut input = vec![0.0f32; BLOCK];
    let mut outs = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
    for b in 0..blocks {
        for (i, x) in input.iter_mut().enumerate() {
            *x = signal(b * BLOCK + i);
        }
        let ins = [input.as_ptr()];
        let o: Vec<*mut f32> = outs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { engine.process(&ins, &o, BLOCK) };
    }
}

fn tones(i: usize) -> f32 {
    let t = i as f32 / SR;
    0.5 * (std::f32::consts::TAU * 200.0 * t).sin() + 0.5 * (std::f32::consts::TAU * 5000.0 * t).sin()
}

/// The value a parameter takes for `text` (a choice, a Values item or a number).
fn value(kind: ParamKind, text: &str) -> f32 {
    match kind {
        ParamKind::Choice(items) => items.iter().position(|t| *t == text).unwrap() as f32,
        ParamKind::Values(items) => items.iter().position(|v| (*v - text.parse::<f32>().unwrap()).abs() < 1e-3).unwrap() as f32,
        _ => text.parse().unwrap(),
    }
}

#[test]
fn dual_dynamic_eq_reports_each_band_up_or_down() {
    let engine = engine();
    let ch = engine.channel_at(0).unwrap();
    let m = model_index(DYN_MODELS, "DEQ2").unwrap();
    let d = &ch.params.dynamics;
    d.set_model(m);
    // Band 1 cuts 200 Hz by 6 dB, band 2 lifts 5 kHz by 4 dB (both above -40 dB).
    for (id, v) in [
        ("1-filt", "BP"),
        ("1-f", "200"),
        ("1-thr", "-40"),
        ("1-ratio", "10"),
        ("1-g", "-6"),
        ("1-mode", "high"),
        ("2-filt", "BP"),
        ("2-f", "5000"),
        ("2-thr", "-40"),
        ("2-ratio", "10"),
        ("2-g", "4"),
        ("2-mode", "high"),
    ] {
        let i = DYN_MODELS[m].param_index(id).unwrap();
        d.values[i].store(value(DYN_MODELS[m].params[i].kind, v));
    }
    d.on.store(true, Relaxed);
    run(&engine, 200, tones);
    let (b1, b2) = (ch.band_meters.get(false, 0), ch.band_meters.get(false, 1));
    assert!((b1 + 6.0).abs() < 0.3, "band 1: {b1} dB");
    assert!((b2 - 4.0).abs() < 0.3, "band 2: {b2} dB");
    // The reduction meter still shows only the cut.
    assert!((ch.meters.dyn_reduction_db.load() - 6.0).abs() < 0.3);
    // Out of range, the GATE slot, and the slot off: 0.
    assert_eq!(ch.band_meters.get(false, 7), 0.0);
    assert_eq!(ch.band_meters.get(true, 0), 0.0);
    d.on.store(false, Relaxed);
    run(&engine, 20, tones);
    assert_eq!(ch.band_meters.get(false, 0), 0.0);
    assert_eq!(ch.band_meters.get(false, 1), 0.0);
}

#[test]
fn triple_deq_reports_each_band_up_or_down() {
    let engine = engine();
    let ch = engine.channel_at(0).unwrap();
    let m = model_index(FX_MODELS, "DEQ3").unwrap();
    assert!(engine.set_fx_model(0, m, true));
    let p = &engine.controls.fx[0];
    let spec = &FX_MODELS[m];
    // Band 1: -6 dB at 200 Hz; band 2 off (GAIN 0); band 3: +4 dB at 5 kHz.
    for (id, v) in [
        ("1-filt", "BP"),
        ("1-f", "200"),
        ("1-thr", "-40"),
        ("1-ratio", "10"),
        ("1-g", "-6"),
        ("1-mode", "high"),
        ("2-g", "0"),
        ("3-filt", "BP"),
        ("3-f", "5000"),
        ("3-thr", "-40"),
        ("3-ratio", "10"),
        ("3-g", "4"),
        ("3-mode", "high"),
    ] {
        let i = spec.param_index(id).unwrap();
        p.values[i].store(value(spec.params[i].kind, v));
    }
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);
    run(&engine, 200, tones);
    let rack = engine.fx(0).unwrap();
    let g: Vec<f32> = (0..fx::FX_BANDS).map(|b| rack.band_gain(b)).collect();
    assert!((g[0] + 6.0).abs() < 0.3 && g[1] == 0.0 && (g[2] - 4.0).abs() < 0.3, "{g:?}");
    assert_eq!(rack.band_gain(fx::FX_BANDS), 0.0);
    // Another device: no bands.
    assert!(engine.set_fx_model(0, model_index(FX_MODELS, "DE-S2").unwrap(), true));
    run(&engine, 4, tones);
    assert_eq!(engine.fx(0).unwrap().band_gain(0), 0.0);
}
